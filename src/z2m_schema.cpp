#include "z2m_schema.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace phicore::z2m::ipc {

namespace {

QJsonObject field(const QString &key,
                  const QString &type,
                  const QString &label,
                  const QString &description,
                  const QJsonValue &defaultValue = QJsonValue(),
                  const QJsonArray &flags = {},
                  const QString &parentActionId = QString(),
                  const QJsonObject &meta = {})
{
    QJsonObject out;
    out.insert(QStringLiteral("key"), key);
    out.insert(QStringLiteral("type"), type);
    out.insert(QStringLiteral("label"), label);
    out.insert(QStringLiteral("description"), description);
    if (!defaultValue.isUndefined() && !defaultValue.isNull())
        out.insert(QStringLiteral("default"), defaultValue);
    if (!flags.isEmpty())
        out.insert(QStringLiteral("flags"), flags);
    if (!parentActionId.isEmpty())
        out.insert(QStringLiteral("parentActionId"), parentActionId);
    if (!meta.isEmpty())
        out.insert(QStringLiteral("meta"), meta);
    return out;
}

QJsonObject responsive(int xs, int sm, int md, int lg, int xl, int xxl)
{
    QJsonObject out;
    out.insert(QStringLiteral("xs"), xs);
    out.insert(QStringLiteral("sm"), sm);
    out.insert(QStringLiteral("md"), md);
    out.insert(QStringLiteral("lg"), lg);
    out.insert(QStringLiteral("xl"), xl);
    out.insert(QStringLiteral("xxl"), xxl);
    return out;
}

QJsonObject section(const QString &title, const QString &description, const QJsonArray &fields)
{
    QJsonObject layout;
    layout.insert(QStringLiteral("gridUnits"), 24);

    QJsonArray gutter;
    gutter.append(12);
    gutter.append(8);
    layout.insert(QStringLiteral("gutter"), gutter);

    QJsonObject defaults;
    defaults.insert(QStringLiteral("span"), responsive(24, 24, 12, 12, 12, 12));
    defaults.insert(QStringLiteral("labelPosition"), QStringLiteral("Left"));
    defaults.insert(QStringLiteral("labelSpan"), 8);
    defaults.insert(QStringLiteral("controlSpan"), 16);
    defaults.insert(QStringLiteral("actionPosition"), QStringLiteral("Inline"));
    defaults.insert(QStringLiteral("actionSpan"), 6);
    layout.insert(QStringLiteral("defaults"), defaults);

    QJsonObject out;
    out.insert(QStringLiteral("title"), title);
    out.insert(QStringLiteral("description"), description);
    out.insert(QStringLiteral("layout"), layout);
    out.insert(QStringLiteral("fields"), fields);
    return out;
}

QJsonArray baseSchemaFields(const QString &parentActionId = QString())
{
    QJsonArray fields;

    QJsonArray requiredFlags;
    requiredFlags.append(QStringLiteral("Required"));

    fields.append(field(QStringLiteral("host"),
                        QStringLiteral("Hostname"),
                        QStringLiteral("MQTT Host"),
                        QStringLiteral("IP address or hostname of the MQTT broker."),
                        QJsonValue(QStringLiteral("localhost")),
                        requiredFlags,
                        parentActionId));

    fields.append(field(QStringLiteral("port"),
                        QStringLiteral("Port"),
                        QStringLiteral("MQTT Port"),
                        QStringLiteral("TCP port of the MQTT broker."),
                        QJsonValue(1883),
                        {},
                        parentActionId));

    fields.append(field(QStringLiteral("user"),
                        QStringLiteral("String"),
                        QStringLiteral("MQTT Username"),
                        QStringLiteral("Username for MQTT authentication (optional)."),
                        QJsonValue(),
                        {},
                        parentActionId));

    QJsonArray secretFlags;
    secretFlags.append(QStringLiteral("Secret"));
    fields.append(field(QStringLiteral("password"),
                        QStringLiteral("Password"),
                        QStringLiteral("MQTT Password"),
                        QStringLiteral("Password for MQTT authentication (optional)."),
                        QJsonValue(),
                        secretFlags,
                        parentActionId));

    fields.append(field(QStringLiteral("baseTopic"),
                        QStringLiteral("String"),
                        QStringLiteral("Base topic"),
                        QStringLiteral("Zigbee2MQTT base topic (default: zigbee2mqtt)."),
                        QJsonValue(QStringLiteral("zigbee2mqtt")),
                        {},
                        parentActionId));

    fields.append(field(QStringLiteral("retryIntervalMs"),
                        QStringLiteral("Integer"),
                        QStringLiteral("Retry interval"),
                        QStringLiteral("Reconnect interval while the broker is offline."),
                        QJsonValue(10000),
                        {},
                        parentActionId));

    return fields;
}

QJsonArray instanceSettingsFields()
{
    QJsonArray fields;

    QJsonArray roFlags;
    roFlags.append(QStringLiteral("ReadOnly"));
    roFlags.append(QStringLiteral("InstanceOnly"));
    QJsonArray instanceOnlyFlags;
    instanceOnlyFlags.append(QStringLiteral("InstanceOnly"));

    fields.append(field(QStringLiteral("z2mVersion"),
                        QStringLiteral("String"),
                        QStringLiteral("Z2M Version"),
                        QStringLiteral("Detected Zigbee2MQTT version."),
                        QJsonValue(),
                        roFlags,
                        QStringLiteral("settings")));

    fields.append(field(QStringLiteral("z2mCommit"),
                        QStringLiteral("String"),
                        QStringLiteral("Z2M Commit"),
                        QStringLiteral("Detected Zigbee2MQTT commit."),
                        QJsonValue(),
                        roFlags,
                        QStringLiteral("settings")));

    QJsonObject zigbeeChannelMeta;
    zigbeeChannelMeta.insert(QStringLiteral("min"), 11);
    zigbeeChannelMeta.insert(QStringLiteral("max"), 26);
    zigbeeChannelMeta.insert(QStringLiteral("step"), 1);
    fields.append(field(QStringLiteral("zigbeeChannel"),
                        QStringLiteral("Integer"),
                        QStringLiteral("Zigbee channel"),
                        QStringLiteral("Zigbee channel (11-26). Requires restart."),
                        QJsonValue(),
                        instanceOnlyFlags,
                        QStringLiteral("settings"),
                        zigbeeChannelMeta));

    fields.append(field(QStringLiteral("panId"),
                        QStringLiteral("String"),
                        QStringLiteral("PAN ID"),
                        QStringLiteral("Current Zigbee PAN ID."),
                        QJsonValue(),
                        roFlags,
                        QStringLiteral("settings")));

    fields.append(field(QStringLiteral("extPanId"),
                        QStringLiteral("String"),
                        QStringLiteral("Extended PAN ID"),
                        QStringLiteral("Current Zigbee extended PAN ID."),
                        QJsonValue(),
                        roFlags,
                        QStringLiteral("settings")));

    fields.append(field(QStringLiteral("serialPort"),
                        QStringLiteral("String"),
                        QStringLiteral("Serial port"),
                        QStringLiteral("Configured coordinator serial port."),
                        QJsonValue(),
                        roFlags,
                        QStringLiteral("settings")));

    fields.append(field(QStringLiteral("serialAdapter"),
                        QStringLiteral("String"),
                        QStringLiteral("USB adapter"),
                        QStringLiteral("Configured coordinator USB adapter."),
                        QJsonValue(),
                        roFlags,
                        QStringLiteral("settings")));

    fields.append(field(QStringLiteral("coordinatorType"),
                        QStringLiteral("String"),
                        QStringLiteral("Coordinator type"),
                        QStringLiteral("Detected Zigbee coordinator type."),
                        QJsonValue(),
                        roFlags,
                        QStringLiteral("settings")));

    fields.append(field(QStringLiteral("coordinatorFirmware"),
                        QStringLiteral("String"),
                        QStringLiteral("Coordinator firmware"),
                        QStringLiteral("Detected Zigbee coordinator firmware revision."),
                        QJsonValue(),
                        roFlags,
                        QStringLiteral("settings")));

    fields.append(field(QStringLiteral("permitJoin"),
                        QStringLiteral("Boolean"),
                        QStringLiteral("Permit join"),
                        QStringLiteral("Current Zigbee permit-join state."),
                        QJsonValue(),
                        roFlags,
                        QStringLiteral("settings")));

    return fields;
}

} // namespace

phicore::adapter::v1::Utf8String displayName()
{
    return "Zigbee";
}

phicore::adapter::v1::Utf8String description()
{
    return "Connect to Zigbee via MQTT.";
}

phicore::adapter::v1::Utf8String iconSvg()
{
    return
        "<svg width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#26A69A\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\" xmlns=\"http://www.w3.org/2000/svg\" role=\"img\" aria-label=\"Zigbee2MQTT\">"
        "<circle cx=\"12\" cy=\"12\" r=\"2\"/>"
        "<path d=\"M12 4v2M12 18v2M4 12h2M18 12h2\"/>"
        "<path d=\"M6 6l1.5 1.5M16.5 16.5L18 18\"/>"
        "<path d=\"M6 18l1.5-1.5M16.5 7.5L18 6\"/>"
        "</svg>";
}

phicore::adapter::v1::AdapterCapabilities capabilities()
{
    namespace v1 = phicore::adapter::v1;

    v1::AdapterCapabilities caps;
    caps.required = v1::AdapterRequirement::Host | v1::AdapterRequirement::UsesRetryInterval;
    caps.optional = v1::AdapterRequirement::Port
        | v1::AdapterRequirement::Username
        | v1::AdapterRequirement::Password;
    caps.flags = v1::AdapterFlag::SupportsDiscovery
        | v1::AdapterFlag::SupportsProbe
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

    caps.defaultsJson = R"({"host":"localhost","port":1883,"retryIntervalMs":10000,"baseTopic":"zigbee2mqtt"})";
    return caps;
}

phicore::adapter::v1::JsonText configSchemaJson()
{
    const QJsonArray baseFields = baseSchemaFields();
    const QJsonArray instanceFields = instanceSettingsFields();

    QJsonObject schema;
    schema.insert(QStringLiteral("factory"),
                  section(QStringLiteral("Zigbee2MQTT"),
                          QStringLiteral("Configure the MQTT broker used by Zigbee2MQTT."),
                          baseFields));
    schema.insert(QStringLiteral("instance"),
                  section(QStringLiteral("Zigbee2MQTT"),
                          QStringLiteral("Configure the MQTT broker used by Zigbee2MQTT."),
                          instanceFields));

    return QJsonDocument(schema).toJson(QJsonDocument::Compact).toStdString();
}

} // namespace phicore::z2m::ipc
