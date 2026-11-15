#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "engine/sound/pcm_source.h"

typedef struct ps2e_movie ps2e_movie;

namespace te::movie {

class MoviePlayer final : public ps2r::sound::PcmSource {
public:
    static constexpr uint32_t kPlayerSize = 0x41D0;
    static constexpr uint32_t kLentMemorySize = 0x2E040;
    static constexpr uint32_t kPictureQueue = 2;
    static constexpr uint32_t kSoundBufferFrames = 0x6000 / 4;
    static constexpr uint32_t kReadSlice = 64 * 2048;

    struct Picture {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;
        std::vector<uint8_t> yuv;
    };
    struct Player {
        bool constructed = false, presenting = false, open = false, sound_started = false;
        uint32_t present = 0;
        uint32_t audio_channel = 0, lent_memory = 0;
        int32_t width = 0;
        std::string file;

        uint64_t image_offset = 0;
        uint32_t file_size = 0, read_cursor = 0;
        bool input_done = false;

        ps2e_movie* decoder = nullptr;
        std::unique_ptr<Picture> decoded;
        std::deque<std::unique_ptr<Picture>> queue;

        struct Held { std::unique_ptr<Picture> picture; uint64_t index = 0, boundary = 0; };
        std::deque<Held> held;
        uint64_t picture_index = 0;
        uint32_t frames = 0;
        uint32_t texture = 0;
        int texture_w = 0, texture_h = 0;
        bool texture_filled = false;
        bool texture_yuv = false;
        uint32_t texture_tables = 0;

        bool quad_pending = false;
        float quad[6] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f};

        double fps = 0.0;
        uint64_t fields = 0;
        uint64_t presented = 0;
        float volume = 0.f;

        uint32_t pcm_popped = 0;
    };

    MoviePlayer();
    ~MoviePlayer() override;

    void Construct(uint32_t player);
    void BeginPresenting(uint32_t player, uint32_t present);
    bool Open(uint32_t player, const char* file, uint32_t audioChannel, int32_t width, uint32_t lentMemory);
    void StartSound(uint32_t player, float volume);
    bool Step(uint32_t player);
    void WaitFrame(uint32_t player);
    void QueuePicture(uint32_t player);
    void Draw(uint32_t player, uint32_t area, int32_t width, int32_t height, uint32_t skippedRows);
    void Close(uint32_t player);

    std::array<float, 2> Gains() const override;
    int Pop(int16_t* stereo, int frames) override;


    static uint64_t DueField(uint64_t present, double fps, uint32_t hz);

private:
    Player* Find(uint32_t player);
    bool Pump(Player& p);
    bool DecodeNext(Player& p);
    void FeedSound(Player& p);
    bool Ended(const Player& p) const;
    Player* Boundary(uint32_t player);
    bool PictureDue(const Player& p) const;

    void UploadPicture(Player& p, Picture& picture);

    void ReleaseHeld(Player& p, bool all);

    bool UploadPlanes(Player& p, Picture& picture);
    void UploadRgba(Player& p, std::vector<uint8_t>&& rgba, int w, int h);
    void DestroyDecoder(Player& p);
    void EnsureMixerSource();

    bool mixer_hooked_ = false;
    int yuv_device_ = -1;
    std::map<uint32_t, bool> yuv_checked_;
    std::map<uint32_t, bool> gpu_checked_;
    std::vector<uint8_t> check_scratch_;
    bool YuvOnDevice();
    Player* playing_ = nullptr;
    std::map<uint32_t, Player> players_;
};

MoviePlayer& Service();

}
