#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tabletdisplay::wifi
{
    // A tablet app advertising _tabletdisplay._tcp on the local network (protocol section 10.1).
    struct DiscoveredTablet
    {
        std::wstring name;
        std::wstring id;
        std::wstring address;
        std::uint16_t port{};
    };

    // Browses DNS-SD for browseMs and resolves the instances found. Returns what answered.
    [[nodiscard]] std::vector<DiscoveredTablet> DiscoverTablets(unsigned browseMs);
}
