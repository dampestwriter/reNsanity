#pragma once

#include <string>

#include "engine/core/config/config.h"

namespace ps2r {

class ConfigLoader {
public:
    static bool ApplyIniText(const std::string& text, Config& c, std::string* err);
    static bool ApplyIniFile(const std::string& path, Config& c, std::string* err, bool* existed);
};

}
