#include "z2m_runtime_convert.h"

#include <cstdint>

#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>

namespace phicore::z2m::ipc {

namespace {

namespace AdapterCompat = phicore::adapter;
namespace V1 = phicore::adapter::v1;

std::string jsonCompact(const QJsonObject &obj)
{
    if (obj.isEmpty())
        return {};
    return QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString();
}

QJsonObject parseJsonText(const std::string &json)
{
    if (json.empty())
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(json));
    if (!doc.isObject())
        return {};
    return doc.object();
}

QVariant normalizeIntegerVariant(const QVariant &value)
{
    bool ok = false;
    const qlonglong i = value.toLongLong(&ok);
    if (ok)
        return QVariant::fromValue<qlonglong>(i);
    return value;
}

} // namespace

V1::ScalarValue toScalarValue(const QVariant &value)
{
    if (!value.isValid() || value.isNull())
        return std::monostate{};

    const int typeId = value.typeId();
    if (typeId == QMetaType::Bool)
        return value.toBool();
    if (typeId == QMetaType::Int
        || typeId == QMetaType::UInt
        || typeId == QMetaType::LongLong
        || typeId == QMetaType::ULongLong
        || typeId == QMetaType::Short
        || typeId == QMetaType::UShort
        || typeId == QMetaType::Char
        || typeId == QMetaType::SChar
        || typeId == QMetaType::UChar) {
        bool ok = false;
        const qlonglong i = value.toLongLong(&ok);
        if (ok)
            return static_cast<std::int64_t>(i);
    }
    if (typeId == QMetaType::Float || typeId == QMetaType::Double) {
        bool ok = false;
        const double d = value.toDouble(&ok);
        if (ok)
            return d;
    }
    if (typeId == QMetaType::QString)
        return value.toString().toStdString();
    if (typeId == QMetaType::QByteArray)
        return QString::fromUtf8(value.toByteArray()).toStdString();

    if (value.canConvert<QJsonObject>()) {
        const QJsonObject obj = value.toJsonObject();
        return QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString();
    }
    if (value.canConvert<QJsonArray>()) {
        const QJsonArray arr = value.toJsonArray();
        return QJsonDocument(arr).toJson(QJsonDocument::Compact).toStdString();
    }

    return value.toString().toStdString();
}

QVariant toQVariant(const V1::ScalarValue &value)
{
    if (const auto *b = std::get_if<bool>(&value))
        return QVariant(*b);
    if (const auto *i = std::get_if<std::int64_t>(&value))
        return QVariant::fromValue<qlonglong>(static_cast<qlonglong>(*i));
    if (const auto *d = std::get_if<double>(&value))
        return QVariant(*d);
    if (const auto *s = std::get_if<std::string>(&value))
        return QVariant(QString::fromStdString(*s));
    return QVariant();
}

V1::CmdResponse toV1(const AdapterCompat::CmdResponse &response)
{
    V1::CmdResponse out;
    out.id = static_cast<V1::CmdId>(response.id);
    out.status = static_cast<V1::CmdStatus>(response.status);
    out.error = response.error.toStdString();
    out.errorContext = response.errorCtx.toStdString();
    out.tsMs = response.tsMs;
    out.finalValue = toScalarValue(response.finalValue);
    out.errorParams.reserve(response.errorParams.size());
    for (const QVariant &entry : response.errorParams)
        out.errorParams.push_back(toScalarValue(normalizeIntegerVariant(entry)));
    return out;
}

V1::ActionResponse toV1(const AdapterCompat::ActionResponse &response)
{
    V1::ActionResponse out;
    out.id = static_cast<V1::CmdId>(response.id);
    out.status = static_cast<V1::CmdStatus>(response.status);
    out.error = response.error.toStdString();
    out.errorContext = response.errorCtx.toStdString();
    out.tsMs = response.tsMs;
    out.resultType = static_cast<V1::ActionResultType>(response.resultType);
    out.resultValue = toScalarValue(response.resultValue);
    out.errorParams.reserve(response.errorParams.size());
    for (const QVariant &entry : response.errorParams)
        out.errorParams.push_back(toScalarValue(normalizeIntegerVariant(entry)));
    return out;
}

V1::Channel toV1(const AdapterCompat::Channel &channel)
{
    V1::Channel out;
    out.name = channel.name.toStdString();
    out.externalId = channel.id.toStdString();
    out.kind = static_cast<V1::ChannelKind>(channel.kind);
    out.dataType = static_cast<V1::ChannelDataType>(channel.dataType);
    out.flags = static_cast<V1::ChannelFlag>(static_cast<std::uint32_t>(channel.flags));
    out.unit = channel.unit.toStdString();
    out.minValue = channel.minValue;
    out.maxValue = channel.maxValue;
    out.stepValue = channel.stepValue;
    out.metaJson = jsonCompact(channel.meta);
    out.hasValue = channel.hasValue;
    out.lastUpdateMs = channel.lastUpdateMs;
    if (channel.hasValue)
        out.lastValue = toScalarValue(channel.lastValue);

    out.choices.reserve(channel.choices.size());
    for (const AdapterCompat::AdapterConfigOption &option : channel.choices) {
        V1::AdapterConfigOption outOption;
        outOption.value = option.value.toStdString();
        outOption.label = option.label.toStdString();
        out.choices.push_back(std::move(outOption));
    }

    return out;
}

V1::ChannelList toV1(const AdapterCompat::ChannelList &channels)
{
    V1::ChannelList out;
    out.reserve(channels.size());
    for (const AdapterCompat::Channel &channel : channels)
        out.push_back(toV1(channel));
    return out;
}

V1::Device toV1(const AdapterCompat::Device &device)
{
    V1::Device out;
    out.name = device.name.toStdString();
    out.externalId = device.id.toStdString();
    out.deviceClass = static_cast<V1::DeviceClass>(device.deviceClass);
    out.flags = static_cast<V1::DeviceFlag>(static_cast<std::uint32_t>(device.flags));
    out.manufacturer = device.manufacturer.toStdString();
    out.firmware = device.firmware.toStdString();
    out.model = device.model.toStdString();
    out.metaJson = jsonCompact(device.meta);

    out.effects.reserve(device.effects.size());
    for (const AdapterCompat::DeviceEffectDescriptor &effect : device.effects) {
        V1::DeviceEffectDescriptor outEffect;
        outEffect.effect = static_cast<V1::DeviceEffect>(effect.effect);
        outEffect.id = effect.id.toStdString();
        outEffect.label = effect.label.toStdString();
        outEffect.description = effect.description.toStdString();
        outEffect.requiresParams = effect.requiresParams;
        outEffect.metaJson = jsonCompact(effect.meta);
        out.effects.push_back(std::move(outEffect));
    }

    return out;
}

V1::Room toV1(const AdapterCompat::Room &room)
{
    V1::Room out;
    out.externalId = room.externalId.toStdString();
    out.name = room.name.toStdString();
    out.zone = room.zone.toStdString();
    out.metaJson = jsonCompact(room.meta);
    out.deviceExternalIds.reserve(room.deviceExternalIds.size());
    for (const QString &id : room.deviceExternalIds)
        out.deviceExternalIds.push_back(id.toStdString());
    return out;
}

V1::Group toV1(const AdapterCompat::Group &group)
{
    V1::Group out;
    out.externalId = group.id.toStdString();
    out.name = group.name.toStdString();
    out.zone = group.zone.toStdString();
    out.metaJson = jsonCompact(group.meta);
    out.deviceExternalIds.reserve(group.deviceExternalIds.size());
    for (const QString &id : group.deviceExternalIds)
        out.deviceExternalIds.push_back(id.toStdString());
    return out;
}

V1::Scene toV1(const AdapterCompat::Scene &scene)
{
    V1::Scene out;
    out.externalId = scene.id.toStdString();
    out.name = scene.name.toStdString();
    out.description = scene.description.toStdString();
    out.scopeExternalId = scene.scopeId.toStdString();
    out.scopeType = scene.scopeType.toStdString();
    out.avatarColor = scene.avatarColor.toStdString();
    out.image = scene.image.toStdString();
    out.presetTag = scene.presetTag.toStdString();
    out.state = static_cast<V1::SceneState>(scene.state);
    out.flags = static_cast<V1::SceneFlag>(static_cast<std::uint32_t>(scene.flags));
    out.metaJson = jsonCompact(scene.meta);
    return out;
}

V1::SceneList toV1(const QList<AdapterCompat::Scene> &scenes)
{
    V1::SceneList out;
    out.reserve(scenes.size());
    for (const AdapterCompat::Scene &scene : scenes)
        out.push_back(toV1(scene));
    return out;
}

AdapterCompat::Adapter fromV1(const V1::Adapter &adapter, const QJsonObject &metaOverride)
{
    AdapterCompat::Adapter out;
    out.name = QString::fromStdString(adapter.name);
    out.host = QString::fromStdString(adapter.host);
    out.ip = QString::fromStdString(adapter.ip);
    out.port = static_cast<quint16>(adapter.port);
    out.user = QString::fromStdString(adapter.user);
    out.pw = QString::fromStdString(adapter.password);
    out.token = QString::fromStdString(adapter.token);
    out.plugin = QString::fromStdString(adapter.pluginType);
    out.id = QString::fromStdString(adapter.externalId);
    out.flags = static_cast<AdapterCompat::AdapterFlags>(static_cast<std::uint32_t>(adapter.flags));

    QJsonObject parsedMeta = parseJsonText(adapter.metaJson);
    if (!metaOverride.isEmpty()) {
        for (auto it = metaOverride.constBegin(); it != metaOverride.constEnd(); ++it)
            parsedMeta.insert(it.key(), it.value());
    }
    out.meta = parsedMeta;
    return out;
}

} // namespace phicore::z2m::ipc
