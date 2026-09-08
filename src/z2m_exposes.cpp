#include "z2m_exposes.h"

#include <algorithm>
#include <functional>
#include <optional>

#include "phi/adapter/v1/enum_names.h"
#include "phi/runtime/str.h"

#include "z2m_actions.h"

namespace phicore::z2m::ipc {

namespace str = phi::str;
namespace v1 = phicore::adapter::v1;

namespace {

constexpr int kAccessState = 0b001;
constexpr int kAccessSet = 0b010;
constexpr int kAccessGet = 0b100;

constexpr const char *kAvailabilityChannel = "connectivity";
constexpr const char *kUpdateChannel = "device_software_update";

v1::ChannelFlags flagsFromAccess(int access)
{
    v1::ChannelFlags flags = v1::ChannelFlag::None;
    if (access & kAccessState)
        flags |= v1::ChannelFlag::Readable | v1::ChannelFlag::Reportable | v1::ChannelFlag::Retained;
    if (access & kAccessSet)
        flags |= v1::ChannelFlag::Writable;
    if (flags == v1::ChannelFlag::None)
        flags = v1::kChannelFlagDefaultRead;
    return flags;
}

v1::ChannelFlags forceReadOnly(v1::ChannelFlags flags)
{
    using U = std::underlying_type_t<v1::ChannelFlag>;
    const U cleared = static_cast<U>(flags) & ~static_cast<U>(v1::ChannelFlag::Writable);
    return static_cast<v1::ChannelFlags>(cleared) | v1::ChannelFlag::Readable
        | v1::ChannelFlag::Reportable | v1::ChannelFlag::Retained;
}

std::vector<std::string> stringList(const Json &value)
{
    std::vector<std::string> out;
    if (!value.is_array())
        return out;
    for (const Json &item : value) {
        if (!item.is_string())
            continue;
        const std::string text = str::trimmed(item.get<std::string>());
        if (!text.empty())
            out.push_back(text);
    }
    return out;
}

std::map<std::string, std::vector<std::string>> stringListMap(const Json &root, const char *key)
{
    std::map<std::string, std::vector<std::string>> out;
    const Json obj = jsonValue(root, key);
    if (!obj.is_object())
        return out;
    for (const auto &entry : obj.items()) {
        const std::string mapKey = str::trimmed(entry.key());
        if (mapKey.empty())
            continue;
        std::vector<std::string> values = stringList(entry.value());
        if (!values.empty())
            out.emplace(mapKey, std::move(values));
    }
    return out;
}

bool startsWithAny(std::string_view property, const std::vector<std::string> &prefixes)
{
    for (const std::string &prefix : prefixes) {
        if (!prefix.empty() && str::startsWithIgnoreCase(property, prefix))
            return true;
    }
    return false;
}

const std::vector<std::string> *find(const std::map<std::string, std::vector<std::string>> &map,
                                     std::string_view key)
{
    const auto it = map.find(std::string(key));
    return it == map.end() ? nullptr : &it->second;
}

// The other direction is this adapter's job and stays here: zigbee2mqtt has its
// own words for these ("single_rocker", "very_high"), and turning a foreign
// system's vocabulary into contract values is what an adapter is for. The SDK
// parses contract names, which is a different question.
std::optional<int> mapRockerMode(std::string_view raw)
{
    const std::string key = str::toLower(str::trimmed(raw));
    if (key == "single_rocker" || key == "singlerocker")
        return static_cast<int>(v1::RockerMode::SingleRocker);
    if (key == "dual_rocker" || key == "dualrocker")
        return static_cast<int>(v1::RockerMode::DualRocker);
    if (key == "single_push_button" || key == "singlepushbutton")
        return static_cast<int>(v1::RockerMode::SinglePush);
    if (key == "dual_push_button" || key == "dualpushbutton")
        return static_cast<int>(v1::RockerMode::DualPush);
    return std::nullopt;
}

std::optional<int> mapSensitivityLevel(std::string_view raw)
{
    const std::string key = str::toLower(str::trimmed(raw));
    if (key == "low")
        return static_cast<int>(v1::SensitivityLevel::Low);
    if (key == "medium")
        return static_cast<int>(v1::SensitivityLevel::Medium);
    if (key == "high")
        return static_cast<int>(v1::SensitivityLevel::High);
    if (key == "very_high" || key == "veryhigh")
        return static_cast<int>(v1::SensitivityLevel::VeryHigh);
    if (key == "max")
        return static_cast<int>(v1::SensitivityLevel::Max);
    return std::nullopt;
}

/// Numbers for enum keys that stay put: what is already mapped keeps its
/// value, the rest are numbered on after it in sorted order.
std::map<std::string, int> stableEnumMap(const std::vector<std::string> &rawKeys,
                                         const std::map<std::string, int> &existing)
{
    std::map<std::string, int> map = existing;
    int maxValue = 0;
    for (const auto &[key, value] : existing)
        maxValue = std::max(maxValue, value);
    std::vector<std::string> sorted = rawKeys;
    std::sort(sorted.begin(), sorted.end(), [](const std::string &a, const std::string &b) {
        return str::toLower(a) < str::toLower(b);
    });
    for (const std::string &key : sorted) {
        if (key.empty() || map.count(key))
            continue;
        map.emplace(key, ++maxValue);
    }
    return map;
}

std::string labelFromProperty(std::string_view property, std::string_view fallback)
{
    const std::string given = str::trimmed(fallback);
    if (!given.empty())
        return given;
    if (property == "color_temp")
        return "Color Temperature";
    if (property == "co2")
        return "CO2";
    std::vector<std::string> parts = str::split(property, '_', true);
    for (std::string &part : parts) {
        if (!part.empty())
            part[0] = str::upperAscii(part[0]);
    }
    return str::join(parts, " ");
}

std::string channelIdFor(const std::string &property, const std::string &endpoint)
{
    if (endpoint.empty())
        return property;
    const std::string suffix = "_" + endpoint;
    if (str::endsWithIgnoreCase(property, suffix))
        return property;
    return property + suffix;
}

std::string labelForEndpoint(const std::string &label, const std::string &endpoint)
{
    if (endpoint.empty())
        return label;
    const std::string upper = str::toUpper(endpoint);
    if (str::endsWithIgnoreCase(label, " " + upper))
        return label;
    return label + " " + upper;
}

v1::DeviceClass inferDeviceClass(const std::vector<Json> &exposes)
{
    bool hasLight = false;
    bool hasSwitch = false;
    bool hasSensor = false;
    bool hasButton = false;
    for (const Json &expose : exposes) {
        const std::string property = jsonString(expose, "property");
        if (property == "brightness" || property == "color_temp" || property == "color")
            hasLight = true;
        else if (property == "state")
            hasSwitch = true;
        else if (property == "action")
            hasButton = true;
        else if (property == "temperature" || property == "humidity" || property == "illuminance"
                 || property == "illumination" || property == "occupancy" || property == "motion"
                 || property == "co2")
            hasSensor = true;
    }
    // Some remotes (the Hue dial, say) expose brightness-like action metadata
    // without being lights. An action with no switch state is a button.
    if (hasButton && !hasSwitch)
        return v1::DeviceClass::Button;
    if (hasLight)
        return v1::DeviceClass::Light;
    if (hasSwitch)
        return v1::DeviceClass::Switch;
    if (hasSensor)
        return v1::DeviceClass::Sensor;
    return v1::DeviceClass::Unknown;
}

bool isSensorMeasurementKind(v1::ChannelKind kind)
{
    switch (kind) {
    case v1::ChannelKind::Temperature:
    case v1::ChannelKind::Humidity:
    case v1::ChannelKind::Illuminance:
    case v1::ChannelKind::CO2:
    case v1::ChannelKind::Power:
    case v1::ChannelKind::Voltage:
    case v1::ChannelKind::Current:
    case v1::ChannelKind::Energy:
    case v1::ChannelKind::Battery:
    case v1::ChannelKind::Motion:
    case v1::ChannelKind::Tamper:
    case v1::ChannelKind::AmbientLightLevel:
    case v1::ChannelKind::LinkQuality:
    case v1::ChannelKind::SignalStrength:
    case v1::ChannelKind::ButtonEvent:
        return true;
    default:
        return false;
    }
}

struct Mapping {
    const char *property;
    v1::ChannelKind kind;
    v1::ChannelDataType dataType;
    const char *unit;
    bool scalePercent;
};

constexpr Mapping kMappings[] = {
    {"state", v1::ChannelKind::PowerOnOff, v1::ChannelDataType::Bool, "", false},
    {"brightness", v1::ChannelKind::Brightness, v1::ChannelDataType::Float, "%", true},
    {"color_temp", v1::ChannelKind::ColorTemperature, v1::ChannelDataType::Float, "mired", false},
    {"color", v1::ChannelKind::ColorRGB, v1::ChannelDataType::Color, "", false},
    {"temperature", v1::ChannelKind::Temperature, v1::ChannelDataType::Float, "C", false},
    {"humidity", v1::ChannelKind::Humidity, v1::ChannelDataType::Float, "%", false},
    {"illuminance", v1::ChannelKind::Illuminance, v1::ChannelDataType::Int, "lx", false},
    {"illumination", v1::ChannelKind::AmbientLightLevel, v1::ChannelDataType::Enum, "", false},
    {"occupancy", v1::ChannelKind::Motion, v1::ChannelDataType::Bool, "", false},
    {"motion", v1::ChannelKind::Motion, v1::ChannelDataType::Bool, "", false},
    {"battery", v1::ChannelKind::Battery, v1::ChannelDataType::Int, "%", false},
    {"battery_low", v1::ChannelKind::Unknown, v1::ChannelDataType::Bool, "", false},
    {"linkquality", v1::ChannelKind::LinkQuality, v1::ChannelDataType::Float, "%", false},
    {"keep_time", v1::ChannelKind::Duration, v1::ChannelDataType::Int, "s", false},
    {"tamper", v1::ChannelKind::Tamper, v1::ChannelDataType::Bool, "", false},
    {"power", v1::ChannelKind::Power, v1::ChannelDataType::Float, "W", false},
    {"voltage", v1::ChannelKind::Voltage, v1::ChannelDataType::Float, "V", false},
    {"current", v1::ChannelKind::Current, v1::ChannelDataType::Float, "A", false},
    {"energy", v1::ChannelKind::Energy, v1::ChannelDataType::Float, "kWh", false},
    {"co2", v1::ChannelKind::CO2, v1::ChannelDataType::Float, "ppm", false},
    {"action", v1::ChannelKind::ButtonEvent, v1::ChannelDataType::Int, "", false},
};

const Mapping *mappingFor(std::string_view property)
{
    for (const Mapping &mapping : kMappings) {
        if (property == mapping.property)
            return &mapping;
    }
    return nullptr;
}

void addBinding(DeviceEntry &entry, const v1::Channel &channel, ChannelBinding binding)
{
    entry.channels.push_back(channel);
    binding.channelId = channel.externalId;
    entry.bindings.emplace(channel.externalId, std::move(binding));
}

void addActionChannels(const Json &expose, const std::string &property, const std::string &endpoint,
                       const std::string &channelId, DeviceEntry &entry)
{
    const int access = jsonInt(expose, "access", kAccessState);
    const v1::ChannelFlags flags = flagsFromAccess(access);
    std::vector<std::string> actionValues;
    bool hasDial = false;
    const Json values = jsonValue(expose, "values");
    if (values.is_array()) {
        for (const Json &value : values) {
            if (!value.is_string())
                continue;
            const std::string action = str::trimmed(value.get<std::string>());
            if (action.empty())
                continue;
            actionValues.push_back(action);
            if (isDialAction(action))
                hasDial = true;
        }
    }

    const auto buttonChannel = [&](const std::string &id, const std::string &name, int buttonId) {
        v1::Channel button;
        button.externalId = id;
        button.name = name;
        button.kind = v1::ChannelKind::ButtonEvent;
        button.dataType = v1::ChannelDataType::Int;
        button.flags = flags;
        ChannelBinding binding;
        binding.property = property;
        binding.kind = button.kind;
        binding.dataType = button.dataType;
        binding.flags = button.flags;
        binding.endpoint = endpoint;
        binding.actionButtonId = buttonId;
        addBinding(entry, button, std::move(binding));
    };

    const std::set<int> buttonIds = buttonIdsFromActions(actionValues);
    if (buttonIds.empty()) {
        buttonChannel(channelId,
                      labelForEndpoint(labelFromProperty(property, jsonString(expose, "label")),
                                       endpoint),
                      0);
    } else {
        for (const int buttonId : buttonIds) {
            std::string id = "button" + str::number(buttonId);
            if (!endpoint.empty())
                id += "_" + endpoint;
            buttonChannel(id, labelForEndpoint("Button " + str::number(buttonId), endpoint),
                          buttonId);
        }
    }

    if (hasDial) {
        v1::Channel dial;
        dial.externalId = endpoint.empty() ? "dial" : "dial_" + endpoint;
        dial.name = labelForEndpoint("Dial Rotation", endpoint);
        dial.kind = v1::ChannelKind::RelativeRotation;
        dial.dataType = v1::ChannelDataType::Int;
        dial.flags = flags;
        ChannelBinding binding;
        binding.property = property;
        binding.kind = dial.kind;
        binding.dataType = dial.dataType;
        binding.flags = dial.flags;
        binding.endpoint = endpoint;
        binding.actionIsDial = true;
        addBinding(entry, dial, std::move(binding));
    }
}

void addChannelFromExpose(const Json &expose, const ExposeFilter &filter, DeviceEntry &entry)
{
    const std::string property = jsonString(expose, "property");
    if (property.empty())
        return;
    if (filter.suppressed(property, entry.device.model, jsonString(entry.meta, "model_id")))
        return;
    const std::string propLower = str::toLower(property);
    const bool isMinMaxHelper = propLower == "min" || propLower == "max"
        || propLower.rfind("min_", 0) == 0 || propLower.rfind("max_", 0) == 0
        || str::endsWithIgnoreCase(propLower, "_min") || str::endsWithIgnoreCase(propLower, "_max");
    if (isMinMaxHelper)
        return;

    std::string endpoint;
    const Json endpointValue = jsonValue(expose, "endpoint");
    if (endpointValue.is_string())
        endpoint = str::trimmed(endpointValue.get<std::string>());
    else if (endpointValue.is_number())
        endpoint = str::number(endpointValue.get<int>());

    std::string mappingProperty = property;
    if (!endpoint.empty()) {
        const std::string suffix = "_" + endpoint;
        if (str::endsWithIgnoreCase(mappingProperty, suffix))
            mappingProperty.erase(mappingProperty.size() - suffix.size());
    }

    const std::string channelId = channelIdFor(property, endpoint);
    if (entry.bindings.count(channelId))
        return;

    const Mapping *mapping = mappingFor(mappingProperty);
    const std::string exposeType = jsonString(expose, "type");
    const bool isEnum = exposeType == "enum";
    const bool isBinary = exposeType == "binary";
    const bool isNumeric = exposeType == "numeric";
    if (mapping == nullptr && !(isEnum || isBinary || isNumeric))
        return;

    if (mappingProperty == "action" && isEnum) {
        addActionChannels(expose, property, endpoint, channelId, entry);
        return;
    }

    v1::Channel channel;
    channel.externalId = channelId;
    channel.name = labelForEndpoint(labelFromProperty(property, jsonString(expose, "label")),
                                    endpoint);
    if (mapping != nullptr) {
        channel.kind = mapping->kind;
        channel.dataType = mapping->dataType;
        channel.unit = mapping->unit;
    } else if (isEnum) {
        channel.kind = v1::ChannelKind::Unknown;
        channel.dataType = v1::ChannelDataType::Enum;
    } else if (isBinary) {
        channel.kind = v1::ChannelKind::Unknown;
        channel.dataType = v1::ChannelDataType::Bool;
    } else {
        channel.kind = v1::ChannelKind::Unknown;
        channel.dataType = v1::ChannelDataType::Float;
    }
    if (isEnum)
        channel.dataType = v1::ChannelDataType::Enum;

    const int access = jsonInt(expose, "access", kAccessState);
    channel.flags = flagsFromAccess(access);

    if (entry.device.deviceClass == v1::DeviceClass::Sensor) {
        static constexpr const char *kWritableSensorConfigTokens[] = {
            "calibration", "sensitivity", "threshold", "alarm", "keep_time", "interval", "unit", "mode",
        };
        bool sensorConfigWritable = false;
        for (const char *token : kWritableSensorConfigTokens) {
            if (propLower.find(token) != std::string::npos) {
                sensorConfigWritable = true;
                break;
            }
        }
        if (isSensorMeasurementKind(channel.kind))
            channel.flags = forceReadOnly(channel.flags);
        if (channel.kind == v1::ChannelKind::Unknown && !sensorConfigWritable)
            channel.flags = forceReadOnly(channel.flags);
    }

    double rawMin = jsonDouble(expose, "value_min", 0.0);
    double rawMax = jsonDouble(expose, "value_max", 0.0);
    const double rawStep = jsonDouble(expose, "value_step", 1.0);

    if (channel.kind == v1::ChannelKind::Brightness) {
        if (rawMax <= rawMin) {
            rawMin = 0.0;
            rawMax = 254.0;
        }
        channel.minValue = 0.0;
        channel.maxValue = 100.0;
        channel.stepValue = (rawMax > rawMin && rawStep > 0.0)
            ? (rawStep / (rawMax - rawMin)) * 100.0
            : 1.0;
    } else if (channel.kind == v1::ChannelKind::LinkQuality) {
        channel.minValue = 0.0;
        channel.maxValue = 100.0;
        channel.stepValue = 1.0;
    } else if (channel.kind == v1::ChannelKind::Battery
               && channel.dataType == v1::ChannelDataType::Int) {
        channel.minValue = 0.0;
        channel.maxValue = rawMax > 0.0 ? rawMax : 100.0;
        channel.stepValue = rawStep > 0.0 ? rawStep : 1.0;
    } else if (channel.dataType == v1::ChannelDataType::Float
               || channel.dataType == v1::ChannelDataType::Int) {
        channel.minValue = rawMin;
        channel.maxValue = rawMax;
        channel.stepValue = rawStep;
    }

    Json channelMeta = Json::object();
    std::map<std::string, int> enumRawToValue;
    std::map<int, std::string> enumValueToRaw;
    if (isEnum) {
        std::string enumName;
        if (property == "device_mode")
            enumName = "RockerMode";
        else if (property == "motion_sensitivity" || property == "sensitivity")
            enumName = "SensitivityLevel";

        std::vector<std::string> rawKeys;
        std::map<std::string, int> normalized;
        const Json values = jsonValue(expose, "values");
        bool allNumeric = values.is_array() && !values.empty();
        if (values.is_array()) {
            for (const Json &value : values) {
                std::string key;
                if (value.is_string())
                    key = value.get<std::string>();
                else if (value.is_number())
                    key = str::number(value.get<int>());
                if (key.empty())
                    continue;
                rawKeys.push_back(key);
                bool numericOk = false;
                (void)str::toInt(key, &numericOk);
                if (!numericOk)
                    allNumeric = false;
                if (enumName == "RockerMode") {
                    if (const auto mapped = mapRockerMode(key))
                        normalized.emplace(key, *mapped);
                } else if (enumName == "SensitivityLevel") {
                    if (const auto mapped = mapSensitivityLevel(key))
                        normalized.emplace(key, *mapped);
                }
            }
        }

        std::map<std::string, int> stable;
        if (allNumeric) {
            // Real numbers stay real: a device exposing ["10","30","60"]
            // gets 10, 30, 60, not 1, 2, 3.
            for (const std::string &key : rawKeys) {
                bool ok = false;
                const int numeric = str::toInt(key, &ok);
                if (ok)
                    stable.emplace(key, numeric);
            }
        } else {
            stable = stableEnumMap(rawKeys, normalized);
        }
        if (!enumName.empty())
            channelMeta["enumName"] = enumName;
        if (!stable.empty()) {
            Json stableObj = Json::object();
            for (const auto &[key, value] : stable)
                stableObj[key] = value;
            channelMeta["enumMap"] = stableObj;
        }

        for (const std::string &key : rawKeys) {
            const auto it = stable.find(key);
            const int mapped = it == stable.end() ? 0 : it->second;
            if (mapped == 0)
                continue;
            v1::AdapterConfigOption option;
            option.value = str::number(mapped);
            std::string label;
            if (!enumName.empty())
                label = v1::enum_names::enumNameFor(enumName, mapped, false);
            option.label = label.empty() ? key : label;
            channel.choices.push_back(std::move(option));
            enumRawToValue.emplace(key, mapped);
            enumValueToRaw.emplace(mapped, key);
        }
    }

    const std::string exposeUnit = jsonString(expose, "unit");
    if (channel.unit.empty() && !exposeUnit.empty())
        channel.unit = exposeUnit;
    if (channel.kind == v1::ChannelKind::Voltage && exposeUnit == "mV") {
        channel.unit = "V";
        channel.minValue /= 1000.0;
        channel.maxValue /= 1000.0;
        if (channel.stepValue > 0.0)
            channel.stepValue /= 1000.0;
    }
    if (!channelMeta.empty())
        channel.metaJson = dump(channelMeta);

    ChannelBinding binding;
    binding.property = property;
    binding.kind = channel.kind;
    binding.dataType = channel.dataType;
    binding.flags = channel.flags;
    binding.unit = channel.unit;
    binding.rawMin = rawMin;
    binding.rawMax = rawMax;
    binding.rawStep = rawStep;
    binding.scalePercent = mapping != nullptr && mapping->scalePercent;
    binding.valueScale = 1.0;
    binding.endpoint = endpoint;
    binding.gettable = (access & kAccessGet) != 0;
    binding.enumRawToValue = std::move(enumRawToValue);
    binding.enumValueToRaw = std::move(enumValueToRaw);
    if (binding.kind == v1::ChannelKind::PowerOnOff) {
        binding.valueOn = jsonScalarText(jsonValue(expose, "value_on"));
        binding.valueOff = jsonScalarText(jsonValue(expose, "value_off"));
    }
    if (binding.kind == v1::ChannelKind::Voltage && exposeUnit == "mV") {
        binding.valueScale = 0.001;
        binding.unit = "V";
    }
    if (binding.kind == v1::ChannelKind::LinkQuality) {
        binding.valueScale = 100.0 / 255.0;
        binding.unit = "%";
    }
    if (binding.kind == v1::ChannelKind::ColorRGB) {
        bool hasX = false;
        bool hasY = false;
        bool hasHue = false;
        bool hasSat = false;
        const Json features = jsonValue(expose, "features");
        if (features.is_array()) {
            for (const Json &feature : features) {
                const std::string fprop = jsonString(feature, "property");
                if (fprop == "x")
                    hasX = true;
                else if (fprop == "y")
                    hasY = true;
                else if (fprop == "hue" || fprop == "h")
                    hasHue = true;
                else if (fprop == "saturation" || fprop == "s")
                    hasSat = true;
            }
        }
        binding.colorMode = (hasX && hasY) ? "xy" : ((hasHue && hasSat) ? "hs" : "xy");
    }

    addBinding(entry, channel, std::move(binding));
}

} // namespace

const ChannelBinding *DeviceEntry::binding(std::string_view channelId) const
{
    const auto it = bindings.find(std::string(channelId));
    return it == bindings.end() ? nullptr : &it->second;
}

const ChannelBinding *DeviceEntry::availabilityBinding() const
{
    return binding(kAvailabilityChannel);
}

const ChannelBinding *DeviceEntry::updateBinding() const
{
    return binding(kUpdateChannel);
}

v1::Device DeviceEntry::forWire() const
{
    v1::Device out = device;
    out.metaJson = meta.empty() ? std::string() : dump(meta);
    return out;
}

ExposeFilter ExposeFilter::fromStaticConfig(const Json &config)
{
    ExposeFilter filter;
    filter.suppressedPrefixes = stringList(jsonValue(config, "suppressedPropertyPrefixes"));
    filter.suppressedByModel = stringListMap(config, "suppressedPropertyPrefixesByModel");
    filter.suppressedByModelId = stringListMap(config, "suppressedPropertyPrefixesByModelId");
    filter.allowedByModel = stringListMap(config, "allowedPropertyPrefixesByModel");
    filter.allowedByModelId = stringListMap(config, "allowedPropertyPrefixesByModelId");
    return filter;
}

bool ExposeFilter::suppressed(std::string_view property, std::string_view model,
                              std::string_view modelId) const
{
    bool suppressed = startsWithAny(property, suppressedPrefixes);
    const std::string modelKey = str::trimmed(model);
    if (!modelKey.empty()) {
        if (const auto *list = find(suppressedByModel, modelKey))
            suppressed = suppressed || startsWithAny(property, *list);
        if (const auto *list = find(allowedByModel, modelKey); list && startsWithAny(property, *list))
            suppressed = false;
    }
    const std::string modelIdKey = str::trimmed(modelId);
    if (!modelIdKey.empty()) {
        if (const auto *list = find(suppressedByModelId, modelIdKey))
            suppressed = suppressed || startsWithAny(property, *list);
        if (const auto *list = find(allowedByModelId, modelIdKey);
            list && startsWithAny(property, *list))
            suppressed = false;
    }
    return suppressed;
}

std::string definitionKeyOf(const Json &deviceObj)
{
    const Json definition = jsonValue(deviceObj, "definition");
    const std::string text = dump(jsonValue(definition, "exposes")) + "|"
        + jsonString(definition, "model") + "|" + jsonString(deviceObj, "type");
    return str::number(static_cast<unsigned long long>(std::hash<std::string>{}(text)));
}

std::vector<Json> collectExposes(const Json &exposes)
{
    std::vector<Json> out;
    std::function<void(const Json &)> walk = [&](const Json &value) {
        if (value.is_array()) {
            for (const Json &entry : value)
                walk(entry);
            return;
        }
        if (!value.is_object())
            return;
        const std::string property = jsonString(value, "property");
        const std::string type = jsonString(value, "type");
        if (!property.empty()) {
            out.push_back(value);
            if (property == "color" && type == "composite")
                return;
        }
        const Json features = jsonValue(value, "features");
        if (features.is_array())
            walk(features);
    };
    walk(exposes);
    return out;
}

DeviceEntry buildDeviceEntry(const Json &obj, const ExposeFilter &filter)
{
    DeviceEntry entry;
    const std::string mqttId = jsonString(obj, "friendly_name");
    entry.mqttId = mqttId;
    entry.device.name = mqttId;
    entry.device.flags = v1::DeviceFlag::Wireless;
    entry.definitionKey = definitionKeyOf(obj);

    const std::string powerSource = jsonString(obj, "power_source");
    if (str::equalsIgnoreCase(powerSource, "Battery"))
        entry.device.flags |= v1::DeviceFlag::Battery;

    const Json def = jsonValue(obj, "definition");
    if (def.is_object() && !def.empty()) {
        entry.device.model = jsonString(def, "model", false);
        entry.device.manufacturer = jsonString(def, "vendor", false);
        entry.meta["description"] = jsonString(def, "description", false);
        const std::string model = jsonString(def, "model");
        if (!model.empty())
            entry.meta["iconUrl"] = "https://www.zigbee2mqtt.io/images/devices/" + model + ".png";
    }
    entry.meta["friendly_name"] = mqttId;
    entry.ieee = jsonString(obj, "ieee_address");
    if (!entry.ieee.empty())
        entry.meta["ieee_address"] = entry.ieee;
    const std::string deviceType = jsonString(obj, "type", false);
    entry.meta["type"] = deviceType;
    const std::string modelId = jsonString(obj, "model_id", false);
    if (!modelId.empty())
        entry.meta["model_id"] = modelId;
    if (!powerSource.empty())
        entry.meta["power_source"] = powerSource;
    const std::string manufacturer = jsonString(obj, "manufacturer", false);
    if (!manufacturer.empty())
        entry.meta["manufacturer"] = manufacturer;
    const std::string softwareBuild = jsonString(obj, "software_build_id", false);
    if (!softwareBuild.empty())
        entry.meta["software_build_id"] = softwareBuild;
    const std::string dateCode = jsonString(obj, "date_code", false);
    if (!dateCode.empty())
        entry.meta["date_code"] = dateCode;
    entry.device.externalId = entry.ieee.empty() ? mqttId : entry.ieee;
    if (str::equalsIgnoreCase(deviceType, "Coordinator")) {
        entry.coordinator = true;
        entry.device.deviceClass = v1::DeviceClass::Gateway;
        entry.meta["coordinator"] = true;
    }
    for (const char *key : {"interview_completed", "interviewing", "supported", "disabled"}) {
        if (obj.contains(key))
            entry.meta[key] = obj.at(key);
    }
    const Json availabilityValue = jsonValue(obj, "availability");
    std::string availabilityState;
    if (availabilityValue.is_string())
        availabilityState = str::trimmed(availabilityValue.get<std::string>());
    else if (availabilityValue.is_object())
        availabilityState = jsonString(availabilityValue, "state");
    if (!availabilityState.empty())
        entry.meta["availability"] = availabilityState;

    std::vector<Json> exposes;
    if (def.is_object() && def.contains("exposes"))
        exposes = collectExposes(def.at("exposes"));

    if (!entry.coordinator)
        entry.device.deviceClass = inferDeviceClass(exposes);

    for (const Json &expose : exposes)
        addChannelFromExpose(expose, filter, entry);

    v1::Channel availability;
    availability.externalId = kAvailabilityChannel;
    availability.name = "Connectivity";
    availability.kind = v1::ChannelKind::ConnectivityStatus;
    availability.dataType = v1::ChannelDataType::Enum;
    availability.flags = v1::kChannelFlagDefaultRead;
    ChannelBinding availabilityBinding;
    availabilityBinding.property = "availability";
    availabilityBinding.kind = availability.kind;
    availabilityBinding.dataType = availability.dataType;
    availabilityBinding.flags = availability.flags;
    availabilityBinding.isAvailability = true;
    addBinding(entry, availability, std::move(availabilityBinding));

    v1::Channel update;
    update.externalId = kUpdateChannel;
    update.name = "Firmware Update";
    update.kind = v1::ChannelKind::DeviceSoftwareUpdate;
    update.dataType = v1::ChannelDataType::Enum;
    update.flags = v1::kChannelFlagDefaultRead;
    ChannelBinding updateBinding;
    updateBinding.property = "update";
    updateBinding.kind = update.kind;
    updateBinding.dataType = update.dataType;
    updateBinding.flags = update.flags;
    updateBinding.isUpdate = true;
    addBinding(entry, update, std::move(updateBinding));

    return entry;
}

} // namespace phicore::z2m::ipc
