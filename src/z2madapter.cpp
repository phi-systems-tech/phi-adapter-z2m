#include "z2madapter.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QSet>
#include <QtGlobal>
#include <QStringList>

#include "mqttclient.h"

Q_LOGGING_CATEGORY(adapterLog, "phi-core.adapters.z2m")

namespace {

constexpr int kDefaultPort = 1883;
constexpr int kAccessState = 0b001;
constexpr int kAccessSet = 0b010;

phicore::ChannelFlags forceReadOnly(phicore::ChannelFlags flags)
{
    if (flags.testFlag(phicore::ChannelFlag::ChannelFlagWritable))
        flags ^= phicore::ChannelFlag::ChannelFlagWritable;
    flags |= phicore::ChannelFlag::ChannelFlagReadable
        | phicore::ChannelFlag::ChannelFlagReportable
        | phicore::ChannelFlag::ChannelFlagRetained;
    return flags;
}

QString enumLabelFor(const QString &enumName, int value)
{
    if (enumName.compare(QStringLiteral("RockerMode"), Qt::CaseInsensitive) == 0) {
        switch (value) {
        case static_cast<int>(phicore::RockerMode::SingleRocker):
            return QStringLiteral("SingleRocker");
        case static_cast<int>(phicore::RockerMode::DualRocker):
            return QStringLiteral("DualRocker");
        case static_cast<int>(phicore::RockerMode::SinglePush):
            return QStringLiteral("SinglePush");
        case static_cast<int>(phicore::RockerMode::DualPush):
            return QStringLiteral("DualPush");
        default:
            return QString();
        }
    }
    if (enumName.compare(QStringLiteral("SensitivityLevel"), Qt::CaseInsensitive) == 0) {
        switch (value) {
        case static_cast<int>(phicore::SensitivityLevel::Low):
            return QStringLiteral("Low");
        case static_cast<int>(phicore::SensitivityLevel::Medium):
            return QStringLiteral("Medium");
        case static_cast<int>(phicore::SensitivityLevel::High):
            return QStringLiteral("High");
        case static_cast<int>(phicore::SensitivityLevel::VeryHigh):
            return QStringLiteral("VeryHigh");
        case static_cast<int>(phicore::SensitivityLevel::Max):
            return QStringLiteral("Max");
        default:
            return QString();
        }
    }
    return QString();
}

bool isKnownEnumName(const QString &name, const char *enumName)
{
    return name.compare(QString::fromLatin1(enumName), Qt::CaseInsensitive) == 0;
}

QHash<QString, int> buildStableEnumMap(const QStringList &rawKeys, const QJsonObject &existing)
{
    QHash<QString, int> map;
    int maxValue = 0;
    for (auto it = existing.begin(); it != existing.end(); ++it) {
        if (!it.value().isDouble())
            continue;
        const int v = it.value().toInt();
        if (v <= 0)
            continue;
        map.insert(it.key(), v);
        if (v > maxValue)
            maxValue = v;
    }
    QStringList sorted = rawKeys;
    sorted.sort(Qt::CaseInsensitive);
    for (const QString &key : sorted) {
        if (key.isEmpty())
            continue;
        if (map.contains(key))
            continue;
        map.insert(key, ++maxValue);
    }
    return map;
}

std::optional<int> mapRockerMode(const QString &raw)
{
    const QString key = raw.trimmed().toLower();
    if (key == QStringLiteral("single_rocker") || key == QStringLiteral("singlerocker"))
        return static_cast<int>(phicore::RockerMode::SingleRocker);
    if (key == QStringLiteral("dual_rocker") || key == QStringLiteral("dualrocker"))
        return static_cast<int>(phicore::RockerMode::DualRocker);
    if (key == QStringLiteral("single_push_button") || key == QStringLiteral("singlepushbutton"))
        return static_cast<int>(phicore::RockerMode::SinglePush);
    if (key == QStringLiteral("dual_push_button") || key == QStringLiteral("dualpushbutton"))
        return static_cast<int>(phicore::RockerMode::DualPush);
    return std::nullopt;
}

std::optional<int> mapSensitivityLevel(const QString &raw)
{
    const QString key = raw.trimmed().toLower();
    if (key == QStringLiteral("low"))
        return static_cast<int>(phicore::SensitivityLevel::Low);
    if (key == QStringLiteral("medium"))
        return static_cast<int>(phicore::SensitivityLevel::Medium);
    if (key == QStringLiteral("high"))
        return static_cast<int>(phicore::SensitivityLevel::High);
    if (key == QStringLiteral("very_high") || key == QStringLiteral("veryhigh"))
        return static_cast<int>(phicore::SensitivityLevel::VeryHigh);
    if (key == QStringLiteral("max"))
        return static_cast<int>(phicore::SensitivityLevel::Max);
    return std::nullopt;
}

}

namespace phicore {

Z2mAdapter::Z2mAdapter(QObject *parent)
    : AdapterInterface(parent)
{
}

Z2mAdapter::~Z2mAdapter()
{
    stop();
}

bool Z2mAdapter::start(QString &errorString)
{
    errorString.clear();
    m_pendingFullSync = false;
    applyConfig();

    if (!m_client) {
        m_client = new MqttClient(this);
        m_client->setClientId(QStringLiteral("phi-core-z2m-%1").arg(adapter().id));
        connect(m_client, &MqttClient::connected, this, [this]() {
            qCInfo(adapterLog) << "Z2M MQTT connected, subscribing";
            m_mqttConnected = true;
            updateConnectionState();
            ensureSubscriptions();
            const QByteArray requestPayload = QByteArrayLiteral("{}");
            m_client->publish(QStringLiteral("%1/bridge/request/info").arg(m_baseTopic),
                              requestPayload);
        });
        connect(m_client, &MqttClient::disconnected, this, [this]() {
            m_mqttConnected = false;
            updateConnectionState();
            scheduleReconnect();
        });
        connect(m_client, &MqttClient::messageReceived, this,
                [this](const QByteArray &message, const QString &topic) {
            handleMqttMessage(message, topic);
        });
        connect(m_client, &MqttClient::errorOccurred, this, [this](int code, const QString &message) {
            if (m_client->state() == MqttClient::State::Connected)
                return;
            qCWarning(adapterLog) << "Z2M MQTT error:" << code << message;
        });
    }

    qCInfo(adapterLog) << "Starting Z2M adapter for" << adapter().id
                       << "host" << adapter().ip.trimmed()
                       << "port" << (adapter().port > 0 ? adapter().port : kDefaultPort)
                       << "baseTopic" << m_baseTopic
                       << "retryIntervalMs" << m_retryIntervalMs;

    if (adapter().ip.trimmed().isEmpty()) {
        qCWarning(adapterLog) << "Z2mAdapter: IP not configured; staying disconnected";
    }

    connectToBroker();
    qCInfo(adapterLog) << "Z2M start() finished for" << adapter().id;
    return true;
}

void Z2mAdapter::stop()
{
    stopReconnectTimer();
    disconnectFromBroker();
    for (auto it = m_postSetRefreshTimers.begin(); it != m_postSetRefreshTimers.end(); ++it) {
        if (it.value()) {
            it.value()->stop();
            it.value()->deleteLater();
        }
    }
    m_postSetRefreshTimers.clear();
    if (m_client) {
        m_client->deleteLater();
        m_client = nullptr;
    }
    m_mqttConnected = false;
    updateConnectionState();
}

void Z2mAdapter::adapterConfigUpdated()
{
    disconnectFromBroker();
    applyConfig();
    connectToBroker();
}

void Z2mAdapter::requestFullSync()
{
    m_pendingFullSync = true;
    qCInfo(adapterLog) << "Z2M requestFullSync() pending=true";
    if (m_client && m_client->state() == MqttClient::State::Connected) {
        const QByteArray requestPayload = QByteArrayLiteral("{}");
        const QString topic = QStringLiteral("%1/bridge/request/devices").arg(m_baseTopic);
        qCInfo(adapterLog) << "Z2M full sync requested via" << topic;
        m_client->publish(topic, requestPayload);
    }
    if (!m_devices.isEmpty()) {
        for (auto it = m_devices.constBegin(); it != m_devices.constEnd(); ++it) {
            emit deviceUpdated(it.value().device, it.value().channels);
        }
    }
}

void Z2mAdapter::updateChannelState(const QString &deviceExternalId,
                                    const QString &channelExternalId,
                                    const QVariant &value,
                                    CmdId cmdId)
{
    CmdResponse response;
    response.id = cmdId;
    response.tsMs = QDateTime::currentMSecsSinceEpoch();

    const QString mqttId = m_mqttByExternal.value(deviceExternalId, deviceExternalId);
    const auto deviceIt = m_devices.find(mqttId);
    if (deviceIt == m_devices.end()) {
        response.status = CmdStatus::NotSupported;
        response.error = QStringLiteral("Unknown device");
        emit cmdResult(response);
        return;
    }

    const Z2mDeviceEntry &entry = deviceIt.value();
    const auto bindingIt = entry.bindingsByChannel.find(channelExternalId);
    if (bindingIt == entry.bindingsByChannel.end()) {
        response.status = CmdStatus::NotSupported;
        response.error = QStringLiteral("Unknown channel");
        emit cmdResult(response);
        return;
    }

    const Z2mChannelBinding &binding = bindingIt.value();
    if (!binding.flags.testFlag(ChannelFlag::ChannelFlagWritable)) {
        response.status = CmdStatus::NotSupported;
        response.error = QStringLiteral("Channel is read-only");
        emit cmdResult(response);
        return;
    }

    if (!m_connected || !m_client || m_client->state() != MqttClient::State::Connected) {
        response.status = CmdStatus::TemporarilyOffline;
        response.error = QStringLiteral("MQTT broker not connected");
        emit cmdResult(response);
        return;
    }

    QJsonObject payload;
    QString errorString;
    if (!buildCommandPayload(deviceExternalId, binding, value, payload, errorString)) {
        response.status = CmdStatus::InvalidArgument;
        response.error = errorString;
        emit cmdResult(response);
        return;
    }

    if (!publishCommand(mqttId, payload, binding.endpoint, errorString)) {
        response.status = CmdStatus::Failure;
        response.error = errorString;
        emit cmdResult(response);
        return;
    }

    // Debounced post-set refresh to read back all reported channels.
    QTimer *refreshTimer = m_postSetRefreshTimers.value(mqttId);
    if (!refreshTimer) {
        refreshTimer = new QTimer(this);
        refreshTimer->setSingleShot(true);
        m_postSetRefreshTimers.insert(mqttId, refreshTimer);
        connect(refreshTimer, &QTimer::timeout, this, [this, mqttId]() {
            if (!m_client || m_client->state() != MqttClient::State::Connected)
                return;
            const QString topic = QStringLiteral("%1/%2/get").arg(m_baseTopic, mqttId);
            const qint32 msgId = m_client->publish(topic, QByteArrayLiteral("{}"));
            if (msgId < 0) {
                qCWarning(adapterLog) << "Z2M post-set refresh publish failed for" << mqttId;
            } else {
                qCInfo(adapterLog) << "Z2M post-set refresh requested for" << mqttId;
            }
        });
    }
    refreshTimer->start(1000);

    response.status = CmdStatus::Success;
    emit cmdResult(response);
}

void Z2mAdapter::updateDeviceName(const QString &deviceId, const QString &name, CmdId cmdId)
{
    CmdResponse response;
    response.id = cmdId;
    response.tsMs = QDateTime::currentMSecsSinceEpoch();

    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        response.status = CmdStatus::InvalidArgument;
        response.error = QStringLiteral("Name must not be empty");
        emit cmdResult(response);
        return;
    }

    const QString mqttId = m_mqttByExternal.value(deviceId, deviceId);
    if (mqttId.isEmpty()) {
        response.status = CmdStatus::NotSupported;
        response.error = QStringLiteral("Unknown device");
        emit cmdResult(response);
        return;
    }
    if (m_pendingRename.contains(deviceId)) {
        response.status = CmdStatus::TemporarilyOffline;
        response.error = QStringLiteral("Rename already pending");
        emit cmdResult(response);
        return;
    }

    if (!m_connected || !m_client || m_client->state() != MqttClient::State::Connected) {
        response.status = CmdStatus::TemporarilyOffline;
        response.error = QStringLiteral("MQTT broker not connected");
        emit cmdResult(response);
        return;
    }

    QJsonObject payload;
    payload.insert(QStringLiteral("from"), mqttId);
    payload.insert(QStringLiteral("to"), trimmed);
    const QString topic = QStringLiteral("%1/bridge/request/device/rename").arg(m_baseTopic);
    const qint32 msgId = m_client->publish(topic, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    if (msgId < 0) {
        response.status = CmdStatus::Failure;
        response.error = QStringLiteral("MQTT publish failed.");
        emit cmdResult(response);
        return;
    }

    PendingRename pending;
    pending.cmdId = cmdId;
    pending.targetName = trimmed;
    pending.requestedAtMs = response.tsMs;
    m_pendingRename.insert(deviceId, pending);
    QTimer::singleShot(10000, this, [this, deviceId]() {
        const auto it = m_pendingRename.constFind(deviceId);
        if (it == m_pendingRename.constEnd())
            return;
        CmdResponse timeoutResp;
        timeoutResp.id = it.value().cmdId;
        timeoutResp.tsMs = QDateTime::currentMSecsSinceEpoch();
        timeoutResp.status = CmdStatus::Failure;
        timeoutResp.error = QStringLiteral("Rename timeout");
        emit cmdResult(timeoutResp);
        m_pendingRename.remove(deviceId);
    });
}

void Z2mAdapter::invokeAdapterAction(const QString &actionId,
                                     const QJsonObject &params,
                                     CmdId cmdId)
{
    Q_UNUSED(params);
    if (actionId != QStringLiteral("permitJoin") && actionId != QStringLiteral("restartZ2M")) {
        AdapterInterface::invokeAdapterAction(actionId, params, cmdId);
        return;
    }

    ActionResponse resp;
    if (cmdId != 0)
        resp.id = cmdId;
    resp.tsMs = QDateTime::currentMSecsSinceEpoch();

    if (!m_client || m_client->state() != MqttClient::State::Connected) {
        resp.status = CmdStatus::Failure;
        resp.error = QStringLiteral("MQTT client not connected.");
        emit actionResult(resp);
        return;
    }
    if (!m_bridgeOnline) {
        resp.status = CmdStatus::Failure;
        resp.error = QStringLiteral("Z2M bridge is offline.");
        emit actionResult(resp);
        return;
    }

    QJsonObject payload;
    QString topic;
    if (actionId == QStringLiteral("restartZ2M")) {
        topic = QStringLiteral("%1/bridge/request/restart").arg(m_baseTopic);
    } else {
        payload.insert(QStringLiteral("value"), true);
        payload.insert(QStringLiteral("time"), 120);
        topic = QStringLiteral("%1/bridge/request/permit_join").arg(m_baseTopic);
    }
    const qint32 msgId = m_client->publish(topic,
                                           QJsonDocument(payload).toJson(QJsonDocument::Compact));
    if (msgId < 0) {
        resp.status = CmdStatus::Failure;
        resp.error = QStringLiteral("MQTT publish failed.");
        emit actionResult(resp);
        return;
    }

    resp.status = CmdStatus::Success;
    emit actionResult(resp);
}

void Z2mAdapter::setConnected(bool connected)
{
    if (m_connected == connected)
        return;
    m_connected = connected;
    if (m_connected)
        stopReconnectTimer();
    emit connectionStateChanged(m_connected);
}

void Z2mAdapter::updateConnectionState()
{
    setConnected(m_mqttConnected && m_bridgeOnline);
}

void Z2mAdapter::applyConfig()
{
    const int retry = adapter().meta.value(QStringLiteral("retryIntervalMs")).toInt(10000);
    m_retryIntervalMs = retry >= 1000 ? retry : 10000;

    const QString baseTopic = adapter().meta.value(QStringLiteral("baseTopic")).toString().trimmed();
    if (!baseTopic.isEmpty())
        m_baseTopic = baseTopic;
    else
        m_baseTopic = QStringLiteral("zigbee2mqtt");
    if (m_baseTopic.endsWith(QLatin1Char('/')))
        m_baseTopic.chop(1);

    if (!m_client)
        return;

    const QString ip = adapter().ip.trimmed();
    if (!ip.isEmpty())
        m_client->setHostname(ip);
    m_client->setPort(adapter().port > 0 ? adapter().port : kDefaultPort);
    m_client->setUsername(adapter().user.trimmed());
    m_client->setPassword(adapter().pw);
}

void Z2mAdapter::connectToBroker()
{
    if (!m_client)
        return;
    if (m_client->state() == MqttClient::State::Connected || m_client->state() == MqttClient::State::Connecting)
        return;
    const QString ip = adapter().ip.trimmed();
    if (ip.isEmpty())
        return;
    m_client->setHostname(ip);
    m_client->setPort(adapter().port > 0 ? adapter().port : kDefaultPort);
    m_client->connectToHost();
}

void Z2mAdapter::disconnectFromBroker()
{
    if (!m_client)
        return;
    if (m_client->state() == MqttClient::State::Connected || m_client->state() == MqttClient::State::Connecting)
        m_client->disconnectFromHost();
}

void Z2mAdapter::scheduleReconnect()
{
    if (m_retryIntervalMs <= 0)
        return;
    if (!m_reconnectTimer) {
        m_reconnectTimer = new QTimer(this);
        m_reconnectTimer->setSingleShot(false);
        connect(m_reconnectTimer, &QTimer::timeout, this, [this]() {
            connectToBroker();
        });
    }
    m_reconnectTimer->setInterval(m_retryIntervalMs);
    if (!m_reconnectTimer->isActive())
        m_reconnectTimer->start();
}

void Z2mAdapter::stopReconnectTimer()
{
    if (m_reconnectTimer)
        m_reconnectTimer->stop();
}

void Z2mAdapter::ensureSubscriptions()
{
    if (!m_client || m_client->state() != MqttClient::State::Connected)
        return;
    m_client->subscribe(QStringLiteral("%1/#").arg(m_baseTopic));
}

void Z2mAdapter::handleMqttMessage(const QByteArray &message, const QString &topic)
{
    if (topic.endsWith(QStringLiteral("/bridge/devices"))
        || topic.endsWith(QStringLiteral("/bridge/response/devices"))) {
        const QString payloadText = QString::fromUtf8(message);
        qCInfo(adapterLog) << "Z2M received bridge/devices payload bytes:" << message.size();
        qCInfo(adapterLog).noquote() << "Z2M bridge/devices payload:" << payloadText;
    }
    const QString prefix = m_baseTopic + QLatin1Char('/');
    if (!topic.startsWith(prefix))
        return;
    const QString suffix = topic.mid(prefix.size());

    if (suffix.startsWith(QStringLiteral("bridge/"))) {
        if (suffix == QStringLiteral("bridge/state")) {
            const QString payloadText = QString::fromUtf8(message).trimmed().toLower();
            if (payloadText == QStringLiteral("{\"state\":\"offline\"}")
                || payloadText == QStringLiteral("offline")) {
                qCInfo(adapterLog) << "Z2M bridge/state -> offline";
                m_bridgeOnline = false;
                updateConnectionState();
                return;
            }
            if (payloadText == QStringLiteral("{\"state\":\"online\"}")
                || payloadText == QStringLiteral("online")) {
                qCInfo(adapterLog) << "Z2M bridge/state -> online";
                m_bridgeOnline = true;
                updateConnectionState();
                if (!m_lastSeenRequested) {
                    QJsonObject advanced;
                    advanced.insert(QStringLiteral("last_seen"), QStringLiteral("epoch"));
                    QJsonObject options;
                    options.insert(QStringLiteral("advanced"), advanced);
                    QJsonObject payload;
                    payload.insert(QStringLiteral("options"), options);
                    const QString topic = QStringLiteral("%1/bridge/request/options").arg(m_baseTopic);
                    m_client->publish(topic,
                                      QJsonDocument(payload).toJson(QJsonDocument::Compact));
                    m_lastSeenRequested = true;
                    qCInfo(adapterLog) << "Z2M options requested: last_seen=epoch";
                }
                return;
            }
        }
        if (suffix == QStringLiteral("bridge/health")) {
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(message, &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject()) {
                qCWarning(adapterLog) << "Z2M: failed to parse bridge/health payload:" << err.errorString();
                return;
            }
            QJsonObject metaPatch;
            metaPatch.insert(QStringLiteral("health"), doc.object());
            emit adapterMetaUpdated(metaPatch);
            return;
        }
        if (suffix == QStringLiteral("bridge/response/device/rename")) {
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(message, &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject()) {
                qCWarning(adapterLog) << "Z2M: failed to parse bridge/response/device/rename payload:" << err.errorString();
                return;
            }
            const QJsonObject resp = doc.object();
            const QJsonObject data = resp.value(QStringLiteral("data")).toObject();
            const QString status = resp.value(QStringLiteral("status")).toString().trimmed().toLower();
            const QString from = data.value(QStringLiteral("from")).toString().trimmed();
            const QString to = data.value(QStringLiteral("to")).toString().trimmed();
            if (status == QStringLiteral("ok")) {
                auto it = m_pendingRename.begin();
                while (it != m_pendingRename.end()) {
                    const QString ieee = it.key();
                    const QString currentMqtt = m_mqttByExternal.value(ieee);
                    if ((!to.isEmpty() && it.value().targetName == to)
                        || (!from.isEmpty() && currentMqtt == from)) {
                        CmdResponse response;
                        response.id = it.value().cmdId;
                        response.tsMs = QDateTime::currentMSecsSinceEpoch();
                        response.status = CmdStatus::Success;
                        emit cmdResult(response);
                        it = m_pendingRename.erase(it);
                        const QString mqttId = !to.isEmpty() ? to : currentMqtt;
                        const auto entryIt = m_devices.find(mqttId);
                        if (entryIt != m_devices.end()) {
                            for (auto bindIt = entryIt.value().bindingsByChannel.begin();
                                 bindIt != entryIt.value().bindingsByChannel.end();
                                 ++bindIt) {
                                if (!bindIt.value().isAvailability)
                                    continue;
                                emit channelStateUpdated(entryIt.value().device.id,
                                                         bindIt.value().channelId,
                                                         static_cast<int>(ConnectivityStatus::Connected),
                                                         QDateTime::currentMSecsSinceEpoch());
                                break;
                            }
                        }
                        continue;
                    }
                    ++it;
                }
            }
            return;
        }
        if (suffix == QStringLiteral("bridge/response/options")) {
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(message, &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject()) {
                qCWarning(adapterLog) << "Z2M: failed to parse bridge/response/options payload:" << err.errorString();
                return;
            }
            const QJsonObject resp = doc.object();
            const QString status = resp.value(QStringLiteral("status")).toString().trimmed().toLower();
            const bool restartRequired = resp.value(QStringLiteral("restart_required")).toBool(false);
            qCInfo(adapterLog) << "Z2M options response"
                               << "status" << (status.isEmpty() ? QStringLiteral("unknown") : status)
                               << "restart_required" << restartRequired;
            return;
        }
        if (suffix == QStringLiteral("bridge/response/device/get")) {
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(message, &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject()) {
                qCWarning(adapterLog) << "Z2M: failed to parse bridge/response/device/get payload:" << err.errorString();
                return;
            }
            const QJsonObject resp = doc.object();
            const QJsonObject data = resp.value(QStringLiteral("data")).toObject();
            const QJsonObject deviceObj = data.isEmpty() ? resp : data;
            const QString ieee = deviceObj.value(QStringLiteral("ieee_address")).toString().trimmed();
            const QString friendly = deviceObj.value(QStringLiteral("friendly_name")).toString().trimmed();
            if (!ieee.isEmpty() && m_pendingRename.contains(ieee)) {
                const PendingRename pending = m_pendingRename.take(ieee);
                CmdResponse response;
                response.id = pending.cmdId;
                response.tsMs = QDateTime::currentMSecsSinceEpoch();
                if (!friendly.isEmpty() && friendly == pending.targetName) {
                    response.status = CmdStatus::Success;
                } else {
                    response.status = CmdStatus::Failure;
                    response.error = QStringLiteral("Rename not applied");
                }
                emit cmdResult(response);
            }
            return;
        }
        if (suffix == QStringLiteral("bridge/info")) {
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(message, &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject()) {
                qCWarning(adapterLog) << "Z2M: failed to parse bridge/info payload:" << err.errorString();
                return;
            }
            handleBridgeInfoPayload(doc.object(), QDateTime::currentMSecsSinceEpoch());
            return;
        }
        if (suffix == QStringLiteral("bridge/devices")
            || suffix == QStringLiteral("bridge/response/devices")) {
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(message, &err);
            if (err.error != QJsonParseError::NoError) {
                qCWarning(adapterLog) << "Z2M: failed to parse bridge/devices payload:" << err.errorString();
                return;
            }
            QJsonArray devices;
            if (doc.isArray()) {
                devices = doc.array();
            } else if (doc.isObject()) {
                const QJsonObject obj = doc.object();
                const QJsonValue data = obj.value(QStringLiteral("data"));
                if (data.isArray())
                    devices = data.toArray();
                else if (obj.value(QStringLiteral("status")).toString().trimmed().toLower() == QStringLiteral("ok")
                         && obj.contains(QStringLiteral("result"))
                         && obj.value(QStringLiteral("result")).isArray()) {
                    devices = obj.value(QStringLiteral("result")).toArray();
                }
            }
            if (devices.isEmpty()) {
                qCWarning(adapterLog) << "Z2M: bridge/devices payload has no device array";
                return;
            }
            const bool fullSnapshot = (suffix == QStringLiteral("bridge/devices"));
            handleBridgeDevicesPayload(devices, fullSnapshot);
        }
        return;
    }

    if (suffix.endsWith(QStringLiteral("/availability"))) {
        const int slashIndex = suffix.indexOf(QLatin1Char('/'));
        if (slashIndex <= 0)
            return;
        const QString deviceId = suffix.left(slashIndex);
        qCInfo(adapterLog).noquote()
            << "Z2M availability payload for" << deviceId << ":" << QString::fromUtf8(message).trimmed();
        QString payloadText = QString::fromUtf8(message).trimmed();
        if (payloadText.startsWith(QLatin1Char('{'))) {
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(message, &err);
            if (err.error == QJsonParseError::NoError && doc.isObject()) {
                payloadText = doc.object().value(QStringLiteral("state")).toString(payloadText);
            }
        }
        handleAvailabilityPayload(deviceId, payloadText, QDateTime::currentMSecsSinceEpoch());
        return;
    }

    if (suffix.endsWith(QStringLiteral("/get")) || suffix.endsWith(QStringLiteral("/set"))) {
        return;
    }
    if (suffix.contains(QLatin1Char('/'))) {
        qCInfo(adapterLog).noquote()
            << "Z2M payload ignored for topic" << suffix << ":" << QString::fromUtf8(message).trimmed();
        return;
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(message, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qCInfo(adapterLog).noquote()
            << "Z2M state payload ignored for" << suffix << ":" << QString::fromUtf8(message).trimmed();
        return;
    }
    qCInfo(adapterLog).noquote()
        << "Z2M state payload for" << suffix << ":" << QString::fromUtf8(message).trimmed();
    handleDeviceStatePayload(suffix, doc.object(), QDateTime::currentMSecsSinceEpoch());
}

void Z2mAdapter::handleBridgeDevicesPayload(const QJsonArray &devices, bool fullSnapshot)
{
    qCInfo(adapterLog) << "Z2M bridge/devices payload count:" << devices.size();
    QSet<QString> seen;
    for (const QJsonValue &value : devices) {
        if (!value.isObject())
            continue;
        const QJsonObject obj = value.toObject();
        const QString deviceId = obj.value(QStringLiteral("friendly_name")).toString().trimmed();
        if (deviceId.isEmpty())
            continue;
        const QString ieeeAddress = obj.value(QStringLiteral("ieee_address")).toString().trimmed();
        const bool interviewCompleted = obj.value(QStringLiteral("interview_completed")).toBool(true);
        const bool supported = obj.value(QStringLiteral("supported")).toBool(true);
        if (!interviewCompleted || !supported) {
            const QString existingMqttId = ieeeAddress.isEmpty()
                ? deviceId
                : m_mqttByExternal.value(ieeeAddress, deviceId);
            if (m_devices.contains(existingMqttId)) {
                emit deviceRemoved(m_devices.value(existingMqttId).device.id);
                if (!m_devices.value(existingMqttId).device.id.isEmpty())
                    m_mqttByExternal.remove(m_devices.value(existingMqttId).device.id);
                m_devices.remove(existingMqttId);
            }
            m_pendingStatePayloads.remove(existingMqttId);
            continue;
        }
        const QJsonObject def = obj.value(QStringLiteral("definition")).toObject();
        const QJsonArray exposes = def.value(QStringLiteral("exposes")).toArray();
        qCInfo(adapterLog) << "Z2M device" << deviceId
                           << "exposesCount" << exposes.size()
                           << "type" << obj.value(QStringLiteral("type")).toString();
        seen.insert(deviceId);
        auto availabilityFromValue = [](const QJsonValue &val) -> QString {
            if (val.isString())
                return val.toString().trimmed();
            if (val.isObject())
                return val.toObject().value(QStringLiteral("state")).toString().trimmed();
            return QString();
        };
        auto lastSeenMsFromValue = [](const QJsonValue &val) -> qint64 {
            if (val.isDouble()) {
                const double raw = val.toDouble();
                if (raw > 1000000000000.0)
                    return static_cast<qint64>(raw);
                if (raw > 0.0)
                    return static_cast<qint64>(raw * 1000.0);
                return 0;
            }
            if (val.isString()) {
                const QDateTime parsed = QDateTime::fromString(val.toString(), Qt::ISODate);
                if (parsed.isValid())
                    return parsed.toMSecsSinceEpoch();
            }
            return 0;
        };

        bool renameDetected = false;
        const QString previousMqttId = ieeeAddress.isEmpty()
            ? QString()
            : m_mqttByExternal.value(ieeeAddress);
        if (!previousMqttId.isEmpty() && previousMqttId != deviceId) {
            renameDetected = true;
        }

        Z2mDeviceEntry entry;
        if (!previousMqttId.isEmpty() && m_devices.contains(previousMqttId)) {
            entry = m_devices.value(previousMqttId);
            entry.mqttId = deviceId;
            entry.device.name = deviceId;
            entry.device.meta.insert(QStringLiteral("friendly_name"), deviceId);
            if (renameDetected) {
                m_devices.remove(previousMqttId);
            }
        } else {
            entry = buildDeviceEntry(obj);
        }
        if (renameDetected) {
            auto pendingIt = m_pendingStatePayloads.find(previousMqttId);
            if (pendingIt != m_pendingStatePayloads.end()) {
                m_pendingStatePayloads.insert(deviceId, pendingIt.value());
                m_pendingStatePayloads.erase(pendingIt);
            }
        }

        if (!ieeeAddress.isEmpty()) {
            const auto pendingIt = m_pendingRename.constFind(ieeeAddress);
            if (pendingIt != m_pendingRename.constEnd() && pendingIt.value().targetName == entry.mqttId) {
                CmdResponse response;
                response.id = pendingIt.value().cmdId;
                response.tsMs = QDateTime::currentMSecsSinceEpoch();
                response.status = CmdStatus::Success;
                emit cmdResult(response);
                m_pendingRename.remove(ieeeAddress);
            }
        }
        m_devices.insert(entry.mqttId, entry);
        if (!entry.device.id.isEmpty())
            m_mqttByExternal.insert(entry.device.id, entry.mqttId);
        emit deviceUpdated(entry.device, entry.channels);
        auto pendingPayloadIt = m_pendingStatePayloads.find(entry.mqttId);
        if (pendingPayloadIt != m_pendingStatePayloads.end()) {
            const QJsonObject pendingPayload = pendingPayloadIt.value();
            m_pendingStatePayloads.erase(pendingPayloadIt);
            handleDeviceStatePayload(entry.mqttId, pendingPayload, QDateTime::currentMSecsSinceEpoch());
        }
        if (renameDetected) {
            // Rename does not imply connectivity; avoid forcing Connected here.
        }
        QString availability = availabilityFromValue(obj.value(QStringLiteral("availability")));
        if (availability.isEmpty())
            availability = obj.value(QStringLiteral("availability_state")).toString().trimmed();
        const qint64 lastSeenMs = lastSeenMsFromValue(obj.value(QStringLiteral("last_seen")));
        for (const Z2mChannelBinding &binding : entry.bindingsByChannel) {
            if (!binding.isAvailability)
                continue;
            const QString externalId = entry.device.id;
            QTimer::singleShot(0, this, [this, availability, lastSeenMs, externalId, channelId = binding.channelId]() {
                ConnectivityStatus status = ConnectivityStatus::Unknown;
                QString state = availability.toLower();
                if (state.isEmpty()) {
                    constexpr qint64 kStaleThresholdMs = 5 * 60 * 1000;
                    if (lastSeenMs > 0) {
                        const qint64 ageMs = QDateTime::currentMSecsSinceEpoch() - lastSeenMs;
                        status = ageMs > kStaleThresholdMs
                            ? ConnectivityStatus::Disconnected
                            : ConnectivityStatus::Connected;
                    } else {
                        return;
                    }
                } else if (state == QStringLiteral("online")) {
                    status = ConnectivityStatus::Connected;
                } else if (state == QStringLiteral("offline")) {
                    status = ConnectivityStatus::Disconnected;
                }
                qCInfo(adapterLog) << "Z2M availability default for" << externalId << "->"
                                   << static_cast<int>(status);
                emit channelStateUpdated(externalId, channelId,
                                         static_cast<int>(status),
                                         QDateTime::currentMSecsSinceEpoch());
            });
            break;
        }

        const QString deviceType = obj.value(QStringLiteral("type")).toString();
        if (deviceType.compare(QStringLiteral("Coordinator"), Qt::CaseInsensitive) == 0) {
            m_coordinatorId = entry.device.id;
            if (!m_pendingBridgeInfo.isEmpty()) {
                handleBridgeInfoPayload(m_pendingBridgeInfo, QDateTime::currentMSecsSinceEpoch());
                m_pendingBridgeInfo = QJsonObject();
            }
        }
    }

    if (fullSnapshot) {
        auto it = m_devices.begin();
        while (it != m_devices.end()) {
            if (!seen.contains(it.key())) {
                emit deviceRemoved(it.value().device.id);
                if (!it.value().device.id.isEmpty())
                    m_mqttByExternal.remove(it.value().device.id);
                it = m_devices.erase(it);
                continue;
            }
            ++it;
        }
    }

    if (m_pendingFullSync) {
        qCInfo(adapterLog) << "Z2M full sync completed via bridge/devices payload";
        emit fullSyncCompleted();
        m_pendingFullSync = false;
    }
}

void Z2mAdapter::handleDeviceStatePayload(const QString &deviceId,
                                          const QJsonObject &payload,
                                          qint64 tsMs)
{
    const auto deviceIt = m_devices.find(deviceId);
    if (deviceIt == m_devices.end()) {
        m_pendingStatePayloads.insert(deviceId, payload);
        return;
    }
    Z2mDeviceEntry &entry = deviceIt.value();
    const QString externalId = entry.device.id;
    bool metaChanged = false;
    bool connectivityUpdated = false;
    ConnectivityStatus connectivityStatus = ConnectivityStatus::Unknown;
    if (payload.contains(QStringLiteral("update")) && payload.value(QStringLiteral("update")).isObject()) {
        entry.device.meta.insert(QStringLiteral("update"), payload.value(QStringLiteral("update")).toObject());
        metaChanged = true;
    }
    if (payload.contains(QStringLiteral("last_seen"))) {
        const QJsonValue lastSeenValue = payload.value(QStringLiteral("last_seen"));
        entry.device.meta.insert(QStringLiteral("last_seen"), lastSeenValue);
        metaChanged = true;
        qint64 lastSeenMs = 0;
        if (lastSeenValue.isDouble()) {
            const double raw = lastSeenValue.toDouble();
            if (raw > 1000000000000.0)
                lastSeenMs = static_cast<qint64>(raw);
            else if (raw > 0.0)
                lastSeenMs = static_cast<qint64>(raw * 1000.0);
        } else if (lastSeenValue.isString()) {
            const QDateTime parsed = QDateTime::fromString(lastSeenValue.toString(), Qt::ISODate);
            if (parsed.isValid())
                lastSeenMs = parsed.toMSecsSinceEpoch();
        }
        if (lastSeenMs > 0) {
            constexpr qint64 kStaleThresholdMs = 5 * 60 * 1000;
            const qint64 ageMs = tsMs - lastSeenMs;
            connectivityStatus = ageMs > kStaleThresholdMs
                ? ConnectivityStatus::Disconnected
                : ConnectivityStatus::Connected;
            connectivityUpdated = true;
        }
    }
    if (payload.contains(QStringLiteral("availability"))) {
        const QJsonValue availabilityValue = payload.value(QStringLiteral("availability"));
        QString state = availabilityValue.toString().trimmed().toLower();
        if (state.isEmpty() && availabilityValue.isObject()) {
            state = availabilityValue.toObject().value(QStringLiteral("state")).toString().trimmed().toLower();
        }
        if (state == QStringLiteral("online")) {
            connectivityStatus = ConnectivityStatus::Connected;
            connectivityUpdated = true;
        } else if (state == QStringLiteral("offline")) {
            connectivityStatus = ConnectivityStatus::Disconnected;
            connectivityUpdated = true;
        }
    }
    if (!connectivityUpdated && !payload.isEmpty()) {
        connectivityStatus = ConnectivityStatus::Connected;
        connectivityUpdated = true;
    }
    if (metaChanged) {
        emit deviceUpdated(entry.device, entry.channels);
    }

    for (auto it = entry.bindingsByChannel.begin(); it != entry.bindingsByChannel.end(); ++it) {
        const Z2mChannelBinding &binding = it.value();
        if (!binding.isAvailability)
            continue;
        if (connectivityUpdated) {
            emit channelStateUpdated(externalId, binding.channelId,
                                     static_cast<int>(connectivityStatus), tsMs);
        }
        break;
    }

    for (auto it = entry.bindingsByChannel.begin(); it != entry.bindingsByChannel.end(); ++it) {
        const Z2mChannelBinding &binding = it.value();
        if (binding.isAvailability)
            continue;
        if (binding.channelId == QStringLiteral("device_software_update")) {
            const QJsonValue updateValue = payload.value(QStringLiteral("update"));
            if (updateValue.isObject()) {
                const QJsonObject updateObj = updateValue.toObject();
                const QString status = updateObj.value(QStringLiteral("state")).toString();
                const QString currentVersion = updateObj.contains(QStringLiteral("installed_version"))
                    ? QString::number(updateObj.value(QStringLiteral("installed_version")).toDouble(), 'f', 0)
                    : QString();
                const QString targetVersion = updateObj.contains(QStringLiteral("latest_version"))
                    ? QString::number(updateObj.value(QStringLiteral("latest_version")).toDouble(), 'f', 0)
                    : QString();
                QJsonObject updatePayload;
                if (!status.isEmpty())
                    updatePayload.insert(QStringLiteral("status"), status);
                if (!currentVersion.isEmpty())
                    updatePayload.insert(QStringLiteral("currentVersion"), currentVersion);
                if (!targetVersion.isEmpty())
                    updatePayload.insert(QStringLiteral("targetVersion"), targetVersion);
                emit channelStateUpdated(externalId, binding.channelId, updatePayload, tsMs);
            }
            continue;
        }
        const QJsonValue value = payload.value(binding.property);
        if (value.isUndefined())
            continue;

        QVariant outValue;
        switch (binding.kind) {
        case ChannelKind::PowerOnOff: {
            if (value.isBool()) {
                outValue = value.toBool();
            } else if (value.isString()) {
                const QString state = value.toString();
                if (!binding.valueOn.isEmpty() || !binding.valueOff.isEmpty()) {
                    outValue = state.compare(binding.valueOn, Qt::CaseInsensitive) == 0;
                } else {
                    outValue = state.compare(QStringLiteral("ON"), Qt::CaseInsensitive) == 0;
                }
            } else if (value.isDouble()) {
                outValue = value.toDouble() != 0.0;
            }
            break;
        }
        case ChannelKind::Brightness: {
            const double raw = value.toDouble();
            outValue = scaleToPercent(raw, binding.rawMin, binding.rawMax);
            break;
        }
        case ChannelKind::ColorTemperature: {
            outValue = value.toDouble();
            break;
        }
        case ChannelKind::ColorRGB: {
            if (!value.isObject())
                break;
            const QJsonObject colorObj = value.toObject();
            if (binding.colorMode == QStringLiteral("xy")) {
                const double x = colorObj.value(QStringLiteral("x")).toDouble();
                const double y = colorObj.value(QStringLiteral("y")).toDouble();
                phicore::Color c = phicore::colorFromXy(x, y, 1.0);
                outValue = QVariant::fromValue(c);
            } else if (binding.colorMode == QStringLiteral("hs")) {
                const double h = colorObj.value(QStringLiteral("hue")).toDouble(colorObj.value(QStringLiteral("h")).toDouble());
                const double s = colorObj.value(QStringLiteral("saturation")).toDouble(colorObj.value(QStringLiteral("s")).toDouble());
                phicore::Color c = phicore::hsvToColor(h, s / 100.0, 1.0);
                outValue = QVariant::fromValue(c);
            }
            break;
        }
        case ChannelKind::Temperature:
        case ChannelKind::Humidity:
        case ChannelKind::Illuminance:
        case ChannelKind::CO2:
        case ChannelKind::Power:
        case ChannelKind::Voltage:
        case ChannelKind::Current:
        case ChannelKind::Energy: {
            const double raw = value.toDouble();
            outValue = raw * binding.valueScale;
            break;
        }
        case ChannelKind::AmbientLightLevel: {
            if (value.isString()) {
                const QString raw = value.toString();
                if (!binding.enumRawToValue.isEmpty()) {
                    const auto it = binding.enumRawToValue.constFind(raw);
                    if (it != binding.enumRawToValue.constEnd()) {
                        outValue = it.value();
                        break;
                    }
                }
                outValue = raw;
            } else if (value.isDouble()) {
                outValue = value.toInt();
            }
            break;
        }
        case ChannelKind::Duration: {
            outValue = value.toInt();
            break;
        }
        case ChannelKind::SignalStrength: {
            outValue = value.toInt();
            break;
        }
        case ChannelKind::LinkQuality: {
            const double raw = value.toDouble();
            outValue = qBound(0.0, raw * binding.valueScale, 100.0);
            break;
        }
        case ChannelKind::Motion: {
            if (value.isBool()) {
                outValue = value.toBool();
            } else if (value.isString()) {
                const QString state = value.toString().toLower();
                outValue = (state == QStringLiteral("true")
                            || state == QStringLiteral("on")
                            || state == QStringLiteral("occupied"));
            } else if (value.isDouble()) {
                outValue = value.toDouble() != 0.0;
            }
            break;
        }
        case ChannelKind::Battery: {
            outValue = value.toInt();
            break;
        }
        case ChannelKind::ButtonEvent: {
            if (value.isString()) {
                const ButtonEventCode code = actionToButtonEvent(value.toString());
                outValue = static_cast<int>(code);
            }
            break;
        }
        case ChannelKind::Unknown: {
            if (binding.dataType == ChannelDataType::Bool) {
                if (value.isBool())
                    outValue = value.toBool();
                else if (value.isDouble())
                    outValue = value.toDouble() != 0.0;
                else if (value.isString())
                    outValue = value.toString().toLower() == QStringLiteral("true");
            } else if (binding.dataType == ChannelDataType::Int) {
                outValue = value.toInt();
            } else if (binding.dataType == ChannelDataType::Float) {
                outValue = value.toDouble() * binding.valueScale;
            } else if (binding.dataType == ChannelDataType::Enum) {
                if (value.isString()) {
                    const QString raw = value.toString();
                    if (!binding.enumRawToValue.isEmpty()) {
                        const auto it = binding.enumRawToValue.constFind(raw);
                        if (it != binding.enumRawToValue.constEnd()) {
                            outValue = it.value();
                            break;
                        }
                    }
                    outValue = raw;
                } else if (value.isDouble()) {
                    outValue = value.toInt();
                }
            }
            break;
        }
        default:
            break;
        }

        if (!outValue.isValid())
            continue;
        qCInfo(adapterLog).noquote()
            << "Z2M channel update" << externalId
            << binding.channelId << "value" << outValue.toString();
        emit channelStateUpdated(externalId, binding.channelId, outValue, tsMs);
    }
}

void Z2mAdapter::handleAvailabilityPayload(const QString &deviceId,
                                           const QString &payload,
                                           qint64 tsMs)
{
    const auto deviceIt = m_devices.find(deviceId);
    if (deviceIt == m_devices.end())
        return;
    const Z2mDeviceEntry &entry = deviceIt.value();
    const QString externalId = entry.device.id;
    for (auto it = entry.bindingsByChannel.begin(); it != entry.bindingsByChannel.end(); ++it) {
        const Z2mChannelBinding &binding = it.value();
        if (!binding.isAvailability)
            continue;
        const QString state = payload.trimmed().toLower();
        ConnectivityStatus status = ConnectivityStatus::Unknown;
        if (state == QStringLiteral("online"))
            status = ConnectivityStatus::Connected;
        else if (state == QStringLiteral("offline"))
            status = ConnectivityStatus::Disconnected;
        emit channelStateUpdated(externalId, binding.channelId,
                                 static_cast<int>(status), tsMs);
        break;
    }
}

void Z2mAdapter::handleBridgeInfoPayload(const QJsonObject &payload, qint64 tsMs)
{
    if (m_coordinatorId.isEmpty()) {
        m_pendingBridgeInfo = payload;
        return;
    }

    const QString coordinatorMqttId = m_mqttByExternal.value(m_coordinatorId, m_coordinatorId);
    auto deviceIt = m_devices.find(coordinatorMqttId);
    if (deviceIt == m_devices.end()) {
        m_pendingBridgeInfo = payload;
        return;
    }

    Z2mDeviceEntry &entry = deviceIt.value();
    Device updated = entry.device;
    const QJsonObject coordinator = payload.value(QStringLiteral("coordinator")).toObject();
    const QJsonObject coordinatorMeta = coordinator.value(QStringLiteral("meta")).toObject();

    const QString manufacturer = coordinatorMeta.value(QStringLiteral("manufacturer")).toString();
    if (!manufacturer.isEmpty())
        updated.manufacturer = manufacturer;

    const QString model = coordinatorMeta.value(QStringLiteral("model")).toString();
    if (!model.isEmpty())
        updated.model = model;

    QString firmware = coordinatorMeta.value(QStringLiteral("revision")).toString();
    if (firmware.isEmpty())
        firmware = coordinatorMeta.value(QStringLiteral("firmware")).toString();
    if (firmware.isEmpty())
        firmware = coordinatorMeta.value(QStringLiteral("version")).toString();
    if (!firmware.isEmpty())
        updated.firmware = firmware;

    updated.deviceClass = DeviceClass::Gateway;

    QJsonObject meta = updated.meta;
    meta.insert(QStringLiteral("coordinator"), coordinator);
    const QJsonObject config = payload.value(QStringLiteral("config")).toObject();
    const QJsonObject serial = config.value(QStringLiteral("serial")).toObject();
    const QString serialPort = serial.value(QStringLiteral("port")).toString().trimmed();
    if (!serialPort.isEmpty())
        meta.insert(QStringLiteral("serial_port"), serialPort);
    const QString serialAdapter = serial.value(QStringLiteral("adapter")).toString().trimmed();
    if (!serialAdapter.isEmpty())
        meta.insert(QStringLiteral("serial_adapter"), serialAdapter);
    const QString z2mVersion = payload.value(QStringLiteral("version")).toString();
    const QString z2mCommit = payload.value(QStringLiteral("commit")).toString();
    updated.meta = meta;
    entry.device = updated;
    emit deviceUpdated(entry.device, entry.channels);

    {
        QJsonObject metaPatch;
        metaPatch.insert(QStringLiteral("bridge_info"), payload);
        if (!z2mVersion.isEmpty())
            metaPatch.insert(QStringLiteral("z2m_version"), z2mVersion);
        if (!z2mCommit.isEmpty())
            metaPatch.insert(QStringLiteral("z2m_commit"), z2mCommit);
        if (payload.contains(QStringLiteral("permit_join")))
            metaPatch.insert(QStringLiteral("permit_join"), payload.value(QStringLiteral("permit_join")));
        if (payload.contains(QStringLiteral("log_level")))
            metaPatch.insert(QStringLiteral("log_level"), payload.value(QStringLiteral("log_level")));
        emit adapterMetaUpdated(metaPatch);
    }

    if (m_mqttConnected && m_bridgeOnline) {
        for (auto it = entry.bindingsByChannel.begin(); it != entry.bindingsByChannel.end(); ++it) {
            if (!it.value().isAvailability)
                continue;
            emit channelStateUpdated(m_coordinatorId, it.value().channelId,
                                     static_cast<int>(ConnectivityStatus::Connected), tsMs);
            break;
        }
    }

    const QJsonValue updateValue = payload.value(QStringLiteral("update"));
    if (updateValue.isObject()) {
        QJsonObject updatePayload;
        const QJsonObject updateObj = updateValue.toObject();
        const QString status = updateObj.value(QStringLiteral("state")).toString();
        if (!status.isEmpty())
            updatePayload.insert(QStringLiteral("status"), status);
        const QString targetVersion = updateObj.value(QStringLiteral("version")).toString();
        if (!targetVersion.isEmpty())
            updatePayload.insert(QStringLiteral("targetVersion"), targetVersion);
        const auto updateIt = entry.bindingsByChannel.constFind(QStringLiteral("device_software_update"));
        if (updateIt != entry.bindingsByChannel.constEnd()) {
            emit channelStateUpdated(m_coordinatorId, updateIt.value().channelId, updatePayload, tsMs);
        }
    }
}

Z2mAdapter::Z2mDeviceEntry Z2mAdapter::buildDeviceEntry(const QJsonObject &obj) const
{
    Z2mDeviceEntry entry;
    const QString mqttId = obj.value(QStringLiteral("friendly_name")).toString().trimmed();
    entry.mqttId = mqttId;
    entry.device.name = mqttId;
    entry.device.flags = DeviceFlag::DeviceFlagWireless;

    const QString powerSource = obj.value(QStringLiteral("power_source")).toString();
    if (powerSource.compare(QStringLiteral("Battery"), Qt::CaseInsensitive) == 0)
        entry.device.flags |= DeviceFlag::DeviceFlagBattery;

    const QJsonObject def = obj.value(QStringLiteral("definition")).toObject();
    if (!def.isEmpty()) {
        entry.device.model = def.value(QStringLiteral("model")).toString();
        entry.device.manufacturer = def.value(QStringLiteral("vendor")).toString();
        entry.device.meta.insert(QStringLiteral("description"), def.value(QStringLiteral("description")).toString());
        const QString model = def.value(QStringLiteral("model")).toString().trimmed();
        if (!model.isEmpty()) {
            entry.device.meta.insert(QStringLiteral("iconUrl"),
                                     QStringLiteral("https://www.zigbee2mqtt.io/images/devices/%1.png").arg(model));
        }
    }
    entry.device.meta.insert(QStringLiteral("friendly_name"), mqttId);
    const QString ieeeAddress = obj.value(QStringLiteral("ieee_address")).toString().trimmed();
    if (!ieeeAddress.isEmpty())
        entry.device.meta.insert(QStringLiteral("ieee_address"), ieeeAddress);
    const QString deviceType = obj.value(QStringLiteral("type")).toString();
    entry.device.meta.insert(QStringLiteral("type"), deviceType);
    const QString modelId = obj.value(QStringLiteral("model_id")).toString();
    if (!modelId.isEmpty())
        entry.device.meta.insert(QStringLiteral("model_id"), modelId);
    if (!powerSource.isEmpty())
        entry.device.meta.insert(QStringLiteral("power_source"), powerSource);
    const QString manufacturer = obj.value(QStringLiteral("manufacturer")).toString();
    if (!manufacturer.isEmpty())
        entry.device.meta.insert(QStringLiteral("manufacturer"), manufacturer);
    const QString softwareBuild = obj.value(QStringLiteral("software_build_id")).toString();
    if (!softwareBuild.isEmpty())
        entry.device.meta.insert(QStringLiteral("software_build_id"), softwareBuild);
    const QString dateCode = obj.value(QStringLiteral("date_code")).toString();
    if (!dateCode.isEmpty())
        entry.device.meta.insert(QStringLiteral("date_code"), dateCode);
    entry.device.id = !ieeeAddress.isEmpty() ? ieeeAddress : mqttId;
    if (deviceType.compare(QStringLiteral("Coordinator"), Qt::CaseInsensitive) == 0) {
        entry.device.deviceClass = DeviceClass::Gateway;
        entry.device.meta.insert(QStringLiteral("coordinator"), true);
    }
    if (obj.contains(QStringLiteral("interview_completed")))
        entry.device.meta.insert(QStringLiteral("interview_completed"), obj.value(QStringLiteral("interview_completed")));
    if (obj.contains(QStringLiteral("interviewing")))
        entry.device.meta.insert(QStringLiteral("interviewing"), obj.value(QStringLiteral("interviewing")));
    if (obj.contains(QStringLiteral("supported")))
        entry.device.meta.insert(QStringLiteral("supported"), obj.value(QStringLiteral("supported")));
    if (obj.contains(QStringLiteral("disabled")))
        entry.device.meta.insert(QStringLiteral("disabled"), obj.value(QStringLiteral("disabled")));
    const QJsonValue availabilityValue = obj.value(QStringLiteral("availability"));
    QString availabilityState = availabilityValue.toString().trimmed();
    if (availabilityState.isEmpty() && availabilityValue.isObject())
        availabilityState = availabilityValue.toObject().value(QStringLiteral("state")).toString().trimmed();
    if (!availabilityState.isEmpty())
        entry.device.meta.insert(QStringLiteral("availability"), availabilityState);

    QList<QJsonObject> exposes;
    if (def.contains(QStringLiteral("exposes"))) {
        collectExposeEntries(def.value(QStringLiteral("exposes")), exposes);
    }

    entry.device.deviceClass = inferDeviceClass(exposes);

    for (const QJsonObject &expose : exposes) {
        addChannelFromExpose(expose, entry);
    }

    Channel availability;
    availability.id = QStringLiteral("connectivity");
    availability.name = QStringLiteral("Connectivity");
    availability.kind = ChannelKind::ConnectivityStatus;
    availability.dataType = ChannelDataType::Enum;
    availability.flags = ChannelFlag::ChannelFlagReadable
        | ChannelFlag::ChannelFlagReportable
        | ChannelFlag::ChannelFlagRetained;
    entry.channels.push_back(availability);

    Z2mChannelBinding availabilityBinding;
    availabilityBinding.channelId = availability.id;
    availabilityBinding.property = QStringLiteral("availability");
    availabilityBinding.kind = availability.kind;
    availabilityBinding.dataType = availability.dataType;
    availabilityBinding.flags = availability.flags;
    availabilityBinding.isAvailability = true;
    entry.bindingsByChannel.insert(availability.id, availabilityBinding);
    entry.channelByProperty.insert(availabilityBinding.property, availability.id);

    Channel updateChannel;
    updateChannel.id = QStringLiteral("device_software_update");
    updateChannel.name = QStringLiteral("Firmware Update");
    updateChannel.kind = ChannelKind::DeviceSoftwareUpdate;
    updateChannel.dataType = ChannelDataType::Enum;
    updateChannel.flags = ChannelFlagDefaultRead;
    entry.channels.push_back(updateChannel);

    Z2mChannelBinding updateBinding;
    updateBinding.channelId = updateChannel.id;
    updateBinding.property = QStringLiteral("update");
    updateBinding.kind = updateChannel.kind;
    updateBinding.dataType = updateChannel.dataType;
    updateBinding.flags = updateChannel.flags;
    entry.bindingsByChannel.insert(updateChannel.id, updateBinding);

    for (const Channel &channel : entry.channels) {
        qCInfo(adapterLog).noquote()
            << "Z2M channel defined" << entry.device.id
            << channel.id << "kind" << static_cast<int>(channel.kind)
            << "dataType" << static_cast<int>(channel.dataType);
    }

    return entry;
}

void Z2mAdapter::collectExposeEntries(const QJsonValue &value, QList<QJsonObject> &out) const
{
    if (value.isArray()) {
        const QJsonArray arr = value.toArray();
        for (const QJsonValue &entry : arr)
            collectExposeEntries(entry, out);
        return;
    }
    if (!value.isObject())
        return;
    const QJsonObject obj = value.toObject();
    const QString property = obj.value(QStringLiteral("property")).toString().trimmed();
    const QString type = obj.value(QStringLiteral("type")).toString().trimmed();
    if (!property.isEmpty()) {
        out.push_back(obj);
        if (property == QStringLiteral("color") && type == QStringLiteral("composite"))
            return;
    }
    const QJsonValue features = obj.value(QStringLiteral("features"));
    if (features.isArray()) {
        for (const QJsonValue &feature : features.toArray())
            collectExposeEntries(feature, out);
    }
}

void Z2mAdapter::addChannelFromExpose(const QJsonObject &expose, Z2mDeviceEntry &entry) const
{
    const QString property = expose.value(QStringLiteral("property")).toString().trimmed();
    if (property.isEmpty())
        return;
    const QString propLower = property.toLower();
    const bool isMinMaxHelper =
        propLower == QStringLiteral("min")
        || propLower == QStringLiteral("max")
        || propLower.startsWith(QStringLiteral("min_"))
        || propLower.startsWith(QStringLiteral("max_"))
        || propLower.endsWith(QStringLiteral("_min"))
        || propLower.endsWith(QStringLiteral("_max"));
    if (isMinMaxHelper)
        return;

    QString endpoint;
    if (expose.value(QStringLiteral("endpoint")).isString())
        endpoint = expose.value(QStringLiteral("endpoint")).toString().trimmed();
    else if (expose.value(QStringLiteral("endpoint")).isDouble())
        endpoint = QString::number(expose.value(QStringLiteral("endpoint")).toInt());

    QString channelId = property;
    if (!endpoint.isEmpty())
        channelId = property + QLatin1Char('_') + endpoint;
    if (entry.bindingsByChannel.contains(channelId))
        return;

    struct Mapping {
        ChannelKind kind;
        ChannelDataType dataType;
        QString unit;
        bool scalePercent;
    };
    static const QHash<QString, Mapping> kMappings = {
        { QStringLiteral("state"), { ChannelKind::PowerOnOff, ChannelDataType::Bool, QString(), false } },
        { QStringLiteral("brightness"), { ChannelKind::Brightness, ChannelDataType::Float, QStringLiteral("%"), true } },
        { QStringLiteral("color_temp"), { ChannelKind::ColorTemperature, ChannelDataType::Float, QStringLiteral("mired"), false } },
        { QStringLiteral("color"), { ChannelKind::ColorRGB, ChannelDataType::Color, QString(), false } },
        { QStringLiteral("temperature"), { ChannelKind::Temperature, ChannelDataType::Float, QStringLiteral("C"), false } },
        { QStringLiteral("humidity"), { ChannelKind::Humidity, ChannelDataType::Float, QStringLiteral("%"), false } },
        { QStringLiteral("illuminance"), { ChannelKind::Illuminance, ChannelDataType::Int, QStringLiteral("lx"), false } },
        { QStringLiteral("illumination"), { ChannelKind::AmbientLightLevel, ChannelDataType::Enum, QString(), false } },
        { QStringLiteral("occupancy"), { ChannelKind::Motion, ChannelDataType::Bool, QString(), false } },
        { QStringLiteral("motion"), { ChannelKind::Motion, ChannelDataType::Bool, QString(), false } },
        { QStringLiteral("battery"), { ChannelKind::Battery, ChannelDataType::Int, QStringLiteral("%"), false } },
        { QStringLiteral("battery_low"), { ChannelKind::Unknown, ChannelDataType::Bool, QString(), false } },
        { QStringLiteral("linkquality"), { ChannelKind::LinkQuality, ChannelDataType::Float, QStringLiteral("%"), false } },
        { QStringLiteral("keep_time"), { ChannelKind::Duration, ChannelDataType::Int, QStringLiteral("s"), false } },
        { QStringLiteral("tamper"), { ChannelKind::Tamper, ChannelDataType::Bool, QString(), false } },
        { QStringLiteral("power"), { ChannelKind::Power, ChannelDataType::Float, QStringLiteral("W"), false } },
        { QStringLiteral("voltage"), { ChannelKind::Voltage, ChannelDataType::Float, QStringLiteral("V"), false } },
        { QStringLiteral("current"), { ChannelKind::Current, ChannelDataType::Float, QStringLiteral("A"), false } },
        { QStringLiteral("energy"), { ChannelKind::Energy, ChannelDataType::Float, QStringLiteral("kWh"), false } },
        { QStringLiteral("co2"), { ChannelKind::CO2, ChannelDataType::Float, QStringLiteral("ppm"), false } },
        { QStringLiteral("action"), { ChannelKind::ButtonEvent, ChannelDataType::Int, QString(), false } }
    };

    const auto mapIt = kMappings.find(property);
    const QString exposeType = expose.value(QStringLiteral("type")).toString().trimmed();
    const bool isEnum = exposeType == QLatin1String("enum");
    const bool isBinary = exposeType == QLatin1String("binary");
    const bool isNumeric = exposeType == QLatin1String("numeric");

    if (mapIt == kMappings.end() && !(isEnum || isBinary || isNumeric))
        return;

    Channel channel;
    channel.id = channelId;
    channel.name = labelFromProperty(property, expose.value(QStringLiteral("label")).toString());
    if (mapIt != kMappings.end()) {
        channel.kind = mapIt->kind;
        channel.dataType = mapIt->dataType;
        channel.unit = mapIt->unit;
    } else if (isEnum) {
        channel.kind = ChannelKind::Unknown;
        channel.dataType = ChannelDataType::Enum;
    } else if (isBinary) {
        channel.kind = ChannelKind::Unknown;
        channel.dataType = ChannelDataType::Bool;
    } else {
        channel.kind = ChannelKind::Unknown;
        channel.dataType = ChannelDataType::Float;
    }
    if (isEnum) {
        channel.dataType = ChannelDataType::Enum;
    }

    const int access = expose.value(QStringLiteral("access")).toInt(kAccessState);
    channel.flags = flagsFromAccess(access);

    if (entry.device.deviceClass == DeviceClass::Sensor) {
        const auto isSensorMeasurementKind = [](ChannelKind kind) {
            switch (kind) {
            case ChannelKind::Temperature:
            case ChannelKind::Humidity:
            case ChannelKind::Illuminance:
            case ChannelKind::CO2:
            case ChannelKind::Power:
            case ChannelKind::Voltage:
            case ChannelKind::Current:
            case ChannelKind::Energy:
            case ChannelKind::Battery:
            case ChannelKind::Motion:
            case ChannelKind::Tamper:
            case ChannelKind::AmbientLightLevel:
            case ChannelKind::LinkQuality:
            case ChannelKind::SignalStrength:
            case ChannelKind::ButtonEvent:
                return true;
            default:
                return false;
            }
        };
        const QStringList writableSensorConfigTokens = {
            QStringLiteral("calibration"),
            QStringLiteral("sensitivity"),
            QStringLiteral("threshold"),
            QStringLiteral("alarm"),
            QStringLiteral("keep_time"),
            QStringLiteral("interval"),
            QStringLiteral("unit"),
            QStringLiteral("mode")
        };
        bool sensorConfigWritable = false;
        for (const QString &token : writableSensorConfigTokens) {
            if (propLower.contains(token)) {
                sensorConfigWritable = true;
                break;
            }
        }
        if (isSensorMeasurementKind(channel.kind))
            channel.flags = forceReadOnly(channel.flags);
        if (channel.kind == ChannelKind::Unknown && !sensorConfigWritable)
            channel.flags = forceReadOnly(channel.flags);
    }

    double rawMin = expose.value(QStringLiteral("value_min")).toDouble(0.0);
    double rawMax = expose.value(QStringLiteral("value_max")).toDouble(0.0);
    const double rawStep = expose.value(QStringLiteral("value_step")).toDouble(1.0);

    if (channel.kind == ChannelKind::Brightness) {
        if (rawMax <= rawMin) {
            rawMin = 0.0;
            rawMax = 254.0;
        }
        channel.minValue = 0.0;
        channel.maxValue = 100.0;
        if (rawMax > rawMin && rawStep > 0.0) {
            channel.stepValue = (rawStep / (rawMax - rawMin)) * 100.0;
        } else {
            channel.stepValue = 1.0;
        }
    } else if (channel.kind == ChannelKind::LinkQuality) {
        channel.minValue = 0.0;
        channel.maxValue = 100.0;
        channel.stepValue = 1.0;
    } else if (channel.kind == ChannelKind::Battery && channel.dataType == ChannelDataType::Int) {
        channel.minValue = 0.0;
        channel.maxValue = rawMax > 0.0 ? rawMax : 100.0;
        channel.stepValue = rawStep > 0.0 ? rawStep : 1.0;
    } else if (channel.dataType == ChannelDataType::Float || channel.dataType == ChannelDataType::Int) {
        channel.minValue = rawMin;
        channel.maxValue = rawMax;
        channel.stepValue = rawStep;
    }

    QHash<QString, int> enumRawToValue;
    QHash<int, QString> enumValueToRaw;
    if (isEnum) {
        const QJsonArray values = expose.value(QStringLiteral("values")).toArray();
        QString enumName;
        if (property == QStringLiteral("device_mode")) {
            enumName = QStringLiteral("RockerMode");
        } else if (property == QStringLiteral("motion_sensitivity")
                   || property == QStringLiteral("sensitivity")) {
            enumName = QStringLiteral("SensitivityLevel");
        }

        QStringList rawKeys;
        rawKeys.reserve(values.size());
        QHash<QString, int> normalizedMap;
        bool allNumericEnumValues = !values.isEmpty();
        for (const QJsonValue &val : values) {
            const QString key = val.isString()
                ? val.toString()
                : val.isDouble() ? QString::number(val.toInt()) : QString();
            if (key.isEmpty())
                continue;
            rawKeys.push_back(key);
            bool numericOk = false;
            key.toInt(&numericOk);
            if (!numericOk)
                allNumericEnumValues = false;
            if (!enumName.isEmpty()) {
                if (isKnownEnumName(enumName, "RockerMode")) {
                    if (const auto mapped = mapRockerMode(key)) {
                        normalizedMap.insert(key, *mapped);
                    }
                } else if (isKnownEnumName(enumName, "SensitivityLevel")) {
                    if (const auto mapped = mapSensitivityLevel(key)) {
                        normalizedMap.insert(key, *mapped);
                    }
                }
            }
        }

        QJsonObject enumMapObj;
        for (auto it = normalizedMap.begin(); it != normalizedMap.end(); ++it) {
            enumMapObj.insert(it.key(), it.value());
        }
        QHash<QString, int> fallbackMap;
        if (allNumericEnumValues) {
            for (const QString &key : rawKeys) {
                bool numericOk = false;
                const int numeric = key.toInt(&numericOk);
                if (numericOk)
                    fallbackMap.insert(key, numeric);
            }
        } else {
            fallbackMap = buildStableEnumMap(rawKeys, enumMapObj);
        }
        if (!enumName.isEmpty())
            channel.meta.insert(QStringLiteral("enumName"), enumName);
        if (!fallbackMap.isEmpty()) {
            QJsonObject stableMapObj;
            for (auto it = fallbackMap.begin(); it != fallbackMap.end(); ++it) {
                stableMapObj.insert(it.key(), it.value());
            }
            channel.meta.insert(QStringLiteral("enumMap"), stableMapObj);
        }

        for (const QString &key : rawKeys) {
            const int mappedValue = fallbackMap.value(key, 0);
            if (mappedValue == 0)
                continue;
            AdapterConfigOption opt;
            opt.value = QString::number(mappedValue);
            QString label;
            if (!enumName.isEmpty()) {
                label = enumLabelFor(enumName, mappedValue);
            }
            if (label.isEmpty())
                label = key;
            opt.label = label;
            channel.choices.push_back(opt);
            enumRawToValue.insert(key, mappedValue);
            if (!enumValueToRaw.contains(mappedValue))
                enumValueToRaw.insert(mappedValue, key);
        }
    }

    const QString exposeUnit = expose.value(QStringLiteral("unit")).toString().trimmed();
    if (channel.unit.isEmpty() && !exposeUnit.isEmpty())
        channel.unit = exposeUnit;
    if (channel.kind == ChannelKind::Voltage && exposeUnit == QLatin1String("mV")) {
        channel.unit = QStringLiteral("V");
        channel.minValue = channel.minValue / 1000.0;
        channel.maxValue = channel.maxValue / 1000.0;
        if (channel.stepValue > 0.0)
            channel.stepValue = channel.stepValue / 1000.0;
    }

    entry.channels.push_back(channel);

    Z2mChannelBinding binding;
    binding.channelId = channelId;
    binding.property = property;
    binding.kind = channel.kind;
    binding.dataType = channel.dataType;
    binding.flags = channel.flags;
    binding.unit = channel.unit;
    binding.rawMin = rawMin;
    binding.rawMax = rawMax;
    binding.rawStep = rawStep;
    binding.scalePercent = mapIt != kMappings.end() ? mapIt->scalePercent : false;
    binding.valueScale = 1.0;
    binding.endpoint = endpoint;
    if (!enumRawToValue.isEmpty())
        binding.enumRawToValue = enumRawToValue;
    if (!enumValueToRaw.isEmpty())
        binding.enumValueToRaw = enumValueToRaw;
    if (binding.kind == ChannelKind::PowerOnOff) {
        binding.valueOn = expose.value(QStringLiteral("value_on")).toString();
        binding.valueOff = expose.value(QStringLiteral("value_off")).toString();
    }
    if (binding.kind == ChannelKind::Voltage && exposeUnit == QLatin1String("mV")) {
        binding.valueScale = 0.001;
        binding.unit = QStringLiteral("V");
    }
    if (binding.kind == ChannelKind::LinkQuality) {
        binding.valueScale = 100.0 / 255.0;
        binding.unit = QStringLiteral("%");
    }
    if (binding.kind == ChannelKind::ColorRGB) {
        const QJsonArray features = expose.value(QStringLiteral("features")).toArray();
        bool hasX = false;
        bool hasY = false;
        bool hasHue = false;
        bool hasSat = false;
        for (const QJsonValue &featureVal : features) {
            if (!featureVal.isObject())
                continue;
            const QString fprop = featureVal.toObject().value(QStringLiteral("property")).toString().trimmed();
            if (fprop == QStringLiteral("x"))
                hasX = true;
            else if (fprop == QStringLiteral("y"))
                hasY = true;
            else if (fprop == QStringLiteral("hue") || fprop == QStringLiteral("h"))
                hasHue = true;
            else if (fprop == QStringLiteral("saturation") || fprop == QStringLiteral("s"))
                hasSat = true;
        }
        if (hasX && hasY)
            binding.colorMode = QStringLiteral("xy");
        else if (hasHue && hasSat)
            binding.colorMode = QStringLiteral("hs");
        else
            binding.colorMode = QStringLiteral("xy");
    }

    entry.bindingsByChannel.insert(channelId, binding);
    entry.channelByProperty.insert(property, channelId);
}

ChannelFlags Z2mAdapter::flagsFromAccess(int access) const
{
    ChannelFlags flags = ChannelFlag::ChannelFlagNone;
    if (access & kAccessState) {
        flags |= ChannelFlag::ChannelFlagReadable
            | ChannelFlag::ChannelFlagReportable
            | ChannelFlag::ChannelFlagRetained;
    }
    if (access & kAccessSet)
        flags |= ChannelFlag::ChannelFlagWritable;
    if (flags == ChannelFlag::ChannelFlagNone)
        flags = ChannelFlagDefaultRead;
    return flags;
}

QString Z2mAdapter::labelFromProperty(const QString &property, const QString &fallback) const
{
    if (!fallback.trimmed().isEmpty())
        return fallback.trimmed();
    static const QHash<QString, QString> kLabels = {
        { QStringLiteral("color_temp"), QStringLiteral("Color Temperature") },
        { QStringLiteral("co2"), QStringLiteral("CO2") }
    };
    const auto it = kLabels.find(property);
    if (it != kLabels.end())
        return it.value();
    QStringList parts = property.split(QLatin1Char('_'), Qt::SkipEmptyParts);
    for (QString &part : parts) {
        if (part.isEmpty())
            continue;
        part[0] = part[0].toUpper();
    }
    return parts.join(QLatin1Char(' '));
}

DeviceClass Z2mAdapter::inferDeviceClass(const QList<QJsonObject> &exposes) const
{
    bool hasLight = false;
    bool hasSwitch = false;
    bool hasSensor = false;
    bool hasButton = false;
    for (const QJsonObject &expose : exposes) {
        const QString property = expose.value(QStringLiteral("property")).toString();
        if (property == QStringLiteral("brightness")
            || property == QStringLiteral("color_temp")
            || property == QStringLiteral("color")) {
            hasLight = true;
        } else if (property == QStringLiteral("state")) {
            hasSwitch = true;
        } else if (property == QStringLiteral("action")) {
            hasButton = true;
        } else if (property == QStringLiteral("temperature")
                   || property == QStringLiteral("humidity")
                   || property == QStringLiteral("illuminance")
                   || property == QStringLiteral("illumination")
                   || property == QStringLiteral("occupancy")
                   || property == QStringLiteral("motion")
                   || property == QStringLiteral("co2")) {
            hasSensor = true;
        }
    }
    if (hasLight)
        return DeviceClass::Light;
    if (hasSwitch)
        return DeviceClass::Switch;
    if (hasButton)
        return DeviceClass::Button;
    if (hasSensor)
        return DeviceClass::Sensor;
    return DeviceClass::Unknown;
}

ButtonEventCode Z2mAdapter::actionToButtonEvent(const QString &action) const
{
    const QString value = action.toLower();
    if (value.contains(QStringLiteral("double")))
        return ButtonEventCode::DoublePress;
    if (value.contains(QStringLiteral("triple")))
        return ButtonEventCode::TriplePress;
    if (value.contains(QStringLiteral("quad")))
        return ButtonEventCode::QuadruplePress;
    if (value.contains(QStringLiteral("quint")))
        return ButtonEventCode::QuintuplePress;
    if (value.contains(QStringLiteral("long_release")) || value.contains(QStringLiteral("hold_release")))
        return ButtonEventCode::LongPressRelease;
    if (value.contains(QStringLiteral("release")))
        return ButtonEventCode::ShortPressRelease;
    if (value.contains(QStringLiteral("hold")) || value.contains(QStringLiteral("long")))
        return ButtonEventCode::LongPress;
    if (value.contains(QStringLiteral("single")) || value.contains(QStringLiteral("press")))
        return ButtonEventCode::InitialPress;
    return ButtonEventCode::None;
}

bool Z2mAdapter::publishCommand(const QString &deviceId,
                                const QJsonObject &payload,
                                const QString &endpoint,
                                QString &errorString)
{
    if (!m_client || m_client->state() != MqttClient::State::Connected) {
        errorString = QStringLiteral("MQTT client not connected.");
        return false;
    }
    QJsonDocument doc(payload);
    const QString topic = endpoint.isEmpty()
        ? QStringLiteral("%1/%2/set").arg(m_baseTopic, deviceId)
        : QStringLiteral("%1/%2/%3/set").arg(m_baseTopic, deviceId, endpoint);
    const qint32 msgId = m_client->publish(topic, doc.toJson(QJsonDocument::Compact));
    if (msgId < 0) {
        errorString = QStringLiteral("MQTT publish failed.");
        return false;
    }
    return true;
}

bool Z2mAdapter::buildCommandPayload(const QString &deviceId,
                                     const Z2mChannelBinding &binding,
                                     const QVariant &value,
                                     QJsonObject &payload,
                                     QString &errorString) const
{
    Q_UNUSED(deviceId);
    errorString.clear();

    if (binding.dataType == ChannelDataType::Enum) {
        if (value.typeId() == QMetaType::Int || value.typeId() == QMetaType::LongLong
            || value.typeId() == QMetaType::Double) {
            const int enumVal = value.toInt();
            if (!binding.enumValueToRaw.isEmpty()) {
                const auto it = binding.enumValueToRaw.constFind(enumVal);
                if (it != binding.enumValueToRaw.constEnd()) {
                    payload.insert(binding.property, it.value());
                    return true;
                }
            }
            payload.insert(binding.property, enumVal);
            return true;
        }
        const QString text = value.toString();
        if (!binding.enumRawToValue.isEmpty() && binding.enumRawToValue.contains(text)) {
            payload.insert(binding.property, text);
            return true;
        }
        bool ok = false;
        const int numeric = text.toInt(&ok);
        if (ok) {
            if (!binding.enumValueToRaw.isEmpty()) {
                const auto it = binding.enumValueToRaw.constFind(numeric);
                if (it != binding.enumValueToRaw.constEnd()) {
                    payload.insert(binding.property, it.value());
                    return true;
                }
            }
            payload.insert(binding.property, numeric);
            return true;
        }
        payload.insert(binding.property, text);
        return true;
    }

    switch (binding.kind) {
    case ChannelKind::PowerOnOff: {
        const bool on = value.toBool();
        if (!binding.valueOn.isEmpty() || !binding.valueOff.isEmpty())
            payload.insert(binding.property, on ? binding.valueOn : binding.valueOff);
        else
            payload.insert(binding.property, on ? QStringLiteral("ON") : QStringLiteral("OFF"));
        break;
    }
    case ChannelKind::Brightness: {
        const double percent = value.toDouble();
        const double raw = scaleFromPercent(percent, binding.rawMin, binding.rawMax);
        payload.insert(binding.property, raw);
        break;
    }
    case ChannelKind::ColorTemperature: {
        payload.insert(binding.property, value.toDouble());
        break;
    }
    case ChannelKind::ColorRGB: {
        if (!value.canConvert<phicore::Color>()) {
            errorString = QStringLiteral("Invalid color value.");
            return false;
        }
        const phicore::Color color = value.value<phicore::Color>();
        QJsonObject colorObj;
        if (binding.colorMode == QStringLiteral("xy")) {
            double x = 0.0;
            double y = 0.0;
            phicore::colorToXy(color, x, y);
            colorObj.insert(QStringLiteral("x"), x);
            colorObj.insert(QStringLiteral("y"), y);
        } else {
            const phicore::Hsv hsv = phicore::colorToHsv(color);
            colorObj.insert(QStringLiteral("hue"), hsv.hDeg);
            colorObj.insert(QStringLiteral("saturation"), hsv.s * 100.0);
        }
        payload.insert(binding.property, colorObj);
        break;
    }
    case ChannelKind::Temperature:
    case ChannelKind::Humidity:
    case ChannelKind::Illuminance:
    case ChannelKind::CO2:
    case ChannelKind::Power:
    case ChannelKind::Voltage:
    case ChannelKind::Current:
    case ChannelKind::Energy:
    case ChannelKind::SignalStrength:
    case ChannelKind::LinkQuality:
    case ChannelKind::Battery:
    case ChannelKind::Duration: {
        const double scaled = value.toDouble() / (binding.valueScale > 0.0 ? binding.valueScale : 1.0);
        payload.insert(binding.property, scaled);
        break;
    }
    case ChannelKind::Unknown: {
        if (binding.dataType == ChannelDataType::Bool) {
            payload.insert(binding.property, value.toBool());
        } else if (binding.dataType == ChannelDataType::Enum) {
            if (value.typeId() == QMetaType::Int || value.typeId() == QMetaType::LongLong
                || value.typeId() == QMetaType::Double) {
                const int enumVal = value.toInt();
                if (!binding.enumValueToRaw.isEmpty()) {
                    const auto it = binding.enumValueToRaw.constFind(enumVal);
                    if (it != binding.enumValueToRaw.constEnd()) {
                        payload.insert(binding.property, it.value());
                        break;
                    }
                }
                payload.insert(binding.property, enumVal);
            } else {
                const QString text = value.toString();
                if (!binding.enumRawToValue.isEmpty() && binding.enumRawToValue.contains(text)) {
                    payload.insert(binding.property, text);
                    break;
                }
                bool ok = false;
                const int numeric = text.toInt(&ok);
                if (ok) {
                    if (!binding.enumValueToRaw.isEmpty()) {
                        const auto it = binding.enumValueToRaw.constFind(numeric);
                        if (it != binding.enumValueToRaw.constEnd()) {
                            payload.insert(binding.property, it.value());
                            break;
                        }
                    }
                    payload.insert(binding.property, numeric);
                } else {
                    payload.insert(binding.property, text);
                }
            }
        } else {
            const double scaled = value.toDouble() / (binding.valueScale > 0.0 ? binding.valueScale : 1.0);
            payload.insert(binding.property, scaled);
        }
        break;
    }
    default:
        errorString = QStringLiteral("Unsupported channel");
        return false;
    }
    return true;
}

double Z2mAdapter::scaleToPercent(double raw, double rawMin, double rawMax) const
{
    if (rawMax <= rawMin)
        return raw;
    const double clamped = qBound(rawMin, raw, rawMax);
    return ((clamped - rawMin) / (rawMax - rawMin)) * 100.0;
}

double Z2mAdapter::scaleFromPercent(double percent, double rawMin, double rawMax) const
{
    if (rawMax <= rawMin)
        return percent;
    const double clamped = qBound(0.0, percent, 100.0);
    return rawMin + ((rawMax - rawMin) * (clamped / 100.0));
}

} // namespace phicore
