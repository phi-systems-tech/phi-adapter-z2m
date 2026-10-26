#include "z2m_schema.h"

#include <string>
#include <utility>

#include "phi/adapter/v1/tlsconfig.h"

namespace phicore::z2m::ipc {

namespace v1 = phicore::adapter::v1;

namespace {

v1::AdapterConfigField field(const char *key, v1::AdapterConfigFieldType type, const char *label,
                             const char *description, v1::ScalarValue defaultValue = {},
                             v1::AdapterConfigFieldFlags flags = v1::AdapterConfigFieldFlag::None,
                             const char *parentActionId = "")
{
    v1::AdapterConfigField out(key, type, label);
    out.setDescription(description)
        .setDefaultValue(std::move(defaultValue))
        .setFlags(flags)
        .setParentActionId(parentActionId);
    return out;
}

/// The broker, in two columns: address beside port, account beside password,
/// the TLS switch on a row of its own with its details after it.
v1::AdapterConfigSection brokerSection()
{
    using Type = v1::AdapterConfigFieldType;
    v1::AdapterConfigSection out;
    out.setTitle("Zigbee2MQTT")
        .setDescription("Configure the MQTT broker used by Zigbee2MQTT.")
        .layout()
        .setColumns(2);

    v1::AdapterConfigField host = field("host", Type::Hostname, "MQTT Host", "IP address or hostname of the MQTT broker.",
                                        v1::Utf8String("localhost"), v1::AdapterConfigFieldFlag::Required);
    v1::AdapterConfigField port =
        field("port", Type::Port, "MQTT Port", "TCP port of the MQTT broker.", std::int64_t{1883});
    port.layout().setControlWidth(v1::AdapterConfigSize::Narrow);
    v1::AdapterConfigField user =
        field("user", Type::String, "MQTT Username", "Username for MQTT authentication (optional).");
    v1::AdapterConfigField password = field("password", Type::Password, "MQTT Password",
                                            "Password for MQTT authentication (optional).", {},
                                            v1::AdapterConfigFieldFlag::Secret);
    out.setFields({host, port, user, password});

    // The transport, in the one spelling every adapter uses. Appended from the
    // SDK rather than written here: a broker reached over TLS is the same
    // question a Hue bridge or anything else would ask, and an operator should
    // not have to learn this adapter's opinion about it.
    v1::AdapterConfigFieldList tls = v1::tlsConfigFields();
    if (!tls.empty())
        tls.front().layout().setNewRow(true);
    for (const v1::AdapterConfigField &tlsField : tls)
        out.addField(tlsField);

    v1::AdapterConfigField baseTopic = field("baseTopic", Type::String, "Base topic",
                                             "Zigbee2MQTT base topic (default: zigbee2mqtt).",
                                             v1::Utf8String("zigbee2mqtt"));
    baseTopic.layout().setNewRow(true);
    v1::AdapterConfigField retry = field("retryIntervalMs", Type::Integer, "Retry interval",
                                         "Reconnect interval while the broker is offline.", std::int64_t{10000});
    retry.layout().setControlWidth(v1::AdapterConfigSize::Narrow);
    out.addField(baseTopic).addField(retry);
    return out;
}

/// What Zigbee2MQTT reports about itself, two to a row; the channel is the
/// one thing that can be changed.
v1::AdapterConfigSection instanceSection()
{
    using Type = v1::AdapterConfigFieldType;
    using Flag = v1::AdapterConfigFieldFlag;
    const v1::AdapterConfigFieldFlags readOnly = Flag::ReadOnly | Flag::InstanceOnly;
    const char *settings = "settings";

    v1::AdapterConfigSection out;
    out.setTitle("Zigbee2MQTT").setDescription("Configure the MQTT broker used by Zigbee2MQTT.");

    v1::AdapterConfigField channel = field(
        "zigbeeChannel", Type::Integer, "Zigbee channel",
        "The channel the network runs on, 11 to 26; 11, 15, 20 and 25 keep clear of Wi-Fi. A change restarts "
        "Zigbee2MQTT, and devices follow without pairing again - battery devices when they next wake. Only ember "
        "and zstack coordinators can do that.",
        {}, Flag::InstanceOnly, settings);
    channel.setMinValue(11).setMaxValue(26).setStep(1);
    channel.layout().setControlWidth(v1::AdapterConfigSize::Narrow);
    v1::AdapterConfigField activeChannel = field("activeChannel", Type::Integer, "Active channel",
                                                 "The channel the network runs on right now.", {}, readOnly, settings);
    activeChannel.layout().setControlWidth(v1::AdapterConfigSize::Narrow);

    out.setFields({
        field("z2mVersion", Type::String, "Z2M Version", "Detected Zigbee2MQTT version.", {}, readOnly, settings),
        field("z2mCommit", Type::String, "Z2M Commit", "Detected Zigbee2MQTT commit.", {}, readOnly, settings),
        channel,
        activeChannel,
        field("panId", Type::String, "PAN ID", "Current Zigbee PAN ID.", {}, readOnly, settings),
        field("extPanId", Type::String, "Extended PAN ID", "Current Zigbee extended PAN ID.", {}, readOnly, settings),
        field("serialPort", Type::String, "Serial port", "Configured coordinator serial port.", {}, readOnly, settings),
        field("serialAdapter", Type::String, "USB adapter", "Configured coordinator USB adapter.", {}, readOnly,
              settings),
        field("coordinatorType", Type::String, "Coordinator type", "Detected Zigbee coordinator type.", {}, readOnly,
              settings),
        field("coordinatorFirmware", Type::String, "Coordinator firmware",
              "Detected Zigbee coordinator firmware revision.", {}, readOnly, settings),
        field("permitJoin", Type::Boolean, "Permit join", "Current Zigbee permit-join state.", {}, readOnly, settings),
    });
    return out;
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
    caps.setTopologies(v1::Topology::Local).setPairingStyles(v1::PairingStyle::JoinWindow);
    caps.setRequired(v1::AdapterRequirement::Host | v1::AdapterRequirement::UsesRetryInterval)
        .setOptional(v1::AdapterRequirement::Port | v1::AdapterRequirement::Username
                     | v1::AdapterRequirement::Password)
        .setFlags(v1::AdapterCapability::SupportsDiscovery | v1::AdapterCapability::SupportsProbe
                  | v1::AdapterCapability::SupportsDeviceRename
                  // Groups are Zigbee2MQTT's own, with a numeric id a rename leaves
                  // alone; its scenes are not entities of their own and have no name
                  // this adapter could carry, so the other two bits stay off.
                  | v1::AdapterCapability::SupportsGroupRename);

    caps.addFactoryAction({"probe", "Test connection", "Reachability check."});

    v1::AdapterActionDescriptor settings("settings", "Settings", "Edit Zigbee2MQTT instance settings.");
    settings.setHasForm(true).setLoadFormOnOpen(true);
    settings.formLayout()
        .setWidth(v1::AdapterConfigSize::Wide)
        .setColumns(2)
        .setLabelWidth(v1::AdapterConfigSize::Wide);
    caps.addInstanceAction(settings);

    v1::AdapterActionDescriptor restart("restartZ2M", "Restart Zigbee2MQTT",
                                        "Restarts Zigbee2MQTT. Devices may be unavailable briefly.");
    restart.setDanger(true).setConfirm(
        {"Restart Zigbee2MQTT?", "This will briefly disconnect Zigbee devices. Continue?", "Restart", "Cancel"});
    caps.addInstanceAction(restart);

    v1::AdapterActionDescriptor deleteDevice("device.delete", "Delete device",
                                             "Remove a device from Zigbee2MQTT.");
    deleteDevice.setDanger(true)
        .setPlacement(v1::AdapterActionPlacement::Device)
        .setConfirm({"Delete device?", "This removes the device from Zigbee2MQTT. Continue?", "Delete", "Cancel"});
    caps.addInstanceAction(deleteDevice);

    return caps;
}

v1::AdapterConfigSchema configSchema()
{
    v1::AdapterConfigSchema schema;
    schema.setFactory(brokerSection()).setInstance(instanceSection());
    return schema;
}

} // namespace phicore::z2m::ipc
