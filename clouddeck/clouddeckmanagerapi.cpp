#include "clouddeckmanagerapi.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QUrl>
#include <QDebug>
#include <limits>

namespace {

constexpr const char kCognitoEndpoint[] = "https://cognito-idp.eu-central-1.amazonaws.com/";
constexpr const char kCognitoClientId[] = "2e7an7pt3vqdae0abgskfs38k8";
constexpr const char kCloudDeckApiEndpoint[] = "https://api.clouddeck.app";
constexpr const char kSessionTimerHoursKey[] = "clouddeck/sessionTimerHours";
constexpr const char kSessionTimerDisplayModeKey[] = "clouddeck/sessionTimerDisplayMode";
constexpr const char kSessionTimerWarnMinutesKey[] = "clouddeck/sessionTimerWarnMinutes";
constexpr const char kSessionTimerHourlyReminderEnabledKey[] = "clouddeck/sessionTimerHourlyReminderEnabled";
constexpr const char kSessionTimerHourlyReminderSecondsKey[] = "clouddeck/sessionTimerHourlyReminderSeconds";
constexpr int kDefaultSessionTimerHours = 8;
constexpr int kMinSessionTimerHours = 1;
constexpr int kMaxSessionTimerHours = 24;
constexpr int kSessionTimerDisplayModeAlways = 0;
constexpr int kSessionTimerDisplayModeBeforeEnd = 1;
constexpr int kSessionTimerDisplayModeHidden = 2;
constexpr int kDefaultSessionTimerDisplayMode = kSessionTimerDisplayModeBeforeEnd;
constexpr int kMinSessionTimerDisplayMode = kSessionTimerDisplayModeAlways;
constexpr int kMaxSessionTimerDisplayMode = kSessionTimerDisplayModeHidden;
constexpr int kDefaultSessionTimerWarnMinutes = 5;
constexpr int kMinSessionTimerWarnMinutes = 1;
constexpr int kMaxSessionTimerWarnMinutes = 120;
constexpr bool kDefaultSessionTimerHourlyReminderEnabled = true;
constexpr int kDefaultSessionTimerHourlyReminderSeconds = 5;
constexpr int kMinSessionTimerHourlyReminderSeconds = 1;
constexpr int kMaxSessionTimerHourlyReminderSeconds = 60;

int sanitizeSessionTimerHours(int hours)
{
    if (hours < kMinSessionTimerHours) {
        return kDefaultSessionTimerHours;
    }
    if (hours > kMaxSessionTimerHours) {
        return kMaxSessionTimerHours;
    }
    return hours;
}

int sanitizeSessionTimerDisplayMode(int mode)
{
    if (mode < kMinSessionTimerDisplayMode || mode > kMaxSessionTimerDisplayMode) {
        return kDefaultSessionTimerDisplayMode;
    }
    return mode;
}

int sanitizeSessionTimerWarnMinutes(int minutes)
{
    if (minutes < kMinSessionTimerWarnMinutes) {
        return kDefaultSessionTimerWarnMinutes;
    }
    if (minutes > kMaxSessionTimerWarnMinutes) {
        return kMaxSessionTimerWarnMinutes;
    }
    return minutes;
}

int sanitizeSessionTimerHourlyReminderSeconds(int seconds)
{
    if (seconds < kMinSessionTimerHourlyReminderSeconds) {
        return kDefaultSessionTimerHourlyReminderSeconds;
    }
    if (seconds > kMaxSessionTimerHourlyReminderSeconds) {
        return kMaxSessionTimerHourlyReminderSeconds;
    }
    return seconds;
}

QString extractErrorCode(const QJsonObject &obj)
{
    QString code;

    if (obj.contains("__type") && obj.value("__type").isString()) {
        code = obj.value("__type").toString();
    } else if (obj.contains("code") && obj.value("code").isString()) {
        code = obj.value("code").toString();
    } else if (obj.contains("error") && obj.value("error").isString()) {
        code = obj.value("error").toString();
    } else if (obj.contains("Error") && obj.value("Error").isString()) {
        code = obj.value("Error").toString();
    } else if (obj.contains("errorCode") && obj.value("errorCode").isString()) {
        code = obj.value("errorCode").toString();
    }

    return code;
}

QString extractErrorMessage(const QJsonObject &obj)
{
    if (obj.contains("message") && obj.value("message").isString()) {
        return obj.value("message").toString();
    }
    if (obj.contains("Message") && obj.value("Message").isString()) {
        return obj.value("Message").toString();
    }
    if (obj.contains("error_description") && obj.value("error_description").isString()) {
        return obj.value("error_description").toString();
    }

    return QString();
}

QString normalizeAddress(const QString& value)
{
    QString trimmed = value.trimmed();
    if (trimmed.isEmpty()) {
        return trimmed;
    }

    QUrl url = QUrl::fromUserInput(trimmed);
    if (url.isValid() && !url.host().isEmpty()) {
        return url.host();
    }

    int slashIndex = trimmed.indexOf('/');
    if (slashIndex > 0) {
        trimmed = trimmed.left(slashIndex);
    }

    if (trimmed.startsWith('[')) {
        int closingBracket = trimmed.indexOf(']');
        if (closingBracket > 1) {
            return trimmed.mid(1, closingBracket - 1);
        }
    }

    int lastColon = trimmed.lastIndexOf(':');
    if (lastColon > 0 && trimmed.count(':') == 1) {
        return trimmed.left(lastColon);
    }

    return trimmed;
}

void applyAuthHeader(QNetworkRequest &request, const QString &accessToken)
{
    if (accessToken.trimmed().isEmpty()) {
        return;
    }
    request.setRawHeader("Authorization", QByteArray("Bearer ") + accessToken.toUtf8());
}

} // namespace

CloudDeckManagerApi::CloudDeckManagerApi(QObject *parent)
    : QObject(parent)
    , m_networkAccessManager(new QNetworkAccessManager(this))
    , m_loginReply(nullptr)
    , m_getUserReply(nullptr)
    , m_accountReply(nullptr)
    , m_machineStatusReply(nullptr)
    , m_machineCommandReply(nullptr)
    , m_machineClientReply(nullptr)
    , m_loginInProgress(false)
    , m_getUserInProgress(false)
    , m_accountInProgress(false)
    , m_accessTokenExpiresAtUtc()
    , m_accessToken()
    , m_idToken()
    , m_refreshToken()
    , m_tokenType()
    , m_expiresIn(0)
    , m_machineId()
    , m_accountId()
    , m_machineAccessToken()
    , m_lastLoginEmail()
    , m_lastLoginPassword()
    , m_machineStatus()
    , m_machinePassword()
    , m_machinePublicIp()
    , m_machineLastStarted(0)
    , m_machineCreatedAt(0)
    , m_machineAction(MachineNone)
    , m_transitionCheckCount(0)
    , m_machinePollTimer(new QTimer(this))
{
    m_networkAccessManager->setStrictTransportSecurityEnabled(true);
    m_networkAccessManager->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);

    m_machinePollTimer->setInterval(2000);
    connect(m_machinePollTimer, &QTimer::timeout, this, &CloudDeckManagerApi::pollMachineStatus);
}

QString CloudDeckManagerApi::accessToken() const
{
    return m_accessToken;
}

QString CloudDeckManagerApi::idToken() const
{
    return m_idToken;
}

QString CloudDeckManagerApi::refreshToken() const
{
    return m_refreshToken;
}

QString CloudDeckManagerApi::tokenType() const
{
    return m_tokenType;
}

int CloudDeckManagerApi::accessTokenExpiresIn() const
{
    return m_expiresIn;
}

QString CloudDeckManagerApi::machineId() const
{
    return m_machineId;
}

QString CloudDeckManagerApi::machineStatus() const
{
    return m_machineStatus;
}

QString CloudDeckManagerApi::machinePassword() const
{
    return m_machinePassword;
}

QString CloudDeckManagerApi::machinePublicIp() const
{
    return m_machinePublicIp;
}

qint64 CloudDeckManagerApi::machineLastStarted() const
{
    return m_machineLastStarted;
}

qint64 CloudDeckManagerApi::machineCreatedAt() const
{
    return m_machineCreatedAt;
}

void CloudDeckManagerApi::addMachineClient(const QString &accessToken,
                                           const QString &machineId,
                                           const QString &pin)
{
    const QString trimmedToken = accessToken.trimmed();
    const QString trimmedId = machineId.trimmed();
    const QString trimmedPin = pin.trimmed();

    if (trimmedToken.isEmpty()) {
        emit machineClientAdded(false, QVariantMap(), "EmptyAccessToken", "Access token is required");
        return;
    }
    if (trimmedId.isEmpty()) {
        emit machineClientAdded(false, QVariantMap(), "EmptyMachineId", "Machine ID is required");
        return;
    }
    if (trimmedPin.isEmpty()) {
        emit machineClientAdded(false, QVariantMap(), "EmptyPin", "PIN is required");
        return;
    }
    if (m_machineClientReply) {
        emit machineClientAdded(false, QVariantMap(), "InProgress", "Client add already in progress");
        return;
    }

    QJsonObject payload;
    payload.insert("pin", trimmedPin);

    const QUrl url(QString("%1/machines/%2/clients").arg(QLatin1String(kCloudDeckApiEndpoint), trimmedId));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    applyAuthHeader(request, trimmedToken);

    m_machineClientReply = m_networkAccessManager->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(m_machineClientReply, &QNetworkReply::finished, this, &CloudDeckManagerApi::handleMachineClientReply);
}

int CloudDeckManagerApi::accessTokenSecondsRemaining() const
{
    if (!m_accessTokenExpiresAtUtc.isValid()) {
        return 0;
    }

    const qint64 seconds = QDateTime::currentDateTimeUtc().secsTo(m_accessTokenExpiresAtUtc);
    if (seconds <= 0) {
        return 0;
    }
    if (seconds > std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(seconds);
}

bool CloudDeckManagerApi::isAccessTokenExpired() const
{
    return accessTokenSecondsRemaining() == 0;
}

bool CloudDeckManagerApi::hasStoredCredentials() const
{
    QSettings settings;
    return settings.contains("clouddeck/email") && settings.contains("clouddeck/password");
}

void CloudDeckManagerApi::clearStoredCredentials()
{
    QSettings settings;
    settings.remove("clouddeck/email");
    settings.remove("clouddeck/password");
    settings.remove("clouddeck/hostUser");
    settings.remove("clouddeck/hostPassword");
    settings.remove("clouddeck/serverAddress");
}

QString CloudDeckManagerApi::getStoredHostPassword() const
{
    QSettings settings;
    return settings.value("clouddeck/hostPassword").toString();
}

QString CloudDeckManagerApi::getStoredHostUser() const
{
    QSettings settings;
    QString hostUser = settings.value("clouddeck/hostUser").toString();
    if (!hostUser.isEmpty()) {
        return hostUser;
    }
    return settings.value("clouddeck/email").toString();
}

QString CloudDeckManagerApi::getStoredServerAddress() const
{
    QSettings settings;
    return settings.value("clouddeck/serverAddress").toString();
}

QString CloudDeckManagerApi::getStoredEmail() const
{
    QSettings settings;
    return settings.value("clouddeck/email").toString();
}

QString CloudDeckManagerApi::getStoredPassword() const
{
    QSettings settings;
    return settings.value("clouddeck/password").toString();
}

int CloudDeckManagerApi::getSessionTimerHours() const
{
    QSettings settings;
    return sanitizeSessionTimerHours(settings.value(QLatin1String(kSessionTimerHoursKey),
                                                    kDefaultSessionTimerHours).toInt());
}

void CloudDeckManagerApi::setSessionTimerHours(int hours)
{
    QSettings settings;
    settings.setValue(QLatin1String(kSessionTimerHoursKey), sanitizeSessionTimerHours(hours));
}

int CloudDeckManagerApi::getSessionTimerDisplayMode() const
{
    QSettings settings;
    return sanitizeSessionTimerDisplayMode(settings.value(QLatin1String(kSessionTimerDisplayModeKey),
                                                          kDefaultSessionTimerDisplayMode).toInt());
}

void CloudDeckManagerApi::setSessionTimerDisplayMode(int mode)
{
    QSettings settings;
    settings.setValue(QLatin1String(kSessionTimerDisplayModeKey), sanitizeSessionTimerDisplayMode(mode));
}

int CloudDeckManagerApi::getSessionTimerWarnMinutes() const
{
    QSettings settings;
    return sanitizeSessionTimerWarnMinutes(settings.value(QLatin1String(kSessionTimerWarnMinutesKey),
                                                          kDefaultSessionTimerWarnMinutes).toInt());
}

void CloudDeckManagerApi::setSessionTimerWarnMinutes(int minutes)
{
    QSettings settings;
    settings.setValue(QLatin1String(kSessionTimerWarnMinutesKey), sanitizeSessionTimerWarnMinutes(minutes));
}

bool CloudDeckManagerApi::getSessionTimerHourlyReminderEnabled() const
{
    QSettings settings;
    return settings.value(QLatin1String(kSessionTimerHourlyReminderEnabledKey),
                          kDefaultSessionTimerHourlyReminderEnabled).toBool();
}

void CloudDeckManagerApi::setSessionTimerHourlyReminderEnabled(bool enabled)
{
    QSettings settings;
    settings.setValue(QLatin1String(kSessionTimerHourlyReminderEnabledKey), enabled);
}

int CloudDeckManagerApi::getSessionTimerHourlyReminderSeconds() const
{
    QSettings settings;
    return sanitizeSessionTimerHourlyReminderSeconds(settings.value(QLatin1String(kSessionTimerHourlyReminderSecondsKey),
                                                                    kDefaultSessionTimerHourlyReminderSeconds).toInt());
}

void CloudDeckManagerApi::setSessionTimerHourlyReminderSeconds(int seconds)
{
    QSettings settings;
    settings.setValue(QLatin1String(kSessionTimerHourlyReminderSecondsKey),
                      sanitizeSessionTimerHourlyReminderSeconds(seconds));
}

bool CloudDeckManagerApi::isCloudDeckHost(const QString &hostAddress) const
{
    QSettings settings;
    const QString storedAddress = settings.value("clouddeck/serverAddress").toString();
    const QString normalizedHost = normalizeAddress(hostAddress);
    const QString normalizedStored = normalizeAddress(storedAddress);
    if (normalizedStored.isEmpty() || normalizedHost.isEmpty()) {
        return false;
    }
    return normalizedHost.compare(normalizedStored, Qt::CaseInsensitive) == 0;
}

void CloudDeckManagerApi::fetchMachineId(const QString &accessToken)
{
    const QString trimmedToken = accessToken.trimmed();
    if (trimmedToken.isEmpty()) {
        emit machineIdFetched(false, QString(), "EmptyAccessToken", "Access token is required");
        return;
    }
    if (m_getUserInProgress || m_accountInProgress) {
        emit machineIdFetched(false, QString(), "InProgress", "GetUser already in progress");
        return;
    }

    m_machineId.clear();
    m_accountId.clear();
    m_machineAccessToken = trimmedToken;

    QJsonObject payload;
    payload.insert("AccessToken", trimmedToken);

    QNetworkRequest request{QUrl(QLatin1String(kCognitoEndpoint))};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-amz-json-1.1");
    request.setRawHeader("X-Amz-Target", "AWSCognitoIdentityProviderService.GetUser");
    request.setRawHeader("X-Amz-User-Agent", "aws-amplify/5.0.4 auth framework/3");

    m_getUserInProgress = true;
    m_getUserReply = m_networkAccessManager->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(m_getUserReply, &QNetworkReply::finished, this, &CloudDeckManagerApi::handleGetUserReply);
}

void CloudDeckManagerApi::cancelCurrentOperation()
{
    if (m_loginReply) {
        m_loginReply->abort();
        m_loginReply->deleteLater();
        m_loginReply = nullptr;
    }
    if (m_getUserReply) {
        m_getUserReply->abort();
        m_getUserReply->deleteLater();
        m_getUserReply = nullptr;
    }
    if (m_accountReply) {
        m_accountReply->abort();
        m_accountReply->deleteLater();
        m_accountReply = nullptr;
    }
    if (m_machineStatusReply) {
        m_machineStatusReply->abort();
        m_machineStatusReply->deleteLater();
        m_machineStatusReply = nullptr;
    }
    if (m_machineCommandReply) {
        m_machineCommandReply->abort();
        m_machineCommandReply->deleteLater();
        m_machineCommandReply = nullptr;
    }
    if (m_machineClientReply) {
        m_machineClientReply->abort();
        m_machineClientReply->deleteLater();
        m_machineClientReply = nullptr;
    }

    m_machinePollTimer->stop();
    m_machineAction = MachineNone;
    m_transitionCheckCount = 0;
    m_loginInProgress = false;
    m_getUserInProgress = false;
    m_accountInProgress = false;
}

void CloudDeckManagerApi::fetchMachineStatus(const QString &machineId, const QString &accessToken)
{
    const QString trimmedId = machineId.trimmed();
    if (trimmedId.isEmpty()) {
        emit machineStatusFailed("EmptyMachineId", "Machine ID is required");
        return;
    }
    const QString trimmedToken = accessToken.trimmed();
    if (trimmedToken.isEmpty()) {
        emit machineStatusFailed("EmptyAccessToken", "Access token is required");
        return;
    }
    if (m_machineStatusReply) {
        return;
    }

    m_machineId = trimmedId;
    m_machineAccessToken = trimmedToken;

    const QUrl url(QString("%1/machines/%2").arg(QLatin1String(kCloudDeckApiEndpoint), trimmedId));
    QNetworkRequest request(url);
    applyAuthHeader(request, trimmedToken);

    m_machineStatusReply = m_networkAccessManager->get(request);
    connect(m_machineStatusReply, &QNetworkReply::finished, this, &CloudDeckManagerApi::handleMachineStatusReply);
}

void CloudDeckManagerApi::startMachine(const QString &machineId, const QString &accessToken)
{
    const QString trimmedId = machineId.trimmed();
    if (trimmedId.isEmpty()) {
        emit machineStartFinished(false, QString(), "EmptyMachineId", "Machine ID is required");
        return;
    }
    const QString trimmedToken = accessToken.trimmed();
    if (trimmedToken.isEmpty()) {
        emit machineStartFinished(false, QString(), "EmptyAccessToken", "Access token is required");
        return;
    }
    if (m_machineAction != MachineNone) {
        emit machineStartFinished(false, m_machineStatus, "InProgress", "Machine operation already in progress");
        return;
    }

    m_machineId = trimmedId;
    m_machineAccessToken = trimmedToken;
    m_machineAction = MachineStarting;
    m_transitionCheckCount = 0;

    sendMachineCommand(m_machineId, "start", trimmedToken);
    m_machinePollTimer->start();
}

void CloudDeckManagerApi::stopMachine(const QString &machineId, const QString &accessToken)
{
    const QString trimmedId = machineId.trimmed();
    if (trimmedId.isEmpty()) {
        emit machineStopFinished(false, QString(), "EmptyMachineId", "Machine ID is required");
        return;
    }
    const QString trimmedToken = accessToken.trimmed();
    if (trimmedToken.isEmpty()) {
        emit machineStopFinished(false, QString(), "EmptyAccessToken", "Access token is required");
        return;
    }
    if (m_machineAction != MachineNone) {
        emit machineStopFinished(false, m_machineStatus, "InProgress", "Machine operation already in progress");
        return;
    }

    m_machineId = trimmedId;
    m_machineAccessToken = trimmedToken;
    m_machineAction = MachineStopping;
    m_transitionCheckCount = 0;

    sendMachineCommand(m_machineId, "stop", trimmedToken);
    m_machinePollTimer->start();
}

void CloudDeckManagerApi::loginWithCredentials(const QString &email, const QString &password)
{
    if (m_loginInProgress) {
        finishLogin(AuthInProgress, QString(), 0, QString(), QString(), QString(),
                    "InProgress", "Login already in progress", QString(), QVariantMap());
        return;
    }

    const QString trimmedEmail = email.trimmed();
    const bool emailEmpty = trimmedEmail.isEmpty();
    const bool passwordEmpty = password.trimmed().isEmpty();

    if (emailEmpty && passwordEmpty) {
        finishLogin(AuthInvalidInput, QString(), 0, QString(), QString(), QString(),
                    "EmptyCredentials", "Email and password are required", QString(), QVariantMap());
        return;
    }
    if (emailEmpty) {
        finishLogin(AuthInvalidInput, QString(), 0, QString(), QString(), QString(),
                    "EmptyEmail", "Email is required", QString(), QVariantMap());
        return;
    }
    if (passwordEmpty) {
        finishLogin(AuthInvalidInput, QString(), 0, QString(), QString(), QString(),
                    "EmptyPassword", "Password is required", QString(), QVariantMap());
        return;
    }

    const int atIndex = trimmedEmail.indexOf('@');
    if (atIndex <= 0 || trimmedEmail.indexOf('.', atIndex) == -1) {
        finishLogin(AuthInvalidInput, QString(), 0, QString(), QString(), QString(),
                    "InvalidEmail", "Email address is invalid", QString(), QVariantMap());
        return;
    }

    m_lastLoginEmail = trimmedEmail;
    m_lastLoginPassword = password;

    QJsonObject authParams;
    authParams.insert("USERNAME", trimmedEmail);
    authParams.insert("PASSWORD", password);

    QJsonObject payload;
    payload.insert("AuthFlow", "USER_PASSWORD_AUTH");
    payload.insert("ClientId", QLatin1String(kCognitoClientId));
    payload.insert("AuthParameters", authParams);

    QNetworkRequest request{QUrl(QLatin1String(kCognitoEndpoint))};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-amz-json-1.1");
    request.setRawHeader("X-Amz-Target", "AWSCognitoIdentityProviderService.InitiateAuth");

    m_loginInProgress = true;
    m_loginReply = m_networkAccessManager->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(m_loginReply, &QNetworkReply::finished, this, &CloudDeckManagerApi::handleLoginReply);
}

void CloudDeckManagerApi::handleLoginReply()
{
    if (!m_loginReply) {
        finishLogin(AuthUnknownError, QString(), 0, QString(), QString(), QString(),
                    "MissingReply", "Login reply missing", QString(), QVariantMap());
        return;
    }

    QNetworkReply *reply = m_loginReply;
    m_loginReply = nullptr;

    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkErrorText = reply->errorString();
    const QByteArray body = reply->readAll();
    reply->deleteLater();

    if (networkError != QNetworkReply::NoError && body.isEmpty()) {
        finishLogin(AuthNetworkError, QString(), 0, QString(), QString(), QString(),
                    "NetworkError", networkErrorText, QString(), QVariantMap());
        return;
    }

    if (body.isEmpty()) {
        if (statusCode >= 400) {
            finishLogin(AuthHttpError, QString(), 0, QString(), QString(), QString(),
                        QString("Http%1").arg(statusCode),
                        QString("HTTP error %1").arg(statusCode),
                        QString(), QVariantMap());
        } else {
            finishLogin(AuthParseError, QString(), 0, QString(), QString(), QString(),
                        "EmptyResponse", "Empty response from server", QString(), QVariantMap());
        }
        return;
    }

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (doc.isNull() || !doc.isObject()) {
        QString parseMessage = parseError.errorString().isEmpty() ? "Invalid JSON response" : parseError.errorString();
        finishLogin(AuthParseError, QString(), 0, QString(), QString(), QString(),
                    "ParseError", parseMessage, QString(), QVariantMap());
        return;
    }

    const QJsonObject obj = doc.object();

    if (obj.contains("AuthenticationResult") && obj.value("AuthenticationResult").isObject()) {
        const QJsonObject result = obj.value("AuthenticationResult").toObject();
        const QString accessToken = result.value("AccessToken").toString();
        const int expiresIn = result.value("ExpiresIn").toInt();
        const QString idToken = result.value("IdToken").toString();
        const QString refreshToken = result.value("RefreshToken").toString();
        const QString tokenType = result.value("TokenType").toString();

        if (accessToken.isEmpty()) {
            finishLogin(AuthParseError, QString(), 0, QString(), QString(), QString(),
                        "MissingAccessToken", "Access token missing from response", QString(), QVariantMap());
            return;
        }

        finishLogin(AuthSuccess, accessToken, expiresIn, idToken, refreshToken, tokenType,
                    QString(), QString(), QString(), QVariantMap());
        return;
    }

    if (obj.contains("ChallengeName")) {
        const QString challengeName = obj.value("ChallengeName").toString();
        const QVariantMap challengeParams = obj.value("ChallengeParameters").toObject().toVariantMap();
        finishLogin(AuthChallengeRequired, QString(), 0, QString(), QString(), QString(),
                    "ChallengeRequired", "Additional authentication required", challengeName, challengeParams);
        return;
    }

    QString errorCode = normalizeErrorCode(extractErrorCode(obj));
    QString errorMessage = extractErrorMessage(obj);

    if (errorCode.isEmpty() && statusCode >= 400) {
        errorCode = QString("Http%1").arg(statusCode);
    }
    if (errorMessage.isEmpty() && statusCode >= 400) {
        errorMessage = QString("HTTP error %1").arg(statusCode);
    }

    if (errorCode.isEmpty()) {
        finishLogin(AuthUnknownError, QString(), 0, QString(), QString(), QString(),
                    "UnknownError", "Unknown login error", QString(), QVariantMap());
        return;
    }

    const AuthStatus status = mapErrorCodeToStatus(errorCode);
    if (errorMessage.isEmpty()) {
        errorMessage = defaultMessageForStatus(status);
    }

    finishLogin(status, QString(), 0, QString(), QString(), QString(),
                errorCode, errorMessage, QString(), QVariantMap());
}

void CloudDeckManagerApi::handleGetUserReply()
{
    if (!m_getUserReply) {
        m_getUserInProgress = false;
        emit machineIdFetched(false, QString(), "MissingReply", "GetUser reply missing");
        return;
    }

    QNetworkReply *reply = m_getUserReply;
    m_getUserReply = nullptr;

    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkErrorText = reply->errorString();
    const QByteArray body = reply->readAll();
    reply->deleteLater();

    m_getUserInProgress = false;

    if (networkError != QNetworkReply::NoError && body.isEmpty()) {
        emit machineIdFetched(false, QString(), "NetworkError", networkErrorText);
        return;
    }

    if (body.isEmpty()) {
        if (statusCode >= 400) {
            emit machineIdFetched(false, QString(),
                                  QString("Http%1").arg(statusCode),
                                  QString("HTTP error %1").arg(statusCode));
        } else {
            emit machineIdFetched(false, QString(), "EmptyResponse", "Empty response from server");
        }
        return;
    }

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (doc.isNull() || !doc.isObject()) {
        QString parseMessage = parseError.errorString().isEmpty() ? "Invalid JSON response" : parseError.errorString();
        emit machineIdFetched(false, QString(), "ParseError", parseMessage);
        return;
    }

    const QJsonObject obj = doc.object();
    if (obj.contains("UserAttributes") && obj.value("UserAttributes").isArray()) {
        const QJsonArray attrs = obj.value("UserAttributes").toArray();
        for (const QJsonValue &entry : attrs) {
            if (!entry.isObject()) {
                continue;
            }
            const QJsonObject attr = entry.toObject();
            const QString name = attr.value("Name").toString();
            if (name == "custom:account") {
                const QString value = attr.value("Value").toString();
                if (value.isEmpty()) {
                    emit machineIdFetched(false, QString(), "MissingAccountId", "custom:account value missing");
                    return;
                }
                m_accountId = value;
                fetchAccountMachineId(value, m_machineAccessToken);
                return;
            }
        }

        emit machineIdFetched(false, QString(), "MissingAccountId", "custom:account not found");
        return;
    }

    QString errorCode = normalizeErrorCode(extractErrorCode(obj));
    QString errorMessage = extractErrorMessage(obj);
    if (errorCode.isEmpty() && statusCode >= 400) {
        errorCode = QString("Http%1").arg(statusCode);
    }
    if (errorMessage.isEmpty() && statusCode >= 400) {
        errorMessage = QString("HTTP error %1").arg(statusCode);
    }
    if (errorCode.isEmpty()) {
        errorCode = "UnknownError";
    }
    if (errorMessage.isEmpty()) {
        errorMessage = "Unknown GetUser error";
    }
    emit machineIdFetched(false, QString(), errorCode, errorMessage);
}

void CloudDeckManagerApi::fetchAccountMachineId(const QString &accountId, const QString &accessToken)
{
    const QString trimmedAccountId = accountId.trimmed();
    if (trimmedAccountId.isEmpty()) {
        emit machineIdFetched(false, QString(), "EmptyAccountId", "Account ID is required");
        return;
    }
    const QString trimmedToken = accessToken.trimmed();
    if (trimmedToken.isEmpty()) {
        emit machineIdFetched(false, QString(), "EmptyAccessToken", "Access token is required");
        return;
    }
    if (m_accountInProgress) {
        emit machineIdFetched(false, QString(), "InProgress", "Account lookup already in progress");
        return;
    }

    m_accountId = trimmedAccountId;

    const QUrl url(QString("%1/accounts/%2").arg(QLatin1String(kCloudDeckApiEndpoint), trimmedAccountId));
    QNetworkRequest request(url);
    applyAuthHeader(request, trimmedToken);

    m_accountInProgress = true;
    m_accountReply = m_networkAccessManager->get(request);
    connect(m_accountReply, &QNetworkReply::finished, this, &CloudDeckManagerApi::handleAccountReply);
}

void CloudDeckManagerApi::handleAccountReply()
{
    if (!m_accountReply) {
        m_accountInProgress = false;
        emit machineIdFetched(false, QString(), "MissingReply", "Account reply missing");
        return;
    }

    QNetworkReply *reply = m_accountReply;
    m_accountReply = nullptr;

    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkErrorText = reply->errorString();
    const QByteArray body = reply->readAll();
    reply->deleteLater();

    m_accountInProgress = false;

    if (networkError != QNetworkReply::NoError && body.isEmpty()) {
        emit machineIdFetched(false, QString(), "NetworkError", networkErrorText);
        return;
    }

    if (body.isEmpty()) {
        if (statusCode >= 400) {
            emit machineIdFetched(false, QString(),
                                  QString("Http%1").arg(statusCode),
                                  QString("HTTP error %1").arg(statusCode));
        } else {
            emit machineIdFetched(false, QString(), "EmptyResponse", "Empty response from server");
        }
        return;
    }

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (doc.isNull() || !doc.isObject()) {
        QString parseMessage = parseError.errorString().isEmpty() ? "Invalid JSON response" : parseError.errorString();
        emit machineIdFetched(false, QString(), "ParseError", parseMessage);
        return;
    }

    const QJsonObject obj = doc.object();
    if (statusCode >= 400 || networkError != QNetworkReply::NoError) {
        QString errorCode = normalizeErrorCode(extractErrorCode(obj));
        QString errorMessage = extractErrorMessage(obj);
        if (errorCode.isEmpty()) {
            errorCode = networkError != QNetworkReply::NoError ? "NetworkError" : QString("Http%1").arg(statusCode);
        }
        if (errorMessage.isEmpty()) {
            errorMessage = networkError != QNetworkReply::NoError ? networkErrorText
                                                                  : QString("HTTP error %1").arg(statusCode);
        }
        emit machineIdFetched(false, QString(), errorCode, errorMessage);
        return;
    }

    const QString machineId = obj.value("machine_id").toString();
    if (machineId.isEmpty()) {
        emit machineIdFetched(false, QString(), "MissingMachineId", "machine_id missing from account response");
        return;
    }

    m_machineId = machineId;
    emit machineIdFetched(true, machineId, QString(), QString());
}

void CloudDeckManagerApi::handleMachineStatusReply()
{
    if (!m_machineStatusReply) {
        return;
    }

    QNetworkReply *reply = m_machineStatusReply;
    m_machineStatusReply = nullptr;

    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkErrorText = reply->errorString();
    const QByteArray body = reply->readAll();
    reply->deleteLater();

    if (networkError != QNetworkReply::NoError && body.isEmpty()) {
        emit machineStatusFailed("NetworkError", networkErrorText);
        return;
    }

    if (body.isEmpty()) {
        if (statusCode >= 400) {
            emit machineStatusFailed(QString("Http%1").arg(statusCode),
                                     QString("HTTP error %1").arg(statusCode));
        } else {
            emit machineStatusFailed("EmptyResponse", "Empty response from server");
        }
        return;
    }

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (doc.isNull() || !doc.isObject()) {
        QString parseMessage = parseError.errorString().isEmpty() ? "Invalid JSON response" : parseError.errorString();
        emit machineStatusFailed("ParseError", parseMessage);
        return;
    }

    const QJsonObject obj = doc.object();
    if (statusCode >= 400 || networkError != QNetworkReply::NoError) {
        QString errorCode = normalizeErrorCode(extractErrorCode(obj));
        QString errorMessage = extractErrorMessage(obj);
        if (errorCode.isEmpty()) {
            errorCode = networkError != QNetworkReply::NoError ? "NetworkError" : QString("Http%1").arg(statusCode);
        }
        if (errorMessage.isEmpty()) {
            errorMessage = networkError != QNetworkReply::NoError ? networkErrorText
                                                                  : QString("HTTP error %1").arg(statusCode);
        }
        emit machineStatusFailed(errorCode, errorMessage);
        return;
    }

    if (obj.contains("id") && obj.value("id").isString()) {
        m_machineId = obj.value("id").toString();
    }
    m_machineStatus = obj.value("status").toString();
    m_machinePassword = obj.value("password").toString();
    m_machinePublicIp = obj.value("public_ip").toString();
    m_machineLastStarted = static_cast<qint64>(obj.value("last_started").toDouble());
    m_machineCreatedAt = static_cast<qint64>(obj.value("created_at").toDouble());

    if (m_machineStatus.isEmpty()) {
        emit machineStatusFailed("MissingStatus", "Machine status missing from response");
        return;
    }

    QSettings settings;
    if (!m_machinePublicIp.isEmpty()) {
        settings.setValue("clouddeck/serverAddress", m_machinePublicIp);
    }
    if (!m_machinePassword.isEmpty()) {
        settings.setValue("clouddeck/hostPassword", m_machinePassword);
    }
    if (!m_lastLoginEmail.isEmpty()) {
        settings.setValue("clouddeck/hostUser", m_lastLoginEmail);
    }

    emit machineStatusUpdated(m_machineStatus, m_machinePublicIp, m_machinePassword,
                              m_machineLastStarted, m_machineCreatedAt);

    if (m_machineAction == MachineStarting) {
        if (m_machineStatus.compare("running", Qt::CaseInsensitive) == 0) {
            m_machinePollTimer->stop();
            m_machineAction = MachineNone;
            emit machineStartFinished(true, m_machineStatus, QString(), QString());
            return;
        }
        if (m_machineStatus.compare("starting", Qt::CaseInsensitive) == 0) {
            m_transitionCheckCount = 0;
            return;
        }

        m_transitionCheckCount++;
        if (m_transitionCheckCount >= 2) {
            m_transitionCheckCount = 0;
            sendMachineCommand(m_machineId, "start", m_machineAccessToken);
        }
        return;
    }

    if (m_machineAction == MachineStopping) {
        if (m_machineStatus.compare("off", Qt::CaseInsensitive) == 0) {
            m_machinePollTimer->stop();
            m_machineAction = MachineNone;
            emit machineStopFinished(true, m_machineStatus, QString(), QString());
            return;
        }
        if (m_machineStatus.compare("stopping", Qt::CaseInsensitive) == 0) {
            m_transitionCheckCount = 0;
            return;
        }

        m_transitionCheckCount++;
        if (m_transitionCheckCount >= 2) {
            m_transitionCheckCount = 0;
            sendMachineCommand(m_machineId, "stop", m_machineAccessToken);
        }
        return;
    }
}

void CloudDeckManagerApi::handleMachineCommandReply()
{
    if (!m_machineCommandReply) {
        return;
    }

    QNetworkReply *reply = m_machineCommandReply;
    m_machineCommandReply = nullptr;

    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkErrorText = reply->errorString();
    const QByteArray body = reply->readAll();
    reply->deleteLater();

    if (networkError == QNetworkReply::NoError && statusCode < 400) {
        return;
    }

    QString errorCode;
    QString errorMessage;
    if (!body.isEmpty()) {
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
        if (!doc.isNull() && doc.isObject()) {
            const QJsonObject obj = doc.object();
            errorCode = normalizeErrorCode(extractErrorCode(obj));
            errorMessage = extractErrorMessage(obj);
        }
    }

    if (errorCode.isEmpty()) {
        errorCode = networkError != QNetworkReply::NoError ? "NetworkError" : QString("Http%1").arg(statusCode);
    }
    if (errorMessage.isEmpty()) {
        errorMessage = networkError != QNetworkReply::NoError ? networkErrorText
                                                             : QString("HTTP error %1").arg(statusCode);
    }

    if (m_machineAction == MachineStarting) {
        m_machinePollTimer->stop();
        m_machineAction = MachineNone;
        emit machineStartFinished(false, m_machineStatus, errorCode, errorMessage);
        return;
    }
    if (m_machineAction == MachineStopping) {
        m_machinePollTimer->stop();
        m_machineAction = MachineNone;
        emit machineStopFinished(false, m_machineStatus, errorCode, errorMessage);
        return;
    }

    emit machineStatusFailed(errorCode, errorMessage);
}

void CloudDeckManagerApi::handleMachineClientReply()
{
    if (!m_machineClientReply) {
        return;
    }

    QNetworkReply *reply = m_machineClientReply;
    m_machineClientReply = nullptr;

    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkErrorText = reply->errorString();
    const QByteArray body = reply->readAll();
    reply->deleteLater();

    if (networkError != QNetworkReply::NoError && body.isEmpty()) {
        emit machineClientAdded(false, QVariantMap(), "NetworkError", networkErrorText);
        return;
    }

    QVariantMap responseMap;
    if (!body.isEmpty()) {
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
        if (!doc.isNull() && doc.isObject()) {
            responseMap = doc.object().toVariantMap();
        } else if (statusCode >= 400) {
            const QString parseMessage = parseError.errorString().isEmpty() ? "Invalid JSON response" : parseError.errorString();
            emit machineClientAdded(false, QVariantMap(), "ParseError", parseMessage);
            return;
        }
    }

    if (statusCode >= 400 || networkError != QNetworkReply::NoError) {
        QString errorCode;
        QString errorMessage;
        if (!responseMap.isEmpty()) {
            QJsonObject obj = QJsonObject::fromVariantMap(responseMap);
            errorCode = normalizeErrorCode(extractErrorCode(obj));
            errorMessage = extractErrorMessage(obj);
        }
        if (errorCode.isEmpty()) {
            errorCode = networkError != QNetworkReply::NoError ? "NetworkError" : QString("Http%1").arg(statusCode);
        }
        if (errorMessage.isEmpty()) {
            errorMessage = networkError != QNetworkReply::NoError ? networkErrorText
                                                                  : QString("HTTP error %1").arg(statusCode);
        }
        emit machineClientAdded(false, responseMap, errorCode, errorMessage);
        return;
    }

    emit machineClientAdded(true, responseMap, QString(), QString());
}

void CloudDeckManagerApi::pollMachineStatus()
{
    if (m_machineId.isEmpty()) {
        return;
    }
    if (m_machineAccessToken.isEmpty()) {
        m_machinePollTimer->stop();
        emit machineStatusFailed("EmptyAccessToken", "Access token is required");
        return;
    }
    fetchMachineStatus(m_machineId, m_machineAccessToken);
}

void CloudDeckManagerApi::sendMachineCommand(const QString &machineId, const QString &action, const QString &accessToken)
{
    if (machineId.trimmed().isEmpty()) {
        return;
    }
    if (accessToken.trimmed().isEmpty()) {
        if (m_machineAction == MachineStarting) {
            emit machineStartFinished(false, m_machineStatus, "EmptyAccessToken", "Access token is required");
        } else if (m_machineAction == MachineStopping) {
            emit machineStopFinished(false, m_machineStatus, "EmptyAccessToken", "Access token is required");
        } else {
            emit machineStatusFailed("EmptyAccessToken", "Access token is required");
        }
        return;
    }

    if (m_machineCommandReply) {
        m_machineCommandReply->abort();
        m_machineCommandReply->deleteLater();
        m_machineCommandReply = nullptr;
    }

    const QUrl url(QString("%1/machines/%2/%3").arg(QLatin1String(kCloudDeckApiEndpoint), machineId, action));
    QNetworkRequest request(url);
    applyAuthHeader(request, accessToken);

    m_machineCommandReply = m_networkAccessManager->post(request, QByteArray());
    connect(m_machineCommandReply, &QNetworkReply::finished, this, &CloudDeckManagerApi::handleMachineCommandReply);
}

void CloudDeckManagerApi::finishLogin(AuthStatus status,
                                      const QString &accessToken,
                                      int expiresIn,
                                      const QString &idToken,
                                      const QString &refreshToken,
                                      const QString &tokenType,
                                      const QString &errorCode,
                                      const QString &errorMessage,
                                      const QString &challengeName,
                                      const QVariantMap &challengeParameters)
{
    m_loginInProgress = false;
    if (status == AuthSuccess && expiresIn > 0) {
        m_accessTokenExpiresAtUtc = QDateTime::currentDateTimeUtc().addSecs(expiresIn);
        m_accessToken = accessToken;
        m_idToken = idToken;
        m_refreshToken = refreshToken;
        m_tokenType = tokenType;
        m_expiresIn = expiresIn;

        if (!m_lastLoginEmail.isEmpty() && !m_lastLoginPassword.isEmpty()) {
            QSettings settings;
            settings.setValue("clouddeck/email", m_lastLoginEmail);
            settings.setValue("clouddeck/password", m_lastLoginPassword);
            settings.setValue("clouddeck/hostUser", m_lastLoginEmail);
        }
    } else {
        m_accessTokenExpiresAtUtc = QDateTime();
        m_accessToken.clear();
        m_idToken.clear();
        m_refreshToken.clear();
        m_tokenType.clear();
        m_expiresIn = 0;
    }
    emit loginCompleted(status, accessToken, expiresIn, idToken, refreshToken, tokenType,
                        errorCode, errorMessage, challengeName, challengeParameters);
}

QString CloudDeckManagerApi::normalizeErrorCode(const QString &rawCode)
{
    if (rawCode.isEmpty()) {
        return rawCode;
    }

    QString code = rawCode;
    int hashIndex = code.lastIndexOf('#');
    if (hashIndex >= 0 && hashIndex + 1 < code.size()) {
        code = code.mid(hashIndex + 1);
    }
    int slashIndex = code.lastIndexOf('/');
    if (slashIndex >= 0 && slashIndex + 1 < code.size()) {
        code = code.mid(slashIndex + 1);
    }

    return code.trimmed();
}

CloudDeckManagerApi::AuthStatus CloudDeckManagerApi::mapErrorCodeToStatus(const QString &errorCode)
{
    if (errorCode == "EmptyCredentials" || errorCode == "EmptyEmail" || errorCode == "EmptyPassword" ||
        errorCode == "InvalidEmail") {
        return AuthInvalidInput;
    }
    if (errorCode == "NotAuthorizedException") {
        return AuthNotAuthorized;
    }
    if (errorCode == "UserNotFoundException") {
        return AuthUserNotFound;
    }
    if (errorCode == "UserNotConfirmedException") {
        return AuthUserNotConfirmed;
    }
    if (errorCode == "PasswordResetRequiredException") {
        return AuthPasswordResetRequired;
    }
    if (errorCode == "InvalidParameterException") {
        return AuthInvalidParameter;
    }
    if (errorCode == "InvalidPasswordException") {
        return AuthInvalidPassword;
    }
    if (errorCode == "TooManyRequestsException") {
        return AuthTooManyRequests;
    }
    if (errorCode == "LimitExceededException" || errorCode == "RequestLimitExceeded") {
        return AuthLimitExceeded;
    }
    if (errorCode == "ResourceNotFoundException") {
        return AuthResourceNotFound;
    }
    if (errorCode == "InternalErrorException" || errorCode == "InternalError") {
        return AuthInternalError;
    }
    if (errorCode == "NetworkError") {
        return AuthNetworkError;
    }
    if (errorCode.startsWith("Http")) {
        return AuthHttpError;
    }
    if (errorCode == "ParseError" || errorCode == "MissingAccessToken" || errorCode == "EmptyResponse") {
        return AuthParseError;
    }
    if (errorCode == "InProgress") {
        return AuthInProgress;
    }
    if (errorCode == "ChallengeRequired") {
        return AuthChallengeRequired;
    }

    return AuthUnknownError;
}

QString CloudDeckManagerApi::defaultMessageForStatus(CloudDeckManagerApi::AuthStatus status)
{
    switch (status) {
    case AuthInvalidInput:
        return "Invalid login input";
    case AuthInProgress:
        return "Login already in progress";
    case AuthChallengeRequired:
        return "Additional authentication required";
    case AuthNotAuthorized:
        return "Incorrect username or password";
    case AuthUserNotFound:
        return "User not found";
    case AuthUserNotConfirmed:
        return "User not confirmed";
    case AuthPasswordResetRequired:
        return "Password reset required";
    case AuthInvalidParameter:
        return "Invalid parameters";
    case AuthInvalidPassword:
        return "Invalid password";
    case AuthTooManyRequests:
        return "Too many requests";
    case AuthLimitExceeded:
        return "Request limit exceeded";
    case AuthResourceNotFound:
        return "Resource not found";
    case AuthInternalError:
        return "Internal server error";
    case AuthNetworkError:
        return "Network error";
    case AuthHttpError:
        return "HTTP error";
    case AuthParseError:
        return "Invalid response from server";
    case AuthUnknownError:
        return "Unknown error";
    case AuthSuccess:
        return QString();
    }

    return QString();
}
