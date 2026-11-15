#pragma once

#include <cstdint>
#include <string>
#include <vector>


namespace ps2r {

enum class CdvdLatencyModel : uint8_t { Instant = 0, Modelled };
enum class CdvdDeliveryVisibility : uint8_t { Boundary = 0, Poll };

struct CdvdConfig {
    CdvdLatencyModel latency_model = CdvdLatencyModel::Instant;
    double           latency_scale = 1.0;
    int              mc_latency_polls = 4;
    bool             load_turbo = false;
    CdvdDeliveryVisibility delivery_visibility = CdvdDeliveryVisibility::Poll;
    double           poll_latency_scale = 1.0;
};

struct VideoConfig {
    int scale = 1;
    int custom_offset_x = 0;
    int custom_offset_y = 0;
    bool custom_textures = false;
};

struct GameConfig {
    std::string language = "english";
};

class Config {
public:
    VideoConfig video;
    GameConfig  game;

    static bool IsKey(const std::string& key);

    bool Get(const std::string& key, std::string* out) const;

    bool Set(const std::string& key, const std::string& value, std::string* err);
};

}
