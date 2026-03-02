#include "z2m_sidecar.h"

#include <algorithm>
#include <chrono>
#include <iostream>

#include <QDateTime>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QTcpSocket>

#include "z2m_runtime_convert.h"
#include "z2m_schema.h"

namespace phicore::z2m::ipc {

namespace {

namespace v1 = phicore::adapter::v1;
namespace sdk = phicore::adapter::sdk;
namespace runtimeapi = phicore::adapter;

QJsonObject parseJsonObject(const std::string &json)
{
    if (json.empty())
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(json));
    if (!doc.isObject())
        return {};
    return doc.object();
}

QVariant parseValueJson(const std::string &json)
{
    if (json.empty())
        return {};

    const QByteArray raw = QByteArray::fromStdString(json);
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (doc.isObject())
        return doc.object().toVariantMap();
    if (doc.isArray())
        return doc.array().toVariantList();

    const QString trimmed = QString::fromUtf8(raw).trimmed();
    if (trimmed.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0)
        return true;
    if (trimmed.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0)
        return false;

    bool intOk = false;
    const qlonglong i = trimmed.toLongLong(&intOk);
    if (intOk)
        return i;

    bool doubleOk = false;
    const double d = trimmed.toDouble(&doubleOk);
    if (doubleOk)
        return d;

    return trimmed;
}

QJsonObject nestedObject(const QJsonObject &root, const QString &key)
{
    const QJsonValue value = root.value(key);
    return value.isObject() ? value.toObject() : QJsonObject{};
}

} // namespace

Z2mSidecar::Z2mSidecar()
{
    wireRuntimeSignals();
}

void Z2mSidecar::onConnected()
{
    std::cerr << "z2m-ipc connected" << '\n';
}

void Z2mSidecar::onDisconnected()
{
    m_connected = false;
    m_started = false;
    m_runtime.stopAdapter();

    v1::Utf8String err;
    sendConnectionStateChanged(false, &err);

    std::cerr << "z2m-ipc disconnected" << '\n';
}

void Z2mSidecar::onBootstrap(const sdk::BootstrapRequest &request)
{
    AdapterSidecar::onBootstrap(request);
    m_started = false;
    m_runtime.stopAdapter();

    std::cerr << "z2m-ipc bootstrap adapterId=" << request.adapterId
              << " externalId=" << request.adapter.externalId
              << " pluginType=" << request.adapter.pluginType
              << '\n';
}

void Z2mSidecar::onConfigChanged(const sdk::ConfigChangedRequest &request)
{
    AdapterSidecar::onConfigChanged(request);
    applyRuntimeConfig(request);

    m_started = false;
    m_runtime.startAdapterAsync();

    std::cerr << "z2m-ipc config.changed adapterId=" << request.adapterId
              << " externalId=" << request.adapter.externalId
              << " pluginType=" << request.adapter.pluginType
              << '\n';
}

phicore::adapter::v1::CmdResponse Z2mSidecar::onChannelInvoke(const sdk::ChannelInvokeRequest &request)
{
    if (!m_started)
        return makeFailure(request.cmdId, CmdStatus::TemporarilyOffline, QStringLiteral("Adapter not started"));

    QVariant value;
    if (request.hasScalarValue)
        value = toQVariant(request.value);
    else
        value = parseValueJson(request.valueJson);

    return waitCmdResponse(
        request.cmdId,
        [&]() {
            m_runtime.invokeChannelUpdate(QString::fromStdString(request.deviceExternalId),
                                          QString::fromStdString(request.channelExternalId),
                                          value,
                                          request.cmdId);
        },
        timeoutMs());
}

phicore::adapter::v1::ActionResponse Z2mSidecar::onAdapterActionInvoke(
    const sdk::AdapterActionInvokeRequest &request)
{
    const QString actionId = QString::fromStdString(request.actionId).trimmed();
    const QJsonObject params = parseJsonObject(request.paramsJson);

    if (actionId == QLatin1String("probe"))
        return invokeProbe(request.cmdId, params);

    if (!m_started) {
        return makeActionFailure(request.cmdId,
                                 CmdStatus::TemporarilyOffline,
                                 QStringLiteral("Adapter not started"));
    }

    return waitActionResponse(
        request.cmdId,
        [&]() {
            m_runtime.invokeAction(actionId, params, request.cmdId);
        },
        timeoutMs());
}

phicore::adapter::v1::CmdResponse Z2mSidecar::onDeviceNameUpdate(
    const sdk::DeviceNameUpdateRequest &request)
{
    if (!m_started)
        return makeFailure(request.cmdId, CmdStatus::TemporarilyOffline, QStringLiteral("Adapter not started"));

    return waitCmdResponse(
        request.cmdId,
        [&]() {
            m_runtime.invokeNameUpdate(QString::fromStdString(request.deviceExternalId),
                                       QString::fromStdString(request.name),
                                       request.cmdId);
        },
        timeoutMs());
}

phicore::adapter::v1::CmdResponse Z2mSidecar::onDeviceEffectInvoke(
    const sdk::DeviceEffectInvokeRequest &request)
{
    if (!m_started)
        return makeFailure(request.cmdId, CmdStatus::TemporarilyOffline, QStringLiteral("Adapter not started"));

    const QJsonObject params = parseJsonObject(request.paramsJson);

    return waitCmdResponse(
        request.cmdId,
        [&]() {
            m_runtime.invokeEffect(QString::fromStdString(request.deviceExternalId),
                                   static_cast<runtimeapi::DeviceEffect>(request.effect),
                                   QString::fromStdString(request.effectId),
                                   params,
                                   request.cmdId);
        },
        timeoutMs());
}

phicore::adapter::v1::CmdResponse Z2mSidecar::onSceneInvoke(const sdk::SceneInvokeRequest &request)
{
    if (!m_started)
        return makeFailure(request.cmdId, CmdStatus::TemporarilyOffline, QStringLiteral("Adapter not started"));

    return waitCmdResponse(
        request.cmdId,
        [&]() {
            m_runtime.invokeSceneAction(QString::fromStdString(request.sceneExternalId),
                                        QString::fromStdString(request.groupExternalId),
                                        QString::fromStdString(request.action),
                                        request.cmdId);
        },
        timeoutMs());
}

phicore::adapter::v1::Utf8String Z2mSidecar::displayName() const
{
    return phicore::z2m::ipc::displayName();
}

phicore::adapter::v1::Utf8String Z2mSidecar::description() const
{
    return phicore::z2m::ipc::description();
}

phicore::adapter::v1::Utf8String Z2mSidecar::iconSvg() const
{
    return phicore::z2m::ipc::iconSvg();
}

phicore::adapter::v1::Utf8String Z2mSidecar::apiVersion() const
{
    return "1.0.0";
}

int Z2mSidecar::timeoutMs() const
{
    return 15000;
}

int Z2mSidecar::maxInstances() const
{
    return 0;
}

phicore::adapter::v1::AdapterCapabilities Z2mSidecar::capabilities() const
{
    return phicore::z2m::ipc::capabilities();
}

phicore::adapter::v1::JsonText Z2mSidecar::configSchemaJson() const
{
    return phicore::z2m::ipc::configSchemaJson();
}

void Z2mSidecar::wireRuntimeSignals()
{
    QObject::connect(&m_runtime,
                     &runtimeapi::AdapterInterface::connectionStateChanged,
                     &m_runtime,
                     [this](bool connected) {
                         m_connected = connected;
                         if (connected)
                             m_started = true;
                         v1::Utf8String err;
                         sendConnectionStateChanged(connected, &err);
                     });

    QObject::connect(&m_runtime,
                     &runtimeapi::AdapterInterface::errorOccurred,
                     &m_runtime,
                     [this](const QString &message, const QVariantList &params, const QString &ctx) {
                         v1::ScalarList outParams;
                         outParams.reserve(params.size());
                         for (const QVariant &entry : params)
                             outParams.push_back(toScalarValue(entry));
                         v1::Utf8String err;
                         sendError(message.toStdString(), outParams, ctx.toStdString(), &err);
                     });

    QObject::connect(&m_runtime, &runtimeapi::AdapterInterface::fullSyncCompleted, &m_runtime, [this]() {
        v1::Utf8String err;
        sendFullSyncCompleted(&err);
    });

    QObject::connect(&m_runtime,
                     &runtimeapi::AdapterInterface::deviceUpdated,
                     &m_runtime,
                     [this](const runtimeapi::Device &device, const runtimeapi::ChannelList &channels) {
                         v1::Utf8String err;
                         sendDeviceUpdated(toV1(device), toV1(channels), &err);
                     });

    QObject::connect(&m_runtime,
                     &runtimeapi::AdapterInterface::deviceRemoved,
                     &m_runtime,
                     [this](const QString &deviceExternalId) {
                         v1::Utf8String err;
                         sendDeviceRemoved(deviceExternalId.toStdString(), &err);
                     });

    QObject::connect(&m_runtime,
                     &runtimeapi::AdapterInterface::channelUpdated,
                     &m_runtime,
                     [this](const QString &deviceExternalId, const runtimeapi::Channel &channel) {
                         v1::Utf8String err;
                         sendChannelUpdated(deviceExternalId.toStdString(), toV1(channel), &err);
                     });

    QObject::connect(&m_runtime,
                     &runtimeapi::AdapterInterface::roomUpdated,
                     &m_runtime,
                     [this](const runtimeapi::Room &room) {
                         v1::Utf8String err;
                         sendRoomUpdated(toV1(room), &err);
                     });

    QObject::connect(&m_runtime,
                     &runtimeapi::AdapterInterface::roomRemoved,
                     &m_runtime,
                     [this](const QString &roomExternalId) {
                         v1::Utf8String err;
                         sendRoomRemoved(roomExternalId.toStdString(), &err);
                     });

    QObject::connect(&m_runtime,
                     &runtimeapi::AdapterInterface::groupUpdated,
                     &m_runtime,
                     [this](const runtimeapi::Group &group) {
                         v1::Utf8String err;
                         sendGroupUpdated(toV1(group), &err);
                     });

    QObject::connect(&m_runtime,
                     &runtimeapi::AdapterInterface::groupRemoved,
                     &m_runtime,
                     [this](const QString &groupExternalId) {
                         v1::Utf8String err;
                         sendGroupRemoved(groupExternalId.toStdString(), &err);
                     });

    QObject::connect(&m_runtime,
                     &runtimeapi::AdapterInterface::scenesUpdated,
                     &m_runtime,
                     [this](const QList<runtimeapi::Scene> &scenes) {
                         v1::Utf8String err;
                         sendScenesUpdated(toV1(scenes), &err);
                     });

    QObject::connect(&m_runtime,
                     &runtimeapi::AdapterInterface::channelStateUpdated,
                     &m_runtime,
                     [this](const QString &deviceExternalId,
                            const QString &channelExternalId,
                            const QVariant &value,
                            qint64 tsMs) {
                         v1::Utf8String err;
                         sendChannelStateUpdated(deviceExternalId.toStdString(),
                                                 channelExternalId.toStdString(),
                                                 toScalarValue(value),
                                                 tsMs,
                                                 &err);
                     });

    QObject::connect(&m_runtime,
                     &runtimeapi::AdapterInterface::adapterMetaUpdated,
                     &m_runtime,
                     [this](const QJsonObject &metaPatch) {
                         v1::Utf8String err;
                         const std::string json = QJsonDocument(metaPatch)
                                                      .toJson(QJsonDocument::Compact)
                                                      .toStdString();
                         sendAdapterMetaUpdated(json, &err);
                     });

    QObject::connect(&m_runtime,
                     &runtimeapi::AdapterInterface::started,
                     &m_runtime,
                     [this](bool ok, const QString &errorString) {
                         m_started = ok;
                         if (!ok) {
                             v1::Utf8String err;
                             sendConnectionStateChanged(false, &err);
                             if (!errorString.trimmed().isEmpty())
                                 sendError(errorString.toStdString(), {}, "start", &err);
                         }
                     });
}

void Z2mSidecar::applyRuntimeConfig(const sdk::ConfigChangedRequest &request)
{
    m_runtimeAdapter = request.adapter;
    m_runtimeMeta = parseJsonObject(request.adapter.metaJson);
    m_staticConfig = parseJsonObject(request.staticConfigJson);

    const runtimeapi::Adapter adapterInfo = fromV1(request.adapter, m_runtimeMeta);
    m_runtime.assignAdapter(adapterInfo);
    m_runtime.setStaticConfig(m_staticConfig);
}

Z2mSidecar::CmdResponse Z2mSidecar::waitCmdResponse(std::uint64_t cmdId,
                                                    const std::function<void()> &invoke,
                                                    int timeoutMs)
{
    std::optional<runtimeapi::CmdResponse> response;

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    timer.setInterval(std::max(1000, timeoutMs));

    const QMetaObject::Connection resultConn = QObject::connect(
        &m_runtime,
        &runtimeapi::AdapterInterface::cmdResult,
        &loop,
        [&](const runtimeapi::CmdResponse &runtimeResponse) {
            if (runtimeResponse.id != cmdId)
                return;
            response = runtimeResponse;
            loop.quit();
        });

    const QMetaObject::Connection timeoutConn = QObject::connect(&timer, &QTimer::timeout, &loop, [&loop]() {
        loop.quit();
    });

    timer.start();
    invoke();

    if (!response.has_value() && timer.isActive())
        loop.exec();

    QObject::disconnect(resultConn);
    QObject::disconnect(timeoutConn);

    if (response.has_value())
        return toV1(*response);

    return makeFailure(cmdId, CmdStatus::Timeout, QStringLiteral("Command timed out"));
}

Z2mSidecar::ActionResponse Z2mSidecar::waitActionResponse(std::uint64_t cmdId,
                                                          const std::function<void()> &invoke,
                                                          int timeoutMs)
{
    std::optional<runtimeapi::ActionResponse> response;

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    timer.setInterval(std::max(1000, timeoutMs));

    const QMetaObject::Connection resultConn = QObject::connect(
        &m_runtime,
        &runtimeapi::AdapterInterface::actionResult,
        &loop,
        [&](const runtimeapi::ActionResponse &runtimeResponse) {
            if (runtimeResponse.id != cmdId)
                return;
            response = runtimeResponse;
            loop.quit();
        });

    const QMetaObject::Connection timeoutConn = QObject::connect(&timer, &QTimer::timeout, &loop, [&loop]() {
        loop.quit();
    });

    timer.start();
    invoke();

    if (!response.has_value() && timer.isActive())
        loop.exec();

    QObject::disconnect(resultConn);
    QObject::disconnect(timeoutConn);

    if (response.has_value())
        return toV1(*response);

    return makeActionFailure(cmdId, CmdStatus::Timeout, QStringLiteral("Action timed out"));
}

Z2mSidecar::ActionResponse Z2mSidecar::invokeProbe(std::uint64_t cmdId, const QJsonObject &params)
{
    QJsonObject source = params;
    const QJsonObject factoryAdapter = nestedObject(params, QStringLiteral("factoryAdapter"));
    if (!factoryAdapter.isEmpty())
        source = factoryAdapter;

    const QString host = source.value(QStringLiteral("host")).toString().trimmed().isEmpty()
        ? source.value(QStringLiteral("ip")).toString().trimmed()
        : source.value(QStringLiteral("host")).toString().trimmed();
    const int port = source.value(QStringLiteral("port")).toInt(1883);

    if (host.isEmpty())
        return makeActionFailure(cmdId, CmdStatus::InvalidArgument, QStringLiteral("Host must not be empty."));

    QTcpSocket socket;
    socket.connectToHost(host, static_cast<quint16>(port > 0 ? port : 1883));
    if (!socket.waitForConnected(2000)) {
        const QString error = socket.errorString().trimmed().isEmpty()
            ? QStringLiteral("Connection failed")
            : socket.errorString().trimmed();
        socket.abort();
        return makeActionFailure(cmdId, CmdStatus::Failure, error);
    }

    socket.disconnectFromHost();
    return makeActionSuccess(cmdId, true);
}

Z2mSidecar::CmdResponse Z2mSidecar::makeFailure(std::uint64_t cmdId,
                                                CmdStatus status,
                                                const QString &message) const
{
    CmdResponse out;
    out.id = cmdId;
    out.status = status;
    out.error = message.toStdString();
    out.tsMs = nowMs();
    return out;
}

Z2mSidecar::CmdResponse Z2mSidecar::makeSuccess(std::uint64_t cmdId) const
{
    CmdResponse out;
    out.id = cmdId;
    out.status = CmdStatus::Success;
    out.tsMs = nowMs();
    return out;
}

Z2mSidecar::ActionResponse Z2mSidecar::makeActionFailure(std::uint64_t cmdId,
                                                         CmdStatus status,
                                                         const QString &message) const
{
    ActionResponse out;
    out.id = cmdId;
    out.status = status;
    out.error = message.toStdString();
    out.tsMs = nowMs();
    return out;
}

Z2mSidecar::ActionResponse Z2mSidecar::makeActionSuccess(
    std::uint64_t cmdId,
    const phicore::adapter::v1::ScalarValue &result) const
{
    ActionResponse out;
    out.id = cmdId;
    out.status = CmdStatus::Success;
    out.tsMs = nowMs();
    out.resultValue = result;

    if (std::holds_alternative<bool>(result))
        out.resultType = v1::ActionResultType::Boolean;
    else if (std::holds_alternative<std::int64_t>(result))
        out.resultType = v1::ActionResultType::Integer;
    else if (std::holds_alternative<double>(result))
        out.resultType = v1::ActionResultType::Float;
    else if (std::holds_alternative<std::string>(result))
        out.resultType = v1::ActionResultType::String;
    else
        out.resultType = v1::ActionResultType::None;

    return out;
}

std::int64_t Z2mSidecar::nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

} // namespace phicore::z2m::ipc
