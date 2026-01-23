#include "clouddeckmanager.h"

#ifdef HAVE_WEBENGINE
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineCookieStore>
#include <QDebug>
#include <QSettings>
#include <QStandardPaths>
#include <QDateTime>

namespace {
QString toBase64String(const QString& value)
{
    return QString::fromLatin1(value.toUtf8().toBase64());
}

}

CloudDeckManager::CloudDeckManager(QObject *parent)
    : QObject(parent)
    , m_webProfile(nullptr)
    , m_webPage(nullptr)
    , m_timeoutTimer(new QTimer(this))
    , m_pollTimer(new QTimer(this))
    , m_formSubmitted(false)
    , m_loginInProgress(false)
    , m_webEngineInitialized(false)
    , m_parseStep(0)
    , m_statusPollTimer(new QTimer(this))
    , m_waitingForInstanceStart(false)
    , m_operationMode(MODE_PAIRING)
    , m_pollCount(0)
    , m_pollStartTime(0)
{
    // Set up timeout timer
    m_timeoutTimer->setSingleShot(true);
    m_timeoutTimer->setInterval(30000); // 30 second timeout
    
    // Set up poll timer for SPA detection
    m_pollTimer->setInterval(2000); // Check every 2 seconds
    
    // Connect timeout signal
    connect(m_timeoutTimer, &QTimer::timeout, this, [this]() {
        m_pollTimer->stop();
        emit loginCompleted(false, "Login timeout");
        m_loginInProgress = false;
        m_formSubmitted = false;
    });
    
    // Connect poll timer to check page after form submission
    connect(m_pollTimer, &QTimer::timeout, this, &CloudDeckManager::checkPageAfterSubmission);
    
    // Set up status poll timer for instance start monitoring
    m_statusPollTimer->setInterval(5000); // Check every 5 seconds
    connect(m_statusPollTimer, &QTimer::timeout, this, &CloudDeckManager::pollInstanceStatus);
}

CloudDeckManager::~CloudDeckManager()
{
    if (m_webPage) {
        delete m_webPage;
    }
    if (m_webProfile) {
        delete m_webProfile;
    }
}

void CloudDeckManager::loginWithCredentials(const QString &email, const QString &password)
{
    qInfo() << "CloudDeck: loginWithCredentials called with email:" << email;

    if (m_loginInProgress) {
        qInfo() << "CloudDeck: Login already in progress, aborting";
        emit loginCompleted(false, "Login already in progress");
        return;
    }
    
    // Initialize web engine if not already done
    if (!m_webEngineInitialized) {
        qInfo() << "CloudDeck: Initializing web engine...";
        initializeWebEngine();
    }
    
    // Reset any in-flight state from a previous attempt
    m_timeoutTimer->stop();
    m_pollTimer->stop();
    m_statusPollTimer->stop();
    m_formSubmitted = false;
    m_waitingForInstanceStart = false;
    m_pollCount = 0;
    m_pollStartTime = 0;
    m_currentPin.clear();
    m_machineStatus.clear();
    m_userPassword.clear();
    m_sessionDuration.clear();
    m_serverAddress.clear();

    m_email = email;
    m_password = password;
    m_loginInProgress = true;
    
    qInfo() << "CloudDeck: Starting login process (mode: " << (m_operationMode == MODE_PAIRING ? "PAIRING" : "MANUAL_START") << ")...";
    
    // Emit status for pairing mode
    if (m_operationMode == MODE_PAIRING) {
        emit pairingStatusChanged("Connecting to CloudDeck...");
    }
    
    // Start timeout timer
    m_timeoutTimer->start();
    
    // Navigate to CloudDeck login page
    // Note: If already logged in from previous session, onPageLoadFinished will detect
    // the dashboard and proceed directly (no need to clear cookies)
    qInfo() << "CloudDeck: Loading login page...";
    m_webPage->load(QUrl("https://portal.clouddeck.app/login"));
}

void CloudDeckManager::startPairingWithCredentials(const QString &email, const QString &password)
{
    m_operationMode = MODE_PAIRING;
    loginWithCredentials(email, password);
}

void CloudDeckManager::startInstanceWithCredentials(const QString &email, const QString &password)
{
    m_operationMode = MODE_MANUAL_START;
    loginWithCredentials(email, password);
}

void CloudDeckManager::cancelCurrentOperation()
{
    if (m_timeoutTimer) {
        m_timeoutTimer->stop();
    }
    if (m_pollTimer) {
        m_pollTimer->stop();
    }
    if (m_statusPollTimer) {
        m_statusPollTimer->stop();
    }

    m_loginInProgress = false;
    m_formSubmitted = false;
    m_waitingForInstanceStart = false;
    m_pollCount = 0;
    m_pollStartTime = 0;
    m_operationMode = MODE_PAIRING;
    m_currentPin.clear();

    if (m_webPage) {
        m_webPage->triggerAction(QWebEnginePage::Stop);
    }
}

void CloudDeckManager::onPageLoadFinished(bool ok)
{
    qInfo() << "CloudDeck: Page load finished, ok=" << ok << ", URL:" << m_webPage->url().toString();
    
    if (!ok) {
        qInfo() << "CloudDeck: Page load FAILED";
        emit loginCompleted(false, "Failed to load CloudDeck login page");
        m_loginInProgress = false;
        m_timeoutTimer->stop();
        return;
    }
    
    // Check if we're on the login page
    m_webPage->toHtml([this](const QString &html) {
        qInfo() << "CloudDeck: Analyzing page content, length:" << html.length();
        qInfo() << "CloudDeck: Page title:" << m_webPage->title();
        
        // Check for login page indicators
        bool hasEmailInput = html.contains("type=\"email\"");
        bool hasPasswordInput = html.contains("type=\"password\"");
        bool hasLoginForm = html.contains("input-dark") || html.contains("login");
        
        qInfo() << "CloudDeck: Page analysis - hasEmail:" << hasEmailInput << "hasPassword:" << hasPasswordInput << "hasLoginForm:" << hasLoginForm;
        
        if (hasEmailInput && hasPasswordInput && hasLoginForm) {
            // We're on the login page, fill the form
            qInfo() << "CloudDeck: Detected login page, filling form";
            fillLoginForm(m_email, m_password);
        } else {
            // We might already be logged in (cookies from previous session) or on dashboard
            qInfo() << "CloudDeck: Not a login page - already logged in or redirected to dashboard";
            m_loginInProgress = false;
            m_timeoutTimer->stop();
            
            emit loginCompleted(true, "");
            
            // Continue with the appropriate flow based on operation mode
            if (m_operationMode == MODE_PAIRING) {
                qInfo() << "CloudDeck: [PAIRING] Already logged in, parsing dashboard...";
                emit pairingStatusChanged("Already logged in! Loading dashboard...");
                m_parseStep = 0;
                parseDashboard();
            } else if (m_operationMode == MODE_MANUAL_START) {
                qInfo() << "CloudDeck: [MANUAL_START] Already logged in, starting instance...";
                startInstanceOnly();
            }
        }
    });
}

void CloudDeckManager::fillLoginForm(const QString &email, const QString &password)
{
    qInfo() << "CloudDeck: Filling login form with email:" << email;
    
    if (m_operationMode == MODE_PAIRING) {
        emit pairingStatusChanged("Logging in...");
    }
    
    // JavaScript to fill and submit the login form (React/Vue compatible)
    const QString emailB64 = toBase64String(email);
    const QString passwordB64 = toBase64String(password);
    QString script = QString(R"(
        (function() {
            console.log('=== CloudDeck Form Fill Starting ===');

            const emailValue = atob('%1');
            const passwordValue = atob('%2');
            
            // Helper function to set input value in React/Vue compatible way
            function setNativeValue(element, value) {
                try {
                    const descriptor = Object.getOwnPropertyDescriptor(element, 'value');
                    const prototype = Object.getPrototypeOf(element);
                    const prototypeDescriptor = Object.getOwnPropertyDescriptor(prototype, 'value');
                    
                    if (descriptor && descriptor.set) {
                        if (prototypeDescriptor && prototypeDescriptor.set && descriptor.set !== prototypeDescriptor.set) {
                            prototypeDescriptor.set.call(element, value);
                        } else {
                            descriptor.set.call(element, value);
                        }
                    } else if (prototypeDescriptor && prototypeDescriptor.set) {
                        prototypeDescriptor.set.call(element, value);
                    } else {
                        element.value = value;
                    }
                } catch (e) {
                    console.log('setNativeValue fallback due to: ' + e.message);
                    element.value = value;
                }
            }
            
            // Helper to trigger all necessary events for React/Vue
            function triggerInputEvents(element) {
                element.dispatchEvent(new Event('input', { bubbles: true, cancelable: true }));
                element.dispatchEvent(new Event('change', { bubbles: true, cancelable: true }));
                element.dispatchEvent(new KeyboardEvent('keydown', { bubbles: true }));
                element.dispatchEvent(new KeyboardEvent('keyup', { bubbles: true }));
                element.dispatchEvent(new Event('blur', { bubbles: true }));
            }
            
            // Find and fill email input
            var emailInput = document.querySelector('input[type="email"]');
            if (emailInput) {
                console.log('Found email input: ' + emailInput.className);
                emailInput.focus();
                setNativeValue(emailInput, emailValue);
                triggerInputEvents(emailInput);
                console.log('Email filled, value now: ' + emailInput.value);
            } else {
                console.log('ERROR: Email input not found!');
            }
            
            // Find and fill password input
            var passwordInput = document.querySelector('input[type="password"]');
            if (passwordInput) {
                console.log('Found password input: ' + passwordInput.className);
                passwordInput.focus();
                setNativeValue(passwordInput, passwordValue);
                triggerInputEvents(passwordInput);
                console.log('Password filled, value length: ' + passwordInput.value.length);
            } else {
                console.log('ERROR: Password input not found!');
            }
            
            // List all buttons for debugging
            var allButtons = document.querySelectorAll('button');
            console.log('Found ' + allButtons.length + ' buttons:');
            for (var b = 0; b < allButtons.length; b++) {
                console.log('  Button ' + b + ': type=' + allButtons[b].type + ', text="' + allButtons[b].textContent.trim().substring(0,30) + '"');
            }
            
            // Wait for React/Vue to process state changes, then submit
            setTimeout(function() {
                console.log('=== Attempting form submission ===');
                
                // Try multiple submit strategies
                var submitButton = document.querySelector('button[type="submit"]');
                console.log('button[type=submit] found: ' + !!submitButton);
                
                if (!submitButton) {
                    // Look for any button with login-related text
                    var buttons = document.querySelectorAll('button');
                    for (var i = 0; i < buttons.length; i++) {
                        var text = buttons[i].textContent.toLowerCase();
                        if (text.includes('login') || text.includes('sign in') || text.includes('log in')) {
                            submitButton = buttons[i];
                            console.log('Found button by text: "' + text.trim() + '"');
                            break;
                        }
                    }
                }
                
                if (submitButton) {
                    console.log('Clicking submit button...');
                    submitButton.focus();
                    submitButton.click();
                    console.log('Button clicked!');
                } else {
                    console.log('No submit button found, trying form.submit()');
                    var form = document.querySelector('form');
                    if (form) {
                        console.log('Found form, submitting...');
                        form.submit();
                    } else {
                        console.log('ERROR: No form found either!');
                    }
                }
            }, 500);
            
            return 'Form fill initiated';
        })();
    )").arg(emailB64).arg(passwordB64);
    
    m_webPage->runJavaScript(script, [this](const QVariant &result) {
        qInfo() << "CloudDeck: Form fill result:" << result.toString();
        // Start polling for page changes (SPA doesn't trigger loadFinished)
        m_formSubmitted = true;
        m_pollTimer->start();
        qInfo() << "CloudDeck: Started polling for page changes after form submission";
    });
}

bool CloudDeckManager::hasStoredCredentials()
{
    QSettings settings;
    return settings.contains("clouddeck/email") && settings.contains("clouddeck/password");
}

void CloudDeckManager::clearStoredCredentials()
{
    QSettings settings;
    settings.remove("clouddeck/email");
    settings.remove("clouddeck/password");
    settings.remove("clouddeck/hostPassword");
    settings.remove("clouddeck/serverAddress");
}

QString CloudDeckManager::getStoredHostPassword()
{
    QSettings settings;
    return settings.value("clouddeck/hostPassword").toString();
}

QString CloudDeckManager::getStoredServerAddress()
{
    QSettings settings;
    return settings.value("clouddeck/serverAddress").toString();
}

QString CloudDeckManager::getStoredEmail()
{
    QSettings settings;
    return settings.value("clouddeck/email").toString();
}

QString CloudDeckManager::getStoredPassword()
{
    QSettings settings;
    return settings.value("clouddeck/password").toString();
}

bool CloudDeckManager::isCloudDeckHost(const QString &hostAddress)
{
    QSettings settings;
    QString storedAddress = settings.value("clouddeck/serverAddress").toString();
    qInfo() << "CloudDeck: isCloudDeckHost check - hostAddress:" << hostAddress << "storedAddress:" << storedAddress;
    if (storedAddress.isEmpty() || hostAddress.isEmpty()) {
        qInfo() << "CloudDeck: isCloudDeckHost - one address is empty, returning false";
        return false;
    }
    // Compare addresses (case-insensitive, handle potential variations)
    bool match = hostAddress.compare(storedAddress, Qt::CaseInsensitive) == 0;
    qInfo() << "CloudDeck: isCloudDeckHost - match:" << match;
    return match;
}

void CloudDeckManager::initializeWebEngine()
{
    if (m_webEngineInitialized) {
        return;
    }
    
    qInfo() << "CloudDeck: Creating QWebEnginePage (headless, no widget)...";
    
    // Initialize web engine components without QWidget
    m_webProfile = new QWebEngineProfile(this);
    m_webPage = new QWebEnginePage(m_webProfile, this);
    
    qInfo() << "CloudDeck: Configuring web engine settings...";
    
    // Configure web engine settings
    m_webPage->settings()->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
    m_webPage->settings()->setAttribute(QWebEngineSettings::LocalStorageEnabled, true);
    
    // Connect signals
    connect(m_webPage, &QWebEnginePage::loadFinished, this, &CloudDeckManager::onPageLoadFinished);

    m_webEngineInitialized = true;
    qInfo() << "CloudDeck: Web engine initialized successfully";
}

void CloudDeckManager::checkPageAfterSubmission()
{
    qInfo() << "CloudDeck: Polling - checking page state after form submission...";
    qInfo() << "CloudDeck: Current URL:" << m_webPage->url().toString();
    
    // Use JavaScript to check page state - more reliable than toHtml() string matching
    QString script = R"(
        (function() {
            var result = {};
            
            // Check if we're on login page
            result.hasEmailInput = !!document.querySelector('input[type="email"]');
            result.hasPasswordInput = !!document.querySelector('input[type="password"]');
            result.onLoginPage = result.hasEmailInput && result.hasPasswordInput;
            
            // Check for error messages using DOM queries
            // Case 1: Wrong username or password (after form submit)
            var errorTexts = document.body.innerText;
            result.hasWrongCredentials = errorTexts.includes('Wrong username or password');
            
            // Case 2: Validation errors (invalid email format or missing password)
            var validationErrors = document.querySelectorAll('p.text-red-600, p.text-red-500');
            result.validationErrorCount = validationErrors.length;
            result.validationErrors = [];
            for (var i = 0; i < validationErrors.length; i++) {
                var text = validationErrors[i].textContent.trim();
                result.validationErrors.push(text);
            }
            
            // Check for specific validation messages
            result.hasInvalidEmail = errorTexts.includes('Please provide a valid email');
            result.hasPasswordRequired = errorTexts.includes('Password is required');
            
            return JSON.stringify(result);
        })();
    )";
    
    m_webPage->runJavaScript(script, [this](const QVariant &result) {
        QString jsonStr = result.toString();
        qInfo() << "CloudDeck: Page state check result:" << jsonStr;
        
        // Parse JSON result
        bool onLoginPage = jsonStr.contains("\"onLoginPage\":true");
        bool hasWrongCredentials = jsonStr.contains("\"hasWrongCredentials\":true");
        bool hasInvalidEmail = jsonStr.contains("\"hasInvalidEmail\":true");
        bool hasPasswordRequired = jsonStr.contains("\"hasPasswordRequired\":true");
        
        qInfo() << "CloudDeck: onLoginPage:" << onLoginPage 
                << "hasWrongCredentials:" << hasWrongCredentials
                << "hasInvalidEmail:" << hasInvalidEmail
                << "hasPasswordRequired:" << hasPasswordRequired;
        
        if (!onLoginPage) {
            // We've navigated away from login page - success!
            m_pollTimer->stop();
            m_timeoutTimer->stop();
            m_loginInProgress = false;
            m_formSubmitted = false;
            
            qInfo() << "CloudDeck: Login successful!";
            
            // Store credentials for future use
            QSettings settings;
            settings.setValue("clouddeck/email", m_email);
            settings.setValue("clouddeck/password", m_password);
            qInfo() << "CloudDeck: Credentials stored for future use";
            
            emit loginCompleted(true, "");
            
            // Handle based on operation mode
            if (m_operationMode == MODE_PAIRING) {
                // Full flow: parse dashboard to get status and password
                qInfo() << "CloudDeck: [PAIRING] Parsing dashboard...";
                emit pairingStatusChanged("Login successful! Loading dashboard...");
                m_parseStep = 0;
                parseDashboard();
            } else if (m_operationMode == MODE_MANUAL_START) {
                // Just start the instance
                qInfo() << "CloudDeck: [MANUAL_START] Starting instance...";
                startInstanceOnly();
            }
        } else {
            // Still on login page - check for error messages
            if (hasWrongCredentials) {
                qInfo() << "CloudDeck: Login failed - wrong credentials";
                m_pollTimer->stop();
                m_timeoutTimer->stop();
                m_loginInProgress = false;
                m_formSubmitted = false;
                
                emit loginCompleted(false, "Wrong username or password");
            } else if (hasInvalidEmail) {
                qInfo() << "CloudDeck: Login failed - invalid email format";
                m_pollTimer->stop();
                m_timeoutTimer->stop();
                m_loginInProgress = false;
                m_formSubmitted = false;
                
                emit loginCompleted(false, "Please provide a valid email address");
            } else if (hasPasswordRequired) {
                qInfo() << "CloudDeck: Login failed - password required";
                m_pollTimer->stop();
                m_timeoutTimer->stop();
                m_loginInProgress = false;
                m_formSubmitted = false;
                
                emit loginCompleted(false, "Password is required");
            } else {
                qInfo() << "CloudDeck: Still on login page, waiting...";
            }
        }
    });
}

void CloudDeckManager::onLoginFormFilled()
{
    // Slot called when login form is filled
    // Currently handled by fillLoginForm() directly
}

void CloudDeckManager::parseDashboard()
{
    qInfo() << "CloudDeck: Parsing dashboard (step" << m_parseStep << ")...";
    
    // Wait for SPA content to fully load
    QTimer::singleShot(2000, this, [this]() {
        qInfo() << "CloudDeck: Running dashboard parse after delay...";
        
        // JavaScript to extract machine info and click Show password
        QString script = R"(
            (function() {
                var result = {};
                result.debug = [];
                
                // Debug: Check what elements exist
                result.debug.push('app-machine-status: ' + !!document.querySelector('app-machine-status'));
                result.debug.push('app-machine-info: ' + !!document.querySelector('app-machine-info'));
                result.debug.push('app-dashboard: ' + !!document.querySelector('app-dashboard'));
                
                // Get full body text for extraction
                var bodyText = document.body.innerText;
                result.debug.push('Body text length: ' + bodyText.length);
                
                // Extract machine status from app-machine-status or body text
                var statusElement = document.querySelector('app-machine-status');
                if (statusElement) {
                    result.status = statusElement.textContent.trim();
                } else if (bodyText.includes('Running')) {
                    result.status = 'Running';
                } else if (bodyText.includes('Stopped')) {
                    result.status = 'Stopped';
                } else {
                    result.status = 'Unknown';
                }
                
                // Extract session duration from body text
                var durationMatch = bodyText.match(/Session Duration[:\s]*(\d+h)/i);
                if (durationMatch) {
                    result.sessionDuration = durationMatch[1];
                } else {
                    // Try simpler pattern - just look for "Xh" pattern
                    var hourMatch = bodyText.match(/(\d+h)/);
                    if (hourMatch) {
                        result.sessionDuration = hourMatch[1];
                    } else {
                        result.sessionDuration = 'Unknown';
                    }
                }
                
                // Click Show button to reveal password
                var spans = document.querySelectorAll('span');
                for (var j = 0; j < spans.length; j++) {
                    if (spans[j].textContent.trim() === 'Show') {
                        spans[j].click();
                        result.clickedShow = true;
                        break;
                    }
                }
                
                return JSON.stringify(result);
            })();
        )";
        
        m_webPage->runJavaScript(script, [this](const QVariant &result) {
            QString jsonStr = result.toString();
            qInfo() << "CloudDeck: Dashboard parse result:" << jsonStr;
            
            // Parse the JSON result
            if (jsonStr.contains("status")) {
                // Extract status
                int statusStart = jsonStr.indexOf("\"status\":\"") + 10;
                int statusEnd = jsonStr.indexOf("\"", statusStart);
                if (statusStart > 9 && statusEnd > statusStart) {
                    m_machineStatus = jsonStr.mid(statusStart, statusEnd - statusStart);
                }
                
                // Extract session duration
                int durationStart = jsonStr.indexOf("\"sessionDuration\":\"") + 19;
                int durationEnd = jsonStr.indexOf("\"", durationStart);
                if (durationStart > 18 && durationEnd > durationStart) {
                    m_sessionDuration = jsonStr.mid(durationStart, durationEnd - durationStart);
                }
            }
            
            qInfo() << "CloudDeck: Status:" << m_machineStatus;
            qInfo() << "CloudDeck: Session Duration:" << m_sessionDuration;
            
            // Wait for SPA to update after clicking Show, then get password
            QTimer::singleShot(2500, this, &CloudDeckManager::clickShowPassword);
        });
    });
}

void CloudDeckManager::clickShowPassword()
{
    qInfo() << "CloudDeck: Getting password after Show click...";
    
    QString script = R"(
        (function() {
            var result = {};
            
            // Check if pre.inline element exists (password visible)
            var preElement = document.querySelector('pre.inline');
            result.preFound = !!preElement;
            
            if (preElement) {
                result.password = preElement.textContent.trim();
                return JSON.stringify(result);
            }
            
            // Check if Show button still exists (password not yet visible)
            var showButton = null;
            var spans = document.querySelectorAll('span');
            for (var j = 0; j < spans.length; j++) {
                if (spans[j].textContent.trim() === 'Show') {
                    showButton = spans[j];
                    break;
                }
            }
            result.showButtonExists = !!showButton;
            
            // If Show button exists, click it
            if (showButton) {
                showButton.click();
                result.clickedShow = true;
            }
            
            result.password = '';
            return JSON.stringify(result);
        })();
    )";
    
    m_webPage->runJavaScript(script, [this](const QVariant &result) {
        QString jsonStr = result.toString();
        qInfo() << "CloudDeck: Password check result:" << jsonStr;
        
        // Extract password from JSON
        if (jsonStr.contains("\"password\":\"") && !jsonStr.contains("\"password\":\"\"")) {
            int pwStart = jsonStr.indexOf("\"password\":\"") + 12;
            int pwEnd = jsonStr.indexOf("\"", pwStart);
            if (pwStart > 11 && pwEnd > pwStart) {
                m_userPassword = jsonStr.mid(pwStart, pwEnd - pwStart);
            }
        }
        
        if (m_userPassword.isEmpty()) {
            qInfo() << "CloudDeck: Password not visible yet, trying again...";
            // Try clicking Show again
            QString clickScript = R"(
                (function() {
                    var spans = document.querySelectorAll('span');
                    for (var j = 0; j < spans.length; j++) {
                        if (spans[j].textContent.trim() === 'Show') {
                            spans[j].click();
                            return 'clicked';
                        }
                    }
                    return 'not found';
                })();
            )";
            m_webPage->runJavaScript(clickScript, [this](const QVariant &clickResult) {
                qInfo() << "CloudDeck: Click Show result:" << clickResult.toString();
                QTimer::singleShot(2000, this, [this]() {
                    // Try one more time to get the password from <pre> element
                    QString getPasswordScript = R"(
                        (function() {
                            var preElement = document.querySelector('pre.inline');
                            if (preElement) {
                                return preElement.textContent.trim();
                            }
                            return 'password_not_found';
                        })();
                    )";
                    m_webPage->runJavaScript(getPasswordScript, [this](const QVariant &pwResult) {
                        m_userPassword = pwResult.toString();
                        qInfo() << "CloudDeck: Final password extraction:" << m_userPassword;
                        printMachineInfo();
                    });
                });
            });
        } else {
            printMachineInfo();
        }
    });
}

void CloudDeckManager::clickConnectButton()
{
    qInfo() << "CloudDeck: Clicking Connect button...";
    
    QString script = R"(
        (function() {
            // Helper to find button by class and text
            function findButton(className, text) {
                var btn = document.querySelector('button.' + className);
                if (btn && btn.textContent.includes(text)) return btn;
                // Fallback: search all buttons
                var buttons = document.querySelectorAll('button');
                for (var i = 0; i < buttons.length; i++) {
                    if (buttons[i].textContent.includes(text)) return buttons[i];
                }
                return null;
            }
            
            var btn = findButton('btn-primary', 'Connect');
            if (btn) {
                btn.click();
                return 'clicked';
            }
            return 'not_found';
        })();
    )";
    
    m_webPage->runJavaScript(script, [this](const QVariant &result) {
        qInfo() << "CloudDeck: Connect button click result:" << result.toString();
        
        if (result.toString() == "clicked") {
            // Wait for connection dialog to appear, then extract server address
            QTimer::singleShot(2000, this, &CloudDeckManager::extractServerAddress);
        } else {
            qInfo() << "CloudDeck: Connect button not found";
        }
    });
}

void CloudDeckManager::extractServerAddress()
{
    qInfo() << "CloudDeck: Extracting server address from connection dialog...";
    
    if (m_operationMode == MODE_PAIRING) {
        emit pairingStatusChanged("Extracting server address...");
    }
    
    QString script = R"(
        (function() {
            var result = {};
            
            // Look for the server address in div.input-dark > span.text-white
            var inputDark = document.querySelector('div.input-dark');
            if (inputDark) {
                var span = inputDark.querySelector('span.text-white');
                if (span) {
                    result.serverAddress = span.textContent.trim();
                }
            }
            
            // Check if connection dialog is visible
            var connectionInfo = document.querySelector('app-connection-info');
            result.dialogVisible = !!connectionInfo;
            
            return JSON.stringify(result);
        })();
    )";
    
    m_webPage->runJavaScript(script, [this](const QVariant &result) {
        QString jsonStr = result.toString();
        qInfo() << "CloudDeck: Server address extraction result:" << jsonStr;
        
        // Extract server address from JSON
        if (jsonStr.contains("\"serverAddress\":\"")) {
            int start = jsonStr.indexOf("\"serverAddress\":\"") + 17;
            int end = jsonStr.indexOf("\"", start);
            if (start > 16 && end > start) {
                m_serverAddress = jsonStr.mid(start, end - start);
                qInfo() << "CloudDeck: Server address:" << m_serverAddress;
                
                // Save CloudDeck server address for identification
                QSettings settings;
                settings.setValue("clouddeck/serverAddress", m_serverAddress);
                qInfo() << "CloudDeck: Server address saved to settings";
                
                // Emit signal with server address - Moonlight will add host and generate PIN
                emit serverAddressReady(m_serverAddress);
            }
        } else {
            qInfo() << "CloudDeck: Server address not found, dialog may not be open yet";
            // Retry after a short delay
            QTimer::singleShot(1000, this, &CloudDeckManager::extractServerAddress);
        }
    });
}

void CloudDeckManager::enterPinAndPair(const QString &pin)
{
    qInfo() << "CloudDeck: Entering PIN" << pin << "and clicking Pair...";
    
    // Store PIN for potential retry
    m_currentPin = pin;
    
    // First click Connect to ensure dialog is open
    QString openDialogScript = R"(
        (function() {
            var connectBtn = null;
            var buttons = document.querySelectorAll('button');
            for (var i = 0; i < buttons.length; i++) {
                if (buttons[i].textContent.includes('Connect')) {
                    connectBtn = buttons[i];
                    break;
                }
            }
            if (connectBtn) {
                connectBtn.click();
                return 'clicked_connect';
            }
            return 'connect_not_found';
        })();
    )";
    
    m_webPage->runJavaScript(openDialogScript, [this](const QVariant &result) {
        qInfo() << "CloudDeck: Open dialog result:" << result.toString();
        
        // Wait for dialog to open, then enter PIN
        QTimer::singleShot(2000, this, &CloudDeckManager::enterPinInDialog);
    });
}

void CloudDeckManager::enterPinInDialog()
{
    qInfo() << "CloudDeck: Entering PIN in dialog, PIN=" << m_currentPin;
    
    QString script = QString(R"(
        (function() {
            var result = {};
            
            // Find the PIN input field
            var pinInput = document.querySelector('input[maxlength="4"]');
            if (!pinInput) {
                pinInput = document.querySelector('input.input-dark[maxlength="4"]');
            }
            if (!pinInput) {
                // Try finding any text input that could be PIN
                var inputs = document.querySelectorAll('input[type="text"], input:not([type])');
                for (var i = 0; i < inputs.length; i++) {
                    var placeholder = inputs[i].placeholder || '';
                    if (inputs[i].maxLength === 4 || placeholder.toLowerCase().includes('pin')) {
                        pinInput = inputs[i];
                        break;
                    }
                }
            }
            
            if (!pinInput) {
                result.status = 'input_not_found';
                result.html = document.body.innerHTML.substring(0, 500);
                return JSON.stringify(result);
            }
            
            // Log current state
            result.oldValue = pinInput.value;
            result.inputId = pinInput.id;
            result.inputClass = pinInput.className;
            
            // Clear any existing value first
            var nativeInputValueSetter = Object.getOwnPropertyDescriptor(window.HTMLInputElement.prototype, 'value').set;
            nativeInputValueSetter.call(pinInput, '');
            pinInput.dispatchEvent(new Event('input', { bubbles: true }));
            
            // Now set the new PIN value
            nativeInputValueSetter.call(pinInput, '%1');
            pinInput.dispatchEvent(new Event('input', { bubbles: true }));
            pinInput.dispatchEvent(new Event('change', { bubbles: true }));
            
            // Verify it was set
            result.newValue = pinInput.value;
            result.status = (pinInput.value === '%1') ? 'pin_entered' : 'pin_mismatch';
            return JSON.stringify(result);
        })();
    )").arg(m_currentPin);
    
    m_webPage->runJavaScript(script, [this](const QVariant &result) {
        qInfo() << "CloudDeck: PIN entry result:" << result.toString();
        
        // Wait a moment for Angular to enable Pair button, then click it
        QTimer::singleShot(500, this, &CloudDeckManager::clickPairButton);
    });
}

void CloudDeckManager::clickPairButton()
{
    qInfo() << "CloudDeck: Clicking Pair button...";
    
    QString script = R"(
        (function() {
            var buttons = document.querySelectorAll('button');
            for (var i = 0; i < buttons.length; i++) {
                if (buttons[i].textContent.includes('Pair')) {
                    buttons[i].removeAttribute('disabled');
                    buttons[i].click();
                    return 'clicked';
                }
            }
            return 'not_found';
        })();
    )";
    
    m_webPage->runJavaScript(script, [this](const QVariant &result) {
        qInfo() << "CloudDeck: Pair button click result:" << result.toString();
        if (result.toString() == "clicked") {
            qInfo() << "CloudDeck: Pairing initiated on CloudDeck side";
        }
    });
}

void CloudDeckManager::printMachineInfo()
{
    qInfo() << "";
    qInfo() << "╔══════════════════════════════════════════════════════════════╗";
    qInfo() << "║                    CLOUDDECK MACHINE INFO                    ║";
    qInfo() << "╠══════════════════════════════════════════════════════════════╣";
    qInfo() << "║  Status:           " << m_machineStatus.leftJustified(42) << "║";
    qInfo() << "║  Session Duration: " << m_sessionDuration.leftJustified(42) << "║";
    qInfo() << "║  User Password:    " << m_userPassword.leftJustified(42) << "║";
    qInfo() << "╚══════════════════════════════════════════════════════════════╝";
    qInfo() << "";
    
    emit machineInfoReady(m_machineStatus, m_userPassword, m_sessionDuration);
    
    // Save host password to settings for later retrieval
    if (!m_userPassword.isEmpty()) {
        QSettings settings;
        settings.setValue("clouddeck/hostPassword", m_userPassword);
        qInfo() << "CloudDeck: Host password saved to settings";
    }
    
    // PAIRING MODE: Full flow with password and connection
    if (m_operationMode == MODE_PAIRING) {
        // Check if instance is off - if so, start it and wait
        if (m_machineStatus.contains("Off", Qt::CaseInsensitive) || 
            m_machineStatus.contains("Stopped", Qt::CaseInsensitive)) {
            qInfo() << "CloudDeck: [PAIRING] Instance is off, starting it...";
            emit pairingStatusChanged("Instance is off. Starting CloudDeck...");
            emit instanceStarting();
            QTimer::singleShot(1000, this, &CloudDeckManager::clickStartButton);
        } else {
            // Instance is running, proceed to connect
            qInfo() << "CloudDeck: [PAIRING] Instance is running, connecting...";
            emit pairingStatusChanged("Instance is running! Getting connection info...");
            QTimer::singleShot(1000, this, &CloudDeckManager::clickConnectButton);
        }
    }
    // MANUAL_START MODE: Just start, don't get password or connect
    else if (m_operationMode == MODE_MANUAL_START) {
        qInfo() << "CloudDeck: [MANUAL_START] Mode - skipping password/connect";
        // Already handled by startInstanceOnly()
    }
}

void CloudDeckManager::startCloudDeckInstance()
{
    qInfo() << "CloudDeck: startCloudDeckInstance() called";
    
    // Check if we have stored credentials
    if (!hasStoredCredentials()) {
        qInfo() << "CloudDeck: No stored credentials, cannot start instance";
        emit instanceStatusChanged("Error: No stored credentials");
        return;
    }
    
    // Set operation mode to MANUAL_START
    m_operationMode = MODE_MANUAL_START;
    
    // Initialize web engine if needed
    if (!m_webEngineInitialized) {
        qInfo() << "CloudDeck: Initializing web engine...";
        initializeWebEngine();
    }
    
    // ALWAYS go through the full login flow to ensure proper page state
    // This matches how pairing works and ensures the dashboard is properly loaded
    qInfo() << "CloudDeck: Starting fresh login flow with stored credentials...";
    QSettings settings;
    QString email = settings.value("clouddeck/email").toString();
    QString password = settings.value("clouddeck/password").toString();
    
    // Login flow will handle navigation and page loading properly
    loginWithCredentials(email, password);
}

void CloudDeckManager::startInstanceOnly()
{
    qInfo() << "CloudDeck: [MANUAL_START] Starting instance only (no password/connect)...";
    
    // Wait for dashboard to load, then check status
    QTimer::singleShot(2000, this, [this]() {
        checkInstanceStatusForManualStart();
    });
}

void CloudDeckManager::checkInstanceStatusForManualStart()
{
    qInfo() << "CloudDeck: [MANUAL_START] Checking instance status...";
    
    QString script = R"(
        (function() {
            var result = {};
            result.debug = [];
            
            // Debug: List all custom elements
            var customElements = document.querySelectorAll('*');
            var customTags = [];
            for (var i = 0; i < customElements.length; i++) {
                var tagName = customElements[i].tagName.toLowerCase();
                if (tagName.includes('app-') && customTags.indexOf(tagName) === -1) {
                    customTags.push(tagName);
                }
            }
            result.debug.push('Custom elements: ' + customTags.join(', '));
            
            // Debug: List all buttons
            var buttons = document.querySelectorAll('button');
            var buttonTexts = [];
            for (var i = 0; i < buttons.length; i++) {
                var text = buttons[i].textContent.trim();
                if (text) buttonTexts.push(text);
            }
            result.debug.push('Buttons: ' + buttonTexts.join(', '));
            
            // Get machine status - try multiple selectors
            var statusElement = document.querySelector('app-machine-status');
            if (statusElement) {
                result.status = statusElement.textContent.trim();
                result.debug.push('Found app-machine-status: ' + result.status);
            } else {
                // Try alternative selectors
                var infoElement = document.querySelector('app-machine-info');
                if (infoElement) {
                    var infoText = infoElement.textContent;
                    result.debug.push('Found app-machine-info text: ' + infoText.substring(0, 100));
                    
                    if (infoText.includes('Running')) {
                        result.status = 'Running';
                    } else if (infoText.includes('Starting')) {
                        result.status = 'Starting';
                    } else if (infoText.includes('Off') || infoText.includes('Stopped')) {
                        result.status = 'Off';
                    } else {
                        result.status = 'Unknown';
                    }
                } else {
                    // Fallback to body text
                    var bodyText = document.body.innerText;
                    result.debug.push('Body text length: ' + bodyText.length);
                    
                    if (bodyText.includes('Running')) {
                        result.status = 'Running';
                    } else if (bodyText.includes('Starting')) {
                        result.status = 'Starting';
                    } else if (bodyText.includes('Off') || bodyText.includes('Stopped')) {
                        result.status = 'Off';
                    } else {
                        result.status = 'Unknown';
                    }
                }
            }
            
            return JSON.stringify(result);
        })();
    )";
    
    m_webPage->runJavaScript(script, [this](const QVariant &result) {
        QString jsonStr = result.toString();
        qInfo() << "CloudDeck: [MANUAL_START] Status check result:" << jsonStr;
        
        if (jsonStr.contains("\"status\":\"Running\"")) {
            qInfo() << "CloudDeck: [MANUAL_START] Instance already running!";
            m_machineStatus = "Running";
            emit instanceReady();
            emit instanceStatusChanged("Running");
            handleInstanceRunningForManualStart();
        } else if (jsonStr.contains("\"status\":\"Starting\"")) {
            qInfo() << "CloudDeck: [MANUAL_START] Instance is starting, polling...";
            m_waitingForInstanceStart = true;
            emit instanceStarting();
            m_pollCount = 0;
            m_pollStartTime = QDateTime::currentSecsSinceEpoch();
            m_statusPollTimer->start();
        } else if (jsonStr.contains("\"status\":\"Off\"") || jsonStr.contains("\"status\":\"Stopped\"")) {
            qInfo() << "CloudDeck: [MANUAL_START] Instance is off, starting it...";
            clickStartButton();
        } else {
            qInfo() << "CloudDeck: [MANUAL_START] Unknown status, attempting to start...";
            clickStartButton();
        }
    });
}

void CloudDeckManager::clickStartButton()
{
    qInfo() << "CloudDeck: Clicking Start button...";
    
    // Reset poll tracking when starting
    m_pollCount = 0;
    m_pollStartTime = QDateTime::currentSecsSinceEpoch();
    
    QString script = R"(
        (function() {
            var result = {};
            result.debug = [];
            
            // Helper to find button by class and text (with exclusions)
            function findButton(className, text, excludeTexts) {
                excludeTexts = excludeTexts || [];
                var btn = document.querySelector('button.' + className);
                if (btn && btn.textContent.includes(text)) {
                    var shouldExclude = excludeTexts.some(function(ex) {
                        return btn.textContent.toLowerCase().includes(ex);
                    });
                    if (!shouldExclude) return btn;
                }
                var buttons = document.querySelectorAll('button');
                for (var i = 0; i < buttons.length; i++) {
                    if (buttons[i].textContent.includes(text)) {
                        var shouldExclude = excludeTexts.some(function(ex) {
                            return buttons[i].textContent.toLowerCase().includes(ex);
                        });
                        if (!shouldExclude) return buttons[i];
                    }
                }
                return null;
            }
            
            // Log available buttons for debugging
            var buttons = document.querySelectorAll('button');
            var allButtonTexts = [];
            for (var i = 0; i < buttons.length; i++) {
                var t = buttons[i].textContent.trim();
                if (t) allButtonTexts.push(t);
            }
            result.debug.push('Buttons: ' + allButtonTexts.join(', '));
            
            var btn = findButton('btn-primary', 'Start', ['stop', 'restart']);
            if (btn) {
                result.debug.push('Clicking: ' + btn.textContent.trim());
                btn.click();
                result.clicked = true;
                result.buttonText = btn.textContent.trim();
                return JSON.stringify(result);
            }
            
            result.clicked = false;
            result.error = 'Start button not found';
            result.availableButtons = allButtonTexts;
            return JSON.stringify(result);
        })();
    )";
    
    m_webPage->runJavaScript(script, [this](const QVariant &result) {
        QString jsonStr = result.toString();
        qInfo() << "CloudDeck: Start button click result:" << jsonStr;
        
        if (jsonStr.contains("\"clicked\":true")) {
            qInfo() << "CloudDeck: Start button clicked successfully, waiting for instance to start...";
            m_waitingForInstanceStart = true;
            emit instanceStarting();
            
            // Start polling for status changes
            m_statusPollTimer->start();
        } else {
            qInfo() << "CloudDeck: Failed to click Start button";
        }
    });
}

void CloudDeckManager::pollInstanceStatus()
{
    if (m_pollStartTime == 0) {
        m_pollStartTime = QDateTime::currentSecsSinceEpoch();
        m_pollCount = 0;
    }
    m_pollCount++;
    qint64 elapsedSeconds = QDateTime::currentSecsSinceEpoch() - m_pollStartTime;
    int elapsedMinutes = elapsedSeconds / 60;
    int remainingSeconds = elapsedSeconds % 60;
    
    qInfo() << "CloudDeck: Polling instance status (check #" << m_pollCount << ", elapsed:" << elapsedMinutes << "m" << remainingSeconds << "s)...";
    
    // Refresh the page every 3rd poll to get fresh status from server
    if (m_pollCount % 3 == 0) {
        qInfo() << "CloudDeck: Refreshing page to get fresh status...";
        m_webPage->triggerAction(QWebEnginePage::Reload);
        // Wait for page to reload before checking status
        QTimer::singleShot(2000, this, [this]() {
            checkInstanceStatusAfterRefresh();
        });
        return;
    }
    
    QString script = R"(
        (function() {
            var result = {};
            
            // Get machine status
            var statusElement = document.querySelector('app-machine-status');
            if (statusElement) {
                result.status = statusElement.textContent.trim();
            } else {
                // Try to find status in body text
                var bodyText = document.body.innerText;
                if (bodyText.includes('Running')) {
                    result.status = 'Running';
                } else if (bodyText.includes('Starting')) {
                    result.status = 'Starting';
                } else if (bodyText.includes('Off')) {
                    result.status = 'Off';
                } else if (bodyText.includes('Stopped')) {
                    result.status = 'Stopped';
                } else {
                    result.status = 'Unknown';
                }
            }
            
            return JSON.stringify(result);
        })();
    )";
    
    m_webPage->runJavaScript(script, [this](const QVariant &result) {
        QString jsonStr = result.toString();
        qInfo() << "CloudDeck: Status poll result:" << jsonStr;
        
        // Extract status from JSON
        QString status = "Unknown";
        if (jsonStr.contains("\"status\":\"")) {
            int start = jsonStr.indexOf("\"status\":\"") + 10;
            int end = jsonStr.indexOf("\"", start);
            if (start > 9 && end > start) {
                status = jsonStr.mid(start, end - start);
            }
        }
        
        qInfo() << "CloudDeck: Current status:" << status;
        m_machineStatus = status;
        
        // Calculate elapsed time for user-friendly messages
        qint64 elapsed = QDateTime::currentSecsSinceEpoch() - m_pollStartTime;
        int mins = elapsed / 60;
        int secs = elapsed % 60;
        QString timeStr = mins > 0 ? QString("%1m %2s").arg(mins).arg(secs) : QString("%1s").arg(secs);
        
        // Check if instance is now running
        if (status.contains("Running", Qt::CaseInsensitive)) {
            qInfo() << "CloudDeck: Instance is now running!";
            m_statusPollTimer->stop();
            m_waitingForInstanceStart = false;
            
            // Emit final status with total time
            QString finalStatus = QString("Running (started in %1)").arg(timeStr);
            emit instanceStatusChanged(finalStatus);
            emit instanceReady();
            
            // Handle based on operation mode
            if (m_operationMode == MODE_PAIRING) {
                handleInstanceRunningForPairing();
            } else {
                handleInstanceRunningForManualStart();
            }
        } else if (status.contains("Starting", Qt::CaseInsensitive)) {
            qInfo() << "CloudDeck: Instance is starting, continuing to poll...";
            
            // Timeout after 3 minutes (180 seconds)
            if (elapsed > 180) {
                qInfo() << "CloudDeck: Instance start timeout after 3 minutes";
                m_statusPollTimer->stop();
                m_waitingForInstanceStart = false;
                emit instanceStatusChanged("Timeout: Instance taking too long to start");
                if (m_operationMode == MODE_PAIRING) {
                    emit pairingCompleted(false, "CloudDeck instance is taking too long to start. Please try again or check CloudDeck portal.");
                } else {
                    m_operationMode = MODE_PAIRING;
                }
                return;
            }
            
            // Emit status with timing info
            QString statusMsg = QString("Starting... (waiting %1, check #%2)\nNext check in 5 seconds...").arg(timeStr).arg(m_pollCount);
            emit instanceStatusChanged(statusMsg);
            
            if (m_operationMode == MODE_PAIRING) {
                emit pairingStatusChanged(QString("Instance is starting...\n\nWaiting: %1 | Check #%2\nNext check in 5 seconds...").arg(timeStr).arg(m_pollCount));
            }
        } else if (status.contains("Off", Qt::CaseInsensitive) || 
                   status.contains("Stopped", Qt::CaseInsensitive)) {
            qInfo() << "CloudDeck: Instance is still off/stopped";
            
            // Timeout after 3 minutes (180 seconds)
            if (elapsed > 180) {
                qInfo() << "CloudDeck: Instance start timeout after 3 minutes";
                m_statusPollTimer->stop();
                m_waitingForInstanceStart = false;
                emit instanceStatusChanged("Timeout: Instance failed to start");
                if (m_operationMode == MODE_PAIRING) {
                    emit pairingCompleted(false, "CloudDeck instance failed to start. Please check CloudDeck portal.");
                } else {
                    m_operationMode = MODE_PAIRING;
                }
                return;
            }
            
            QString statusMsg = QString("Off/Stopped (waiting %1, check #%2)\nNext check in 5 seconds...").arg(timeStr).arg(m_pollCount);
            emit instanceStatusChanged(statusMsg);
            
            if (m_operationMode == MODE_PAIRING) {
                emit pairingStatusChanged(QString("Waiting for instance to start...\n\nWaiting: %1 | Check #%2\nNext check in 5 seconds...").arg(timeStr).arg(m_pollCount));
            }
        } else {
            // Unknown status - also timeout after 3 minutes
            if (elapsed > 180) {
                qInfo() << "CloudDeck: Unknown status timeout after 3 minutes";
                m_statusPollTimer->stop();
                m_waitingForInstanceStart = false;
                emit instanceStatusChanged("Timeout: Unable to determine instance status");
                if (m_operationMode == MODE_PAIRING) {
                    emit pairingCompleted(false, "Unable to determine CloudDeck instance status. Please check CloudDeck portal.");
                } else {
                    m_operationMode = MODE_PAIRING;
                }
                return;
            }
            
            QString statusMsg = QString("%1 (waiting %2, check #%3)\nNext check in 5 seconds...").arg(status).arg(timeStr).arg(m_pollCount);
            emit instanceStatusChanged(statusMsg);
        }
    });
}

void CloudDeckManager::checkInstanceStatus()
{
    qInfo() << "CloudDeck: checkInstanceStatus() called";
    pollInstanceStatus();
}

void CloudDeckManager::waitForInstanceRunning()
{
    qInfo() << "CloudDeck: waitForInstanceRunning() - starting status polling...";
    
    // Reset poll tracking
    m_pollCount = 0;
    m_pollStartTime = QDateTime::currentSecsSinceEpoch();
    
    if (m_operationMode == MODE_PAIRING) {
        emit pairingStatusChanged("Waiting for instance to start...\n\nChecking status every 5 seconds...");
    }
    
    m_waitingForInstanceStart = true;
    m_statusPollTimer->start();
}

void CloudDeckManager::handleInstanceRunningForPairing()
{
    qInfo() << "CloudDeck: [PAIRING] Instance running, getting password and connecting...";
    emit pairingStatusChanged("Instance running! Preparing to connect...");
    
    // Re-parse dashboard to get updated info including password
    m_parseStep = 0;
    QTimer::singleShot(2000, this, &CloudDeckManager::parseDashboard);
}

void CloudDeckManager::handleInstanceRunningForManualStart()
{
    qInfo() << "CloudDeck: [MANUAL_START] Instance running, done!";
    // Just notify that instance is ready, don't get password or connect
    // The UI will show success message via instanceReady() signal
    m_operationMode = MODE_PAIRING;
}

void CloudDeckManager::checkInstanceStatusAfterRefresh()
{
    qInfo() << "CloudDeck: Checking status after page refresh...";
    
    QString script = R"(
        (function() {
            var result = {};
            
            // Get machine status
            var statusElement = document.querySelector('app-machine-status');
            if (statusElement) {
                result.status = statusElement.textContent.trim();
            } else {
                // Try to find status in body text
                var bodyText = document.body.innerText;
                if (bodyText.includes('Running')) {
                    result.status = 'Running';
                } else if (bodyText.includes('Starting')) {
                    result.status = 'Starting';
                } else if (bodyText.includes('Off')) {
                    result.status = 'Off';
                } else if (bodyText.includes('Stopped')) {
                    result.status = 'Stopped';
                } else {
                    result.status = 'Unknown';
                }
            }
            
            return JSON.stringify(result);
        })();
    )";
    
    m_webPage->runJavaScript(script, [this](const QVariant &result) {
        QString jsonStr = result.toString();
        qInfo() << "CloudDeck: Status after refresh:" << jsonStr;
        
        // Extract status from JSON
        QString status = "Unknown";
        if (jsonStr.contains("\"status\":\"")) {
            int start = jsonStr.indexOf("\"status\":\"") + 10;
            int end = jsonStr.indexOf("\"", start);
            if (start > 9 && end > start) {
                status = jsonStr.mid(start, end - start);
            }
        }
        
        qInfo() << "CloudDeck: Current status:" << status;
        m_machineStatus = status;
        
        // Calculate elapsed time
        qint64 elapsed = QDateTime::currentSecsSinceEpoch() - m_pollStartTime;
        int mins = elapsed / 60;
        int secs = elapsed % 60;
        QString timeStr = mins > 0 ? QString("%1m %2s").arg(mins).arg(secs) : QString("%1s").arg(secs);
        
        // Check if instance is now running
        if (status.contains("Running", Qt::CaseInsensitive)) {
            qInfo() << "CloudDeck: Instance is now running!";
            m_statusPollTimer->stop();
            m_waitingForInstanceStart = false;
            
            QString finalStatus = QString("Running (started in %1)").arg(timeStr);
            emit instanceStatusChanged(finalStatus);
            emit instanceReady();
            
            if (m_operationMode == MODE_PAIRING) {
                handleInstanceRunningForPairing();
            } else {
                handleInstanceRunningForManualStart();
            }
        } else if (status.contains("Starting", Qt::CaseInsensitive)) {
            QString statusMsg = QString("Starting... (waiting %1, check #%2)").arg(timeStr).arg(m_pollCount);
            emit instanceStatusChanged(statusMsg);
            
            if (m_operationMode == MODE_PAIRING) {
                emit pairingStatusChanged(QString("Instance is starting...\n\nWaiting: %1 | Check #%2").arg(timeStr).arg(m_pollCount));
            }
        } else {
            // Check for timeout
            if (elapsed > 180) {
                qInfo() << "CloudDeck: Instance start timeout after 3 minutes";
                m_statusPollTimer->stop();
                m_waitingForInstanceStart = false;
                emit instanceStatusChanged("Timeout: Instance failed to start");
                if (m_operationMode == MODE_PAIRING) {
                    emit pairingCompleted(false, "CloudDeck instance is taking too long to start. Please try again.");
                } else {
                    m_operationMode = MODE_PAIRING;
                }
            } else {
                QString statusMsg = QString("Off/Stopped (waiting %1, check #%2)").arg(timeStr).arg(m_pollCount);
                emit instanceStatusChanged(statusMsg);
            }
        }
    });
}

#else
#include <QDebug>
#include <QSettings>

CloudDeckManager::CloudDeckManager(QObject *parent)
    : QObject(parent)
    , m_webProfile(nullptr)
    , m_webPage(nullptr)
    , m_timeoutTimer(nullptr)
    , m_pollTimer(nullptr)
    , m_formSubmitted(false)
    , m_loginInProgress(false)
    , m_webEngineInitialized(false)
    , m_parseStep(0)
    , m_statusPollTimer(nullptr)
    , m_waitingForInstanceStart(false)
    , m_operationMode(MODE_PAIRING)
    , m_pollCount(0)
    , m_pollStartTime(0)
{
    qInfo() << "CloudDeck: QtWebEngine not available; CloudDeck disabled";
}

CloudDeckManager::~CloudDeckManager() = default;

void CloudDeckManager::loginWithCredentials(const QString &email, const QString &password)
{
    Q_UNUSED(email)
    Q_UNUSED(password)
    emit loginCompleted(false, "CloudDeck is not supported in this build");
}

void CloudDeckManager::startPairingWithCredentials(const QString &email, const QString &password)
{
    loginWithCredentials(email, password);
}

void CloudDeckManager::startInstanceWithCredentials(const QString &email, const QString &password)
{
    Q_UNUSED(email)
    Q_UNUSED(password)
    emit instanceStatusChanged("Error: CloudDeck is not supported in this build");
}

bool CloudDeckManager::hasStoredCredentials()
{
    QSettings settings;
    return settings.contains("clouddeck/email") && settings.contains("clouddeck/password");
}

void CloudDeckManager::clearStoredCredentials()
{
    QSettings settings;
    settings.remove("clouddeck/email");
    settings.remove("clouddeck/password");
    settings.remove("clouddeck/hostPassword");
    settings.remove("clouddeck/serverAddress");
}

void CloudDeckManager::enterPinAndPair(const QString &pin)
{
    Q_UNUSED(pin)
    emit pairingCompleted(false, "CloudDeck is not supported in this build");
}

void CloudDeckManager::startCloudDeckInstance()
{
    emit instanceStatusChanged("Error: CloudDeck is not supported in this build");
}

void CloudDeckManager::checkInstanceStatus()
{
    emit instanceStatusChanged("Error: CloudDeck is not supported in this build");
}

void CloudDeckManager::cancelCurrentOperation()
{
}

QString CloudDeckManager::getStoredHostPassword()
{
    QSettings settings;
    return settings.value("clouddeck/hostPassword").toString();
}

QString CloudDeckManager::getStoredServerAddress()
{
    QSettings settings;
    return settings.value("clouddeck/serverAddress").toString();
}

QString CloudDeckManager::getStoredEmail()
{
    QSettings settings;
    return settings.value("clouddeck/email").toString();
}

QString CloudDeckManager::getStoredPassword()
{
    QSettings settings;
    return settings.value("clouddeck/password").toString();
}

bool CloudDeckManager::isCloudDeckHost(const QString &hostAddress)
{
    Q_UNUSED(hostAddress)
    return false;
}

void CloudDeckManager::onPageLoadFinished(bool ok)
{
    Q_UNUSED(ok)
}

void CloudDeckManager::onLoginFormFilled()
{
}

void CloudDeckManager::pollInstanceStatus()
{
}
#endif
