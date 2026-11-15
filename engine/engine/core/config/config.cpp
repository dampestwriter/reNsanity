#include "engine/core/config/config.h"

#include <cstdlib>
#include <functional>

namespace ps2r {

namespace {

struct Field {
    const char*                                                     key;
    std::function<std::string(const Config&)>                       get;
    std::function<bool(Config&, const std::string&, std::string*)>  set;
};

bool ParseBool(const std::string& v, bool* out) {
    if (v == "on") { *out = true; return true; }
    if (v == "off") { *out = false; return true; }
    return false;
}

bool ParseInt(const std::string& v, int* out) {
    if (v.empty()) return false;
    char* end = nullptr;
    long n = std::strtol(v.c_str(), &end, 10);
    if (!end || *end) return false;
    *out = (int)n;
    return true;
}

bool Fail(std::string* err, const char* key, const std::string& v, const char* what) {
    if (err) *err = std::string(key) + ": bad " + what + " value '" + v + "'";
    return false;
}

#define INT_FIELD(KEY, MEMBER)                                                              \
    {KEY, [](const Config& c) { return std::to_string(c.MEMBER); },                        \
     [](Config& c, const std::string& v, std::string* err) {                               \
         int n;                                                                            \
         if (!ParseInt(v, &n)) return Fail(err, KEY, v, "integer");                        \
         c.MEMBER = n;                                                                     \
         return true;                                                                      \
     }}
#define BOOL_FIELD(KEY, MEMBER)                                                             \
    {KEY, [](const Config& c) { return std::string(c.MEMBER ? "on" : "off"); },            \
     [](Config& c, const std::string& v, std::string* err) {                               \
         bool b;                                                                           \
         if (!ParseBool(v, &b)) return Fail(err, KEY, v, "on / off");                      \
         c.MEMBER = b;                                                                     \
         return true;                                                                      \
     }}
#define STR_FIELD(KEY, MEMBER)                                                              \
    {KEY, [](const Config& c) { return c.MEMBER; },                                        \
     [](Config& c, const std::string& v, std::string*) {                                   \
         c.MEMBER = v;                                                                     \
         return true;                                                                      \
     }}

const std::vector<Field>& Fields() {
    static const std::vector<Field> f = {
        STR_FIELD("game.language", game.language),
        INT_FIELD("video.custom_offset_x", video.custom_offset_x),
        INT_FIELD("video.custom_offset_y", video.custom_offset_y),
        BOOL_FIELD("video.custom_textures", video.custom_textures),
        INT_FIELD("video.scale", video.scale),
    };
    return f;
}

#undef BOOL_FIELD
#undef INT_FIELD
#undef STR_FIELD

const Field* FindField(const std::string& key) {
    for (const Field& f : Fields())
        if (key == f.key) return &f;
    return nullptr;
}

}

bool Config::IsKey(const std::string& key) { return FindField(key) != nullptr; }

bool Config::Get(const std::string& key, std::string* out) const {
    const Field* f = FindField(key);
    if (!f) return false;
    if (out) *out = f->get(*this);
    return true;
}

bool Config::Set(const std::string& key, const std::string& value, std::string* err) {
    const Field* f = FindField(key);
    if (!f) {
        if (err) *err = "unknown config key '" + key + "'";
        return false;
    }
    return f->set(*this, value, err);
}

}
