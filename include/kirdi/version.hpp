#pragma once

#define KIRDI_VERSION_MAJOR 0
#define KIRDI_VERSION_MINOR 1
#define KIRDI_VERSION_PATCH 0
#define KIRDI_VERSION_STRING "0.1.0"

namespace kirdi {
    constexpr const char* version() { return KIRDI_VERSION_STRING; }
}
