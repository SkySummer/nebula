#ifndef NEBULA_SERVER_HTTP_SUB_REACTOR_CALLBACKS_HPP
#define NEBULA_SERVER_HTTP_SUB_REACTOR_CALLBACKS_HPP

#include <cstddef>
#include <functional>

#include "nebula/server/http_reactor_tasks.hpp"
#include "nebula/server/server_lifecycle_state.hpp"

namespace nebula::server {

using SubReactorRequestDispatchFn = std::function<void(ReactorRequestTask task)>;
using SubReactorLifecycleProviderFn = std::function<LifecycleState()>;
using SubReactorForceCloseProviderFn = std::function<bool()>;
using SubReactorFatalErrorFn = std::function<void(std::size_t reactor_id)>;

}  // namespace nebula::server

#endif  // NEBULA_SERVER_HTTP_SUB_REACTOR_CALLBACKS_HPP
