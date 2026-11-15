#include "games/slus_20909_crash_twinsanity/vu1_library/library_common.h"

#include <cstring>
#include "engine/vu/vu_float.h"

namespace vu1lib {

V4 XformClip(const V4& r0, const V4& r1, const V4& r2, const V4& r3, float vx, float vy, float vz) {
    V4 out;
    for (int k = 0; k < 4; ++k) {
        float acc = CF(CF(bits2f(r3.b[k])) * 1.0f);
        acc = CF(VuFloat::Fma(CF(bits2f(r0.b[k])), vx, acc));
        acc = CF(VuFloat::Fma(CF(bits2f(r1.b[k])), vy, acc));
        out.b[k] = f2bits(CF(VuFloat::Fma(CF(bits2f(r2.b[k])), vz, acc)));
    }
    return out;
}

V4 GuardScale(const V4& r4, const V4& p) {
    V4 out;
    for (int k = 0; k < 4; ++k) out.b[k] = f2bits(CF(CF(bits2f(r4.b[k])) * CF(bits2f(p.b[k]))));
    return out;
}

bool KickAt(QwPtr m, u32 qw, Vu1HleOutput& out) {
    const u32 tag_w0 = m.W(qw, 0), tag_w1 = m.W(qw, 1);
    const u32 nloop = tag_w0 & 0x7FFF;
    const u32 flg = (tag_w1 >> 26) & 3;
    u32 nregs = (tag_w1 >> 28) & 0xF;
    if (nregs == 0) nregs = 16;
    size_t packet_size;
    if (flg == 0) packet_size = 16 + (size_t)nloop * nregs * 16;
    else if (flg == 1) packet_size = 16 + ((size_t)nloop * nregs + 1) / 2 * 16;
    else packet_size = 16 + (size_t)nloop * 16;
    if (packet_size > kVu1HleDataSize) return false;
    std::vector<u8> pkt(packet_size);
    const u32 off = (qw & 0x3FF) * 16;
    for (size_t i = 0; i < packet_size; ++i) pkt[i] = m.mem[(off + i) & 0x3FFF];
    out.kicks.push_back(std::move(pkt));
    return true;
}

void ApplyMac(uint16_t& mac_flag, uint16_t& status, u32 mac) {
    mac_flag = (uint16_t)mac;
    status = (uint16_t)(status & 0xFF0);
    if (mac & 0x000Fu) status |= 0x001;
    if (mac & 0x00F0u) status |= 0x002;
    if (mac & 0x0F00u) status |= 0x004;
    if (mac & 0xF000u) status |= 0x008;
    if (status & 0x001) status |= 0x040;
    if (status & 0x002) status |= 0x080;
    if (status & 0x004) status |= 0x100;
    if (status & 0x008) status |= 0x200;
}

float DivQ(uint16_t& status, u32 num_bits, u32 den_bits) {
    const float num = CF(bits2f(num_bits)), den = CF(bits2f(den_bits));
    uint16_t st = 0;
    float qv;
    if (den == 0.0f) {
        st = (num == 0.0f) ? (uint16_t)0x410 : (uint16_t)0x820;
        const u32 sign = (std::signbit(num) != std::signbit(den)) ? 0x80000000u : 0u;
        qv = bits2f(sign | 0x7F7FFFFFu);
    } else {
        qv = CF(num / den);
    }
    status = (uint16_t)((status & 0xFCF) | (st & 0xC30));
    return qv;
}

std::vector<u8>& WorkingCopy(const Vu1HleInput& in) {
    static thread_local std::vector<u8> s_mem;
    s_mem.assign(in.data, in.data + kVu1HleDataSize);
    return s_mem;
}

void EmitDataWrites(const u8* mem, const Vu1HleInput& in, Vu1HleOutput& out) {
    for (u32 qw = 0; qw < kVu1HleDataSize / 16; ++qw) {
        if (std::memcmp(mem + qw * 16, in.data + qw * 16, 16) != 0) {
            u32 end = qw + 1;
            while (end < kVu1HleDataSize / 16 && std::memcmp(mem + end * 16, in.data + end * 16, 16) != 0) ++end;
            Vu1HleOutput::DataWrite w;
            w.addr = qw * 16;
            w.bytes.assign(mem + qw * 16, mem + end * 16);
            out.data_writes.push_back(std::move(w));
            qw = end;
        }
    }
}

}
