#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/places.hpp"

namespace rummage::platform {
struct NetworkDiscovery {
    std::vector<fs::Place> places;
    std::string error;
};

[[nodiscard]] std::vector<fs::Place> parse_network_services(std::string_view output);
// Bounded external Avahi query. Run on a worker; no GLib or network parser in the UI.
[[nodiscard]] NetworkDiscovery discover_network(const core::CancelToken& token);
}  // namespace rummage::platform
