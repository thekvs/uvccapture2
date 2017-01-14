#include <iostream>
#include <string>

#include "cxxopts/cxxopts.hpp"

int
main(int argc, char** argv)
{
    cxxopts::Options options("uvccapture2", "Capture images from an USB webcam on Linux");

    // clang-format off
    options.add_options()
        ("h,help", "Show help")
        ("D,debug", "Enable debugging")
        ("f,file", "File name", cxxopts::value<std::string>())
        ("d,device", "Camera's device", cxxopts::value<std::string>()->default_value("/dev/video0"))
        ;
    // clang-format on

    options.parse(argc, argv);

    if (options.count("help")) {
        std::cout << options.help(/*{"", "Group"}*/) << std::endl;
        exit(0);
    }

    std::cout << options["file"].as<std::string>() << std::endl;
    std::cout << options["device"].as<std::string>() << std::endl;

    return 0;
}
