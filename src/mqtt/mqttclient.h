#pragma once

#include "phi/adapter/v1/tlsconfig.h"

#include <QByteArray>
#include <QObject>
#include <QThread>

namespace phicore {

class MqttWorker;

class MqttClient : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Disconnected = 0,
        Connecting,
        Connected
    };
    Q_ENUM(State)

    explicit MqttClient(QObject *parent = nullptr);
    ~MqttClient() override;

    void setClientId(const QString &clientId);
    void setHostname(const QString &hostname);
    void setPort(int port);
    void setUsername(const QString &username);
    /// How to reach the broker, in the vocabulary the SDK decided.
    ///
    /// Taken as the contract's own type rather than three arguments, so that
    /// there is no place between the operator's answer and the socket where
    /// this adapter could form a second opinion about what it meant.
    void setTls(const phicore::adapter::v1::TlsSettings &settings);
    void setPassword(const QString &password);
    void setKeepAlive(int keepAliveSeconds);
    void setCleanSession(bool cleanSession);

    /// What the client will send on CONNECT. Symmetric with the setters, and
    /// the only way to tell an adapter that was given no account from one that
    /// was given one and dropped it - which look identical from outside until
    /// a broker starts asking.
    QString username() const;
    [[nodiscard]] phicore::adapter::v1::TlsSettings tls() const;
    QString password() const;

    State state() const;

    void connectToHost();
    void disconnectFromHost();

    int publish(const QString &topic, const QByteArray &payload, int qos = 0, bool retain = false);
    bool subscribe(const QString &topicFilter, int qos = 0);

signals:
    void connected();
    void disconnected();
    void messageReceived(const QByteArray &message, const QString &topic);
    void errorOccurred(int code, const QString &message);
    void stateChanged(phicore::MqttClient::State state);

private:
    void setState(State state);
    void applyConfig();

    MqttWorker *m_worker = nullptr;
    QThread *m_workerThread = nullptr;

    QString m_clientId;
    QString m_hostname;
    QString m_username;
    QString m_password;
    phicore::adapter::v1::TlsSettings m_tls;
    int m_port = 1883;
    int m_keepAliveSeconds = 60;
    bool m_cleanSession = true;
    State m_state = State::Disconnected;
};

} // namespace phicore
