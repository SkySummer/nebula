#include "nebula/net/address_discovery.hpp"

#include <array>
#include <cerrno>
#include <format>
#include <set>
#include <string_view>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

namespace nebula::net {

namespace {

sockaddr_in* as_sockaddr_in(sockaddr* addr) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<sockaddr_in*>(addr);
}

std::string build_http_url(std::string_view host, std::uint16_t port) {
    return std::format("http://{}:{}", host, port);
}

}  // namespace

AccessAddressCollection collect_access_addresses(std::uint16_t port) {
    AccessAddressCollection collection;
    std::set<std::string> seen_urls;
    auto add_address = [&](std::string_view host, std::string interface_name, bool is_network) {
        std::string url = build_http_url(host, port);
        if (!seen_urls.insert(url).second) {
            return;
        }
        collection.addresses.push_back({std::move(url), std::move(interface_name), is_network});
    };

    add_address("localhost", {}, false);
    add_address("127.0.0.1", "lo", true);

    ifaddrs* interfaces = nullptr;
    if (::getifaddrs(&interfaces) != 0) {
        collection.interface_error = errno;
        return collection;
    }

    for (ifaddrs* item = interfaces; item != nullptr; item = item->ifa_next) {
        if (item->ifa_addr == nullptr || item->ifa_name == nullptr) {
            continue;
        }

        if (item->ifa_addr->sa_family != AF_INET) {
            continue;
        }

        if ((item->ifa_flags & IFF_UP) == 0) {
            continue;
        }

        const sockaddr_in* ipv4 = as_sockaddr_in(item->ifa_addr);
        std::array<char, INET_ADDRSTRLEN> text{};
        if (::inet_ntop(AF_INET, &ipv4->sin_addr, text.data(), text.size()) == nullptr) {
            continue;
        }

        add_address(text.data(), item->ifa_name, true);
    }

    ::freeifaddrs(interfaces);
    return collection;
}

}  // namespace nebula::net
