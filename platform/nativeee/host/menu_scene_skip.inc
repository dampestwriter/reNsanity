namespace skipns {
struct State {
    static constexpr uint32_t k = 12;
    bool decided = false, now = false;
    uint64_t key = 0, seq = 0;
    uint32_t run = 0;
    struct Pub { uint64_t key = 0, seq = 0; } pub;
    std::mutex mu;
};
State& S() { static State s; return s; }
inline uint64_t Mix(uint64_t h, uint64_t w) { h = (h ^ w) * 0x9E3779B97F4A7C15ull; return h ^ (h >> 32); }
inline uint32_t Rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint64_t FoldBytes(uint64_t h, const uint8_t* p, size_t n) {
    for (; n >= 8; n -= 8, p += 8) { uint64_t w; std::memcpy(&w, p, 8); h = Mix(h, w); }
    for (; n; --n, ++p) h = Mix(h, *p);
    return h;
}

uint64_t Key(unsigned state) {
    const uint8_t* mem = te::GuestMemory();
    if (!mem) return 0;
    const size_t size = te::GuestMemorySize();
    const uint32_t view = Rd32(mem + 0x00309BE0), clock = Rd32(mem + 0x00309ACC);
    if (!view || uint64_t(view) + 0x170 > size || !clock || uint64_t(clock) + 8 + 8 * 12 > size) return 0;
    uint64_t h = Mix(Mix(Mix(0x6d73u, state), uint64_t(ScreenW())), uint64_t(ScreenH()));
    h = FoldBytes(h, mem + view, 0x170);
    h = FoldBytes(h, mem + clock + 8, 8 * 12);
    return h | 1;
}
}

void SceneSkipDecide() {
    auto& s = skipns::S();
    if (s.decided) return;
    s.decided = true;
    s.now = false;
    s.key = 0;
    te::gfx::g_sceneSkipNow = false;
    unsigned state = 0;
    const bool read = ReadGameState(&state);
    if (!read) return;
    if (!(state == 8 || state == 15)) return;
    s.key = skipns::Key(state);
    if (!s.key) return;
    skipns::State::Pub pub;
    { std::lock_guard<std::mutex> lock(s.mu); pub = s.pub; }
    if (!pub.key || pub.key != s.key || pub.seq != s.seq || s.run >= s.k) return;
    s.now = true;
    te::gfx::g_sceneSkipNow = true;
}

SkipSnap SceneSkipTake() {
    SkipSnap o;
    auto& s = skipns::S();
    o.skipped = s.now;
    o.key = s.key;
    o.seq = ++s.seq;
    if (s.now) ++s.run; else s.run = 0;
    s.decided = s.now = false;
    s.key = 0;
    te::gfx::g_sceneSkipNow = false;
    return o;
}

void SceneSkipPublish(uint64_t key, uint64_t seq) {
    auto& s = skipns::S();
    std::lock_guard<std::mutex> lock(s.mu);
    s.pub = {key, seq};
}
void SceneSkipPublishSeq(uint64_t seq) {
    auto& s = skipns::S();
    std::lock_guard<std::mutex> lock(s.mu);
    s.pub.seq = seq;
}
