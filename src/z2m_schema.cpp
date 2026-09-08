#include "z2m_schema.h"

#include <string>

#include "phi/adapter/v1/enum_names.h"
#include "phi/adapter/v1/tlsconfig.h"
#include "phi/runtime/str.h"

#include "z2m_json.h"

namespace phicore::z2m::ipc {

namespace v1 = phicore::adapter::v1;
namespace str = phi::str;

namespace {

Json field(const std::string &key,
           const std::string &type,
           const std::string &label,
           const std::string &description,
           const Json &defaultValue = Json(),
           const Json &flags = Json::array(),
           const std::string &parentActionId = {},
           const Json &meta = Json::object())
{
    Json out = Json::object();
    out["key"] = key;
    out["type"] = type;
    out["label"] = label;
    out["description"] = description;
    if (!defaultValue.is_null())
        out["default"] = defaultValue;
    if (flags.is_array() && !flags.empty())
        out["flags"] = flags;
    if (!parentActionId.empty())
        out["parentActionId"] = parentActionId;
    if (meta.is_object() && !meta.empty())
        out["meta"] = meta;
    return out;
}

Json jsonOf(const v1::ScalarValue &value)
{
    if (const bool *flag = std::get_if<bool>(&value))
        return *flag;
    if (const std::int64_t *number = std::get_if<std::int64_t>(&value))
        return *number;
    if (const double *number = std::get_if<double>(&value))
        return *number;
    if (const v1::Utf8String *text = std::get_if<v1::Utf8String>(&value))
        return *text;
    return Json();
}

/// A contract field as phi-core reads it. The shape the Qt SDK produced, so
/// nothing about the form changes with the library underneath it.
Json fieldJson(const v1::AdapterConfigField &spec)
{
    Json out = Json::object();
    out["key"] = spec.key;
    out["type"] = v1::enum_names::enumNameFor("AdapterConfigFieldType", static_cast<int>(spec.type));
    out["label"] = spec.label;
    out["description"] = spec.description;
    out["default"] = jsonOf(spec.defaultValue);
    if (!spec.parentActionId.empty())
        out["parentActionId"] = spec.parentActionId;
    if (!spec.visibility.fieldKey.empty()) {
        Json visibility = Json::object();
        visibility["fieldKey"] = spec.visibility.fieldKey;
        visibility["value"] = jsonOf(spec.visibility.value);
        visibility["op"] = str::toLower(v1::enum_names::enumNameFor(
            "AdapterConfigVisibilityOp", static_cast<int>(spec.visibility.op)));
        out["visibility"] = visibility;
    }
    return out;
}

Json responsive(int xs, int sm, int md, int lg, int xl, int xxl)
{
    return Json{{"xs", xs}, {"sm", sm}, {"md", md}, {"lg", lg}, {"xl", xl}, {"xxl", xxl}};
}

Json section(const std::string &title, const std::string &description, const Json &fields)
{
    Json defaults = Json::object();
    defaults["span"] = responsive(24, 24, 12, 12, 12, 12);
    defaults["labelPosition"] = "top";
    defaults["labelSpan"] = 8;
    defaults["controlSpan"] = 16;
    defaults["actionPosition"] = "inline";
    defaults["actionSpan"] = 6;

    Json layout = Json::object();
    layout["gridUnits"] = 24;
    layout["gutter"] = Json::array({12, 8});
    layout["defaults"] = defaults;

    Json out = Json::object();
    out["title"] = title;
    out["description"] = description;
    out["layout"] = layout;
    out["fields"] = fields;
    return out;
}

Json baseSchemaFields(const std::string &parentActionId = {})
{
    Json fields = Json::array();
    fields.push_back(field("host", "Hostname", "MQTT Host",
                           "IP address or hostname of the MQTT broker.", "localhost",
                           Json::array({"Required"}), parentActionId));
    fields.push_back(field("port", "Port", "MQTT Port", "TCP port of the MQTT broker.", 1883,
                           Json::array(), parentActionId));
    fields.push_back(field("user", "String", "MQTT Username",
                           "Username for MQTT authentication (optional).", Json(), Json::array(),
                           parentActionId));
    fields.push_back(field("password", "Password", "MQTT Password",
                           "Password for MQTT authentication (optional).", Json(),
                           Json::array({"Secret"}), parentActionId));

    // The transport, in the one spelling every adapter uses. Appended from the
    // SDK rather than written here: a broker reached over TLS is the same
    // question a Hue bridge or anything else would ask, and an operator should
    // not have to learn this adapter's opinion about it.
    for (const v1::AdapterConfigField &tlsField : v1::tlsConfigFields(parentActionId))
        fields.push_back(fieldJson(tlsField));

    fields.push_back(field("baseTopic", "String", "Base topic",
                           "Zigbee2MQTT base topic (default: zigbee2mqtt).", "zigbee2mqtt",
                           Json::array(), parentActionId));
    fields.push_back(field("retryIntervalMs", "Integer", "Retry interval",
                           "Reconnect interval while the broker is offline.", 10000,
                           Json::array(), parentActionId));
    return fields;
}

Json instanceSettingsFields()
{
    const Json roFlags = Json::array({"ReadOnly", "InstanceOnly"});
    const Json instanceOnly = Json::array({"InstanceOnly"});
    const std::string settings = "settings";

    Json fields = Json::array();
    fields.push_back(field("z2mVersion", "String", "Z2M Version", "Detected Zigbee2MQTT version.",
                           Json(), roFlags, settings));
    fields.push_back(field("z2mCommit", "String", "Z2M Commit", "Detected Zigbee2MQTT commit.",
                           Json(), roFlags, settings));
    fields.push_back(field("zigbeeChannel", "Integer", "Zigbee channel",
                           "Zigbee channel (11-26). Requires restart.", Json(), instanceOnly,
                           settings, Json{{"min", 11}, {"max", 26}, {"step", 1}}));
    fields.push_back(field("panId", "String", "PAN ID", "Current Zigbee PAN ID.", Json(), roFlags,
                           settings));
    fields.push_back(field("extPanId", "String", "Extended PAN ID",
                           "Current Zigbee extended PAN ID.", Json(), roFlags, settings));
    fields.push_back(field("serialPort", "String", "Serial port",
                           "Configured coordinator serial port.", Json(), roFlags, settings));
    fields.push_back(field("serialAdapter", "String", "USB adapter",
                           "Configured coordinator USB adapter.", Json(), roFlags, settings));
    fields.push_back(field("coordinatorType", "String", "Coordinator type",
                           "Detected Zigbee coordinator type.", Json(), roFlags, settings));
    fields.push_back(field("coordinatorFirmware", "String", "Coordinator firmware",
                           "Detected Zigbee coordinator firmware revision.", Json(), roFlags,
                           settings));
    fields.push_back(field("permitJoin", "Boolean", "Permit join",
                           "Current Zigbee permit-join state.", Json(), roFlags, settings));
    return fields;
}

} // namespace

v1::Utf8String displayName()
{
    return "Zigbee";
}

v1::Utf8String description()
{
    return "Connect to Zigbee via MQTT.";
}

v1::Utf8String iconSvg()
{
    return
        "<svg width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#26A69A\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\" xmlns=\"http://www.w3.org/2000/svg\" role=\"img\" aria-label=\"Zigbee2MQTT\">"
        "<circle cx=\"12\" cy=\"12\" r=\"2\"/>"
        "<path d=\"M12 4v2M12 18v2M4 12h2M18 12h2\"/>"
        "<path d=\"M6 6l1.5 1.5M16.5 16.5L18 18\"/>"
        "<path d=\"M6 18l1.5-1.5M16.5 7.5L18 6\"/>"
        "</svg>";
}

v1::AdapterCapabilities capabilities()
{
    v1::AdapterCapabilities caps;
    caps.required = v1::AdapterRequirement::Host | v1::AdapterRequirement::UsesRetryInterval;
    caps.optional = v1::AdapterRequirement::Port | v1::AdapterRequirement::Username
        | v1::AdapterRequirement::Password;
    caps.flags = v1::AdapterFlag::SupportsDiscovery | v1::AdapterFlag::SupportsProbe
        | v1::AdapterFlag::SupportsRename;

    v1::AdapterActionDescriptor probe;
    probe.id = "probe";
    probe.label = "Test connection";
    probe.description = "Reachability check.";
    probe.metaJson = R"({"placement":"card","kind":"command","requiresAck":true})";
    caps.factoryActions.push_back(probe);

    v1::AdapterActionDescriptor settings;
    settings.id = "settings";
    settings.label = "Settings";
    settings.description = "Edit Zigbee2MQTT instance settings.";
    settings.hasForm = true;
    settings.metaJson = R"({"placement":"card","kind":"open_dialog","requiresAck":true})";
    caps.instanceActions.push_back(settings);

    v1::AdapterActionDescriptor permitJoin;
    permitJoin.id = "permitJoin";
    permitJoin.label = "Open pairing (2 min)";
    permitJoin.description = "Allow new Zigbee devices to join for 2 minutes.";
    permitJoin.cooldownMs = 120000;
    permitJoin.metaJson = R"({"placement":"card","kind":"command","requiresAck":true})";
    caps.instanceActions.push_back(permitJoin);

    v1::AdapterActionDescriptor restart;
    restart.id = "restartZ2M";
    restart.label = "Restart Zigbee2MQTT";
    restart.description = "Restarts Zigbee2MQTT. Devices may be unavailable briefly.";
    restart.confirmJson =
        R"({"title":"Restart Zigbee2MQTT?","message":"This will briefly disconnect Zigbee devices. Continue?","okText":"Restart","cancelText":"Cancel","danger":true})";
    restart.metaJson = R"({"placement":"card","kind":"command","requiresAck":true})";
    caps.instanceActions.push_back(restart);

    v1::AdapterActionDescriptor deleteDevice;
    deleteDevice.id = "device.delete";
    deleteDevice.label = "Delete device";
    deleteDevice.description = "Remove a device from Zigbee2MQTT.";
    deleteDevice.danger = true;
    deleteDevice.confirmJson =
        R"({"title":"Delete device?","message":"This removes the device from Zigbee2MQTT. Continue?","okText":"Delete","cancelText":"Cancel","danger":true})";
    deleteDevice.metaJson = R"({"placement":"device","kind":"command","requiresAck":true})";
    caps.instanceActions.push_back(deleteDevice);

    caps.defaultsJson = R"({"host":"localhost","port":1883,"retryIntervalMs":10000,"baseTopic":"zigbee2mqtt"})";
    return caps;
}

v1::JsonText configSchemaJson()
{
    Json schema = Json::object();
    schema["factory"] = section("Zigbee2MQTT", "Configure the MQTT broker used by Zigbee2MQTT.",
                                baseSchemaFields());
    schema["instance"] = section("Zigbee2MQTT", "Configure the MQTT broker used by Zigbee2MQTT.",
                                 instanceSettingsFields());
    return dump(schema);
}

} // namespace phicore::z2m::ipc
