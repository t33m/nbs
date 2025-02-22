#pragma once

#include <memory>

namespace NCloud::NBlockStore {

namespace NProto {
    class TServerConfig;
}

namespace NServer {

////////////////////////////////////////////////////////////////////////////////

class TServerAppConfig;
using TServerAppConfigPtr = std::shared_ptr<TServerAppConfig>;

struct IServer;
using IServerPtr = std::shared_ptr<IServer>;

struct IClientAcceptor;
using IClientAcceptorPtr = std::shared_ptr<IClientAcceptor>;

}   // namespace NServer
}   // namespace NCloud::NBlockStore
