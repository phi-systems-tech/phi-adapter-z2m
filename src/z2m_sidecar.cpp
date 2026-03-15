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

#include "z2m_runtime_convert.h"
#include "z2m_schema.h"

namespace phicore::z2m::ipc {

namespace {

namespace v1 = phicore::adapter::v1;
namespace phi = phicore::adapter::sdk;
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

} // namespace

Z2mSidecar::Z2mSidecar()
{
}

bool Z2mSidecar::start()
{
    if (!ensureRuntime())
        return false;

    m_started = false;
    return true;
}

void Z2mSidecar::stop()
{
    m_started = false;
    if (m_runtime)
        m_runtime->stopAdapter();
}

void Z2mSidecar::onConnected()
{
    std::cerr << "z2m-ipc connected" << '\n';
}

void Z2mSidecar::onDisconnected()
{
    m_started = false;
    if (m_runtime)
        m_runtime->stopAdapter();

    v1::Utf8String err;
    sendConnectionStateChanged(false, &err);

    std::cerr << "z2m-ipc disconnected" << '\n';
}

void Z2mSidecar::onConfigChanged(const phi::ConfigChangedRequest &request)
{
    AdapterInstance::onConfigChanged(request);

    if (!ensureRuntime())
        return;

    applyRuntimeConfig(request);

    m_started = false;
    m_runtime->startAdapterAsync();

    std::cerr << "z2m-ipc config.changed adapterId=" << request.adapterId
              << " externalId=" << request.adapter.externalId
              << " pluginType=" << request.adapter.pluginType
              << '\n';
}

void Z2mSidecar::onChannelInvoke(const phi::ChannelInvokeRequest &request)
{
    if (!m_started)
        return submitCmdResult(makeFailure(request.cmdId,
                                          CmdStatus::TemporarilyOffline,
                                          QStringLiteral("Adapter not started")),
                               "channel.invoke");

    QVariant value;
    if (request.hasScalarValue)
        value = toQVariant(request.value);
    else
        value = parseValueJson(request.valueJson);

    submitCmdResult(waitCmdResponse(
        request.cmdId,
        [&]() {
            m_runtime->invokeChannelUpdate(QString::fromStdString(request.deviceExternalId),
                                          QString::fromStdString(request.channelExternalId),
                                          value,
                                          request.cmdId);
        },
        kDefaultTimeoutMs),
        "channel.invoke");
}

void Z2mSidecar::onAdapterActionInvoke(const phi::AdapterActionInvokeRequest &request)
{
    if (request.actionId == "probe") {
        submitActionResult(makeActionFailure(request.cmdId,
                                            CmdStatus::NotImplemented,
                                            QStringLiteral("probe is factory scoped")),
                          "adapter.action.invoke");
        return;
    }

    if (!m_started) {
        submitActionResult(
            makeActionFailure(request.cmdId,
                             CmdStatus::TemporarilyOffline,
                             QStringLiteral("Adapter not started")),
            "adapter.action.invoke");
        return;
    }

    const QString actionId = QString::fromStdString(request.actionId).trimmed();
    const QJsonObject params = parseJsonObject(request.paramsJson);
    submitActionResult(
        waitActionResponse(
        request.cmdId,
        [&]() {
            m_runtime->invokeAction(actionId, params, request.cmdId);
        },
        kDefaultTimeoutMs),
        "adapter.action.invoke");
}

void Z2mSidecar::onDeviceNameUpdate(const phi::DeviceNameUpdateRequest &request)
{
    if (!m_started)
        return submitCmdResult(makeFailure(request.cmdId,
                                          CmdStatus::TemporarilyOffline,
                                          QStringLiteral("Adapter not started")),
                               "device.name.update");

    submitCmdResult(waitCmdResponse(
        request.cmdId,
        [&]() {
            m_runtime->invokeNameUpdate(QString::fromStdString(request.deviceExternalId),
                                       QString::fromStdString(request.name),
                                       request.cmdId);
        },
        kDefaultTimeoutMs),
        "device.name.update");
}

void Z2mSidecar::onDeviceEffectInvoke(const phi::DeviceEffectInvokeRequest &request)
{
    if (!m_started)
        return submitCmdResult(
            makeFailure(request.cmdId, CmdStatus::TemporarilyOffline, QStringLiteral("Adapter not started")),
            "device.effect.invoke");

    const QJsonObject params = parseJsonObject(request.paramsJson);

    submitCmdResult(
        waitCmdResponse(
            request.cmdId,
            [&]() {
                m_runtime->invokeEffect(QString::fromStdString(request.deviceExternalId),
                                       static_cast<runtimeapi::DeviceEffect>(request.effect),
                                       QString::fromStdString(request.effectId),
                                       params,
                                       request.cmdId);
            },
            kDefaultTimeoutMs),
        "device.effect.invoke");
}

void Z2mSidecar::onSceneInvoke(const phi::SceneInvokeRequest &request)
{
    if (!m_started)
        return submitCmdResult(makeFailure(request.cmdId,
                                          CmdStatus::TemporarilyOffline,
                                          QStringLiteral("Adapter not started")),
                               "scene.invoke");

    submitCmdResult(waitCmdResponse(
        request.cmdId,
        [&]() {
            m_runtime->invokeSceneAction(QString::fromStdString(request.sceneExternalId),
                                        QString::fromStdString(request.groupExternalId),
                                        QString::fromStdString(request.action),
                                        request.cmdId);
        },
        kDefaultTimeoutMs),
        "scene.invoke");
}

void Z2mSidecar::wireRuntimeSignals()
{
    QObject::connect(m_runtime.get(),
                     &runtimeapi::AdapterInterface::connectionStateChanged,
                     m_runtime.get(),
                     [this](bool connected) {
                         if (connected)
                             m_started = true;
                         v1::Utf8String err;
                         sendConnectionStateChanged(connected, &err);
                     });

    QObject::connect(m_runtime.get(),
                     &runtimeapi::AdapterInterface::errorOccurred,
                     m_runtime.get(),
                     [this](const QString &message, const QVariantList &params, const QString &ctx) {
                         v1::ScalarList outParams;
                         outParams.reserve(params.size());
                         for (const QVariant &entry : params)
                             outParams.push_back(toScalarValue(entry));
                         v1::Utf8String err;
                         sendError(phi::LogCategory::Network,
                                   message.toStdString(),
                                   outParams,
                                   ctx.toStdString(),
                                   {},
                                   0,
                                   &err);
                     });

    QObject::connect(m_runtime.get(),
                     &runtimeapi::AdapterInterface::deviceUpdated,
                     m_runtime.get(),
                     [this](const runtimeapi::Device &device, const runtimeapi::ChannelList &channels) {
                         v1::Utf8String err;
                         sendDeviceUpdated(toV1(device), toV1(channels), &err);
                     });

    QObject::connect(m_runtime.get(),
                     &runtimeapi::AdapterInterface::deviceRemoved,
                     m_runtime.get(),
                     [this](const QString &deviceExternalId) {
                         v1::Utf8String err;
                         sendDeviceRemoved(deviceExternalId.toStdString(), &err);
                     });

    QObject::connect(m_runtime.get(),
                     &runtimeapi::AdapterInterface::channelUpdated,
                     m_runtime.get(),
                     [this](const QString &deviceExternalId, const runtimeapi::Channel &channel) {
                         v1::Utf8String err;
                         sendChannelUpdated(deviceExternalId.toStdString(), toV1(channel), &err);
                     });

    QObject::connect(m_runtime.get(),
                     &runtimeapi::AdapterInterface::roomUpdated,
                     m_runtime.get(),
                     [this](const runtimeapi::Room &room) {
                         v1::Utf8String err;
                         sendRoomUpdated(toV1(room), &err);
                     });

    QObject::connect(m_runtime.get(),
                     &runtimeapi::AdapterInterface::roomRemoved,
                     m_runtime.get(),
                     [this](const QString &roomExternalId) {
                         v1::Utf8String err;
                         sendRoomRemoved(roomExternalId.toStdString(), &err);
                     });

    QObject::connect(m_runtime.get(),
                     &runtimeapi::AdapterInterface::groupUpdated,
                     m_runtime.get(),
                     [this](const runtimeapi::Group &group) {
                         v1::Utf8String err;
                         sendGroupUpdated(toV1(group), &err);
                     });

    QObject::connect(m_runtime.get(),
                     &runtimeapi::AdapterInterface::groupRemoved,
                     m_runtime.get(),
                     [this](const QString &groupExternalId) {
                         v1::Utf8String err;
                         sendGroupRemoved(groupExternalId.toStdString(), &err);
                     });

    QObject::connect(m_runtime.get(),
                     &runtimeapi::AdapterInterface::scenesUpdated,
                     m_runtime.get(),
                     [this](const QList<runtimeapi::Scene> &scenes) {
                         v1::Utf8String err;
                         const auto v1Scenes = toV1(scenes);
                         for (const auto &scene : v1Scenes) {
                             if (!sendSceneUpdated(scene, &err))
                                 std::cerr << "failed to send scene updated event: " << err << '\n';
                         }
                     });

    QObject::connect(m_runtime.get(),
                     &runtimeapi::AdapterInterface::channelStateUpdated,
                     m_runtime.get(),
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

    QObject::connect(m_runtime.get(),
                     &runtimeapi::AdapterInterface::adapterMetaUpdated,
                     m_runtime.get(),
                     [this](const QJsonObject &metaPatch) {
                         v1::Utf8String err;
                         const std::string json = QJsonDocument(metaPatch)
                                                      .toJson(QJsonDocument::Compact)
                                                      .toStdString();
                         sendAdapterMetaUpdated(json, &err);
                     });

    QObject::connect(m_runtime.get(),
                     &runtimeapi::AdapterInterface::started,
                     m_runtime.get(),
                     [this](bool ok, const QString &errorString) {
                         m_started = ok;
                         if (!ok) {
                             v1::Utf8String err;
                             sendConnectionStateChanged(false, &err);
                             if (!errorString.trimmed().isEmpty())
                                 sendError(phi::LogCategory::Internal,
                                           errorString.toStdString(),
                                           {},
                                           "start",
                                           {},
                                           0,
                                           &err);
                         }
                     });
}

void Z2mSidecar::applyRuntimeConfig(const phi::ConfigChangedRequest &request)
{
    m_runtimeAdapter = request.adapter;
    m_runtimeMeta = parseJsonObject(request.adapter.metaJson);
    m_staticConfig = parseJsonObject(request.staticConfigJson);

    const runtimeapi::Adapter adapterInfo = fromV1(request.adapter, m_runtimeMeta);
    m_runtime->assignAdapter(adapterInfo);
    m_runtime->setStaticConfig(m_staticConfig);
}

bool Z2mSidecar::ensureRuntime()
{
    if (m_runtime)
        return true;
    m_runtime = std::make_unique<runtimeapi::Z2mAdapter>();
    if (!m_runtime)
        return false;
    wireRuntimeSignals();
    return true;
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
        m_runtime.get(),
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
        m_runtime.get(),
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

std::int64_t Z2mSidecar::nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

void Z2mSidecar::submitCmdResult(CmdResponse response, const char *context)
{
    v1::Utf8String err;
    if (!sendResult(response, &err))
        std::cerr << "failed to send " << context << " result: " << err << '\n';
}

void Z2mSidecar::submitActionResult(ActionResponse response, const char *context)
{
    v1::Utf8String err;
    if (!sendResult(response, &err))
        std::cerr << "failed to send " << context << " result: " << err << '\n';
}

} // namespace phicore::z2m::ipc
