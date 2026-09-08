#include "z2m_state.h"

#include <algorithm>
#include <cmath>
#include <ctime>

#include "phi/runtime/str.h"

namespace phicore::z2m::ipc {

namespace str = phi::str;
namespace v1 = phicore::adapter::v1;

namespace {

bool jsonTruth(const Json &value)
{
    if (value.is_boolean())
        return value.get<bool>();
    if (value.is_number())
        return value.get<double>() != 0.0;
    if (value.is_string()) {
        const std::string lower = str::toLower(str::trimmed(value.get<std::string>()));
        return lower == "true" || lower == "on" || lower == "occupied" || lower == "1";
    }
    return false;
}

double jsonNumber(const Json &value, double fallback = 0.0)
{
    if (value.is_number())
        return value.get<double>();
    if (value.is_boolean())
        return value.get<bool>() ? 1.0 : 0.0;
    if (value.is_string()) {
        bool ok = false;
        const double parsed = str::toDouble(str::trimmed(value.get<std::string>()), &ok);
        if (ok)
            return parsed;
    }
    return fallback;
}

Reading scalar(v1::ScalarValue value)
{
    Reading reading;
    reading.shape = Reading::Shape::Scalar;
    reading.scalar = std::move(value);
    return reading;
}

std::optional<Reading> enumReading(const ChannelBinding &binding, const Json &value)
{
    if (value.is_string()) {
        const std::string raw = value.get<std::string>();
        const auto it = binding.enumRawToValue.find(raw);
        if (it != binding.enumRawToValue.end())
            return scalar(static_cast<std::int64_t>(it->second));
        return scalar(raw);
    }
    if (value.is_number())
        return scalar(static_cast<std::int64_t>(value.get<double>()));
    return std::nullopt;
}

std::string versionText(const Json &value)
{
    if (value.is_number())
        return str::number(static_cast<long long>(std::llround(value.get<double>())));
    if (value.is_string())
        return str::trimmed(value.get<std::string>());
    return {};
}

} // namespace

std::optional<Reading> readingFor(const ChannelBinding &binding, const Json &value)
{
    using Kind = v1::ChannelKind;
    switch (binding.kind) {
    case Kind::PowerOnOff: {
        if (value.is_string() && (!binding.valueOn.empty() || !binding.valueOff.empty()))
            return scalar(str::equalsIgnoreCase(value.get<std::string>(), binding.valueOn));
        if (value.is_string())
            return scalar(str::equalsIgnoreCase(value.get<std::string>(), "ON"));
        if (value.is_boolean() || value.is_number())
            return scalar(jsonTruth(value));
        return std::nullopt;
    }
    case Kind::Brightness:
        return scalar(scaleToPercent(jsonNumber(value), binding.rawMin, binding.rawMax));
    case Kind::ColorTemperature:
        return scalar(jsonNumber(value));
    case Kind::ColorRGB: {
        if (!value.is_object())
            return std::nullopt;
        Reading reading;
        reading.shape = Reading::Shape::Color;
        if (binding.colorMode == "hs") {
            const double h = value.contains("hue") ? jsonNumber(value.at("hue"))
                                                   : jsonDouble(value, "h", 0.0);
            const double s = value.contains("saturation") ? jsonNumber(value.at("saturation"))
                                                          : jsonDouble(value, "s", 0.0);
            reading.color = v1::hsvToColor(h, s / 100.0, 1.0);
        } else {
            reading.color = v1::colorFromXy(jsonDouble(value, "x", 0.0), jsonDouble(value, "y", 0.0),
                                            1.0);
        }
        return reading;
    }
    case Kind::Temperature:
    case Kind::Humidity:
    case Kind::Illuminance:
    case Kind::CO2:
    case Kind::Power:
    case Kind::Voltage:
    case Kind::Current:
    case Kind::Energy:
        return scalar(jsonNumber(value) * binding.valueScale);
    case Kind::AmbientLightLevel:
        return enumReading(binding, value);
    case Kind::Duration:
    case Kind::SignalStrength:
    case Kind::Battery:
        return scalar(static_cast<std::int64_t>(jsonNumber(value)));
    case Kind::LinkQuality:
        return scalar(std::clamp(jsonNumber(value) * binding.valueScale, 0.0, 100.0));
    case Kind::Motion:
    case Kind::Tamper:
        return scalar(jsonTruth(value));
    case Kind::Unknown: {
        switch (binding.dataType) {
        case v1::ChannelDataType::Bool:
            return scalar(jsonTruth(value));
        case v1::ChannelDataType::Int:
            return scalar(static_cast<std::int64_t>(jsonNumber(value)));
        case v1::ChannelDataType::Float:
            return scalar(jsonNumber(value) * binding.valueScale);
        case v1::ChannelDataType::Enum:
            return enumReading(binding, value);
        default:
            return std::nullopt;
        }
    }
    default:
        return std::nullopt;
    }
}

std::optional<Reading> updateReading(const Json &update, bool coordinator)
{
    if (!update.is_object())
        return std::nullopt;
    Reading reading;
    reading.shape = Reading::Shape::Object;
    const std::string status = jsonString(update, "state", false);
    if (!status.empty())
        reading.fields.emplace_back("status", status);
    if (coordinator) {
        const std::string target = versionText(jsonValue(update, "version"));
        if (!target.empty())
            reading.fields.emplace_back("targetVersion", target);
    } else {
        const std::string current = versionText(jsonValue(update, "installed_version"));
        if (!current.empty())
            reading.fields.emplace_back("currentVersion", current);
        const std::string target = versionText(jsonValue(update, "latest_version"));
        if (!target.empty())
            reading.fields.emplace_back("targetVersion", target);
    }
    if (reading.fields.empty())
        return std::nullopt;
    return reading;
}

bool CommandValue::toBool() const
{
    if (const bool *b = std::get_if<bool>(&scalar))
        return *b;
    if (const std::int64_t *i = std::get_if<std::int64_t>(&scalar))
        return *i != 0;
    if (const double *d = std::get_if<double>(&scalar))
        return *d != 0.0;
    if (const std::string *s = std::get_if<std::string>(&scalar)) {
        const std::string lower = str::toLower(str::trimmed(*s));
        return lower == "true" || lower == "on" || lower == "1";
    }
    return jsonTruth(json);
}

double CommandValue::toDouble() const
{
    if (const bool *b = std::get_if<bool>(&scalar))
        return *b ? 1.0 : 0.0;
    if (const std::int64_t *i = std::get_if<std::int64_t>(&scalar))
        return static_cast<double>(*i);
    if (const double *d = std::get_if<double>(&scalar))
        return *d;
    if (const std::string *s = std::get_if<std::string>(&scalar)) {
        bool ok = false;
        const double parsed = str::toDouble(str::trimmed(*s), &ok);
        return ok ? parsed : 0.0;
    }
    return jsonNumber(json);
}

int CommandValue::toInt() const
{
    return static_cast<int>(std::lround(toDouble()));
}

std::string CommandValue::toString() const
{
    if (const std::string *s = std::get_if<std::string>(&scalar))
        return *s;
    if (const bool *b = std::get_if<bool>(&scalar))
        return *b ? "true" : "false";
    if (const std::int64_t *i = std::get_if<std::int64_t>(&scalar))
        return str::number(static_cast<long long>(*i));
    if (const double *d = std::get_if<double>(&scalar))
        return str::number(*d);
    return jsonScalarText(json);
}

bool CommandValue::isNumber() const
{
    if (std::holds_alternative<std::int64_t>(scalar) || std::holds_alternative<double>(scalar))
        return true;
    if (const std::string *s = std::get_if<std::string>(&scalar)) {
        bool ok = false;
        (void)str::toDouble(str::trimmed(*s), &ok);
        return ok;
    }
    return json.is_number();
}

std::optional<v1::Color> CommandValue::toColor() const
{
    Json obj = json;
    if (!obj.is_object()) {
        if (const std::string *s = std::get_if<std::string>(&scalar))
            obj = parseObject(*s);
    }
    if (!obj.is_object() || obj.empty())
        return std::nullopt;
    if (!obj.contains("r") || !obj.contains("g") || !obj.contains("b"))
        return std::nullopt;
    const double r = jsonNumber(obj.at("r"), -1.0);
    const double g = jsonNumber(obj.at("g"), -1.0);
    const double b = jsonNumber(obj.at("b"), -1.0);
    if (r < 0.0 || g < 0.0 || b < 0.0)
        return std::nullopt;
    const bool looks255 = r > 1.0 || g > 1.0 || b > 1.0;
    return v1::makeColor(looks255 ? r / 255.0 : r, looks255 ? g / 255.0 : g,
                         looks255 ? b / 255.0 : b);
}

namespace {

/// An enum write: a contract number goes back as the device's own word for
/// it, a word the device knows goes as it is, anything else as typed.
void enumPayload(const ChannelBinding &binding, const CommandValue &value, Json &payload)
{
    const auto rawFor = [&binding](int number) -> std::optional<std::string> {
        const auto it = binding.enumValueToRaw.find(number);
        if (it == binding.enumValueToRaw.end())
            return std::nullopt;
        return it->second;
    };
    if (value.isNumber()) {
        const int number = value.toInt();
        if (const auto raw = rawFor(number))
            payload[binding.property] = *raw;
        else
            payload[binding.property] = number;
        return;
    }
    const std::string text = value.toString();
    if (binding.enumRawToValue.count(text)) {
        payload[binding.property] = text;
        return;
    }
    payload[binding.property] = text;
}

} // namespace

bool commandPayload(const ChannelBinding &binding, const CommandValue &value, Json &payload,
                    std::string &error)
{
    using Kind = v1::ChannelKind;
    error.clear();

    if (binding.dataType == v1::ChannelDataType::Enum) {
        enumPayload(binding, value, payload);
        return true;
    }

    switch (binding.kind) {
    case Kind::PowerOnOff: {
        const bool on = value.toBool();
        if (!binding.valueOn.empty() || !binding.valueOff.empty())
            payload[binding.property] = on ? binding.valueOn : binding.valueOff;
        else
            payload[binding.property] = on ? "ON" : "OFF";
        return true;
    }
    case Kind::Brightness:
        payload[binding.property] = scaleFromPercent(value.toDouble(), binding.rawMin, binding.rawMax);
        return true;
    case Kind::ColorTemperature:
        payload[binding.property] = value.toDouble();
        return true;
    case Kind::ColorRGB: {
        const std::optional<v1::Color> color = value.toColor();
        if (!color) {
            error = "Invalid color value.";
            return false;
        }
        Json colorObj = Json::object();
        if (binding.colorMode == "hs") {
            const v1::Hsv hsv = v1::colorToHsv(*color);
            colorObj["hue"] = hsv.hDeg;
            colorObj["saturation"] = hsv.s * 100.0;
        } else {
            double x = 0.0;
            double y = 0.0;
            v1::colorToXy(*color, x, y);
            colorObj["x"] = x;
            colorObj["y"] = y;
        }
        payload[binding.property] = colorObj;
        return true;
    }
    case Kind::Temperature:
    case Kind::Humidity:
    case Kind::Illuminance:
    case Kind::CO2:
    case Kind::Power:
    case Kind::Voltage:
    case Kind::Current:
    case Kind::Energy:
    case Kind::SignalStrength:
    case Kind::LinkQuality:
    case Kind::Battery:
    case Kind::Duration:
        payload[binding.property] =
            value.toDouble() / (binding.valueScale > 0.0 ? binding.valueScale : 1.0);
        return true;
    case Kind::Unknown:
        if (binding.dataType == v1::ChannelDataType::Bool)
            payload[binding.property] = value.toBool();
        else
            payload[binding.property] =
                value.toDouble() / (binding.valueScale > 0.0 ? binding.valueScale : 1.0);
        return true;
    default:
        error = "Unsupported channel";
        return false;
    }
}

double scaleToPercent(double raw, double rawMin, double rawMax)
{
    if (rawMax <= rawMin)
        return raw;
    const double clamped = std::clamp(raw, rawMin, rawMax);
    return ((clamped - rawMin) / (rawMax - rawMin)) * 100.0;
}

double scaleFromPercent(double percent, double rawMin, double rawMax)
{
    if (rawMax <= rawMin)
        return percent;
    const double clamped = std::clamp(percent, 0.0, 100.0);
    return rawMin + ((rawMax - rawMin) * (clamped / 100.0));
}

std::int64_t lastSeenMs(const Json &value)
{
    if (value.is_number()) {
        const double raw = value.get<double>();
        if (raw > 1000000000000.0)
            return static_cast<std::int64_t>(raw);
        if (raw > 0.0)
            return static_cast<std::int64_t>(raw * 1000.0);
        return 0;
    }
    if (!value.is_string())
        return 0;
    // ISO 8601, as zigbee2mqtt writes it: 2026-09-08T14:48:31.123+02:00 or
    // with a Z. Fractions and the offset are optional.
    const std::string text = str::trimmed(value.get<std::string>());
    std::tm tm{};
    const char *rest = ::strptime(text.c_str(), "%Y-%m-%dT%H:%M:%S", &tm);
    if (rest == nullptr) {
        bool ok = false;
        const double numeric = str::toDouble(text, &ok);
        return ok ? lastSeenMs(Json(numeric)) : 0;
    }
    std::int64_t ms = static_cast<std::int64_t>(::timegm(&tm)) * 1000;
    if (*rest == '.') {
        ++rest;
        int digits = 0;
        int fraction = 0;
        while (*rest >= '0' && *rest <= '9') {
            if (digits < 3) {
                fraction = fraction * 10 + (*rest - '0');
                ++digits;
            }
            ++rest;
        }
        while (digits < 3) {
            fraction *= 10;
            ++digits;
        }
        ms += fraction;
    }
    if (*rest == '+' || *rest == '-') {
        const int sign = *rest == '+' ? 1 : -1;
        ++rest;
        int hours = 0;
        int minutes = 0;
        if (std::sscanf(rest, "%2d:%2d", &hours, &minutes) >= 1)
            ms -= sign * (hours * 60 + minutes) * 60000LL;
    }
    return ms;
}

v1::ConnectivityStatus availabilityStatus(const Json &value)
{
    std::string state;
    if (value.is_string())
        state = value.get<std::string>();
    else if (value.is_object())
        state = jsonString(value, "state");
    const std::string lower = str::toLower(str::trimmed(state));
    if (lower == "online")
        return v1::ConnectivityStatus::Connected;
    if (lower == "offline")
        return v1::ConnectivityStatus::Disconnected;
    return v1::ConnectivityStatus::Unknown;
}

} // namespace phicore::z2m::ipc
