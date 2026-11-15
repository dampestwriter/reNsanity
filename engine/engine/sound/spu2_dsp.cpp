#include "engine/sound/spu2_dsp.h"

#include <cstring>
#include <algorithm>

namespace ps2r::sound::spu2 {

const int16_t kGauss[512] = {
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1,
    0, 0, 0, 0, 0, 0, 0, 1,
    1, 1, 1, 2, 2, 2, 3, 3,
    3, 4, 4, 5, 5, 6, 7, 7,
    8, 9, 9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 21, 22, 24,
    25, 27, 28, 30, 32, 33, 35, 37,
    39, 41, 44, 46, 48, 51, 53, 56,
    58, 61, 64, 67, 70, 73, 77, 80,
    84, 87, 91, 95, 99, 103, 107, 111,
    116, 120, 125, 130, 135, 140, 145, 150,
    156, 161, 167, 173, 179, 186, 192, 199,
    205, 212, 219, 227, 234, 242, 250, 257,
    266, 274, 283, 291, 300, 309, 319, 328,
    338, 348, 358, 369, 379, 390, 401, 412,
    424, 436, 448, 460, 473, 485, 498, 512,
    525, 539, 553, 567, 582, 597, 612, 627,
    643, 659, 675, 692, 708, 726, 743, 761,
    779, 797, 816, 835, 854, 874, 894, 914,
    935, 956, 977, 999, 1020, 1043, 1066, 1089,
    1112, 1136, 1160, 1184, 1209, 1234, 1260, 1286,
    1312, 1339, 1366, 1394, 1422, 1450, 1479, 1508,
    1537, 1567, 1598, 1628, 1660, 1691, 1723, 1756,
    1789, 1822, 1856, 1890, 1924, 1959, 1995, 2031,
    2067, 2104, 2141, 2179, 2217, 2256, 2295, 2334,
    2374, 2415, 2456, 2497, 2539, 2582, 2624, 2668,
    2712, 2756, 2801, 2846, 2892, 2938, 2985, 3032,
    3079, 3128, 3176, 3225, 3275, 3325, 3376, 3427,
    3479, 3531, 3584, 3637, 3691, 3745, 3799, 3855,
    3910, 3967, 4023, 4081, 4138, 4197, 4255, 4315,
    4374, 4435, 4495, 4557, 4619, 4681, 4744, 4807,
    4871, 4935, 5000, 5065, 5131, 5197, 5264, 5332,
    5399, 5468, 5536, 5606, 5676, 5746, 5817, 5888,
    5959, 6032, 6104, 6177, 6251, 6325, 6400, 6475,
    6550, 6626, 6702, 6779, 6856, 6934, 7012, 7091,
    7170, 7249, 7329, 7409, 7490, 7571, 7653, 7735,
    7817, 7900, 7983, 8066, 8150, 8234, 8319, 8404,
    8489, 8575, 8661, 8748, 8834, 8922, 9009, 9097,
    9185, 9273, 9362, 9451, 9541, 9630, 9720, 9811,
    9901, 9992, 10083, 10174, 10266, 10358, 10450, 10542,
    10635, 10727, 10820, 10913, 11007, 11100, 11194, 11288,
    11382, 11476, 11571, 11665, 11760, 11855, 11950, 12045,
    12140, 12236, 12331, 12427, 12522, 12618, 12714, 12809,
    12905, 13001, 13097, 13193, 13289, 13385, 13481, 13577,
    13673, 13769, 13865, 13961, 14056, 14152, 14248, 14343,
    14439, 14534, 14630, 14725, 14820, 14915, 15010, 15104,
    15199, 15293, 15387, 15481, 15575, 15669, 15762, 15855,
    15948, 16041, 16133, 16226, 16317, 16409, 16500, 16592,
    16682, 16773, 16863, 16953, 17042, 17131, 17220, 17308,
    17396, 17484, 17571, 17658, 17744, 17830, 17916, 18001,
    18086, 18170, 18254, 18337, 18420, 18502, 18584, 18665,
    18746, 18826, 18905, 18985, 19063, 19141, 19219, 19295,
    19372, 19447, 19522, 19597, 19671, 19744, 19816, 19888,
    19959, 20030, 20100, 20169, 20238, 20306, 20373, 20439,
    20505, 20570, 20634, 20698, 20760, 20822, 20884, 20944,
    21004, 21063, 21121, 21178, 21235, 21290, 21345, 21399,
    21452, 21505, 21556, 21607, 21657, 21706, 21754, 21801,
    21848, 21893, 21938, 21982, 22025, 22066, 22107, 22148,
    22187, 22225, 22262, 22299, 22334, 22369, 22402, 22435,
    22467, 22498, 22527, 22556, 22584, 22611, 22637, 22662,
    22686, 22709, 22731, 22752, 22772, 22791, 22809, 22826,
    22842, 22857, 22872, 22885, 22897, 22908, 22918, 22927,
    22935, 22942, 22948, 22953, 22957, 22960, 22962, 22963,
};

const uint16_t kEffectPresets[kEffectModes][32] = {
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001, 0x0001, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001, 0x0001, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    {0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80, 0x5800, 0x5300, 0x04D6, 0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF, 0x0336, 0x01B7, 0x0335, 0x01B6, 0x0334, 0x01B5, 0x0334, 0x01B5, 0x0334, 0x01B5, 0x01B4, 0x0136, 0x00B8, 0x005C, 0x8000, 0x8000},
    {0x0033, 0x0025, 0x70F0, 0x4FA8, 0xBCE0, 0x4410, 0xC0F0, 0x9C00, 0x5280, 0x4EC0, 0x03E4, 0x031B, 0x03A4, 0x02AF, 0x0372, 0x0266, 0x031C, 0x025D, 0x025C, 0x018E, 0x022F, 0x0135, 0x01D2, 0x00B7, 0x018F, 0x00B5, 0x00B4, 0x0080, 0x004C, 0x0026, 0x8000, 0x8000},
    {0x00B1, 0x007F, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xB4C0, 0x5280, 0x4EC0, 0x0904, 0x076B, 0x0824, 0x065F, 0x07A2, 0x0616, 0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305, 0x0462, 0x02B7, 0x042F, 0x0265, 0x0264, 0x01B2, 0x0100, 0x0080, 0x8000, 0x8000},
    {0x00E3, 0x00A9, 0x6F60, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xA680, 0x5680, 0x52C0, 0x0DFB, 0x0B58, 0x0D09, 0x0A3C, 0x0BD9, 0x0973, 0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0, 0x06EF, 0x03D2, 0x05EA, 0x031D, 0x031C, 0x0238, 0x0154, 0x00AA, 0x8000, 0x8000},
    {0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, 0xB800, 0xBC00, 0xC000, 0x6000, 0x5C00, 0x15BA, 0x11BB, 0x14C2, 0x10BD, 0x11BC, 0x0DC1, 0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1, 0x0A00, 0x06CD, 0x09C2, 0x05C1, 0x05C0, 0x041A, 0x0274, 0x013A, 0x8000, 0x8000},
    {0x033D, 0x0231, 0x7E00, 0x5000, 0xB400, 0xB000, 0x4C00, 0xB000, 0x6000, 0x5400, 0x1ED6, 0x1A31, 0x1D14, 0x183B, 0x1BC2, 0x16B2, 0x1A32, 0x15EF, 0x15EE, 0x1055, 0x1334, 0x0F2D, 0x11F6, 0x0C5D, 0x1056, 0x0AE1, 0x0AE0, 0x07A2, 0x0464, 0x0232, 0x8000, 0x8000},
    {0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x8100, 0x0000, 0x0000, 0x1FFD, 0x0FFD, 0x1009, 0x0009, 0x0000, 0x0000, 0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1008, 0x1004, 0x0008, 0x0004, 0x8000, 0x8000},
    {0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1FFD, 0x0FFD, 0x1009, 0x0009, 0x0000, 0x0000, 0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1008, 0x1004, 0x0008, 0x0004, 0x8000, 0x8000},
    {0x0017, 0x0013, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0x8500, 0x5F80, 0x54C0, 0x0371, 0x02AF, 0x02E5, 0x01DF, 0x02B0, 0x01D7, 0x0358, 0x026A, 0x01D6, 0x011E, 0x012D, 0x00B1, 0x011F, 0x0059, 0x01A0, 0x00E3, 0x0058, 0x0040, 0x0028, 0x0014, 0x8000, 0x8000},
};

const uint32_t kEffectAreaHalfwords[kEffectModes] = {0x2 * 4,   0x4D8 * 4,  0x3E8 * 4,  0x908 * 4,  0xDFC * 4,
                                                     0x15BC * 4, 0x1ED8 * 4, 0x3008 * 4, 0x3008 * 4, 0x780 * 4};

namespace {

constexpr int kTaps = 39;
constexpr int16_t kDownTaps[kTaps] = {-1,   0, 2,    0, -10,  0, 35,    0,     -103,  0, 266,  0, -616,
                                      0,    1332, 0, -2960, 0, 10246, 16384, 10246, 0, -2960, 0, 1332, 0,
                                      -616, 0, 266,  0, -103, 0, 35,    0,     -10,   0, 2,    0, -1};
int32_t S16(uint16_t v) { return int16_t(v); }

}

bool RateCounter::Tick(const Rate& r, int32_t& level, int32_t lo, int32_t hi, bool negative) {

    if (r.shift == 0x1F && r.step == 3) return false;
    uint32_t inc = 0x8000u >> std::max(0, int(r.shift) - 11);
    if (r.exponential && !r.decrease && level > 0x6000) inc >>= 2;
    inc = std::max(1u, inc);
    int32_t step = (r.decrease ? -8 + int32_t(r.step) : 7 - int32_t(r.step)) * (1 << std::max(0, 11 - int(r.shift)));
    if (r.exponential && r.decrease) step = int16_t((step * level) >> 15);
    else if (negative) step = -step;
    ticks += inc;
    if (ticks < 0x8000) return false;
    ticks = 0;
    level = std::clamp(level + step, lo, hi);
    return true;
}

void Adsr::Tick() {

    if (phase_ == Sustain && (adsr2_ & 0x1FC0) == 0x1FC0) return;
    switch (phase_) {
    case Attack: {
        const Rate r{(adsr1_ & 0x8000) != 0, false, uint8_t((adsr1_ >> 10) & 0x1F), uint8_t((adsr1_ >> 8) & 3)};
        if (counter_.Tick(r, level_, 0, 0x7FFF) && level_ >= 0x7FFF) phase_ = Decay;
        break;
    }
    case Decay: {
        const Rate r{true, true, uint8_t((adsr1_ >> 4) & 0xF), 0};
        const int32_t sustain = ((adsr1_ & 0xF) + 1) << 11;
        if (counter_.Tick(r, level_, 0, 0x7FFF) && level_ <= sustain) phase_ = Sustain;
        break;
    }
    case Sustain: {
        const Rate r{(adsr2_ & 0x8000) != 0, (adsr2_ & 0x4000) != 0, uint8_t((adsr2_ >> 8) & 0x1F), uint8_t((adsr2_ >> 6) & 3)};
        counter_.Tick(r, level_, 0, 0x7FFF);
        break;
    }
    case Release: {
        const Rate r{(adsr2_ & 0x20) != 0, true, uint8_t(adsr2_ & 0x1F), 0};
        if (counter_.Tick(r, level_, 0, 0x7FFF) && level_ <= 0) phase_ = Off;
        break;
    }
    case Off: break;
    }
}

void Volume::Write(uint16_t reg) {
    reg_ = reg;
    counter_ = {};
    if (!(reg & 0x8000)) level_ = int16_t(uint16_t(reg << 1));
}

void Volume::Tick() {
    if (!(reg_ & 0x8000)) return;
    const bool exponential = reg_ & 0x4000, decrease = reg_ & 0x2000, negative = reg_ & 0x1000;
    const Rate r{exponential, decrease, uint8_t((reg_ >> 2) & 0x1F), uint8_t(reg_ & 3)};

    const int32_t lo = !decrease ? -32768 : (negative && !exponential) ? -32768 : 0;
    const int32_t hi = decrease && negative && !exponential ? 0 : 32767;
    counter_.Tick(r, level_, lo, hi, negative && !(exponential && decrease));
}

Reverb::Reverb() { SetMode(0, 0, 0, true); }

void Reverb::SetMode(int mode, uint16_t delay, uint16_t feedback, bool clear) {
    if (mode < 0 || mode >= kEffectModes) return;
    mode_ = mode;
    std::copy(std::begin(kEffectPresets[mode]), std::end(kEffectPresets[mode]), regs_.begin());
    if (mode == 7 || mode == 8) {
        const uint16_t d = uint16_t((delay + 1) << 5);
        regs_[7] = uint16_t(((feedback << 7) + feedback) << 1);
        regs_[10] = uint16_t((d << 1) - regs_[0]);
        regs_[11] = uint16_t(d - regs_[1]);
        regs_[16] = uint16_t(regs_[17] + d);
        regs_[12] = uint16_t(regs_[13] + d);
        regs_[27] = uint16_t(regs_[29] + d);
        regs_[26] = uint16_t(regs_[28] + d);
    }
    const uint32_t size = kEffectAreaHalfwords[mode];
    if (clear || area_.size() != size) area_.assign(size, 0);
    PrepareAddresses();
}

void Reverb::PrepareAddresses() {
    const int32_t size = int32_t(area_.size());
    area_cursor_ = (cycle_ >> 1) % uint32_t(size);
    const auto& g = regs_;
    auto addr = [&](int i) { return int32_t(g[i]) * 4; };
    for (int r = 0; r < 2; ++r) {
        const int32_t raw[14] = {
            addr(r ? 17 : 16), addr(r ? 11 : 10) - 1,
            addr(r ? 24 : 25), addr(r ? 19 : 18) - 1,
            addr(r ? 13 : 12), addr(r ? 15 : 14), addr(r ? 21 : 20), addr(r ? 23 : 22),
            addr(r ? 27 : 26) - addr(0), addr(r ? 29 : 28) - addr(1),
            addr(r ? 11 : 10), addr(r ? 19 : 18), addr(r ? 27 : 26), addr(r ? 29 : 28)};
        for (int i = 0; i < 14; ++i) {
            const int32_t x = raw[i] % size;
            offsets_[r][i] = uint32_t(x < 0 ? x + size : x);
        }
    }
}

uint32_t Reverb::At(uint32_t offset) const {
    const uint32_t size = uint32_t(area_.size());
    const uint32_t x = area_cursor_ + offset;
    return x >= size ? x - size : x;
}

int32_t Reverb::Down(int channel) const {
    int64_t acc = 0;
    for (int i = 0; i < 19; i += 2) acc += int64_t(down_[channel][(pos_ - kTaps + i) & 63]) * kDownTaps[i];
    acc += int64_t(down_[channel][(pos_ - kTaps + 19) & 63]) * kDownTaps[19];
    for (int i = 20; i < kTaps; i += 2) acc += int64_t(down_[channel][(pos_ - kTaps + i) & 63]) * kDownTaps[i];
    return Limit(acc >> 15);
}

void Reverb::Up(int32_t& left, int32_t& right) const {
    int64_t l = 0, r = 0;
    auto tap = [&](int i, int32_t coeff) {
        const uint32_t k = (pos_ - kTaps + i) & 63;
        l += int64_t(up_[0][k]) * coeff;
        r += int64_t(up_[1][k]) * coeff;
    };
    for (int i = 0; i < 19; i += 2) tap(i, kDownTaps[i] * 2);
    tap(19, 32767);
    for (int i = 20; i < kTaps; i += 2) tap(i, kDownTaps[i] * 2);
    left = Limit(l >> 15);
    right = Limit(r >> 15);
}

void Reverb::Tick(int32_t in_left, int32_t in_right, int32_t& out_left, int32_t& out_right) {
    down_[0][pos_] = Limit(in_left);
    down_[1][pos_] = Limit(in_right);
    const int R = int(cycle_ & 1);
    const auto& g = regs_;
    const auto& off = offsets_[R];
    auto A = [&](int index) -> int32_t { return area_[At(off[index])]; };

    auto MUL = [](int32_t a, int32_t b) { return int32_t((int64_t(a) * b) >> 15); };
    const int32_t in = MUL(S16(R ? g[31] : g[30]), Down(R));
    const int32_t viir = S16(g[2]), vwall = S16(g[7]), vapf1 = S16(g[8]), vapf2 = S16(g[9]);
    const int32_t same = MUL(viir, in + MUL(vwall, A(0)) - A(1)) + A(1);
    const int32_t diff = MUL(viir, in + MUL(vwall, A(2)) - A(3)) + A(3);
    int32_t out = MUL(S16(g[3]), A(4)) + MUL(S16(g[4]), A(5)) + MUL(S16(g[5]), A(6)) + MUL(S16(g[6]), A(7));
    const int32_t apf1_src = A(8);

    const int32_t apf1 = Limit(out - MUL(vapf1, apf1_src));
    out = apf1_src + MUL(vapf1, apf1);
    const int32_t apf2_src = A(9);
    const int32_t apf2 = Limit(out - MUL(vapf2, apf2_src));
    out = apf2_src + MUL(vapf2, apf2);
    if (enabled_) {
        area_[At(off[10])] = Limit(same);
        area_[At(off[11])] = Limit(diff);
        area_[At(off[12])] = Limit(apf1);
        area_[At(off[13])] = Limit(apf2);
    }
    up_[R][pos_] = Limit(out);
    up_[R ^ 1][pos_] = 0;
    pos_ = (pos_ + 1) & 63;
    ++cycle_;
    if (cycle_ == 0) area_cursor_ = 0;
    else if (!(cycle_ & 1) && ++area_cursor_ == area_.size()) area_cursor_ = 0;
    Up(out_left, out_right);
}

}
