#include "z2m_mqtt_client.h"

#include <cerrno>
#include <cstring>
#include <mutex>
#include <utility>

#include <mosquitto.h>

#include "phi/runtime/str.h"

namespace phicore::z2m::ipc {

namespace str = phi::str;
using namespace std::chrono_literals;

namespace {

/// libmosquitto's keepalive and retransmission bookkeeping has to be poked
/// regularly; this is how often. It also drains a partially written packet,
/// which is the one thing the write watch cannot be relied on for (see
/// syncWriteWatch).
constexpr auto kTickInterval = 500ms;

/// mosquitto_lib_init() is process-wide; the factory and every instance each
/// own a client on their own thread, so it happens once and is never undone.
void initLibraryOnce()
{
    static std::once_flag once;
    std::call_once(once, []() { mosquitto_lib_init(); });
}

std::string libraryText(int rc)
{
    const char *text = mosquitto_strerror(rc);
    return text ? std::string(text) : str::arg("mosquitto error %1", rc);
}

} // namespace

MqttClient::MqttClient(phi::runtime::Loop &loop)
    : m_loop(loop)
{
    initLibraryOnce();
}

MqttClient::~MqttClient()
{
    m_readWatch.reset();
    m_writeWatch.reset();
    m_tick.reset();
    m_deadline.reset();
    releaseHandle();
}

bool MqttClient::connect(MqttSettings settings, std::string *error)
{
    const auto fail = [error](std::string message) {
        if (error)
            *error = std::move(message);
        return false;
    };

    if (m_state != State::Disconnected)
        return fail("already connecting");
    if (str::trimmed(settings.host).empty())
        return fail("MQTT hostname is empty");

    m_settings = std::move(settings);
    m_lastLibraryError.clear();
    m_pendingReason.clear();

    // A fresh handle per connect. A mosquitto client cannot be told to stop
    // using TLS - there is no unset - so one made under the old answer would
    // silently keep it.
    releaseHandle();
    const char *clientId = m_settings.clientId.empty() ? nullptr : m_settings.clientId.c_str();
    m_mosq = mosquitto_new(clientId, true, this);
    if (!m_mosq)
        return fail("failed to allocate a mosquitto client");
    mosquitto_connect_callback_set(m_mosq, &MqttClient::handleConnect);
    mosquitto_disconnect_callback_set(m_mosq, &MqttClient::handleDisconnect);
    mosquitto_message_callback_set(m_mosq, &MqttClient::handleMessage);
    mosquitto_log_callback_set(m_mosq, &MqttClient::handleLog);

    if (m_settings.tls.enabled && !applyTls(error)) {
        releaseHandle();
        return false;
    }

    const char *user = m_settings.username.empty() ? nullptr : m_settings.username.c_str();
    const char *password = m_settings.password.empty() ? nullptr : m_settings.password.c_str();
    int rc = mosquitto_username_pw_set(m_mosq, user, password);
    if (rc != MOSQ_ERR_SUCCESS) {
        releaseHandle();
        return fail("cannot set MQTT credentials: " + libraryText(rc));
    }

    // Non-blocking: the TCP connect is started here and finished by the loop.
    // libmosquitto queues the CONNECT packet behind it; the first writable
    // event sends it, the CONNACK arrives as the first readable one. With TLS
    // the handshake rides the same two events - OpenSSL continues it from
    // inside SSL_read/SSL_write, and `want_write` says which side it waits on.
    rc = mosquitto_connect_async(m_mosq, str::trimmed(m_settings.host).c_str(),
                                 m_settings.port, m_settings.keepAliveSeconds);
    if (rc != MOSQ_ERR_SUCCESS) {
        const std::string detail = m_lastLibraryError.empty() ? libraryText(rc)
                                                              : m_lastLibraryError;
        releaseHandle();
        return fail("MQTT connect failed: " + detail);
    }

    const int fd = mosquitto_socket(m_mosq);
    if (fd < 0) {
        releaseHandle();
        return fail("MQTT connect failed: no socket");
    }

    m_state = State::Connecting;
    m_readWatch = m_loop.watchFd(fd, phi::runtime::Loop::FdEvent::Read, [this]() { onReadable(); });
    m_writeWatch = m_loop.watchFd(fd, phi::runtime::Loop::FdEvent::Write, [this]() { onWritable(); });
    m_tick = m_loop.timerEvery(kTickInterval, [this]() { onTick(); });
    m_deadline = m_loop.timerAfter(m_settings.connectTimeout, [this]() {
        finish(str::arg("the broker at %1:%2 did not answer within %3 ms",
                        str::trimmed(m_settings.host), m_settings.port,
                        static_cast<long long>(m_settings.connectTimeout.count())),
               false);
    });
    return true;
}

void MqttClient::disconnect()
{
    if (m_state == State::Disconnected)
        return;
    const bool wasConnected = m_state == State::Connected;
    // Before the DISCONNECT goes out, not after: libmosquitto closes the
    // socket and calls the disconnect callback from inside the send, and a
    // client that is already Disconnected lets that callback pass.
    m_state = State::Disconnected;
    if (m_mosq && wasConnected)
        mosquitto_disconnect(m_mosq);
    finish({}, true);
}

bool MqttClient::publish(const std::string &topic, std::string_view payload, int qos, bool retain,
                         std::string *error)
{
    if (!m_mosq || m_state != State::Connected) {
        if (error)
            *error = "MQTT client not connected";
        return false;
    }
    int mid = 0;
    const int rc = mosquitto_publish(m_mosq, &mid, topic.c_str(), static_cast<int>(payload.size()),
                                     payload.data(), qos, retain);
    if (rc != MOSQ_ERR_SUCCESS) {
        if (error)
            *error = "MQTT publish failed: " + libraryText(rc);
        return false;
    }
    // Whatever did not fit the socket just now goes out on the next writable
    // event rather than the next tick.
    if (m_writeWatch)
        m_writeWatch.setEnabled(true);
    return true;
}

bool MqttClient::subscribe(const std::string &topicFilter, int qos, std::string *error)
{
    if (!m_mosq || m_state != State::Connected) {
        if (error)
            *error = "MQTT client not connected";
        return false;
    }
    int mid = 0;
    const int rc = mosquitto_subscribe(m_mosq, &mid, topicFilter.c_str(), qos);
    if (rc != MOSQ_ERR_SUCCESS) {
        if (error)
            *error = "MQTT subscribe failed: " + libraryText(rc);
        return false;
    }
    if (m_writeWatch)
        m_writeWatch.setEnabled(true);
    return true;
}

/// Turns TLS on for a client that has not connected yet.
///
/// The certificate chain is always verified: there is no option here that
/// accepts any certificate, because an endpoint with a self-signed certificate
/// already has a correct answer - name it - and the blanket version would only
/// ever be the easier way to be insecure.
bool MqttClient::applyTls(std::string *error)
{
    const auto fail = [error](std::string message) {
        if (error)
            *error = std::move(message);
        return false;
    };

    if (!m_settings.tls.caFile.empty()) {
        const int rc = mosquitto_tls_set(m_mosq, m_settings.tls.caFile.c_str(), nullptr, nullptr,
                                         nullptr, nullptr);
        if (rc != MOSQ_ERR_SUCCESS)
            return fail("cannot read the CA certificate " + m_settings.tls.caFile);
    } else {
        // The authorities the machine already trusts. Set as an option rather
        // than by naming a directory, so the answer stays the system's rather
        // than a path we guessed.
        const int rc = mosquitto_int_option(m_mosq, MOSQ_OPT_TLS_USE_OS_CERTS, 1);
        if (rc != MOSQ_ERR_SUCCESS)
            return fail("cannot use the system's trusted certificates");
    }

    if (!m_settings.tls.verifyHostname) {
        // The chain is still checked; this is only about whether the
        // certificate has to name the address that was dialled, which it
        // cannot when somebody connects to a broker by IP.
        const int rc = mosquitto_tls_insecure_set(m_mosq, true);
        if (rc != MOSQ_ERR_SUCCESS)
            return fail("cannot relax the hostname check");
    }
    return true;
}

void MqttClient::onReadable()
{
    if (!m_mosq)
        return;
    ++m_inCallback;
    const int rc = mosquitto_loop_read(m_mosq, 1);
    --m_inCallback;
    if (rc != MOSQ_ERR_SUCCESS || !m_mosq || m_state == State::Disconnected)
        return; // the disconnect callback has already taken it from here
    // Reading can leave something to write: a PINGRESP, a SUBACK's follow-up,
    // or a TLS handshake step that turned the other way.
    ++m_inCallback;
    mosquitto_loop_write(m_mosq, 1);
    --m_inCallback;
    syncWriteWatch();
}

void MqttClient::onWritable()
{
    if (!m_mosq)
        return;
    ++m_inCallback;
    const int rc = mosquitto_loop_write(m_mosq, 1);
    --m_inCallback;
    if (rc != MOSQ_ERR_SUCCESS || !m_mosq || m_state == State::Disconnected)
        return;
    syncWriteWatch();
}

void MqttClient::onTick()
{
    if (!m_mosq)
        return;
    ++m_inCallback;
    // Keepalive: sends the PINGREQ when due and declares the connection lost
    // when the PINGRESP does not come, through the disconnect callback.
    int rc = mosquitto_loop_misc(m_mosq);
    if (rc == MOSQ_ERR_SUCCESS && m_mosq && m_state != State::Disconnected)
        rc = mosquitto_loop_write(m_mosq, 1);
    --m_inCallback;
    if (rc != MOSQ_ERR_SUCCESS || !m_mosq || m_state == State::Disconnected)
        return;
    syncWriteWatch();
}

/// The write watch is armed only while there is something to write, because
/// a socket is nearly always writable and a level-triggered watch on it would
/// spin the loop. libmosquitto tells us about the TLS side (`want_write`) but
/// not about a partially written packet; those are finished by the tick.
void MqttClient::syncWriteWatch()
{
    if (!m_writeWatch || !m_mosq)
        return;
    m_writeWatch.setEnabled(mosquitto_want_write(m_mosq));
}

void MqttClient::handleConnect(::mosquitto *, void *userdata, int rc)
{
    auto *self = static_cast<MqttClient *>(userdata);
    if (!self)
        return;
    if (rc == 0) {
        self->m_state = State::Connected;
        self->m_deadline.reset();
        if (self->m_onConnected)
            self->m_onConnected();
        return;
    }
    // The broker answered, and said no. The socket is about to close from its
    // side; the refusal is what the owner needs to hear, not the closing.
    const char *text = mosquitto_connack_string(rc);
    self->m_pendingReason = str::arg("the broker refused the connection: %1",
                                     text ? text : str::arg("CONNACK %1", rc));
    // The DISCONNECT's own send may already have run the disconnect callback,
    // which finishes with the pending reason; if not, finish here.
    if (self->m_mosq)
        mosquitto_disconnect(self->m_mosq);
    if (self->m_state != State::Disconnected)
        self->finish(self->m_pendingReason, false);
}

void MqttClient::handleDisconnect(::mosquitto *, void *userdata, int rc)
{
    auto *self = static_cast<MqttClient *>(userdata);
    if (!self || self->m_state == State::Disconnected)
        return;
    std::string reason;
    if (!self->m_pendingReason.empty())
        reason = self->m_pendingReason;
    else if (rc == 0)
        reason = "the broker closed the connection";
    else if (!self->m_lastLibraryError.empty())
        reason = self->m_lastLibraryError;
    else
        reason = libraryText(rc);
    self->finish(std::move(reason), false);
}

void MqttClient::handleMessage(::mosquitto *, void *userdata,
                               const ::mosquitto_message *message)
{
    auto *self = static_cast<MqttClient *>(userdata);
    if (!self || !message || !self->m_onMessage)
        return;
    Message out;
    out.topic = message->topic ? message->topic : "";
    if (message->payload && message->payloadlen > 0)
        out.payload.assign(static_cast<const char *>(message->payload),
                           static_cast<std::size_t>(message->payloadlen));
    out.retained = message->retain;
    self->m_onMessage(out);
}

void MqttClient::handleLog(::mosquitto *, void *userdata, int level, const char *text)
{
    auto *self = static_cast<MqttClient *>(userdata);
    if (!self || !text)
        return;
    if (level & MOSQ_LOG_ERR)
        self->m_lastLibraryError = text;
}

void MqttClient::finish(std::string reason, bool quiet)
{
    if (m_state == State::Disconnected && !m_mosq)
        return;
    m_state = State::Disconnected;
    m_readWatch.reset();
    m_writeWatch.reset();
    m_tick.reset();
    m_deadline.reset();
    m_pendingReason.clear();

    // The handle may be the one whose callback we are inside; it is destroyed
    // once that callback has returned.
    if (m_inCallback > 0) {
        ::mosquitto *handle = m_mosq;
        m_mosq = nullptr;
        m_loop.post([handle]() {
            if (handle)
                mosquitto_destroy(handle);
        });
    } else {
        releaseHandle();
    }

    if (!quiet && m_onDisconnected)
        m_onDisconnected(reason);
}

void MqttClient::releaseHandle()
{
    if (!m_mosq)
        return;
    mosquitto_destroy(m_mosq);
    m_mosq = nullptr;
}

} // namespace phicore::z2m::ipc
