#pragma once

#include <QObject>
#include <QString>
#include <QDateTime>
#include <QVariantMap>
#include <QNetworkAccessManager>
#include <QTimer>

class QNetworkReply;

class CloudDeckManagerApi : public QObject
{
    Q_OBJECT

public:
    explicit CloudDeckManagerApi(QObject *parent = nullptr);

    enum AuthStatus {
        AuthSuccess,
        AuthInvalidInput,
        AuthInProgress,
        AuthChallengeRequired,
        AuthNotAuthorized,
        AuthUserNotFound,
        AuthUserNotConfirmed,
        AuthPasswordResetRequired,
        AuthInvalidParameter,
        AuthInvalidPassword,
        AuthTooManyRequests,
        AuthLimitExceeded,
        AuthResourceNotFound,
        AuthInternalError,
        AuthNetworkError,
        AuthHttpError,
        AuthParseError,
        AuthUnknownError
    };
    Q_ENUM(AuthStatus)

    Q_INVOKABLE void loginWithCredentials(const QString &email, const QString &password);
    Q_INVOKABLE QString accessToken() const;
    Q_INVOKABLE QString idToken() const;
    Q_INVOKABLE QString refreshToken() const;
    Q_INVOKABLE QString tokenType() const;
    Q_INVOKABLE int accessTokenExpiresIn() const;
    Q_INVOKABLE int accessTokenSecondsRemaining() const;
    Q_INVOKABLE bool isAccessTokenExpired() const;
    Q_INVOKABLE bool hasStoredCredentials() const;
    Q_INVOKABLE void clearStoredCredentials();
    Q_INVOKABLE QString getStoredHostPassword() const;
    Q_INVOKABLE QString getStoredHostUser() const;
    Q_INVOKABLE QString getStoredServerAddress() const;
    Q_INVOKABLE QString getStoredEmail() const;
    Q_INVOKABLE QString getStoredPassword() const;
    Q_INVOKABLE int getSessionTimerHours() const;
    Q_INVOKABLE void setSessionTimerHours(int hours);
    Q_INVOKABLE int getSessionTimerDisplayMode() const;
    Q_INVOKABLE void setSessionTimerDisplayMode(int mode);
    Q_INVOKABLE int getSessionTimerWarnMinutes() const;
    Q_INVOKABLE void setSessionTimerWarnMinutes(int minutes);
    Q_INVOKABLE bool getSessionTimerHourlyReminderEnabled() const;
    Q_INVOKABLE void setSessionTimerHourlyReminderEnabled(bool enabled);
    Q_INVOKABLE int getSessionTimerHourlyReminderSeconds() const;
    Q_INVOKABLE void setSessionTimerHourlyReminderSeconds(int seconds);
    Q_INVOKABLE bool isCloudDeckHost(const QString &hostAddress) const;
    Q_INVOKABLE QString machineId() const;
    Q_INVOKABLE void fetchMachineId(const QString &accessToken);
    Q_INVOKABLE void fetchMachineStatus(const QString &machineId, const QString &accessToken);
    Q_INVOKABLE void startMachine(const QString &machineId, const QString &accessToken);
    Q_INVOKABLE void stopMachine(const QString &machineId, const QString &accessToken);
    Q_INVOKABLE QString machineStatus() const;
    Q_INVOKABLE QString machinePassword() const;
    Q_INVOKABLE QString machinePublicIp() const;
    Q_INVOKABLE qint64 machineLastStarted() const;
    Q_INVOKABLE qint64 machineCreatedAt() const;
    Q_INVOKABLE void addMachineClient(const QString &accessToken,
                                      const QString &machineId,
                                      const QString &pin);
    Q_INVOKABLE void cancelCurrentOperation();

signals:
    void loginCompleted(CloudDeckManagerApi::AuthStatus status,
                        const QString &accessToken,
                        int expiresIn,
                        const QString &idToken,
                        const QString &refreshToken,
                        const QString &tokenType,
                        const QString &errorCode,
                        const QString &errorMessage,
                        const QString &challengeName,
                        const QVariantMap &challengeParameters);
    void machineIdFetched(bool success,
                          const QString &machineId,
                          const QString &errorCode,
                          const QString &errorMessage);
    void machineStatusUpdated(const QString &status,
                              const QString &publicIp,
                              const QString &password,
                              qint64 lastStarted,
                              qint64 createdAt);
    void machineStatusFailed(const QString &errorCode, const QString &errorMessage);
    void machineStartFinished(bool success, const QString &status, const QString &errorCode, const QString &errorMessage);
    void machineStopFinished(bool success, const QString &status, const QString &errorCode, const QString &errorMessage);
    void machineClientAdded(bool success,
                            const QVariantMap &response,
                            const QString &errorCode,
                            const QString &errorMessage);

private slots:
    void handleLoginReply();
    void handleGetUserReply();
    void handleAccountReply();
    void handleMachineStatusReply();
    void handleMachineCommandReply();
    void handleMachineClientReply();
    void pollMachineStatus();

private:
    void finishLogin(AuthStatus status,
                     const QString &accessToken,
                     int expiresIn,
                     const QString &idToken,
                     const QString &refreshToken,
                     const QString &tokenType,
                     const QString &errorCode,
                     const QString &errorMessage,
                     const QString &challengeName,
                     const QVariantMap &challengeParameters);
    static QString normalizeErrorCode(const QString &rawCode);
    static AuthStatus mapErrorCodeToStatus(const QString &errorCode);
    static QString defaultMessageForStatus(AuthStatus status);
    void fetchAccountMachineId(const QString &accountId, const QString &accessToken);
    void sendMachineCommand(const QString &machineId, const QString &action, const QString &accessToken);

    QNetworkAccessManager *m_networkAccessManager;
    QNetworkReply *m_loginReply;
    QNetworkReply *m_getUserReply;
    QNetworkReply *m_accountReply;
    QNetworkReply *m_machineStatusReply;
    QNetworkReply *m_machineCommandReply;
    QNetworkReply *m_machineClientReply;
    bool m_loginInProgress;
    bool m_getUserInProgress;
    bool m_accountInProgress;
    QDateTime m_accessTokenExpiresAtUtc;
    QString m_accessToken;
    QString m_idToken;
    QString m_refreshToken;
    QString m_tokenType;
    int m_expiresIn;
    QString m_machineId;
    QString m_accountId;
    QString m_machineAccessToken;
    QString m_lastLoginEmail;
    QString m_lastLoginPassword;

    QString m_machineStatus;
    QString m_machinePassword;
    QString m_machinePublicIp;
    qint64 m_machineLastStarted;
    qint64 m_machineCreatedAt;

    enum MachineAction {
        MachineNone,
        MachineStarting,
        MachineStopping
    };
    MachineAction m_machineAction;
    int m_transitionCheckCount;
    QTimer *m_machinePollTimer;
};
