#include "games/slus_20909_crash_twinsanity/vu1_library/zero_kick.h"

#include "engine/vu/vu_chop_round_guard.h"
#include "engine/vu/vu_float.h"

using namespace vu1lib;
#include "games/slus_20909_crash_twinsanity/vu1_library/skin1040_lanes.h"

namespace {

constexpr u32 kEmitterPc = 0x1720;
constexpr u32 kExitPc = 0x90;
constexpr u32 kExitTpc = 0xA0;
constexpr u32 kClassATpc = 0x1750;
constexpr u32 kClassBTpc = 0x1748;
constexpr u32 kClassCTpc = 0x1820;
constexpr u32 kCContEntryPc = 0x1820;
constexpr u32 kCContReentryPc = 0x1ac8;
constexpr u32 kCContTpc = 0x1ac8;
constexpr u32 kACContEntryPc = 0x1750;
constexpr u32 kACContTpc = 0x19e0;
constexpr u16 kDispatchLink = 0x0A;

void Push(uint16_t& mac, uint16_t& status, const u32 b[4], u8 mask) { PushMac(mac, status, b, mask); }

}

bool ZeroKick::RunStub(const Vu1HleInput& in, Vu1HleOutput& out, int cls) {
    if (in.start_pc != 0) return false;

    VuChopRoundGuard chop_guard;

    std::vector<u8>& s_mem = WorkingCopy(in);
    QwPtr m{s_mem.data()};
    out.regs = *in.regs;
    VUregs& r = out.regs;

    const u32 node = m.W(3, 0) & 0xFFFF;
    r.VI[14] = (uint16_t)node;
    r.VI[12] = 1;
    {
        Vu1HleOutput::DataWrite w;
        w.addr = 9 * 16;
        w.bytes = {1, 0, 0, 0};
        out.data_writes.push_back(std::move(w));
    }
    const u32 call_vi = m.W(node, 0) & 0xFFFF;
    r.VI[13] = (uint16_t)call_vi;
    r.VI[15] = kDispatchLink;
    const u32 call_pc = call_vi * 8;
    if (call_pc == kExitPc) {
        r.TPC = kExitTpc;
        out.has_regs = true;
        return true;
    }
    if (call_pc != kEmitterPc) return false;

    auto loadq = [&](u32 qw, int vf) {
        for (int k = 0; k < 4; ++k) r.VF[vf].I[k] = m.W(qw, k);
    };

    if (cls == 0) {
        loadq(82, 4);
        loadq(81, 3);
        loadq(80, 2);
        loadq(79, 1);
        loadq(83, 5);
        r.TPC = kClassATpc;
    } else if (cls == 1) {
        r.VI[13] = (uint16_t)(node + 1);
        r.VI[12] = 0;
        loadq(node + 3, 1);
        loadq(node + 4, 2);
        r.TPC = kClassBTpc;
    } else {

        uint16_t mac = r.mac_flag, status = r.status_flag;
        static const int kSubRegs[12] = {6, 10, 12, 18, 20, 21, 22, 23, 24, 26, 27, 28};
        for (int i = 0; i < 12; ++i) {
            for (int k = 0; k < 4; ++k) r.VF[kSubRegs[i]].I[k] = 0;
            ApplyMac(mac, status, 0x000Fu);
        }
        loadq(79, 1);
        loadq(80, 2);
        loadq(81, 3);
        loadq(82, 4);
        r.VI[13] = (uint16_t)(node + 4);
        r.VI[15] = (uint16_t)node;

        for (int k = 0; k < 4; ++k) r.VF[8].I[k] = 0x47000000u;
        ApplyMac(mac, status, 0);
        r.VF[12].I[3] = 0x47000000u;
        ApplyMac(mac, status, 0);
        r.VF[21].I[3] = 0x47000000u;
        ApplyMac(mac, status, 0);
        r.VF[12].I[3] = 0x00008000u;
        loadq(83, 5);
        loadq(85, 14);
        r.VF[21].I[3] = 0x00008000u;
        r.VF[22].I[3] = 0x00000000u;
        loadq(86, 25);

        for (int k = 0; k < 4; ++k) r.VF[23].I[k] = 0x427b999au;
        ApplyMac(mac, status, 0);

        {
            const float bc = CF(bits2f(r.VF[5].I[0]));
            const u32 res = f2bits(CF(0.0f + bc));
            u32 b4[4] = {res, res, res, res};
            for (int k = 0; k < 4; ++k) r.VF[7].I[k] = res;
            ApplyMac(mac, status, VuFloat::PackMacBits(b4, 0xF));
        }
        loadq(87, 26);

        r.VF[6].I[0] = 0;
        r.VF[6].I[1] = 0x477fff00u;
        r.VF[6].I[2] = 0;
        r.VF[6].I[3] = 0;
        ApplyMac(mac, status, 0);
        r.VI[11] = 1023;

        r.VF[27].I[1] = r.VF[14].I[2];
        r.VF[27].I[2] = r.VF[14].I[3];
        r.VF[28].I[1] = r.VF[25].I[2];
        r.VF[28].I[2] = r.VF[25].I[3];

        for (int k = 0; k < 4; ++k) r.VF[10].I[k] = 0x45d20000u;
        ApplyMac(mac, status, 0);

        {
            const u32 ty = r.VF[27].I[1], tz = r.VF[27].I[2];
            r.VF[27].I[0] = ty;
            r.VF[27].I[1] = tz;
            const u32 uy = r.VF[28].I[1], uz = r.VF[28].I[2];
            r.VF[28].I[0] = uy;
            r.VF[28].I[1] = uz;
        }
        r.VI[4] = (uint16_t)in.top;
        r.I = 0x45d20000u;
        r.mac_flag = mac;
        r.status_flag = status;
        r.TPC = kClassCTpc;
    }
    out.has_regs = true;
    return true;
}

bool ZeroKick::RunA(const Vu1HleInput& in, Vu1HleOutput& out) { return RunStub(in, out, 0); }
bool ZeroKick::RunC(const Vu1HleInput& in, Vu1HleOutput& out) { return RunStub(in, out, 2); }

bool ZeroKick::RunCContForDraw(const Vu1HleInput& in, Vu1HleOutput& out) {
    return RunCContImpl<false>(in, out);
}

template <bool full_state>
bool ZeroKick::RunCContImpl(const Vu1HleInput& in, Vu1HleOutput& out) {

    if ((in.start_pc != kCContEntryPc && in.start_pc != kCContReentryPc) || in.regs == nullptr) return false;

    VuChopRoundGuard chop_guard;

    std::vector<u8>& s_mem = WorkingCopy(in);
    QwPtr m{s_mem.data()};

    out.regs = *in.regs;
    VUregs& r = out.regs;
    uint16_t mac = r.mac_flag, status = r.status_flag;
    auto W = [&](int vf, int k) -> u32& { return r.VF[vf].I[k]; };
    const bool lanes = !full_state;
    auto load_lanes = [&](int vf){return skin1040::LoadWords(&W(vf,0));};
    auto store_lanes = [&](int vf,skin1040::Lanes v,u8 mask){skin1040::StoreMask(v,&W(vf,0),mask);};
    auto vi = [&](int n) -> u16& { return r.VI[n]; };

    auto push = [&](const u32 b[4], u8 mask) { if constexpr (full_state) Push(mac, status, b, mask); };

    auto sub_lane = [&](int vfd, int k, u32 a, u32 b) -> u32 {
        const float f = CF(bits2f(a)) - CF(bits2f(b));
        const u32 rb = f2bits(f);
        u32 raw[4] = {0, 0, 0, 0};
        raw[k] = rb;
        push(raw, (u8)(8u >> k));
        W(vfd, k) = f2bits(CF(f));
        return rb;
    };
    auto add_lane = [&](int vfd, int k, u32 a, u32 b) {
        const float f = CF(bits2f(a)) + CF(bits2f(b));
        u32 raw[4] = {0, 0, 0, 0};
        raw[k] = f2bits(f);
        push(raw, (u8)(8u >> k));
        W(vfd, k) = f2bits(CF(f));
    };
    auto mul_lane = [&](int vfd, int k, u32 a, u32 b) {
        const float f = CF(bits2f(a)) * CF(bits2f(b));
        u32 raw[4] = {0, 0, 0, 0};
        raw[k] = f2bits(f);
        push(raw, (u8)(8u >> k));
        W(vfd, k) = f2bits(CF(f));
    };

    auto vec_add = [&](int vfd, int vfa, int vfb, u8 mask) {
        if(lanes){store_lanes(vfd,skin1040::Add(load_lanes(vfa),load_lanes(vfb)),mask);return;}
        u32 raw[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) {
                const float f = CF(bits2f(W(vfa, k))) + CF(bits2f(W(vfb, k)));
                raw[k] = f2bits(f);
                W(vfd, k) = f2bits(CF(f));
            }
        push(raw, mask);
    };
    auto vec_sub = [&](int vfd, int vfa, int vfb, u8 mask) {
        if(lanes){store_lanes(vfd,skin1040::Sub(load_lanes(vfa),load_lanes(vfb)),mask);return;}
        u32 raw[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) {
                const float f = CF(bits2f(W(vfa, k))) - CF(bits2f(W(vfb, k)));
                raw[k] = f2bits(f);
                W(vfd, k) = f2bits(CF(f));
            }
        push(raw, mask);
    };
    auto vec_mul = [&](int vfd, int vfa, int vfb, u8 mask) {
        if(lanes){store_lanes(vfd,skin1040::Product(load_lanes(vfa),load_lanes(vfb)),mask);return;}
        u32 raw[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) {
                const float f = CF(bits2f(W(vfa, k))) * CF(bits2f(W(vfb, k)));
                raw[k] = f2bits(f);
                W(vfd, k) = f2bits(CF(f));
            }
        push(raw, mask);
    };

    auto mul_bc = [&](int vfd, int vfs, u32 bc_bits, u8 mask) {
        if(lanes){store_lanes(vfd,skin1040::Mul(load_lanes(vfs),CF(bits2f(bc_bits))),mask);return;}
        const float bc = CF(bits2f(bc_bits));
        u32 raw[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) {
                const float f = CF(bits2f(W(vfs, k))) * bc;
                raw[k] = f2bits(f);
                W(vfd, k) = f2bits(CF(f));
            }
        push(raw, mask);
    };

    auto mula_bc = [&](int vfs, u32 bc_bits, u8 mask) {
        if(lanes){skin1040::StoreMask(skin1040::Mul(load_lanes(vfs),bits2f(bc_bits)),r.ACC.I,mask);return;}
        const float bc = bits2f(bc_bits);
        u32 raw[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) {
                const float f = CF(bits2f(W(vfs, k))) * bc;
                raw[k] = f2bits(f);
                r.ACC.I[k] = f2bits(CF(f));
            }
        push(raw, mask);
    };
    auto madda_bc = [&](int vfs, u32 bc_bits, u8 mask) {
        if(lanes){skin1040::StoreMask(skin1040::Madd(load_lanes(vfs),bits2f(bc_bits),skin1040::LoadWords(r.ACC.I)),r.ACC.I,mask);return;}
        const float bc = bits2f(bc_bits);
        u32 raw[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) {
                const float f = VuFloat::Fma(CF(bits2f(W(vfs, k))), bc, CF(bits2f(r.ACC.I[k])));
                raw[k] = f2bits(f);
                r.ACC.I[k] = f2bits(CF(f));
            }
        push(raw, mask);
    };

    auto madd_bc = [&](int vfd, int vfs, u32 bc_bits, u8 mask) {
        if(lanes){store_lanes(vfd,skin1040::Madd(load_lanes(vfs),CF(bits2f(bc_bits)),skin1040::LoadWords(r.ACC.I)),mask);return;}
        const float bc = CF(bits2f(bc_bits));
        u32 raw[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) {
                const float f = VuFloat::Fma(CF(bits2f(W(vfs, k))), bc, CF(bits2f(r.ACC.I[k])));
                raw[k] = f2bits(f);
                W(vfd, k) = f2bits(CF(f));
            }
        push(raw, mask);
    };

    auto div_q = [&](u32 num_bits, u32 den_bits) -> float { return DivQ(status, num_bits, den_bits); };
    auto sq = [&](int vfs, u16 qw) {
        for (int k = 0; k < 4; ++k) m.SetW(qw, k, W(vfs, k));
    };
    auto lq = [&](int vfd, u16 qw) {
        for (int k = 0; k < 4; ++k) W(vfd, k) = m.W(qw, k);
    };

    if (in.start_pc == kCContReentryPc) vi(13) = (u16)(vi(14) + 4);

    {
        u32 raw[4];
        for (int k = 0; k < 4; ++k) {
            const float f = CF(bits2f(W(24, k))) - CF(bits2f(W(24, k)));
            raw[k] = f2bits(f);
            W(24, k) = f2bits(CF(f));
        }
        push(raw, 0xF);
        vi(4) = (u16)in.top;
    }
    lq(26, (u16)(vi(4) + 1));
    lq(18, (u16)(vi(4) + 2));
    vi(3) = (u16)(vi(4) + 1);

    const u32 f1870_raw = sub_lane(26, 3, W(5, 3), W(26, 3));
    vi(2) = (u16)m.W(vi(13), 0);
    {
        const u32 bc = W(0, 3);
        for (int k = 0; k < 4; ++k) W(31, k) = Fmax_raw(W(0, k), bc);
        vi(8) = 8;
    }

    vi(7) = (f1870_raw >> 31) ? (u16)2 : (u16)0;
    mul_lane(20, 3, W(26, 3), W(26, 3));

    mul_bc(18, 18, W(26, 3), 0xF);
    vi(5) = (u16)(vi(2) + 1);
    lq(10, (u16)vi(4));
    vi(12) = 63;
    W(17, 3) = Ftoi0(W(18, 3));
    sq(10, vi(2));

    float q_reg = r.Q;
    int kicks = 0;
    for (int packet_guard = 0;; ++packet_guard) {
        if (packet_guard > 256) return false;
        for (int iter_guard = 0;; ++iter_guard) {
            if (iter_guard > 16) return false;

            vi(9) = (u16)W(17, 3);
            vec_add(26, 26, 18, 0xE);
            W(17, 3) = f2bits((float)(int32_t)W(17, 3));
            vi(5) = (u16)(vi(5) + 9);
            mul_lane(20, 1, W(5, 1), W(20, 3));
            vi(3) = (u16)(vi(3) + 2);
            mula_bc(4, W(0, 3), 0xF);
            madda_bc(1, W(26, 0), 0xF);
            vi(9) = (u16)(vi(9) & vi(12));
            sub_lane(17, 3, W(18, 3), W(17, 3));
            vi(9) = (u16)(vi(9) + vi(9));
            add_lane(26, 1, W(26, 1), W(20, 1));
            lq(29, (u16)(90 + vi(9)));
            madda_bc(3, W(26, 2), 0xF);
            vi(8) = (u16)(vi(8) - 1);

            const u32 f1900_raw = sub_lane(18, 3, W(23, 3), W(18, 3));
            lq(30, (u16)(91 + vi(9)));
            sub_lane(20, 3, W(0, 3), W(17, 3));
            lq(15, (u16)(88 + vi(9)));
            mul_bc(29, 29, W(17, 3), 0xF);
            lq(19, (u16)(89 + vi(9)));
            madd_bc(16, 2, W(26, 1), 0xF);

            vi(12) = (f1900_raw >> 31) ? (u16)2 : (u16)0;
            mul_bc(30, 30, W(17, 3), 0xC);
            mula_bc(15, W(20, 3), 0xF);
            sq(15, (u16)(vi(5) - 8));
            madd_bc(15, 29, W(0, 3), 0xF);
            lq(26, vi(3));

            const u32 f1938_raw = sub_lane(24, 3, W(16, 3), W(7, 3));
            q_reg = div_q(W(0, 3), W(16, 3));
            mula_bc(19, W(20, 3), 0xC);
            vi(12) = (u16)(vi(12) + vi(7));
            madd_bc(19, 30, W(0, 3), 0xC);
            lq(18, (u16)(vi(3) + 1));

            const u32 f1950_raw = sub_lane(26, 3, W(5, 3), W(26, 3));
            W(12, 2) = W(16, 2);
            vi(7) = (f1938_raw >> 31) ? (u16)2 : (u16)0;
            vec_mul(15, 15, 31, 0xC);
            mul_lane(22, 0, W(31, 0), W(15, 2));
            W(21, 2) = W(16, 2);
            vec_mul(19, 19, 31, 0xC);
            vi(12) = (u16)(vi(12) + vi(7));
            vi(7) = (f1950_raw >> 31) ? (u16)2 : (u16)0;
            mul_lane(22, 1, W(31, 1), W(15, 3));
            vec_add(12, 16, 15, 0xC);
            vec_add(16, 16, 19, 0xC);
            W(17, 2) = W(16, 2);
            mul_bc(18, 18, W(26, 3), 0xF);
            const bool deg = ((int16_t)vi(12) > 0);
            vec_add(17, 12, 22, 0xC);
            vi(12) = 63;

            if (!deg) {

                vec_sub(21, 16, 22, 0xC);
                const u32 qb = f2bits(CF(q_reg));
                mul_bc(12, 12, qb, 0xE);
                mul_bc(16, 16, qb, 0xE);
                mul_bc(22, 17, qb, 0xE);
                mul_bc(21, 21, qb, 0xE);
                sq(25, (u16)(vi(5) - 6));

                for (int v : {12, 16, 22, 21}) {
                    W(v, 0) = Fmax_raw(W(v, 0), W(6, 0));
                    W(v, 1) = Fmax_raw(W(v, 1), W(6, 0));
                }
                sq(27, (u16)(vi(5) - 4));
                sq(28, (u16)(vi(5) - 2));
                sq(14, (u16)(vi(5) - 9));
                for (int v : {12, 16, 22, 21}) {
                    W(v, 0) = Fmin_raw(W(v, 0), W(6, 1));
                    W(v, 1) = Fmin_raw(W(v, 1), W(6, 1));
                }
                for (int k = 0; k < 3; ++k) W(12, k) = Ftoi0(W(12, k));
                W(16, 3) = W(22, 3);
                for (int k = 0; k < 3; ++k) W(16, k) = Ftoi0(W(16, k));
                for (int k = 0; k < 3; ++k) W(22, k) = Ftoi0(W(22, k));
                for (int k = 0; k < 3; ++k) W(21, k) = Ftoi0(W(21, k));
                sq(12, (u16)(vi(5) - 7));
                mul_lane(20, 3, W(26, 3), W(26, 3));
                sq(16, (u16)(vi(5) - 1));
                W(17, 3) = Ftoi0(W(18, 3));
                sq(22, (u16)(vi(5) - 3));
                sq(21, (u16)(vi(5) - 5));
            } else {

                mul_lane(20, 3, W(26, 3), W(26, 3));
                sq(12, (u16)(vi(5) - 7));
                W(17, 3) = Ftoi0(W(18, 3));
                sq(12, (u16)(vi(5) - 5));
                {
                    u32 raw[4] = {0, 0, 0, 0};
                    for (int k = 0; k < 3; ++k) {
                        const float f = CF(bits2f(W(24, k))) - CF(bits2f(W(24, k)));
                        raw[k] = f2bits(f);
                        W(24, k) = f2bits(CF(f));
                    }
                    push(raw, 0xE);
                }
                sq(12, (u16)(vi(5) - 3));
                sq(12, (u16)(vi(5) - 1));
            }
            if (vi(8) == 0) break;
        }
        if (++kicks > 64) return false;
        if (!KickAt(m, vi(2), out)) return false;

        vi(13) = (u16)(vi(13) - 1);
        vi(2) = (u16)m.W(vi(13), 0);
        const bool done = (vi(14) == vi(13));
        vi(8) = 8;
        if (done) break;
        vi(5) = (u16)(vi(2) + 1);
        sq(10, vi(2));
    }

    r.Q = q_reg;
    r.mac_flag = mac;
    r.status_flag = status;
    r.TPC = kCContTpc;
    out.has_regs = full_state;

    if constexpr (full_state) EmitDataWrites(s_mem.data(), in, out);
    return true;
}

bool ZeroKick::RunAContForDraw(const Vu1HleInput& in, Vu1HleOutput& out) {
    return RunAContImpl<false>(in, out);
}

template <bool full_state>
bool ZeroKick::RunAContImpl(const Vu1HleInput& in, Vu1HleOutput& out) {

    if (in.start_pc != kACContEntryPc || in.regs == nullptr) return false;

    VuChopRoundGuard chop_guard;

    std::vector<u8>& s_mem = WorkingCopy(in);
    QwPtr m{s_mem.data()};

    out.regs = *in.regs;
    VUregs& r = out.regs;
    uint16_t mac = r.mac_flag, status = r.status_flag;
    auto W = [&](int vf, int k) -> u32& { return r.VF[vf].I[k]; };
    auto vi = [&](int n) -> u16& { return r.VI[n]; };

    auto push = [&](const u32 b[4], u8 mask) { if constexpr (full_state) Push(mac, status, b, mask); };
    auto sub_lane = [&](int vfd, int k, u32 a, u32 b) {
        const float f = CF(bits2f(a)) - CF(bits2f(b));
        u32 raw[4] = {0, 0, 0, 0};
        raw[k] = f2bits(f);
        push(raw, (u8)(8u >> k));
        W(vfd, k) = f2bits(CF(f));
    };
    auto add_lane = [&](int vfd, int k, u32 a, u32 b) {
        const float f = CF(bits2f(a)) + CF(bits2f(b));
        u32 raw[4] = {0, 0, 0, 0};
        raw[k] = f2bits(f);
        push(raw, (u8)(8u >> k));
        W(vfd, k) = f2bits(CF(f));
    };
    auto vec_add = [&](int vfd, int vfa, int vfb, u8 mask) {
        u32 raw[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) {
                const float f = CF(bits2f(W(vfa, k))) + CF(bits2f(W(vfb, k)));
                raw[k] = f2bits(f);
                W(vfd, k) = f2bits(CF(f));
            }
        push(raw, mask);
    };
    auto vec_sub = [&](int vfd, int vfa, int vfb, u8 mask) {
        u32 raw[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) {
                const float f = CF(bits2f(W(vfa, k))) - CF(bits2f(W(vfb, k)));
                raw[k] = f2bits(f);
                W(vfd, k) = f2bits(CF(f));
            }
        push(raw, mask);
    };
    auto mul_bc = [&](int vfd, int vfs, u32 bc_bits, u8 mask) {
        const float bc = CF(bits2f(bc_bits));
        u32 raw[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) {
                const float f = CF(bits2f(W(vfs, k))) * bc;
                raw[k] = f2bits(f);
                W(vfd, k) = f2bits(CF(f));
            }
        push(raw, mask);
    };
    auto mula_bc = [&](int vfs, u32 bc_bits, u8 mask) {
        const float bc = bits2f(bc_bits);
        u32 raw[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) {
                const float f = CF(bits2f(W(vfs, k))) * bc;
                raw[k] = f2bits(f);
                r.ACC.I[k] = f2bits(CF(f));
            }
        push(raw, mask);
    };
    auto madda_bc = [&](int vfs, u32 bc_bits, u8 mask) {
        const float bc = bits2f(bc_bits);
        u32 raw[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) {
                const float f = VuFloat::Fma(CF(bits2f(W(vfs, k))), bc, CF(bits2f(r.ACC.I[k])));
                raw[k] = f2bits(f);
                r.ACC.I[k] = f2bits(CF(f));
            }
        push(raw, mask);
    };
    auto madd_bc = [&](int vfd, int vfs, u32 bc_bits, u8 mask) {
        const float bc = CF(bits2f(bc_bits));
        u32 raw[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) {
                const float f = VuFloat::Fma(CF(bits2f(W(vfs, k))), bc, CF(bits2f(r.ACC.I[k])));
                raw[k] = f2bits(f);
                W(vfd, k) = f2bits(CF(f));
            }
        push(raw, mask);
    };
    auto div_q = [&](u32 num_bits, u32 den_bits) -> float { return DivQ(status, num_bits, den_bits); };
    auto sq = [&](int vfs, u16 qw) {
        for (int k = 0; k < 4; ++k) m.SetW(qw, k, W(vfs, k));
    };
    auto lq = [&](int vfd, u16 qw) {
        for (int k = 0; k < 4; ++k) W(vfd, k) = m.W(qw, k);
    };

    auto itof12 = [&](int vfd, u8 mask) {
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) W(vfd, k) = f2bits((float)(int32_t)W(vfd, k) / 4096.0f);
    };

    vec_sub(6, 0, 0, 0xF);
    vi(2) = (u16)(vi(14) + 4);
    vi(3) = (u16)m.W(vi(2), 0);
    r.I = 0x47000000u;
    add_lane(6, 3, W(6, 3), 0x47000000u);
    vi(4) = 8;
    vi(1) = (u16)in.top;
    sub_lane(7, 3, W(0, 3), W(0, 3));
    vi(5) = vi(3);
    W(6, 3) = Ftoi0(W(6, 3));
    sq(5, vi(3));
    vi(3) = (u16)(vi(3) + 1);

    float q_reg = r.Q;
    int kicks = 0;
    for (int packet_guard = 0;; ++packet_guard) {
        if (packet_guard > 256) return false;
        for (int iter_guard = 0;; ++iter_guard) {
            if (iter_guard > 16) return false;
            lq(8, (u16)(vi(1) + 32));
            vi(6) = (u16)(W(8, 3) & 0xFFFF);
            if (vi(6) == 0) {
                itof12(8, 0xE);
                lq(11, (u16)vi(1));
                vec_add(10, 11, 8, 0xE);
                mula_bc(4, W(0, 3), 0xF);
                vec_sub(12, 11, 8, 0xE);
                madda_bc(1, W(10, 0), 0xF);
                for (int k = 0; k < 3; ++k)
                    W(7, k) = m.W((u16)(vi(1) + 64), k);
                madda_bc(2, W(10, 1), 0xF);
                vi(7) = 63;
                madd_bc(15, 3, W(10, 2), 0xF);
                vi(6) = (u16)(W(11, 3) & 0xFFFF);
                W(9, 3) = Fmax_raw(W(6, 3), W(6, 3));
                vi(6) = (u16)(vi(6) & vi(7));
                itof12(7, 0xE);
                vi(6) = (u16)(vi(6) + vi(6));
                mula_bc(4, W(0, 3), 0xF);
                lq(8, (u16)(85 + vi(6)));
                madda_bc(1, W(12, 0), 0xF);
                const float q1 = div_q(W(0, 3), W(15, 3));
                madda_bc(2, W(12, 1), 0xF);
                lq(10, (u16)(84 + vi(6)));
                madd_bc(14, 3, W(12, 2), 0xF);
                vec_sub(13, 11, 7, 0xE);
                r.R = (W(0, 3) & 0x7FFFFFu) | 0x3F800000u;
                vec_add(9, 11, 7, 0xE);
                {
                    const u32 sy = W(8, 1), sz = W(8, 2), sw = W(8, 3);
                    W(11, 0) = sy;
                    W(11, 1) = sz;
                    W(11, 2) = sw;
                }
                {
                    const u32 sy = W(10, 1), sz = W(10, 2), sw = W(10, 3);
                    W(7, 0) = sy;
                    W(7, 1) = sz;
                    W(7, 2) = sw;
                }
                W(10, 2) = r.R;
                mula_bc(4, W(0, 3), 0xF);
                const float q2 = div_q(W(0, 3), W(14, 3));
                madda_bc(1, W(9, 0), 0xF);
                madda_bc(2, W(9, 1), 0xF);
                {

                    const u32 sx = W(11, 0), sy = W(11, 1), sz = W(11, 2);
                    madd_bc(11, 3, W(9, 2), 0xF);
                    W(12, 0) = sy;
                    W(12, 1) = sz;
                    W(12, 3) = sx;
                }
                mul_bc(10, 10, f2bits(q1), 0xE);
                mul_bc(9, 15, f2bits(q1), 0xE);
                mula_bc(4, W(0, 3), 0xF);
                lq(16, (u16)(vi(1) + 96));
                madda_bc(1, W(13, 0), 0xF);
                const float q3 = div_q(W(0, 3), W(11, 3));
                madda_bc(2, W(13, 1), 0xF);
                sq(10, (u16)(vi(3) + 3));
                madd_bc(10, 3, W(13, 2), 0xF);
                W(12, 2) = r.R;
                mul_bc(13, 14, f2bits(q2), 0xE);
                {
                    const u32 sx = W(7, 0), sy = W(7, 1), sz = W(7, 2);
                    W(14, 0) = sy;
                    W(14, 1) = sz;
                    W(14, 3) = sx;
                }
                for (int k = 0; k < 3; ++k)
                    W(9, k) = Ftoi0(W(9, k));
                mul_bc(12, 12, f2bits(q2), 0xE);
                sq(16, (u16)(vi(3) + 1));
                const float q4 = div_q(W(0, 3), W(10, 3));
                W(11, 3) = Fmax_raw(W(6, 3), W(6, 3));
                W(14, 2) = r.R;
                sq(9, (u16)(vi(3) + 5));
                W(9, 3) = Fmax_raw(W(7, 3), W(7, 3));
                mul_bc(11, 11, f2bits(q3), 0xE);
                sq(16, (u16)(vi(3) + 4));
                for (int k = 0; k < 3; ++k)
                    W(9, k) = Ftoi0(W(13, k));
                W(8, 2) = r.R;
                mul_bc(14, 14, f2bits(q3), 0xE);
                sq(16, (u16)(vi(3) + 7));
                mul_bc(10, 10, f2bits(q4), 0xE);
                sq(16, (u16)(vi(3) + 10));
                for (int k = 0; k < 3; ++k)
                    W(11, k) = Ftoi0(W(11, k));
                sq(12, (u16)(vi(3) + 6));
                mul_bc(8, 8, f2bits(q4), 0xE);
                W(10, 3) = W(7, 3);
                sq(9, (u16)(vi(3) + 8));
                for (int k = 0; k < 3; ++k)
                    W(10, k) = Ftoi0(W(10, k));
                sq(14, (u16)vi(3));
                sq(11, (u16)(vi(3) + 2));
                sq(8, (u16)(vi(3) + 9));
                sq(10, (u16)(vi(3) + 11));
                q_reg = q4;
            } else {
                sq(6, (u16)(vi(3) + 8));
                sq(6, (u16)(vi(3) + 5));
                sq(6, (u16)(vi(3) + 2));
                sq(6, (u16)(vi(3) + 11));
            }
            vi(4) = (u16)(vi(4) - 1);
            vi(3) = (u16)(vi(3) + 12);
            const bool more = (vi(4) != 0);
            vi(1) = (u16)(vi(1) + 1);
            if (!more) break;
        }
        vi(2) = (u16)(vi(2) - 1);
        if (++kicks > 64) return false;
        if (!KickAt(m, vi(5), out)) return false;
        const bool done = (vi(2) == vi(14));
        vi(3) = (u16)m.W(vi(2), 0);
        if (done) break;
        vi(4) = 8;
        vi(5) = vi(3);
        sq(5, vi(3));
        vi(3) = (u16)(vi(3) + 1);
    }

    r.Q = q_reg;
    r.mac_flag = mac;
    r.status_flag = status;
    r.TPC = kACContTpc;
    out.has_regs = full_state;

    if constexpr (full_state) EmitDataWrites(s_mem.data(), in, out);
    return true;
}
