#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include "mqttclient.h"

#include "adapterinterface.h"
#include "color.h"

namespace phicore {

class Z2mAdapter : public AdapterInterface
{
    Q_OBJECT

public:
    explicit Z2mAdapter(QObject *parent = nullptr);
    ~Z2mAdapter() override;

protected:
    bool start(QString &errorString) override;
    void stop() override;
    void adapterConfigUpdated() override;
    void requestFullSync() override;
    void invokeAdapterAction(const QString &actionId,
                             const QJsonObject &params,
                             CmdId cmdId) override;
    void updateChannelState(const QString &deviceExternalId,
                            const QString &channelExternalId,
                            const QVariant &value,
                            CmdId cmdId) override;
    void updateDeviceName(const QString &deviceId, const QString &name, CmdId cmdId) override;

private:
    struct PendingRename {
        CmdId cmdId = 0;
        QString targetName;
        qint64 requestedAtMs = 0;
    };

    struct Z2mChannelBinding {
        QString channelId;
        QString property;
        ChannelKind kind = ChannelKind::Unknown;
        ChannelDataType dataType = ChannelDataType::Unknown;
        ChannelFlags flags = ChannelFlag::ChannelFlagNone;
        QString unit;
        double rawMin = 0.0;
        double rawMax = 0.0;
        double rawStep = 0.0;
        double valueScale = 1.0;
        QString endpoint;
        QString valueOn;
        QString valueOff;
        QString colorMode;
        bool scalePercent = false;
        bool isAvailability = false;
        QHash<QString, int> enumRawToValue;
        QHash<int, QString> enumValueToRaw;
    };

    struct Z2mDeviceEntry {
        Device device;
        QString mqttId;
        ChannelList channels;
        QHash<QString, Z2mChannelBinding> bindingsByChannel;
        QMultiHash<QString, QString> channelByProperty;
    };

    void setConnected(bool connected);
    void updateConnectionState();
    void applyConfig();
    void connectToBroker();
    void disconnectFromBroker();
    void scheduleReconnect();
    void stopReconnectTimer();
    void ensureSubscriptions();

    void handleMqttMessage(const QByteArray &message, const QString &topic);
    void handleBridgeDevicesPayload(const QJsonArray &devices, bool fullSnapshot);
    void handleBridgeInfoPayload(const QJsonObject &payload, qint64 tsMs);
    void handleDeviceStatePayload(const QString &deviceId, const QJsonObject &payload, qint64 tsMs);
    void handleAvailabilityPayload(const QString &deviceId, const QString &payload, qint64 tsMs);

    Z2mDeviceEntry buildDeviceEntry(const QJsonObject &obj) const;
    void collectExposeEntries(const QJsonValue &value, QList<QJsonObject> &out) const;
    void addChannelFromExpose(const QJsonObject &expose, Z2mDeviceEntry &entry) const;
    ChannelFlags flagsFromAccess(int access) const;
    QString labelFromProperty(const QString &property, const QString &fallback) const;
    DeviceClass inferDeviceClass(const QList<QJsonObject> &exposes) const;
    ButtonEventCode actionToButtonEvent(const QString &action) const;

    bool publishCommand(const QString &deviceId,
                        const QJsonObject &payload,
                        const QString &endpoint,
                        QString &errorString);
    bool buildCommandPayload(const QString &deviceId,
                             const Z2mChannelBinding &binding,
                             const QVariant &value,
                             QJsonObject &payload,
                             QString &errorString) const;

    double scaleToPercent(double raw, double rawMin, double rawMax) const;
    double scaleFromPercent(double percent, double rawMin, double rawMax) const;

    MqttClient *m_client = nullptr;
    QTimer *m_reconnectTimer = nullptr;
    bool m_connected = false;
    bool m_mqttConnected = false;
    bool m_bridgeOnline = true;
    bool m_lastSeenRequested = false;
    bool m_pendingFullSync = false;
    int m_retryIntervalMs = 10000;
    QString m_baseTopic = QStringLiteral("zigbee2mqtt");
    QHash<QString, Z2mDeviceEntry> m_devices;
    QHash<QString, QString> m_mqttByExternal;
    QHash<QString, PendingRename> m_pendingRename;
    QHash<QString, QJsonObject> m_pendingStatePayloads;
    QHash<QString, QPointer<QTimer>> m_postSetRefreshTimers;
    QString m_coordinatorId;
    QJsonObject m_pendingBridgeInfo;
};

} // namespace phicore
