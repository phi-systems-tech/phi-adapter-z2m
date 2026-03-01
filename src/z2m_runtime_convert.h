#pragma once

#include <optional>

#include <QJsonObject>
#include <QVariant>

#include "adapterconfig.h"
#include "channel.h"
#include "device.h"
#include "group.h"
#include "room.h"
#include "scene.h"
#include "types.h"
#include "phi/adapter/sdk/sidecar.h"

namespace phicore::z2m::ipc {

phicore::adapter::v1::ScalarValue toScalarValue(const QVariant &value);
QVariant toQVariant(const phicore::adapter::v1::ScalarValue &value);

phicore::adapter::v1::CmdResponse toV1(const phicore::adapter::CmdResponse &response);
phicore::adapter::v1::ActionResponse toV1(const phicore::adapter::ActionResponse &response);

phicore::adapter::v1::Channel toV1(const phicore::adapter::Channel &channel);
phicore::adapter::v1::ChannelList toV1(const phicore::adapter::ChannelList &channels);
phicore::adapter::v1::Device toV1(const phicore::adapter::Device &device);
phicore::adapter::v1::Room toV1(const phicore::adapter::Room &room);
phicore::adapter::v1::Group toV1(const phicore::adapter::Group &group);
phicore::adapter::v1::Scene toV1(const phicore::adapter::Scene &scene);
phicore::adapter::v1::SceneList toV1(const QList<phicore::adapter::Scene> &scenes);

phicore::adapter::Adapter fromV1(const phicore::adapter::v1::Adapter &adapter,
                                 const QJsonObject &metaOverride = {});

} // namespace phicore::z2m::ipc
