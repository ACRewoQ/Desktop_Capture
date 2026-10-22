// dc-ui: user interface (local and remote mode). P0 placeholder without Qt so that the three-process
// layout builds today. Lane C replaces this with the Qt 6 Quick application (verified first in P1/V5).

#include "dc/protocol/constants.hpp"
#include "dc/version.hpp"

#include <cstring>
#include <iostream>

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::cout << "dc-ui " << dc::kVersionString << " (protocol " << dc::proto::kProtocolVersion << ")\n";
            return 0;
        }
    }
    std::cout << "dc-ui " << dc::kVersionString << " placeholder. The Qt Quick UI arrives in phase 2.\n";
    return 0;
}
