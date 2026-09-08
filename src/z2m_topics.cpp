#include "z2m_topics.h"

#include "phi/runtime/str.h"

namespace phicore::z2m::ipc {

namespace str = phi::str;

std::string normalizedBaseTopic(std::string_view raw)
{
    std::string base = str::trimmed(raw);
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    if (base.empty())
        return "zigbee2mqtt";
    return base;
}

TopicParts splitTopic(std::string_view baseTopic, std::string_view topic)
{
    TopicParts parts;
    const std::string prefix = std::string(baseTopic) + "/";
    if (topic.size() <= prefix.size() || topic.substr(0, prefix.size()) != prefix)
        return parts;
    parts.underBase = true;
    parts.suffix = std::string(topic.substr(prefix.size()));
    constexpr std::string_view kBridge = "bridge/";
    if (parts.suffix.size() > kBridge.size()
        && std::string_view(parts.suffix).substr(0, kBridge.size()) == kBridge) {
        parts.bridge = true;
        parts.bridgePath = parts.suffix.substr(kBridge.size());
    }
    return parts;
}

namespace {

bool endsWith(std::string_view s, std::string_view tail)
{
    return s.size() >= tail.size() && s.substr(s.size() - tail.size()) == tail;
}

} // namespace

DeviceTopicRef classifyDeviceTopic(std::string_view suffix,
                                   const std::function<bool(std::string_view)> &isKnownDevice)
{
    DeviceTopicRef ref;
    if (suffix.empty())
        return ref;

    const auto known = [&isKnownDevice](std::string_view name) {
        return isKnownDevice && isKnownDevice(name);
    };

    // A known device first, whatever its name contains.
    if (known(suffix)) {
        ref.kind = DeviceTopic::State;
        ref.device = std::string(suffix);
        return ref;
    }
    struct Tail {
        std::string_view text;
        DeviceTopic kind;
    };
    static constexpr Tail kTails[] = {
        {"/availability", DeviceTopic::Availability},
        {"/set", DeviceTopic::Echo},
        {"/get", DeviceTopic::Echo},
    };
    for (const Tail &tail : kTails) {
        if (!endsWith(suffix, tail.text))
            continue;
        const std::string_view head = suffix.substr(0, suffix.size() - tail.text.size());
        if (!head.empty() && known(head)) {
            ref.kind = tail.kind;
            ref.device = std::string(head);
            return ref;
        }
    }

    // Nobody knows the device. Read the last segment literally.
    for (const Tail &tail : kTails) {
        if (!endsWith(suffix, tail.text))
            continue;
        const std::string_view head = suffix.substr(0, suffix.size() - tail.text.size());
        if (head.empty())
            return ref;
        ref.kind = tail.kind;
        ref.device = std::string(head);
        return ref;
    }
    if (suffix.find('/') != std::string_view::npos)
        return ref;
    ref.kind = DeviceTopic::State;
    ref.device = std::string(suffix);
    return ref;
}

} // namespace phicore::z2m::ipc
