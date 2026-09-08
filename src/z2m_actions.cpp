#include "z2m_actions.h"

#include <cmath>

#include "phi/runtime/str.h"

namespace phicore::z2m::ipc {

namespace str = phi::str;
namespace v1 = phicore::adapter::v1;

std::vector<std::string> actionTokens(std::string_view text)
{
    std::vector<std::string> tokens;
    std::string current;
    for (const char raw : text) {
        const char c = str::lowerAscii(raw);
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (alnum) {
            current.push_back(c);
            continue;
        }
        if (!current.empty()) {
            tokens.push_back(current);
            current.clear();
        }
    }
    if (!current.empty())
        tokens.push_back(current);
    return tokens;
}

namespace {

/// A token that is a whole number in 1..16.
int buttonNumber(const std::string &token)
{
    bool ok = false;
    const int n = str::toInt(token, &ok);
    return (ok && n > 0 && n <= 16) ? n : 0;
}

bool hasToken(const std::vector<std::string> &tokens, std::string_view needle)
{
    for (const std::string &token : tokens) {
        if (token == needle)
            return true;
    }
    return false;
}

} // namespace

int buttonIdFromAction(std::string_view action)
{
    const std::vector<std::string> tokens = actionTokens(action);
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (const int n = buttonNumber(tokens[i]))
            return n;
        if (tokens[i] == "button" && i + 1 < tokens.size()) {
            if (const int n = buttonNumber(tokens[i + 1]))
                return n;
        }
    }
    return 0;
}

std::set<int> buttonIdsFromActions(const std::vector<std::string> &actions)
{
    std::set<int> ids;
    for (const std::string &action : actions) {
        if (const int id = buttonIdFromAction(action))
            ids.insert(id);
    }
    return ids;
}

bool isDialAction(std::string_view action)
{
    const std::string lower = str::toLower(str::trimmed(action));
    if (lower.empty())
        return false;
    return lower.find("rotate") != std::string::npos
        || lower.find("rotation") != std::string::npos
        || lower.find("dial") != std::string::npos
        || lower.find("brightness_step") != std::string::npos
        || lower.find("brightness_move") != std::string::npos;
}

int dialMagnitude(std::string_view action, const Json &payload)
{
    static constexpr const char *kStepKeys[] = {
        "action_step_size", "action_step", "step_size", "step", "rotation", "angle",
    };
    bool hasStepField = false;
    for (const char *key : kStepKeys) {
        if (!payload.is_object() || !payload.contains(key))
            continue;
        hasStepField = true;
        const int parsed = static_cast<int>(std::lround(jsonDouble(payload, key, 0.0)));
        if (parsed != 0)
            return std::abs(parsed);
    }
    if (hasStepField)
        return 0;

    const std::vector<std::string> tokens = actionTokens(action);
    for (auto it = tokens.rbegin(); it != tokens.rend(); ++it) {
        bool ok = false;
        const int parsed = str::toInt(*it, &ok);
        if (ok && parsed != 0)
            return std::abs(parsed);
    }
    return 1;
}

int dialDirection(std::string_view action, const Json &payload)
{
    const std::vector<std::string> tokens = actionTokens(action);
    if (hasToken(tokens, "left") || hasToken(tokens, "ccw") || hasToken(tokens, "counterclockwise")
        || hasToken(tokens, "down")) {
        return -1;
    }
    if (hasToken(tokens, "right") || hasToken(tokens, "cw") || hasToken(tokens, "clockwise")
        || hasToken(tokens, "up")) {
        return 1;
    }
    const int direction = jsonInt(payload, "action_direction", 0);
    if (direction == 1)
        return -1;
    if (direction == 2)
        return 1;
    return 0;
}

v1::ButtonEventCode buttonEventFor(std::string_view text)
{
    const std::string value = str::toLower(str::trimmed(text));
    if (value.empty())
        return v1::ButtonEventCode::None;
    const auto has = [&value](const char *needle) { return value.find(needle) != std::string::npos; };
    if (has("double"))
        return v1::ButtonEventCode::DoublePress;
    if (has("triple"))
        return v1::ButtonEventCode::TriplePress;
    if (has("quad"))
        return v1::ButtonEventCode::QuadruplePress;
    if (has("quint"))
        return v1::ButtonEventCode::QuintuplePress;
    if (has("repeat"))
        return v1::ButtonEventCode::Repeat;
    if (has("long_release") || has("hold_release"))
        return v1::ButtonEventCode::LongPressRelease;
    if (has("release"))
        return v1::ButtonEventCode::ShortPressRelease;
    if (has("hold") || has("long"))
        return v1::ButtonEventCode::LongPress;
    if (has("single") || has("press"))
        return v1::ButtonEventCode::InitialPress;
    return v1::ButtonEventCode::None;
}

std::vector<ButtonPresses::Report> ButtonPresses::flush(Memory &memory)
{
    using Code = v1::ButtonEventCode;
    std::vector<Report> out;
    if (memory.releases <= 0)
        return out;
    Code code = Code::ShortPressRelease;
    if (memory.releases == 2)
        code = Code::DoublePress;
    else if (memory.releases == 3)
        code = Code::TriplePress;
    else if (memory.releases == 4)
        code = Code::QuadruplePress;
    else if (memory.releases >= 5)
        code = Code::QuintuplePress;
    out.push_back({code, memory.lastReleaseTs});
    memory.releases = 0;
    memory.lastReleaseTs = 0;
    return out;
}

ButtonPresses::Outcome ButtonPresses::onEvent(const std::string &key, v1::ButtonEventCode code,
                                              std::int64_t tsMs)
{
    using Code = v1::ButtonEventCode;
    Outcome out;
    if (code == Code::None)
        return out;
    Memory &memory = m_keys[key];

    switch (code) {
    case Code::ShortPressRelease:
        // Held: the next half second decides what it was.
        ++memory.releases;
        memory.lastReleaseTs = tsMs;
        out.windowUntilMs = tsMs + kMultiPressWindowMs;
        break;
    case Code::InitialPress:
        // The second press of a double click is a press like any other;
        // whatever is held stays held.
        out.report.push_back({code, tsMs});
        break;
    case Code::LongPress: {
        // A hold is a different gesture: a click held before it is a click.
        out.report = flush(memory);
        out.cancelWindow = true;
        const bool repeating = (memory.lastCode == static_cast<int>(Code::LongPress)
                                || memory.lastCode == static_cast<int>(Code::Repeat))
            && memory.lastTs > 0 && (tsMs - memory.lastTs) <= kLongPressRepeatWindowMs;
        code = repeating ? Code::Repeat : Code::LongPress;
        out.report.push_back({code, tsMs});
        break;
    }
    default:
        // A device that says "double" itself has done the counting; so has
        // one that says "long release".
        out.report = flush(memory);
        out.cancelWindow = true;
        out.report.push_back({code, tsMs});
        break;
    }

    if (code == Code::LongPressRelease) {
        memory.lastCode = 0;
        memory.lastTs = 0;
    } else if (code != Code::ShortPressRelease) {
        memory.lastCode = static_cast<int>(code);
        memory.lastTs = tsMs;
    }
    return out;
}

std::vector<ButtonPresses::Report> ButtonPresses::onWindowClosed(const std::string &key)
{
    const auto it = m_keys.find(key);
    if (it == m_keys.end())
        return {};
    std::vector<Report> out = flush(it->second);
    if (!out.empty()) {
        it->second.lastCode = static_cast<int>(out.back().code);
        it->second.lastTs = out.back().tsMs;
    }
    return out;
}

} // namespace phicore::z2m::ipc
