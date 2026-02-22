// adapters/plugins/z2m/z2madapterfactory.cpp
#include "z2madapterfactory.h"
#include "z2madapter.h"

#include <QTcpSocket>

namespace phicore::adapter {

static const QByteArray kZ2mIconSvg = QByteArrayLiteral(
    "<svg width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#26A69A\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\" xmlns=\"http://www.w3.org/2000/svg\" role=\"img\" aria-label=\"Zigbee2MQTT\">\n"
    "  <circle cx=\"12\" cy=\"12\" r=\"2\"/>\n"
    "  <path d=\"M12 4v2M12 18v2M4 12h2M18 12h2\"/>\n"
    "  <path d=\"M6 6l1.5 1.5M16.5 16.5L18 18\"/>\n"
    "  <path d=\"M6 18l1.5-1.5M16.5 7.5L18 6\"/>\n"
    "</svg>\n"
);

QByteArray Z2mAdapterFactory::icon() const
{
    return kZ2mIconSvg;
}

AdapterCapabilities Z2mAdapterFactory::capabilities() const
{
    AdapterCapabilities caps;
    caps.required = AdapterRequirement::Host;
    caps.required = caps.required | AdapterRequirement::UsesRetryInterval;
    caps.optional = AdapterRequirement::Port | AdapterRequirement::Username | AdapterRequirement::Password;
    caps.flags |= AdapterFlag::AdapterFlagSupportsDiscovery;
    caps.flags |= AdapterFlag::AdapterFlagSupportsProbe;
    caps.flags |= AdapterFlag::AdapterFlagSupportsRename;
    AdapterActionDescriptor settings;
    settings.id = QStringLiteral("settings");
    settings.label = QStringLiteral("Settings");
    settings.description = QStringLiteral("Edit Zigbee2MQTT connection settings.");
    settings.hasForm = true;
    caps.instanceActions.push_back(settings);
    AdapterActionDescriptor permitJoin;
    permitJoin.id = QStringLiteral("permitJoin");
    permitJoin.label = QStringLiteral("Open pairing (2 min)");
    permitJoin.description = QStringLiteral("Allow new Zigbee devices to join for 2 minutes.");
    permitJoin.cooldownMs = 120000;
    caps.instanceActions.push_back(permitJoin);
    AdapterActionDescriptor restart;
    restart.id = QStringLiteral("restartZ2M");
    restart.label = QStringLiteral("Restart Zigbee2MQTT");
    restart.description = QStringLiteral("Restarts Zigbee2MQTT. Devices may be unavailable briefly.");
    {
        QJsonObject confirm;
        confirm.insert(QStringLiteral("title"), QStringLiteral("Restart Zigbee2MQTT?"));
        confirm.insert(QStringLiteral("message"), QStringLiteral("This will briefly disconnect Zigbee devices. Continue?"));
        confirm.insert(QStringLiteral("okText"), QStringLiteral("Restart"));
        confirm.insert(QStringLiteral("cancelText"), QStringLiteral("Cancel"));
        confirm.insert(QStringLiteral("danger"), true);
        restart.confirm = confirm;
    }
    caps.instanceActions.push_back(restart);
    caps.defaults.insert(QStringLiteral("host"), QStringLiteral("localhost"));
    caps.defaults.insert(QStringLiteral("port"), 1883);
    caps.defaults.insert(QStringLiteral("retryIntervalMs"), 10000);
    caps.defaults.insert(QStringLiteral("baseTopic"), QStringLiteral("zigbee2mqtt"));
    return caps;
}

discovery::DiscoveryList Z2mAdapterFactory::discover() const
{
    constexpr quint16 kDefaultPort = 1883;
    const QString host = QStringLiteral("127.0.0.1");

    discovery::Discovery info;
    info.pluginType = pluginType();
    info.discoveredId = QStringLiteral("z2m");
    info.label = QStringLiteral("Zigbee");
    info.hostname = QStringLiteral("localhost");
    info.ip = host;
    info.port = kDefaultPort;
    info.kind = discovery::DiscoveryKind::Manual;
    return { info };
}

discovery::DiscoveryQueryList Z2mAdapterFactory::discoveryQueries() const
{
    discovery::DiscoveryQuery mdns;
    mdns.pluginType      = pluginType();
    mdns.kind            = discovery::DiscoveryKind::Mdns;
    mdns.mdnsServiceType = QStringLiteral("_mqtt._tcp");
    mdns.defaultPort     = 1883;
    return { mdns };
}

AdapterConfigSchema Z2mAdapterFactory::configSchema(const Adapter &info) const
{
    AdapterConfigSchema schema;
    schema.title       = QStringLiteral("Zigbee2MQTT");
    schema.description = QStringLiteral("Configure the MQTT broker used by Zigbee2MQTT.");

    {
        AdapterConfigField f;
        f.key         = QStringLiteral("host");
        f.type        = AdapterConfigFieldType::Hostname;
        f.label       = QStringLiteral("MQTT Host");
        f.description = QStringLiteral("IP address or hostname of the MQTT broker.");
        f.flags       = AdapterConfigFieldFlag::Required;
        f.placeholder = QStringLiteral("localhost");
        if (!info.host.isEmpty())
            f.defaultValue = info.host;
        schema.fields.push_back(f);
    }

    {
        AdapterConfigField f;
        f.key         = QStringLiteral("port");
        f.type        = AdapterConfigFieldType::Port;
        f.label       = QStringLiteral("MQTT Port");
        f.description = QStringLiteral("TCP port of the MQTT broker.");
        f.defaultValue = info.port > 0 ? info.port : 1883;
        schema.fields.push_back(f);
    }

    {
        AdapterConfigField f;
        f.key         = QStringLiteral("user");
        f.type        = AdapterConfigFieldType::String;
        f.label       = QStringLiteral("MQTT Username");
        f.description = QStringLiteral("Username for MQTT authentication (optional).");
        if (!info.user.isEmpty())
            f.defaultValue = info.user;
        schema.fields.push_back(f);
    }

    {
        AdapterConfigField f;
        f.key         = QStringLiteral("password");
        f.type        = AdapterConfigFieldType::Password;
        f.label       = QStringLiteral("MQTT Password");
        f.description = QStringLiteral("Password for MQTT authentication (optional).");
        f.flags       = AdapterConfigFieldFlag::Secret;
        schema.fields.push_back(f);
    }

    {
        AdapterConfigField f;
        f.key         = QStringLiteral("baseTopic");
        f.type        = AdapterConfigFieldType::String;
        f.label       = QStringLiteral("Base topic");
        f.description = QStringLiteral("Zigbee2MQTT base topic (default: zigbee2mqtt).");
        f.defaultValue = info.meta.value(QStringLiteral("baseTopic")).toString(QStringLiteral("zigbee2mqtt"));
        schema.fields.push_back(f);
    }

    {
        AdapterConfigField f;
        f.key         = QStringLiteral("retryIntervalMs");
        f.type        = AdapterConfigFieldType::Integer;
        f.label       = QStringLiteral("Retry interval");
        f.description = QStringLiteral("Reconnect interval while the broker is offline.");
        f.defaultValue = 10000;
        schema.fields.push_back(f);
    }

    const QJsonObject bridgeInfo = info.meta.value(QStringLiteral("bridge_info")).toObject();
    const QJsonObject network = bridgeInfo.value(QStringLiteral("network")).toObject();
    const QJsonObject config = bridgeInfo.value(QStringLiteral("config")).toObject();
    const QJsonObject serial = config.value(QStringLiteral("serial")).toObject();
    const QJsonObject coordinator = bridgeInfo.value(QStringLiteral("coordinator")).toObject();
    const QJsonObject coordMeta = coordinator.value(QStringLiteral("meta")).toObject();

    auto addReadOnlyField = [&](const QString &key, const QString &label, const QString &value) -> bool {
        if (value.isEmpty())
            return false;
        AdapterConfigField f;
        f.key = key;
        f.label = label;
        f.type = AdapterConfigFieldType::String;
        f.flags = AdapterConfigFieldFlag::ReadOnly | AdapterConfigFieldFlag::InstanceOnly;
        f.meta.insert(QStringLiteral("parentAction"), QStringLiteral("settings"));
        f.defaultValue = value;
        schema.fields.push_back(f);
        return true;
    };

    bool addedInfo = false;
    addedInfo |= addReadOnlyField(QStringLiteral("z2mVersion"),
                                  QStringLiteral("Z2M Version"),
                                  info.meta.value(QStringLiteral("z2m_version")).toString().trimmed());
    addedInfo |= addReadOnlyField(QStringLiteral("z2mCommit"),
                                  QStringLiteral("Z2M Commit"),
                                  info.meta.value(QStringLiteral("z2m_commit")).toString().trimmed());
    {
        const QJsonValue channelJson = network.value(QStringLiteral("channel"));
        const bool hasChannel = channelJson.isDouble();
        const int channelValue = channelJson.toInt();
        AdapterConfigField channelField;
        channelField.key = QStringLiteral("zigbeeChannel");
        channelField.label = QStringLiteral("Zigbee channel");
        channelField.type = AdapterConfigFieldType::Integer;
        channelField.flags = AdapterConfigFieldFlag::InstanceOnly;
        channelField.meta.insert(QStringLiteral("parentAction"), QStringLiteral("settings"));
        channelField.description = QStringLiteral("Zigbee channel (11-26). Requires restart.");
        if (hasChannel)
            channelField.defaultValue = channelValue;
        channelField.meta.insert(QStringLiteral("min"), 11);
        channelField.meta.insert(QStringLiteral("max"), 26);
        channelField.meta.insert(QStringLiteral("step"), 1);
        schema.fields.push_back(channelField);
        addedInfo = true;
    }
    addedInfo |= addReadOnlyField(QStringLiteral("panId"),
                                  QStringLiteral("PAN ID"),
                                  network.value(QStringLiteral("pan_id")).toVariant().toString().trimmed());
    addedInfo |= addReadOnlyField(QStringLiteral("extPanId"),
                                  QStringLiteral("Extended PAN ID"),
                                  network.value(QStringLiteral("extended_pan_id")).toVariant().toString().trimmed());
    addedInfo |= addReadOnlyField(QStringLiteral("serialPort"),
                                  QStringLiteral("Serial port"),
                                  serial.value(QStringLiteral("port")).toString().trimmed());
    addedInfo |= addReadOnlyField(QStringLiteral("serialAdapter"),
                                  QStringLiteral("USB adapter"),
                                  serial.value(QStringLiteral("adapter")).toString().trimmed());
    addedInfo |= addReadOnlyField(QStringLiteral("coordinatorType"),
                                  QStringLiteral("Coordinator type"),
                                  coordinator.value(QStringLiteral("type")).toString().trimmed());
    addedInfo |= addReadOnlyField(QStringLiteral("coordinatorFirmware"),
                                  QStringLiteral("Coordinator firmware"),
                                  coordMeta.value(QStringLiteral("revision")).toString().trimmed());

    const QJsonValue permitJoin = bridgeInfo.value(QStringLiteral("permit_join"));
    if (permitJoin.isBool()) {
        addedInfo = true;
        AdapterConfigField f;
        f.key = QStringLiteral("permitJoin");
        f.label = QStringLiteral("Permit join");
        f.type = AdapterConfigFieldType::Boolean;
        f.flags = AdapterConfigFieldFlag::ReadOnly | AdapterConfigFieldFlag::InstanceOnly;
        f.defaultValue = permitJoin.toBool();
        f.meta.insert(QStringLiteral("parentAction"), QStringLiteral("settings"));
        schema.fields.push_back(f);
    }

    return schema;
}

ActionResponse Z2mAdapterFactory::invokeTestConnection(Adapter &infoInOut) const
{
    ActionResponse resp;
    const QString host = infoInOut.host.trimmed();
    if (host.isEmpty()) {
        resp.status = CmdStatus::InvalidArgument;
        resp.error = QStringLiteral("Host must not be empty.");
        return resp;
    }
    const quint16 port = infoInOut.port > 0 ? infoInOut.port : 1883;

    QTcpSocket socket;
    socket.connectToHost(host, port);
    if (!socket.waitForConnected(2000)) {
        resp.status = CmdStatus::Failure;
        resp.error = socket.errorString();
        socket.abort();
        return resp;
    }
    socket.disconnectFromHost();
    resp.status = CmdStatus::Success;
    return resp;
}

AdapterInterface *Z2mAdapterFactory::create(QObject *parent)
{
    return new Z2mAdapter(parent);
}

} // namespace phicore::adapter
