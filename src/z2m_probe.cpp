#include "z2m_probe.h"

#include <cerrno>
#include <cstring>

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "phi/runtime/str.h"

namespace phicore::z2m::ipc {

namespace str = phi::str;

namespace {

constexpr int kPollSliceMs = 50;

class Descriptor
{
public:
    explicit Descriptor(int fd) : m_fd(fd) {}
    ~Descriptor()
    {
        if (m_fd >= 0)
            ::close(m_fd);
    }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
    int get() const { return m_fd; }

private:
    int m_fd;
};

std::string firstText(const Json &obj, std::initializer_list<const char *> keys)
{
    for (const char *key : keys) {
        const std::string value = jsonString(obj, key);
        if (!value.empty())
            return value;
    }
    return {};
}

} // namespace

ProbeTarget probeTargetFromParams(const Json &params)
{
    const Json form = params.is_object() ? params : Json::object();
    const Json candidate = jsonValue(form, "factoryAdapter").is_object()
        ? jsonValue(form, "factoryAdapter")
        : Json::object();

    ProbeTarget target;
    target.host = firstText(form, {"host", "ip"});
    if (target.host.empty())
        target.host = firstText(candidate, {"host", "ip"});

    int port = jsonInt(form, "port", 0);
    if (port <= 0)
        port = jsonInt(candidate, "port", 0);
    if (port > 0 && port <= 65535)
        target.port = static_cast<std::uint16_t>(port);
    return target;
}

bool probeEndpoint(const ProbeTarget &target,
                   std::chrono::milliseconds timeout,
                   const std::function<bool()> &cancelled,
                   std::string *errorMessage)
{
    const auto fail = [errorMessage](std::string message) {
        if (errorMessage)
            *errorMessage = std::move(message);
        return false;
    };

    ::addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    ::addrinfo *resolved = nullptr;
    const std::string service = str::number(static_cast<int>(target.port));
    const int rc = ::getaddrinfo(target.host.c_str(), service.c_str(), &hints, &resolved);
    if (rc != 0 || resolved == nullptr)
        return fail(std::string("Cannot resolve host: ") + ::gai_strerror(rc));

    Descriptor socket(::socket(resolved->ai_family,
                               resolved->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                               resolved->ai_protocol));
    if (socket.get() < 0) {
        const std::string error = std::strerror(errno);
        ::freeaddrinfo(resolved);
        return fail(error);
    }

    const int connectResult = ::connect(socket.get(), resolved->ai_addr, resolved->ai_addrlen);
    const int connectErrno = errno;
    ::freeaddrinfo(resolved);

    if (connectResult != 0 && connectErrno != EINPROGRESS)
        return fail(std::strerror(connectErrno));

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (connectResult != 0) {
        if (cancelled && cancelled())
            return fail("Probe cancelled: adapter is stopping");
        if (std::chrono::steady_clock::now() >= deadline)
            return fail("Connection timed out");

        ::pollfd waiting{};
        waiting.fd = socket.get();
        waiting.events = POLLOUT;
        const int ready = ::poll(&waiting, 1, kPollSliceMs);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            return fail(std::strerror(errno));
        }
        if (ready == 0)
            continue;

        int soError = 0;
        ::socklen_t length = sizeof(soError);
        if (::getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, &soError, &length) != 0)
            return fail(std::strerror(errno));
        if (soError != 0)
            return fail(std::strerror(soError));
        break;
    }
    return true;
}

} // namespace phicore::z2m::ipc
