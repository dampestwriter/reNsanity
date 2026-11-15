#pragma once

#include <string>
#include <vector>

namespace ps2r {

class SettingsFile {
public:

    static std::string SetInText(const std::string& text, const std::string& key, const std::string& value,
                                 bool* changed = nullptr);

    static bool Set(const std::string& path, const std::string& key, const std::string& value, std::string* err);

    static bool Writable(const std::string& key);
};

}
