#pragma once

// libmosquitto driven by the loop this adapter already runs on.
//
// One thread. The previous client stacked three: libmosquitto's own network
// thread, a Qt worker it reported to, and the adapter's thread the worker
// forwarded to - every message copied twice on the way in, every publish a
// blocking round trip on the way out, and a connect that held the worker (and
// with it every publish) for as long as a TCP handshake to a dead address
// takes. Here the socket libmosquitto opens is watched by the loop, its
// keepalive runs off a timer, and a connect that goes nowhere is ended by a
// deadline of this client's choosing.
//
// What the caller sees: a state, and three callbacks - connected, disconnected
// (with a reason a person can read), message. The reason is the part that was
// missing: a refused password, a rejected certificate and an unreachable
// broker used to look identical from outside, which is to say invisible.

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "phi/adapter/v1/tlsconfig.h"
#include "phi/runtime/loop.h"

struct mosquitto;
struct mosquitto_message;

namespace phicore::z2m::ipc {

struct MqttSettings {
    std::string clientId;
    std::string host;
    int port = 1883;
    std::string username;
    std::string password;
    /// The contract's own vocabulary, so there is no place between the
    /// operator's answer and the socket where a second opinion could form.
    phicore::adapter::v1::TlsSettings tls;
    int keepAliveSeconds = 60;
    /// Resolve, connect, handshake and CONNACK, all within this.
    std::chrono::milliseconds connectTimeout{10000};

    /// Whether a connection made with `other` would be the same connection.
    [[nodiscard]] bool sameConnection(const MqttSettings &other) const
    {
        return clientId == other.clientId && host == other.host && port == other.port
            && username == other.username && password == other.password
            && tls.enabled == other.tls.enabled && tls.caFile == other.tls.caFile
            && tls.verifyHostname == other.tls.verifyHostname
            && keepAliveSeconds == other.keepAliveSeconds;
    }
};

class MqttClient
{
public:
    enum class State { Disconnected, Connecting, Connected };

    struct Message {
        std::string topic;
        std::string payload;
        /// Replayed by the broker on subscribe rather than published just now.
        bool retained = false;
    };

    using ConnectedFn = std::function<void()>;
    /// `reason` is empty when the disconnect was asked for.
    using DisconnectedFn = std::function<void(const std::string &reason)>;
    using MessageFn = std::function<void(const Message &message)>;

    /// Owned by, and used from, the thread whose loop this is.
    explicit MqttClient(phi::runtime::Loop &loop);
    ~MqttClient();

    MqttClient(const MqttClient &) = delete;
    MqttClient &operator=(const MqttClient &) = delete;

    void onConnected(ConnectedFn fn) { m_onConnected = std::move(fn); }
    void onDisconnected(DisconnectedFn fn) { m_onDisconnected = std::move(fn); }
    void onMessage(MessageFn fn) { m_onMessage = std::move(fn); }

    /**
     * @brief Starts connecting. False, with `error`, when it cannot even start.
     *
     * A false return is final: neither callback follows. A true return is
     * followed by exactly one of connected() or disconnected(reason).
     *
     * The host is resolved synchronously - libmosquitto does that itself - so
     * a name that has to go out to a slow resolver holds the loop for as long
     * as the lookup takes. Brokers are addressed by IP or `localhost` in
     * practice, which is why this is noted rather than fixed.
     */
    bool connect(MqttSettings settings, std::string *error = nullptr);

    /// Ends the connection quietly: no disconnected() callback.
    void disconnect();

    [[nodiscard]] State state() const { return m_state; }
    [[nodiscard]] bool connected() const { return m_state == State::Connected; }

    /// What the client was last told to connect with. The one way to tell an
    /// adapter that was handed no account from one that dropped it.
    [[nodiscard]] const MqttSettings &settings() const { return m_settings; }

    bool publish(const std::string &topic, std::string_view payload, int qos = 0,
                 bool retain = false, std::string *error = nullptr);
    bool subscribe(const std::string &topicFilter, int qos = 0, std::string *error = nullptr);

private:
    static void handleConnect(::mosquitto *, void *userdata, int rc);
    static void handleDisconnect(::mosquitto *, void *userdata, int rc);
    static void handleMessage(::mosquitto *, void *userdata,
                              const ::mosquitto_message *message);
    static void handleLog(::mosquitto *, void *userdata, int level, const char *text);

    bool applyTls(std::string *error);
    void onReadable();
    void onWritable();
    void onTick();
    void syncWriteWatch();
    /// Ends whatever is in progress and, unless quiet, tells the owner why.
    void finish(std::string reason, bool quiet);
    void releaseHandle();

    phi::runtime::Loop &m_loop;
    ::mosquitto *m_mosq = nullptr;
    MqttSettings m_settings;
    State m_state = State::Disconnected;

    phi::runtime::FdWatch m_readWatch;
    phi::runtime::FdWatch m_writeWatch;
    phi::runtime::Timer m_tick;
    phi::runtime::Timer m_deadline;

    ConnectedFn m_onConnected;
    DisconnectedFn m_onDisconnected;
    MessageFn m_onMessage;

    /// The last error line libmosquitto logged. A rejected certificate is
    /// reported there and nowhere else.
    std::string m_lastLibraryError;
    /// Set when a CONNACK refused us, so the disconnect that follows carries
    /// the refusal rather than "connection closed".
    std::string m_pendingReason;
    int m_inCallback = 0;
};

} // namespace phicore::z2m::ipc
