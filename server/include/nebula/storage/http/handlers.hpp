#ifndef NEBULA_STORAGE_HTTP_HANDLERS_HPP
#define NEBULA_STORAGE_HTTP_HANDLERS_HPP

#include <memory>

#include "nebula/http/routing/router.hpp"
#include "nebula/storage/application/service.hpp"

namespace nebula::storage {

[[nodiscard]] http::HttpResponse handle_create_directory(const std::shared_ptr<StorageService>& service,
                                                         const http::RouteContext& context);

[[nodiscard]] http::HttpResponse handle_upload_init(const std::shared_ptr<StorageService>& service,
                                                    const http::RouteContext& context);

[[nodiscard]] http::HttpResponse handle_upload_chunk(const std::shared_ptr<StorageService>& service,
                                                     const http::RouteContext& context);

[[nodiscard]] http::HttpResponse handle_upload_complete(const std::shared_ptr<StorageService>& service,
                                                        const http::RouteContext& context);

[[nodiscard]] http::HttpResponse handle_issue_download_ticket(const std::shared_ptr<StorageService>& service,
                                                              const http::RouteContext& context);

[[nodiscard]] http::HttpResponse handle_download_with_ticket(const std::shared_ptr<StorageService>& service,
                                                             const http::RouteContext& context);

[[nodiscard]] http::HttpResponse handle_tree_list(const std::shared_ptr<StorageService>& service,
                                                  const http::RouteContext& context);

[[nodiscard]] http::HttpResponse handle_recent(const std::shared_ptr<StorageService>& service,
                                               const http::RouteContext& context);

[[nodiscard]] http::HttpResponse handle_usage(const std::shared_ptr<StorageService>& service,
                                              const http::RouteContext& context);

[[nodiscard]] http::HttpResponse handle_delete_node(const std::shared_ptr<StorageService>& service,
                                                    const http::RouteContext& context);

[[nodiscard]] http::HttpResponse handle_gc(const std::shared_ptr<StorageService>& service,
                                           const http::RouteContext& context);

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_HTTP_HANDLERS_HPP
