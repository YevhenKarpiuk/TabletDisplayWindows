#include "protocol/ProtocolConstants.h"

#include <iostream>
#include <string_view>

namespace
{
    void PrintHelp()
    {
        std::cout
            << "TabletDisplayHost scaffold\n"
            << "Usage: TabletDisplayHost [--help|--version|--check]\n";
    }
}

int main(int argc, char* argv[])
{
    const std::string_view command = argc > 1 ? argv[1] : "--check";

    if (command == "--help")
    {
        PrintHelp();
        return 0;
    }
    if (command == "--version")
    {
        std::cout << "TabletDisplayHost 0.1.0\n";
        return 0;
    }
    if (command != "--check")
    {
        std::cerr << "Unknown argument: " << command << '\n';
        PrintHelp();
        return 2;
    }

    std::cout
        << "TabletDisplayHost scaffold is ready.\n"
        << "Protocol version: " << tabletdisplay::protocol::kProtocolVersion << '\n'
        << "ADB endpoint: " << tabletdisplay::protocol::kLoopbackAddress
        << ':' << tabletdisplay::protocol::kPort << '\n'
        << "Streaming pipeline: not implemented yet.\n";
    return 0;
}
