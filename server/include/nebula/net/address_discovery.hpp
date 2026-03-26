#ifndef NEBULA_NET_ADDRESS_DISCOVERY_HPP
#define NEBULA_NET_ADDRESS_DISCOVERY_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace nebula::net {

struct AccessAddress {
    std::string url;
    std::string interface_name;
    bool is_network = false;
};

struct AccessAddressCollection {
    std::vector<AccessAddress> addresses;
    int interface_error = 0;
};

[[nodiscard]] AccessAddressCollection collect_access_addresses(std::uint16_t port);

}  // namespace nebula::net

#endif  // NEBULA_NET_ADDRESS_DISCOVERY_HPP
