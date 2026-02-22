// adapters/plugins/z2m/z2madapterfactory.h
#pragma once

#include "adapterfactory.h"

namespace phicore::adapter {

class Z2mAdapterFactory : public AdapterFactory
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PHI_ADAPTER_FACTORY_IID)
    Q_INTERFACES(phicore::adapter::AdapterFactory)

public:
    Z2mAdapterFactory(QObject *parent = nullptr) : AdapterFactory(parent) {}

    QString pluginType()  const override { return QStringLiteral("z2m"); }
    QString displayName() const override { return QStringLiteral("Zigbee"); }
    QString apiVersion()  const override { return QStringLiteral("1.0.0"); }
    QString description() const override { return QStringLiteral("Connect to Zigbee via MQTT."); }
    QByteArray icon() const override;

    AdapterCapabilities capabilities() const override;
    discovery::DiscoveryList discover() const override;
    discovery::DiscoveryQueryList discoveryQueries() const override;
    AdapterConfigSchema configSchema(const Adapter &info) const override;
    ActionResponse invokeTestConnection(Adapter &infoInOut) const override;
    AdapterInterface *create(QObject *parent = nullptr) override;
};

} // namespace phicore::adapter
