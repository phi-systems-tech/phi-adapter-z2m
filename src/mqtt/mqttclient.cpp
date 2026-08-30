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
    Q_INVOKABLE void setTls(bool enabled, const QString &caFile, bool verifyHostname)
    {
        if (m_tlsEnabled == enabled && m_tlsCaFile == caFile
            && m_tlsVerifyHostname == verifyHostname) {
            return;
        }
        m_tlsEnabled = enabled;
        m_tlsCaFile = caFile;
        m_tlsVerifyHostname = verifyHostname;
        // A mosquitto client cannot be told to stop using TLS - there is no
        // unset - so one made under the old answer has to go. Without this,
        // turning TLS off would leave it on and turning it on would be ignored,
        // both of them silently.
        if (m_state == MqttClient::State::Disconnected)
            cleanup();
    }

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

        if (m_tlsEnabled && !applyTls())
            return;

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
            worker->leaveTheLoop();
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
        worker->leaveTheLoop();
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

    /// Ends the network thread, in the order libmosquitto requires.
    ///
    /// Disconnect first. `mosquitto_loop_stop(mosq, false)` waits for the
    /// thread to end, and the thread only ends once the client is disconnected
    /// - so stopping a connected client waited for something that was never
    /// going to happen. The documented contract says as much: the unforced stop
    /// is for a client that has already been told to disconnect.
    ///
    /// Measured, because it did not look like a hang from outside: a client
    /// that had connected happily stayed in its destructor for as long as
    /// anybody was willing to wait.
    void stopLoop()
    {
        if (!m_mosq || !m_loopRunning)
            return;
        mosquitto_disconnect(m_mosq);
        mosquitto_loop_stop(m_mosq, false);
        m_loopRunning = false;
    }

    /// Turns TLS on for a client that has not connected yet.
    ///
    /// The certificate chain is always verified: there is no option here that
    /// accepts any certificate, because an endpoint with a self-signed
    /// certificate already has a correct answer - name it - and the blanket
    /// version would only ever be the easier way to be insecure.
    bool applyTls()
    {
        if (!m_tlsCaFile.isEmpty()) {
            const QByteArray caBytes = m_tlsCaFile.toUtf8();
            const int rc = mosquitto_tls_set(m_mosq, caBytes.constData(), nullptr, nullptr,
                                             nullptr, nullptr);
            if (rc != MOSQ_ERR_SUCCESS) {
                emit errorOccurred(rc, QStringLiteral("Cannot read the CA certificate %1")
                                           .arg(m_tlsCaFile));
                return false;
            }
        } else {
            // The authorities the machine already trusts. Set as an option
            // rather than by naming a directory, so the answer stays the
            // system's rather than a path we guessed.
            const int rc = mosquitto_int_option(m_mosq, MOSQ_OPT_TLS_USE_OS_CERTS, 1);
            if (rc != MOSQ_ERR_SUCCESS) {
                emit errorOccurred(rc, QStringLiteral("Cannot use the system's trusted"
                                                      " certificates"));
                return false;
            }
        }

        if (!m_tlsVerifyHostname) {
            // The chain is still checked; this is only about whether the
            // certificate has to name the address that was dialled, which it
            // cannot when somebody connects to a broker by IP.
            const int rc = mosquitto_tls_insecure_set(m_mosq, true);
            if (rc != MOSQ_ERR_SUCCESS) {
                emit errorOccurred(rc, QStringLiteral("Cannot relax the hostname check"));
                return false;
            }
        }
        return true;
    }

    /// Ends the loop from inside one of its own callbacks.
    ///
    /// `mosquitto_loop_stop` may not be called from a callback - it waits for
    /// the very thread it is running on - so this asks for the disconnect that
    /// makes the loop return on its own, and the stop is queued for afterwards.
    ///
    /// This is not tidiness. Without it, `mosquitto_loop_forever` carries on to
    /// its own reconnect after the connection dies, and when the connection
    /// died before a CONNACK ever arrived it does that with a socket of -1 -
    /// where `FD_SET` walks off the end of an `fd_set` and glibc aborts the
    /// process. A plain client dialling a TLS listener does exactly that, which
    /// is one of the two mistakes offering a TLS switch invites; the other is
    /// the same thing the other way round. Measured, with a core file: the
    /// abort is inside mosquitto_loop_forever, in __fdelt_warn.
    ///
    /// Reconnecting is ours to decide anyway - the adapter already schedules
    /// it - so the loop had no business doing it a second time.
    void leaveTheLoop()
    {
        if (m_mosq)
            mosquitto_disconnect(m_mosq);
        QMetaObject::invokeMethod(this, "stopLoopFromOurOwnThread", Qt::QueuedConnection);
    }

    Q_INVOKABLE void stopLoopFromOurOwnThread() { stopLoop(); }

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
    bool m_tlsEnabled = false;
    QString m_tlsCaFile;
    bool m_tlsVerifyHostname = true;
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

void MqttClient::setTls(const phicore::adapter::v1::TlsSettings &settings)
{
    m_tls = settings;
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, "setTls", Qt::QueuedConnection,
                                  Q_ARG(bool, settings.enabled),
                                  Q_ARG(QString, QString::fromStdString(settings.caFile)),
                                  Q_ARG(bool, settings.verifyHostname));
    }
}

phicore::adapter::v1::TlsSettings MqttClient::tls() const
{
    return m_tls;
}

void MqttClient::setPassword(const QString &password)
{
    m_password = password;
    if (m_worker)
        QMetaObject::invokeMethod(m_worker, "setPassword", Qt::QueuedConnection, Q_ARG(QString, password));
}

QString MqttClient::username() const
{
    return m_username;
}

QString MqttClient::password() const
{
    return m_password;
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
    setTls(m_tls);
}

} // namespace phicore

#include "mqttclient.moc"
