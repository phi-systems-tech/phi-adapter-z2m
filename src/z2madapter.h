#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include "mqttclient.h"

#include "adapterinterface.h"
#include "phi/adapter/qt/color.h"

namespace phicore::adapter {

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
    void updateStaticConfig(const QJsonObject &config) override;
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
        int actionButtonId = 0;
        bool actionIsDial = false;
        QHash<QString, int> enumRawToValue;
        QHash<int, QString> enumValueToRaw;
    };

    // What a zigbee2mqtt device turns into, and the turning of it. Reachable
    // from a subclass so a test can hand it a payload and look at the result -
    // that conversion is most of what this adapter is, and all of it happens
    // before anything touches a broker.
protected:
    struct Z2mDeviceEntry {
        Device device;
        QString mqttId;
        ChannelList channels;
        QHash<QString, Z2mChannelBinding> bindingsByChannel;
        QMultiHash<QString, QString> channelByProperty;
    };

private:

    void setConnected(bool connected, bool forceNotify = false);
    void updateConnectionState(bool forceNotify = false);
    void scheduleConnectionStateRefresh();
    void applyConfig();
    void connectToBroker();
    void disconnectFromBroker();
    void scheduleReconnect();
    void stopReconnectTimer();
    void ensureSubscriptions();

    void handleMqttMessage(const QByteArray &message, const QString &topic);
    void handleBridgeDevicesPayload(const QJsonArray &devices, bool fullSnapshot);
    void handleDeviceStatePayload(const QString &deviceId, const QJsonObject &payload, qint64 tsMs);
    void handleAvailabilityPayload(const QString &deviceId, const QString &payload, qint64 tsMs);

protected:
    Z2mDeviceEntry buildDeviceEntry(const QJsonObject &obj) const;

    /// What `bridge/info` says about the bridge itself, reported without
    /// waiting for anything: which serial adapter Zigbee2MQTT is driving the
    /// coordinator with, on which port, at which firmware, on which channel.
    ///
    /// Protected for the same reason `buildDeviceEntry` is - it belongs to the
    /// adapter and not to its callers, and a test is the one caller allowed to
    /// look.
    void reportBridgeFacts(const QJsonObject &payload);

    /// The whole of what arrives on `bridge/info`: the facts above, and then
    /// the coordinator as a device once there is one to update.
    void handleBridgeInfoPayload(const QJsonObject &payload, qint64 tsMs);

private:
    void collectExposeEntries(const QJsonValue &value, QList<QJsonObject> &out) const;
    void addChannelFromExpose(const QJsonObject &expose, Z2mDeviceEntry &entry) const;
    bool isPropertySuppressed(const QString &property, const Z2mDeviceEntry &entry) const;
    ChannelFlags flagsFromAccess(int access) const;
    QString labelFromProperty(const QString &property, const QString &fallback) const;
    DeviceClass inferDeviceClass(const QList<QJsonObject> &exposes) const;
    ButtonEventCode actionToButtonEvent(const QString &action) const;
    void handleButtonShortPressRelease(const QString &pressKey,
                                       const QString &externalId,
                                       const QString &channelId,
                                       qint64 tsMs);
    void finalizePendingButtonShortPress(const QString &pressKey,
                                         const QString &externalId,
                                         const QString &channelId,
                                         qint64 tsMs = 0);

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

    ::phicore::MqttClient *m_client = nullptr;
    QTimer *m_reconnectTimer = nullptr;
    bool m_connected = false;
    bool m_mqttConnected = false;
    bool m_bridgeOnline = true;
    bool m_lastSeenRequested = false;
    int m_retryIntervalMs = 10000;
    QString m_baseTopic = QStringLiteral("zigbee2mqtt");
    QJsonObject m_staticConfig;
    QStringList m_suppressedPropertyPrefixes;
    QHash<QString, QStringList> m_suppressedPropertyPrefixesByModel;
    QHash<QString, QStringList> m_suppressedPropertyPrefixesByModelId;
    QHash<QString, QStringList> m_allowedPropertyPrefixesByModel;
    QHash<QString, QStringList> m_allowedPropertyPrefixesByModelId;
    QHash<QString, Z2mDeviceEntry> m_devices;
    QHash<QString, QString> m_mqttByExternal;
    QHash<QString, PendingRename> m_pendingRename;
    QHash<QString, QJsonObject> m_pendingStatePayloads;
    QHash<QString, QPointer<QTimer>> m_postSetRefreshTimers;
    QHash<QString, QPointer<QTimer>> m_dialResetTimers;
    QHash<QString, int> m_lastDialValueByChannel;
    QHash<QString, int> m_pendingDialDirectionByChannel;
    QHash<QString, qint64> m_pendingDialDirectionTsByChannel;
    QHash<QString, qint64> m_recentActionTs;
    QHash<QString, QPointer<QTimer>> m_buttonMultiPressTimers;
    QHash<QString, int> m_buttonMultiPressCounts;
    QHash<QString, qint64> m_buttonMultiPressLastTs;
    QHash<QString, int> m_buttonLastEventCode;
    QHash<QString, qint64> m_buttonLastEventTs;
    QString m_coordinatorId;
    QJsonObject m_pendingBridgeInfo;
};

} // namespace phicore::adapter
