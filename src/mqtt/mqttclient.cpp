#include "mqttclient.h"

#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>

#include <mosquitto.h>

namespace phicore {

class MosquittoRuntime
{
public:
    MosquittoRuntime()
    {
        QMutexLocker locker(&s_mutex);
        if (s_refCount == 0)
            mosquitto_lib_init();
        ++s_refCount;
    }

    ~MosquittoRuntime()
    {
        QMutexLocker locker(&s_mutex);
        --s_refCount;
        if (s_refCount == 0)
            mosquitto_lib_cleanup();
    }

private:
    static QMutex s_mutex;
    static int s_refCount;
};

QMutex MosquittoRuntime::s_mutex;
int MosquittoRuntime::s_refCount = 0;

class MqttWorker : public QObject
{
    Q_OBJECT

public:
    explicit MqttWorker(QObject *parent = nullptr)
        : QObject(parent)
        , m_runtime()
    {
    }

    ~MqttWorker() override
    {
        cleanup();
    }

    Q_INVOKABLE void setClientId(const QString &clientId) { m_clientId = clientId; }
    Q_INVOKABLE void setHostname(const QString &hostname) { m_hostname = hostname; }
    Q_INVOKABLE void setPort(int port) { m_port = port; }
    Q_INVOKABLE void setUsername(const QString &username) { m_username = username; }
    Q_INVOKABLE void setPassword(const QString &password) { m_password = password; }
    Q_INVOKABLE void setKeepAlive(int keepAliveSeconds) { m_keepAliveSeconds = keepAliveSeconds; }
    Q_INVOKABLE void setCleanSession(bool cleanSession) { m_cleanSession = cleanSession; }

    Q_INVOKABLE void connectToHost()
    {
        if (m_hostname.trimmed().isEmpty()) {
            emit errorOccurred(MOSQ_ERR_INVAL, QStringLiteral("MQTT hostname is empty"));
            return;
        }
        if (m_state == MqttClient::State::Connecting || m_state == MqttClient::State::Connected)
            return;

        if (!m_mosq) {
            const QByteArray clientIdBytes = m_clientId.toUtf8();
            const char *clientId = clientIdBytes.isEmpty() ? nullptr : clientIdBytes.constData();
            m_mosq = mosquitto_new(clientId, m_cleanSession, this);
            if (!m_mosq) {
                emit errorOccurred(MOSQ_ERR_NOMEM, QStringLiteral("Failed to allocate mosquitto client"));
                return;
            }
            mosquitto_connect_callback_set(m_mosq, &MqttWorker::handleConnect);
            mosquitto_disconnect_callback_set(m_mosq, &MqttWorker::handleDisconnect);
            mosquitto_message_callback_set(m_mosq, &MqttWorker::handleMessage);
            mosquitto_log_callback_set(m_mosq, &MqttWorker::handleLog);
        }

        {
            const QByteArray userBytes = m_username.toUtf8();
            const QByteArray passBytes = m_password.toUtf8();
            const char *userPtr = userBytes.isEmpty() ? nullptr : userBytes.constData();
            const char *passPtr = passBytes.isEmpty() ? nullptr : passBytes.constData();
            const int rc = mosquitto_username_pw_set(m_mosq, userPtr, passPtr);
            if (rc != MOSQ_ERR_SUCCESS) {
                emit errorOccurred(rc, QStringLiteral("Failed to set MQTT credentials"));
                return;
            }
        }

        const int rc = mosquitto_connect(m_mosq,
                                         m_hostname.toUtf8().constData(),
                                         m_port,
                                         m_keepAliveSeconds);
        if (rc != MOSQ_ERR_SUCCESS) {
            emit errorOccurred(rc, QStringLiteral("MQTT connect failed"));
            return;
        }

        setState(MqttClient::State::Connecting);
        ensureLoop();
    }

    Q_INVOKABLE void disconnectFromHost()
    {
        if (!m_mosq)
            return;
        if (m_state == MqttClient::State::Disconnected)
            return;
        const int rc = mosquitto_disconnect(m_mosq);
        if (rc != MOSQ_ERR_SUCCESS)
            emit errorOccurred(rc, QStringLiteral("MQTT disconnect failed"));
    }

    Q_INVOKABLE bool subscribe(const QString &topicFilter, int qos)
    {
        if (!m_mosq)
            return false;
        int mid = 0;
        const int rc = mosquitto_subscribe(m_mosq, &mid, topicFilter.toUtf8().constData(), qos);
        if (rc != MOSQ_ERR_SUCCESS) {
            emit errorOccurred(rc, QStringLiteral("MQTT subscribe failed"));
            return false;
        }
        return true;
    }

    Q_INVOKABLE int publish(const QString &topic, const QByteArray &payload, int qos, bool retain)
    {
        if (!m_mosq)
            return -1;
        int mid = 0;
        const int rc = mosquitto_publish(m_mosq,
                                         &mid,
                                         topic.toUtf8().constData(),
                                         payload.size(),
                                         payload.constData(),
                                         qos,
                                         retain);
        if (rc != MOSQ_ERR_SUCCESS) {
            emit errorOccurred(rc, QStringLiteral("MQTT publish failed"));
            return -1;
        }
        return mid;
    }

    Q_INVOKABLE void shutdown()
    {
        cleanup();
    }

signals:
    void connected();
    void disconnected();
    void messageReceived(const QByteArray &message, const QString &topic);
    void errorOccurred(int code, const QString &message);
    void stateChanged(phicore::MqttClient::State state);

private:
    static void handleConnect(struct mosquitto *, void *userdata, int rc)
    {
        auto *worker = static_cast<MqttWorker *>(userdata);
        if (!worker)
            return;
        if (rc == 0) {
            worker->setState(MqttClient::State::Connected);
            emit worker->connected();
        } else {
            worker->setState(MqttClient::State::Disconnected);
            worker->stopLoop();
            emit worker->errorOccurred(rc, QStringLiteral("MQTT connect refused"));
        }
    }

    static void handleDisconnect(struct mosquitto *, void *userdata, int rc)
    {
        auto *worker = static_cast<MqttWorker *>(userdata);
        if (!worker)
            return;
        Q_UNUSED(rc);
        worker->setState(MqttClient::State::Disconnected);
        worker->stopLoop();
        emit worker->disconnected();
    }

    static void handleMessage(struct mosquitto *, void *userdata, const struct mosquitto_message *msg)
    {
        auto *worker = static_cast<MqttWorker *>(userdata);
        if (!worker || !msg)
            return;
        const QByteArray payload(static_cast<const char *>(msg->payload), msg->payloadlen);
        emit worker->messageReceived(payload, QString::fromUtf8(msg->topic));
    }

    static void handleLog(struct mosquitto *, void *userdata, int level, const char *str)
    {
        auto *worker = static_cast<MqttWorker *>(userdata);
        if (!worker || !str)
            return;
        if (level & MOSQ_LOG_ERR)
            emit worker->errorOccurred(MOSQ_ERR_UNKNOWN, QString::fromUtf8(str));
    }

    void ensureLoop()
    {
        if (!m_mosq || m_loopRunning)
            return;
        const int rc = mosquitto_loop_start(m_mosq);
        if (rc != MOSQ_ERR_SUCCESS) {
            emit errorOccurred(rc, QStringLiteral("MQTT loop_start failed"));
            return;
        }
        m_loopRunning = true;
    }

    void stopLoop()
    {
        if (!m_mosq || !m_loopRunning)
            return;
        mosquitto_loop_stop(m_mosq, false);
        m_loopRunning = false;
    }

    void cleanup()
    {
        stopLoop();
        if (m_mosq) {
            mosquitto_destroy(m_mosq);
            m_mosq = nullptr;
        }
    }

    void setState(MqttClient::State state)
    {
        if (m_state == state)
            return;
        m_state = state;
        emit stateChanged(m_state);
    }

    MosquittoRuntime m_runtime;
    struct mosquitto *m_mosq = nullptr;
    bool m_loopRunning = false;

    QString m_clientId;
    QString m_hostname;
    QString m_username;
    QString m_password;
    int m_port = 1883;
    int m_keepAliveSeconds = 60;
    bool m_cleanSession = true;
    MqttClient::State m_state = MqttClient::State::Disconnected;
};

MqttClient::MqttClient(QObject *parent)
    : QObject(parent)
    , m_worker(new MqttWorker())
    , m_workerThread(new QThread(this))
{
    m_worker->moveToThread(m_workerThread);
    connect(m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);

    connect(m_worker, &MqttWorker::connected, this, &MqttClient::connected);
    connect(m_worker, &MqttWorker::disconnected, this, &MqttClient::disconnected);
    connect(m_worker, &MqttWorker::messageReceived, this, &MqttClient::messageReceived);
    connect(m_worker, &MqttWorker::errorOccurred, this, &MqttClient::errorOccurred);
    connect(m_worker, &MqttWorker::stateChanged, this, [this](MqttClient::State state) {
        setState(state);
    });

    m_workerThread->start();
}

MqttClient::~MqttClient()
{
    if (m_workerThread && m_workerThread->isRunning()) {
        QMetaObject::invokeMethod(m_worker, "shutdown", Qt::BlockingQueuedConnection);
        m_workerThread->quit();
        m_workerThread->wait();
    }
}

void MqttClient::setClientId(const QString &clientId)
{
    m_clientId = clientId;
    if (m_worker)
        QMetaObject::invokeMethod(m_worker, "setClientId", Qt::QueuedConnection, Q_ARG(QString, clientId));
}

void MqttClient::setHostname(const QString &hostname)
{
    m_hostname = hostname;
    if (m_worker)
        QMetaObject::invokeMethod(m_worker, "setHostname", Qt::QueuedConnection, Q_ARG(QString, hostname));
}

void MqttClient::setPort(int port)
{
    m_port = port;
    if (m_worker)
        QMetaObject::invokeMethod(m_worker, "setPort", Qt::QueuedConnection, Q_ARG(int, port));
}

void MqttClient::setUsername(const QString &username)
{
    m_username = username;
    if (m_worker)
        QMetaObject::invokeMethod(m_worker, "setUsername", Qt::QueuedConnection, Q_ARG(QString, username));
}

void MqttClient::setPassword(const QString &password)
{
    m_password = password;
    if (m_worker)
        QMetaObject::invokeMethod(m_worker, "setPassword", Qt::QueuedConnection, Q_ARG(QString, password));
}

void MqttClient::setKeepAlive(int keepAliveSeconds)
{
    m_keepAliveSeconds = keepAliveSeconds;
    if (m_worker)
        QMetaObject::invokeMethod(m_worker, "setKeepAlive", Qt::QueuedConnection, Q_ARG(int, keepAliveSeconds));
}

void MqttClient::setCleanSession(bool cleanSession)
{
    m_cleanSession = cleanSession;
    if (m_worker)
        QMetaObject::invokeMethod(m_worker, "setCleanSession", Qt::QueuedConnection, Q_ARG(bool, cleanSession));
}

MqttClient::State MqttClient::state() const
{
    return m_state;
}

void MqttClient::connectToHost()
{
    applyConfig();
    if (m_worker)
        QMetaObject::invokeMethod(m_worker, "connectToHost", Qt::QueuedConnection);
}

void MqttClient::disconnectFromHost()
{
    if (m_worker)
        QMetaObject::invokeMethod(m_worker, "disconnectFromHost", Qt::QueuedConnection);
}

int MqttClient::publish(const QString &topic, const QByteArray &payload, int qos, bool retain)
{
    if (!m_worker)
        return -1;
    if (QThread::currentThread() == m_workerThread)
        return m_worker->publish(topic, payload, qos, retain);
    int mid = -1;
    QMetaObject::invokeMethod(m_worker,
                              "publish",
                              Qt::BlockingQueuedConnection,
                              Q_RETURN_ARG(int, mid),
                              Q_ARG(QString, topic),
                              Q_ARG(QByteArray, payload),
                              Q_ARG(int, qos),
                              Q_ARG(bool, retain));
    return mid;
}

bool MqttClient::subscribe(const QString &topicFilter, int qos)
{
    if (!m_worker)
        return false;
    if (QThread::currentThread() == m_workerThread)
        return m_worker->subscribe(topicFilter, qos);
    bool ok = false;
    QMetaObject::invokeMethod(m_worker,
                              "subscribe",
                              Qt::BlockingQueuedConnection,
                              Q_RETURN_ARG(bool, ok),
                              Q_ARG(QString, topicFilter),
                              Q_ARG(int, qos));
    return ok;
}

void MqttClient::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(m_state);
}

void MqttClient::applyConfig()
{
    setClientId(m_clientId);
    setHostname(m_hostname);
    setPort(m_port);
    setUsername(m_username);
    setPassword(m_password);
    setKeepAlive(m_keepAliveSeconds);
    setCleanSession(m_cleanSession);
}

} // namespace phicore

#include "mqttclient.moc"
