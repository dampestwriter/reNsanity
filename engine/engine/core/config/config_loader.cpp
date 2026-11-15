#include "engine/core/config/config_loader.h"

#include <fstream>
#include <sstream>

namespace ps2r {

namespace {

std::string Trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string StripComment(const std::string& line) {
    size_t cut = line.size();
    const size_t h = line.find('#');
    const size_t s = line.find(';');
    if (h != std::string::npos) cut = h;
    if (s != std::string::npos && s < cut) cut = s;
    return line.substr(0, cut);
}

bool ReadFile(const std::string& path, std::string* out) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

}

bool ConfigLoader::ApplyIniText(const std::string& text, Config& c, std::string* err) {
    std::istringstream in(text);
    std::string line, section;
    int lineno = 0;
    while (std::getline(in, line)) {
        lineno++;
        std::string l = Trim(StripComment(line));
        if (l.empty()) continue;
        if (l.front() == '[' && l.back() == ']') {
            section = Trim(l.substr(1, l.size() - 2));
            continue;
        }
        const size_t eq = l.find('=');
        if (eq == std::string::npos) {
            if (err) *err = "ini line " + std::to_string(lineno) + ": expected key=value";
            return false;
        }
        std::string key = Trim(l.substr(0, eq));
        const std::string val = Trim(l.substr(eq + 1));
        if (!section.empty() && key.find('.') == std::string::npos) key = section + "." + key;
        if (!Config::IsKey(key)) continue;
        std::string e;
        if (!c.Set(key, val, &e)) {
            if (err) *err = "ini line " + std::to_string(lineno) + ": " + e;
            return false;
        }
    }
    return true;
}

bool ConfigLoader::ApplyIniFile(const std::string& path, Config& c, std::string* err, bool* existed) {
    std::string text;
    const bool ok = ReadFile(path, &text);
    if (existed) *existed = ok;
    if (!ok) return true;
    if (!ApplyIniText(text, c, err)) {
        if (err) *err = path + ": " + *err;
        return false;
    }
    return true;
}

}
