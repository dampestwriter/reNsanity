#include "engine/platform/ps2e/scene_interpolator.h"

#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace ps2r {
namespace ps2e {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMinW = 1e-6;

void ToDouble(const float in[16], double out[16]) {
    for (int i = 0; i < 16; ++i) out[i] = double(in[i]);
}

double Dot3(const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void Normalize4(double q[4]) {
    const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 0) for (int i = 0; i < 4; ++i) q[i] /= n;
    else { q[0] = q[1] = q[2] = 0; q[3] = 1; }
}

void QuatFromRows(const double r[3][3], double q[4]) {

    const double c00 = r[0][0], c11 = r[1][1], c22 = r[2][2];
    const double c01 = r[1][0], c02 = r[2][0], c10 = r[0][1], c12 = r[2][1], c20 = r[0][2], c21 = r[1][2];
    const double trace = c00 + c11 + c22;
    if (trace > 0) {
        const double s = std::sqrt(trace + 1.0) * 2.0;
        q[3] = 0.25 * s;
        q[0] = (c21 - c12) / s;
        q[1] = (c02 - c20) / s;
        q[2] = (c10 - c01) / s;
    } else if (c00 > c11 && c00 > c22) {
        const double s = std::sqrt(1.0 + c00 - c11 - c22) * 2.0;
        q[3] = (c21 - c12) / s;
        q[0] = 0.25 * s;
        q[1] = (c01 + c10) / s;
        q[2] = (c02 + c20) / s;
    } else if (c11 > c22) {
        const double s = std::sqrt(1.0 + c11 - c00 - c22) * 2.0;
        q[3] = (c02 - c20) / s;
        q[0] = (c01 + c10) / s;
        q[1] = 0.25 * s;
        q[2] = (c12 + c21) / s;
    } else {
        const double s = std::sqrt(1.0 + c22 - c00 - c11) * 2.0;
        q[3] = (c10 - c01) / s;
        q[0] = (c02 + c20) / s;
        q[1] = (c12 + c21) / s;
        q[2] = 0.25 * s;
    }
    Normalize4(q);
}

void RowsFromQuat(const double q[4], double r[3][3]) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double c[3][3] = {
        {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
        {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
        {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)},
    };
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i][j] = c[j][i];
}

double Lerp(double a, double b, double f) { return a + (b - a) * f; }

}

void SceneInterpolator::Multiply4(const double a[16], const double b[16], double out[16]) {
    double t[16];

    typedef double D2 __attribute__((vector_size(16)));
    D2 rows[4][2];
    for (int k = 0; k < 4; ++k) {
        std::memcpy(&rows[k][0], b + k * 4, sizeof(D2));
        std::memcpy(&rows[k][1], b + k * 4 + 2, sizeof(D2));
    }
    for (int i = 0; i < 4; ++i)
        for (int h = 0; h < 2; ++h) {
            D2 s = {0, 0};
            for (int k = 0; k < 4; ++k) {
                const D2 ak = {a[i * 4 + k], a[i * 4 + k]};
                s += ak * rows[k][h];
            }
            std::memcpy(t + i * 4 + h * 2, &s, sizeof(D2));
        }
    for (int i = 0; i < 16; ++i) out[i] = t[i];
}

bool SceneInterpolator::Invert4(const double m[16], double out[16]) {
    double inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!std::isfinite(det) || std::fabs(det) < 1e-30) return false;
    const double r = 1.0 / det;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] * r;
    return true;
}

bool SceneInterpolator::DecomposeTrs(const double m[16], double t[3], double q[4], double s[3]) {
    t[0] = m[12]; t[1] = m[13]; t[2] = m[14];
    double r[3][3];
    for (int i = 0; i < 3; ++i) {
        const double* row = m + i * 4;
        s[i] = std::sqrt(Dot3(row, row));
        if (!(s[i] > 1e-12) || !std::isfinite(s[i])) return false;
        for (int j = 0; j < 3; ++j) r[i][j] = row[j] / s[i];
    }

    const double det = r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) - r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
                       r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
    if (det < 0) {
        s[2] = -s[2];
        for (int j = 0; j < 3; ++j) r[2][j] = -r[2][j];
    }
    QuatFromRows(r, q);
    return true;
}

void SceneInterpolator::ComposeTrs(const double t[3], const double q[4], const double s[3], double out[16]) {
    double r[3][3];
    RowsFromQuat(q, r);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) out[i * 4 + j] = r[i][j] * s[i];
        out[i * 4 + 3] = 0;
    }
    out[12] = t[0]; out[13] = t[1]; out[14] = t[2]; out[15] = 1;
}

void SceneInterpolator::SlerpPrepare(const double a[4], const double b[4], SlerpArc& arc) {
    for (int i = 0; i < 4; ++i) arc.b[i] = b[i];
    double cosom = a[0] * arc.b[0] + a[1] * arc.b[1] + a[2] * arc.b[2] + a[3] * arc.b[3];
    if (cosom < 0) {
        cosom = -cosom;
        for (int i = 0; i < 4; ++i) arc.b[i] = -arc.b[i];
    }
    arc.near = cosom > 0.9995;
    if (!arc.near) {
        arc.omega = std::acos(std::min(1.0, cosom));
        arc.sinom = std::sin(arc.omega);
    }
}

void SceneInterpolator::SlerpAt(const double a[4], const SlerpArc& arc, double f, double out[4]) {
    double k0, k1;
    if (arc.near) {
        k0 = 1.0 - f;
        k1 = f;
    } else {
        k0 = std::sin((1.0 - f) * arc.omega) / arc.sinom;
        k1 = std::sin(f * arc.omega) / arc.sinom;
    }
    for (int i = 0; i < 4; ++i) out[i] = a[i] * k0 + arc.b[i] * k1;
    Normalize4(out);
}

void SceneInterpolator::Slerp(const double a[4], const double b[4], double f, double out[4]) {
    SlerpArc arc;
    SlerpPrepare(a, b, arc);
    SlerpAt(a, arc, f, out);
}

double SceneInterpolator::AngleDegrees(const double a[4], const double b[4]) {
    const double d = std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    return 2.0 * std::acos(std::min(1.0, d)) * 180.0 / kPi;
}

void SceneInterpolator::PrepareCamera(const InterpCamera& a, const InterpCamera& b, const Options& options, CameraPair& pair) {
    pair.usable = false;
    pair.cut = false;
    if (!a.valid || !b.valid) return;
    double va[16];
    ToDouble(a.to_camera, va);
    ToDouble(b.to_camera, pair.vb);
    ToDouble(a.camera_to_screen, pair.pa);
    ToDouble(b.camera_to_screen, pair.pb);

    double ia[16], ib[16];
    if (!Invert4(va, ia) || !Invert4(pair.vb, ib)) return;
    if (!DecomposeTrs(ia, pair.ta, pair.qa, pair.sa) || !DecomposeTrs(ib, pair.tb, pair.qb, pair.sb)) return;
    const double* ta = pair.ta;
    const double* tb = pair.tb;
    const double travel = std::sqrt((tb[0] - ta[0]) * (tb[0] - ta[0]) + (tb[1] - ta[1]) * (tb[1] - ta[1]) + (tb[2] - ta[2]) * (tb[2] - ta[2]));
    const double angle = AngleDegrees(pair.qa, pair.qb);
    pair.cut = travel > options.camera_cut_travel || angle > options.camera_cut_angle_degrees;
    pair.usable = true;
}

bool SceneInterpolator::CameraAt(const CameraPair& pair, double fraction, double to_camera[16], double camera_to_screen[16], bool* cut) {
    if (cut) *cut = false;
    if (!pair.usable) return false;
    if (pair.cut) {
        if (cut) *cut = true;
        for (int i = 0; i < 16; ++i) { to_camera[i] = pair.vb[i]; camera_to_screen[i] = pair.pb[i]; }
        return true;
    }
    double t[3], q[4], s[3], pose[16];
    for (int i = 0; i < 3; ++i) { t[i] = Lerp(pair.ta[i], pair.tb[i], fraction); s[i] = Lerp(pair.sa[i], pair.sb[i], fraction); }
    Slerp(pair.qa, pair.qb, fraction, q);
    ComposeTrs(t, q, s, pose);
    if (!Invert4(pose, to_camera)) return false;
    for (int i = 0; i < 16; ++i) camera_to_screen[i] = Lerp(pair.pa[i], pair.pb[i], fraction);
    return true;
}

void SceneInterpolator::ResetHistory() {
    plan_.keyed = plan_.valid = false;
    for (auto& d : decomps_) d.ordinal = 0;
}

bool SceneInterpolator::ReprojectRigid(const InterpItem& item, const double K[16], float off_x, float off_y, float scale, ps2e_draw_vertex* out) {

    const double sc = scale > 0 ? double(scale) : 1.0;
    float k[16];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) k[r * 4 + c] = float(K[r * 4 + c] * (r < 2 ? sc : 1.0) * (c < 2 ? 1.0 / sc : 1.0));
    for (int c = 0; c < 4; ++c)
        k[12 + c] = float((K[12 + c] + double(off_x) * sc * K[c] + double(off_y) * sc * K[4 + c]) * (c < 2 ? 1.0 / sc : 1.0));
    const float kMinWf = float(kMinW);

    typedef float F4 __attribute__((vector_size(16)));
    const F4 r0 = {k[0], k[1], k[2], k[3]}, r1 = {k[4], k[5], k[6], k[7]}, r2 = {k[8], k[9], k[10], k[11]}, r3 = {k[12], k[13], k[14], k[15]};
    for (uint32_t i = 0; i < item.count; ++i) {
        const ps2e_draw_vertex& v = item.verts[i];
        const F4 vx = {v.x, v.x, v.x, v.x}, vy = {v.y, v.y, v.y, v.y}, vz = {v.z, v.z, v.z, v.z};
        const F4 h = vx * r0 + vy * r1 + vz * r2 + r3;
        const float hw = h[3];
        if (!(hw > kMinWf) || !std::isfinite(hw)) return false;
        const float r = 1.0f / hw;
        ps2e_draw_vertex o = v;
        o.x = h[0] * r - off_x;
        o.y = h[1] * r - off_y;
        o.z = std::max(0.0f, h[2] * r);
        if (v.q != 0.f) {
            o.u = v.u * r;
            o.v = v.v * r;
            o.q = v.q * r;
        }
        out[i] = o;
    }
    return true;
}

uint64_t SceneInterpolator::Key(const InterpTag& tag) {
    uint64_t h = tag.id ^ 0x9E3779B97F4A7C15ull;
    h ^= (uint64_t(tag.part) + 1) * 0xBF58476D1CE4E5B9ull;
    h ^= h >> 31;
    return h;
}

uint64_t SceneInterpolator::InstanceKey(const InterpTag& tag, const float world[16]) {
    uint32_t bits[3];
    std::memcpy(bits, world + 12, sizeof bits);
    uint64_t h = 1469598103934665603ull;
    for (uint32_t b : bits) { h ^= b; h *= 1099511628211ull; }
    return tag.id ^ (h | 1);
}

SceneInterpolator::PrevInstance* SceneInterpolator::NearestPrev(uint64_t id, const double t[3], double* distance) {
    const uint32_t* group = prev_rigid_.Find(id);
    if (!group) return nullptr;
    PrevInstance* best = nullptr;
    double best_d = 0;
    for (uint32_t e = prev_groups_[*group].head; e != UINT32_MAX; e = prev_instances_[e].next) {
        PrevInstance& p = prev_instances_[e];
        if (p.taken) continue;
        const double d = std::sqrt((p.t[0] - t[0]) * (p.t[0] - t[0]) + (p.t[1] - t[1]) * (p.t[1] - t[1]) + (p.t[2] - t[2]) * (p.t[2] - t[2]));
        if (!best || d < best_d) { best = &p; best_d = d; }
    }
    if (distance) *distance = best_d;
    return best;
}

void SceneInterpolator::FlatIndex::Reset(size_t entries) {
    size_t cap = 16;
    int bits = 4;
    while (cap < entries * 2) { cap <<= 1; ++bits; }
    if (cap > keys.size()) {
        keys.assign(cap, 0);
        values.assign(cap, 0);
        stamps.assign(cap, 0);
        stamp = 0;
    } else {
        while (cap < keys.size()) { cap <<= 1; ++bits; }
    }
    if (++stamp == 0) {
        std::fill(stamps.begin(), stamps.end(), 0u);
        stamp = 1;
    }
    shift = 64 - bits;
}

const uint32_t* SceneInterpolator::FlatIndex::Find(uint64_t key) const {
    const size_t mask = keys.size() - 1;
    for (size_t i = size_t((key * 0x9E3779B97F4A7C15ull) >> shift);; i = (i + 1) & mask) {
        if (stamps[i] != stamp) return nullptr;
        if (keys[i] == key) return &values[i];
    }
}

uint32_t& SceneInterpolator::FlatIndex::Insert(uint64_t key, uint32_t value, bool* inserted) {
    const size_t mask = keys.size() - 1;
    for (size_t i = size_t((key * 0x9E3779B97F4A7C15ull) >> shift);; i = (i + 1) & mask) {
        if (stamps[i] != stamp) {
            stamps[i] = stamp;
            keys[i] = key;
            values[i] = value;
            if (inserted) *inserted = true;
            return values[i];
        }
        if (keys[i] == key) {
            if (inserted) *inserted = false;
            return values[i];
        }
    }
}

const SceneInterpolator::Decomp& SceneInterpolator::DecompOf(PictureDecomps& pic, size_t item) {
    Decomp& d = pic.decomps[item];
    if (!pic.have[item]) {
        pic.have[item] = 1;
        double m[16];
        ToDouble(pic.items[item].transform->world, m);
        d.ok = DecomposeTrs(m, d.t, d.q, d.s);
    }
    return d;
}

bool SceneInterpolator::PairFits(const InterpScene& prev, const InterpScene& cur) const {
    return plan_.keyed && prev.ordinal && cur.ordinal && plan_.prev_ordinal == prev.ordinal && plan_.cur_ordinal == cur.ordinal &&
           plan_.prev_items == prev.items && plan_.cur_items == cur.items && plan_.prev_count == prev.count && plan_.cur_count == cur.count;
}

namespace {

template <typename Slots, typename Slot>
Slot& ClaimDecomps(Slots& slots, const InterpScene& scene, const Slot* keep) {
    for (Slot& d : slots)
        if (scene.ordinal && d.ordinal == scene.ordinal && d.items == scene.items && d.count == scene.count) return d;
    Slot& d = &slots[0] == keep ? slots[1] : slots[0];
    d.items = scene.items;
    d.count = scene.count;
    d.ordinal = scene.ordinal;
    d.have.assign(scene.count, 0);
    if (d.decomps.size() < scene.count) d.decomps.resize(scene.count);
    return d;
}
}

void SceneInterpolator::BuildPlan(const InterpScene& prev, const InterpScene& cur, bool have_camera) {
    PairPlan& plan = plan_;
    plan.valid = true;
    plan.have_camera = have_camera;
    plan.objects.clear();
    plan.items.assign(cur.count, PlanItem{});
    plan.scratch = 0;
    plan.meshes = 0;
    plan.bone_floats = 0;

    prev_by_key_.Reset(prev.count);
    prev_rigid_.Reset(prev.count);
    prev_instances_.clear();
    prev_groups_.clear();
    for (size_t i = 0; i < prev.count; ++i) {
        const InterpItem& it = prev.items[i];
        if (it.tag.id == 0 || it.tag.kind == InterpKind::Newest) continue;
        if (it.tag.kind == InterpKind::Vertices) prev_by_key_.Insert(Key(it.tag), uint32_t(i), nullptr);
        else if (it.transform) {
            bool fresh = false;
            const uint32_t g = prev_rigid_.Insert(it.tag.id, uint32_t(prev_groups_.size()), &fresh);
            if (fresh) prev_groups_.emplace_back();
            PrevGroup& group = prev_groups_[g];
            const uint64_t key = InstanceKey(it.tag, it.transform->world);
            bool seen = false;
            for (uint32_t e = group.head; e != UINT32_MAX && !seen; e = prev_instances_[e].next) seen = prev_instances_[e].key == key;
            if (!seen) {
                const uint32_t e = uint32_t(prev_instances_.size());
                prev_instances_.push_back(PrevInstance{i, key, {double(it.transform->world[12]), double(it.transform->world[13]), double(it.transform->world[14])}});
                if (group.tail == UINT32_MAX) group.head = e;
                else prev_instances_[group.tail].next = e;
                group.tail = e;
                group.size++;
            }
        }
    }
    PictureDecomps& dp = ClaimDecomps(decomps_, prev, static_cast<const PictureDecomps*>(nullptr));
    PictureDecomps& dc = ClaimDecomps(decomps_, cur, &dp);
    object_index_.Reset(cur.count);
    run_checks_.clear();

    for (size_t i = 0; i < cur.count; ++i) {
        const InterpItem& it = cur.items[i];
        PlanItem& pi = plan.items[i];
        if (it.tag.id != 0 && it.tag.kind == InterpKind::Rigid && it.transform && have_camera) {
            const uint64_t inst = InstanceKey(it.tag, it.transform->world);
            bool fresh = false;
            uint32_t& index = object_index_.Insert(inst, uint32_t(plan.objects.size()), &fresh);
            if (fresh) {
                PlanObject ob;
                ob.cur_item = i;
                const double tb[3] = {double(it.transform->world[12]), double(it.transform->world[13]), double(it.transform->world[14])};
                double distance = 0;
                PrevInstance* p = NearestPrev(it.tag.id, tb, &distance);
                if (p && distance <= options_.object_snap_travel) {
                    p->taken = true;
                    ob.have_prev = true;
                    ob.prev_key = p->key;
                    ob.prev_item = p->item;
                }
                plan.objects.push_back(ob);
            }
            pi.object = index;
        } else if (it.tag.id != 0 && it.tag.kind == InterpKind::Vertices && it.bones) {

            const uint32_t* p = prev_by_key_.Find(Key(it.tag));
            const InterpItem* pv = p ? &prev.items[*p] : nullptr;
            if (pv && pv->bones && pv->mesh == it.mesh && pv->bone_vec4 == it.bone_vec4 && pv->disp_vec4 == it.disp_vec4 && pv->mesh_params && it.mesh_params) {
                pi.op = PlanOp::SkinLerp;
                pi.prev_item = *p;
                pi.at = plan.meshes++;
                pi.bones_at = plan.bone_floats;
                plan.bone_floats += size_t(it.bone_vec4 + it.disp_vec4) * 4;
            }
        } else if (it.tag.id != 0 && it.tag.kind == InterpKind::Vertices) {
            const uint32_t* p = prev_by_key_.Find(Key(it.tag));
            if (p) { pi.vertex_prev = true; pi.prev_item = *p; }
            if (p && prev.items[*p].count == it.count && it.count > 0) {
                run_checks_.push_back(RunCheck{i, *p, false});
            }
        }
    }

    {
        constexpr uint32_t kObjectsPerTask = 48, kRunVerticesPerTask = 4096;
        plan_tasks_.clear();
        const uint32_t n = uint32_t(plan.objects.size());
        for (uint32_t o = 0; o < n; o += kObjectsPerTask) plan_tasks_.push_back(PlanTask{o, std::min(n, o + kObjectsPerTask), 0, 0});
        for (uint32_t r = 0; r < run_checks_.size();) {
            uint32_t end = r;
            size_t vertices = 0;
            while (end < run_checks_.size() && (end == r || vertices + cur.items[run_checks_[end].item].count <= kRunVerticesPerTask))
                vertices += cur.items[run_checks_[end++].item].count;
            plan_tasks_.push_back(PlanTask{0, 0, r, end});
            r = end;
        }
        PlanJob job{this, &prev, &cur, &dp, &dc};
        if (parallel_ && participants_ > 1 && plan_tasks_.size() > 1) parallel_(parallel_runner_, plan_tasks_.size(), &PlanObjects, &job);
        else for (size_t k = 0; k < plan_tasks_.size(); ++k) PlanObjects(&job, k);
    }

    for (const RunCheck& rc : run_checks_) {
        if (rc.steady) plan.items[rc.item].op = PlanOp::Lerp;
    }
    for (size_t i = 0; i < cur.count; ++i) {
        const InterpItem& it = cur.items[i];
        PlanItem& pi = plan.items[i];
        if (pi.object != UINT32_MAX) {
            const PlanObject& object = plan.objects[pi.object];
            if (object.ok && it.mesh && it.mesh_params) {
                pi.op = PlanOp::RigidMesh;
                pi.at = plan.meshes++;
            } else if (object.ok && !it.mesh) {
                pi.op = PlanOp::RigidVerts;
            }
        }
        if (pi.op == PlanOp::RigidVerts || pi.op == PlanOp::Lerp) {
            pi.at = plan.scratch;
            plan.scratch += it.count;
        }
    }

    auto weight = [&](size_t i) -> size_t {
        const PlanItem& pi = plan.items[i];
        size_t w = 1 + (pi.op == PlanOp::RigidVerts || pi.op == PlanOp::Lerp ? cur.items[i].count : 0) + (pi.op == PlanOp::RigidMesh ? 16 : 0) +
                   (pi.op == PlanOp::SkinLerp ? (cur.items[i].bone_vec4 + cur.items[i].disp_vec4) * 4 + 16 : 0);
        if ((pi.op == PlanOp::RigidVerts || pi.op == PlanOp::RigidMesh) && plan.objects[pi.object].cur_item == i) w += 64;
        return w;
    };
    size_t work = 0;
    for (size_t i = 0; i < cur.count; ++i) work += weight(i);
    constexpr size_t kChunkWork = 1024;
    size_t chunks = parallel_ && participants_ > 1 ? std::min<size_t>(work / kChunkWork, size_t(participants_) * 4) : 1;
    if (chunks < 1) chunks = 1;
    plan.chunks.clear();
    plan.chunks.push_back(0);
    size_t acc = 0, k = 1;
    for (size_t i = 0; i < cur.count; ++i) {
        acc += weight(i);
        if (k < chunks && acc * chunks >= work * k) {
            plan.chunks.push_back(i + 1);
            ++k;
        }
    }
    if (plan.chunks.back() != cur.count || plan.chunks.size() == 1) plan.chunks.push_back(cur.count);

    for (size_t c = 0; c + 1 < plan.chunks.size(); ++c)
        for (size_t i = plan.chunks[c]; i < plan.chunks[c + 1]; ++i) {
            PlanItem& pi = plan.items[i];
            if (pi.op != PlanOp::RigidVerts && pi.op != PlanOp::RigidMesh) continue;
            const size_t first = plan.objects[pi.object].cur_item;

            const PlanOp first_op = plan.items[first].op;
            const bool first_computes = first_op == PlanOp::RigidVerts || first_op == PlanOp::RigidMesh;
            pi.k_role = !first_computes ? 3 : (first == i ? 1 : (first >= plan.chunks[c] ? 2 : 3));
        }
}

void SceneInterpolator::PlanObjects(void* ctx, size_t chunk) {
    const PlanJob& job = *static_cast<const PlanJob*>(ctx);
    SceneInterpolator& self = *job.self;
    PairPlan& plan = self.plan_;
    const Options& options = self.options_;
    const PlanTask& task = self.plan_tasks_[chunk];
    for (uint32_t o = task.objects_begin; o < task.objects_end; ++o) {
        PlanObject& ob = plan.objects[o];
        if (!ob.have_prev) continue;
        const InterpItem& it = job.cur->items[ob.cur_item];

        const Decomp& da = DecompOf(*job.dp, ob.prev_item);
        const Decomp& db = DecompOf(*job.dc, ob.cur_item);
        if (!da.ok || !db.ok || AngleDegrees(da.q, db.q) > options.object_snap_angle_degrees) continue;
        std::memcpy(ob.ta, da.t, sizeof ob.ta); std::memcpy(ob.qa, da.q, sizeof ob.qa); std::memcpy(ob.sa, da.s, sizeof ob.sa);
        std::memcpy(ob.tb, db.t, sizeof ob.tb); std::memcpy(ob.sb, db.s, sizeof ob.sb);
        SlerpPrepare(da.q, db.q, ob.arc);

        {
            double mb[16], trs_b[16], trs_b_inv[16];
            ToDouble(it.transform->world, mb);
            ComposeTrs(db.t, db.q, db.s, trs_b);
            if (Invert4(trs_b, trs_b_inv)) {
                Multiply4(trs_b_inv, mb, ob.residual);
                ob.has_residual = true;
            }
        }
        double screen_d[16];
        ToDouble(it.transform->screen, screen_d);
        ob.ok = Invert4(screen_d, ob.screen_inv);
    }

    for (uint32_t r = task.runs_begin; r < task.runs_end; ++r) {
        RunCheck& rc = self.run_checks_[r];
        const InterpItem& it = job.cur->items[rc.item];
        const InterpItem& pv = job.prev->items[rc.prev];
        double cx = 0, cy = 0, worst = 0;
        for (uint32_t v = 0; v < it.count; ++v) {
            const double dx = double(it.verts[v].x) - double(pv.verts[v].x), dy = double(it.verts[v].y) - double(pv.verts[v].y);
            cx += dx; cy += dy;
            worst = std::max(worst, std::max(std::fabs(dx), std::fabs(dy)));
        }
        const double centroid = std::hypot(cx / double(it.count), cy / double(it.count));
        rc.steady = worst <= options.vertex_run_max_step_px && worst <= 2.0 * centroid + 4.0;
    }
}

void SceneInterpolator::ComputeK(const PlanObject& ob, double f, const double world_to_screen[16], double k[16]) {
    double t[3], q[4], sc[3], mf[16], kmat[16];
    for (int c = 0; c < 3; ++c) { t[c] = Lerp(ob.ta[c], ob.tb[c], f); sc[c] = Lerp(ob.sa[c], ob.sb[c], f); }
    SlerpAt(ob.qa, ob.arc, f, q);
    ComposeTrs(t, q, sc, mf);
    if (ob.has_residual) Multiply4(mf, ob.residual, mf);
    Multiply4(ob.screen_inv, mf, kmat);
    Multiply4(kmat, world_to_screen, k);
}

void SceneInterpolator::RunItems(void* ctx, size_t chunk) {
    const FrameJob& job = *static_cast<const FrameJob*>(ctx);
    SceneInterpolator& self = *job.self;
    PairPlan& plan = self.plan_;
    const InterpScene& prev = *job.prev;
    const InterpScene& cur = *job.cur;
    const double fraction = job.fraction;
    double own_k[16];
    uint32_t own_object = UINT32_MAX;
    for (size_t i = plan.chunks[chunk]; i < plan.chunks[chunk + 1]; ++i) {
        const InterpItem& it = cur.items[i];
        const PlanItem& pi = plan.items[i];
        InterpOutput o{it.state, it.verts, it.count, it.copy_frame, it.mesh, it.mesh_params, it.bones, it.bone_vec4, it.disp_vec4};
        const double* K = nullptr;
        if (pi.k_role == 1) {
            PlanObject& ob = plan.objects[pi.object];
            ComputeK(ob, fraction, job.world_to_screen, ob.k);
            K = ob.k;
        } else if (pi.k_role == 2) {
            K = plan.objects[pi.object].k;
        } else if (pi.k_role == 3) {
            if (own_object != pi.object) {
                ComputeK(plan.objects[pi.object], fraction, job.world_to_screen, own_k);
                own_object = pi.object;
            }
            K = own_k;
        }
        switch (pi.op) {
            case PlanOp::RigidMesh: {

                ps2e_draw_mesh_params mp = *it.mesh_params;
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c) {
                        double acc = 0;
                        for (int k = 0; k < 4; ++k) acc += double(it.mesh_params->screen[r * 4 + k]) * K[k * 4 + c];
                        mp.screen[r * 4 + c] = float(acc);
                    }

                mp.box[0] = mp.box[1] = -1e9f;
                mp.box[2] = mp.box[3] = 1e9f;
                self.mesh_scratch_[pi.at] = mp;
                o.mesh_params = &self.mesh_scratch_[pi.at];
                break;
            }
            case PlanOp::RigidVerts: {
                ps2e_draw_vertex* dst = self.scratch_.data() + pi.at;
                if (ReprojectRigid(it, K, cur.off_x, cur.off_y, cur.screen_scale, dst)) {
                    o.verts = dst;
                    o.count = it.count;
                }
                break;
            }
            case PlanOp::Lerp: {
                const InterpItem& pv = prev.items[pi.prev_item];
                ps2e_draw_vertex* dst = self.scratch_.data() + pi.at;
                for (uint32_t v = 0; v < it.count; ++v) {
                    ps2e_draw_vertex ov = it.verts[v];
                    ov.x = float(Lerp(pv.verts[v].x, it.verts[v].x, fraction));
                    ov.y = float(Lerp(pv.verts[v].y, it.verts[v].y, fraction));
                    ov.z = float(Lerp(pv.verts[v].z, it.verts[v].z, fraction));
                    dst[v] = ov;
                }
                o.verts = dst;
                o.count = it.count;
                break;
            }
            case PlanOp::SkinLerp: {
                const InterpItem& pv = prev.items[pi.prev_item];
                float* bones = self.bone_scratch_.data() + pi.bones_at;
                const uint32_t n = (it.bone_vec4 + it.disp_vec4) * 4;
                for (uint32_t k = 0; k < n; ++k) bones[k] = float(Lerp(pv.bones[k], it.bones[k], fraction));
                ps2e_draw_mesh_params mp = *it.mesh_params;
                for (int k = 0; k < 16; ++k) mp.screen[k] = float(Lerp(pv.mesh_params->screen[k], it.mesh_params->screen[k], fraction));
                mp.box[0] = mp.box[1] = -1e9f;
                mp.box[2] = mp.box[3] = 1e9f;
                self.mesh_scratch_[pi.at] = mp;
                o.mesh_params = &self.mesh_scratch_[pi.at];
                o.bones = bones;
                break;
            }
            case PlanOp::Newest:
                break;
        }
        (*job.out)[i] = o;
    }
}

bool SceneInterpolator::Interpolate(const InterpScene& prev, const InterpScene& cur, double fraction, std::vector<InterpOutput>& out) {

    const int rounding = std::fegetround();
    if (cache_rounding_ != rounding) {
        plan_.keyed = plan_.valid = false;
        for (auto& d : decomps_) d.ordinal = 0;
        cache_rounding_ = rounding;
    }
    fraction = std::min(1.0, std::max(0.0, fraction));
    out.clear();

    if (!PairFits(prev, cur)) {
        plan_.keyed = prev.ordinal && cur.ordinal;
        plan_.valid = false;
        plan_.prev_items = prev.items;
        plan_.cur_items = cur.items;
        plan_.prev_count = prev.count;
        plan_.cur_count = cur.count;
        plan_.prev_ordinal = prev.ordinal;
        plan_.cur_ordinal = cur.ordinal;
        PrepareCamera(prev.camera, cur.camera, options_, plan_.camera);
    }

    double to_camera[16], camera_to_screen[16];
    bool cut = false;
    const bool have_camera = CameraAt(plan_.camera, fraction, to_camera, camera_to_screen, &cut);
    if (cur.cut || cut) {
        out.reserve(cur.count);
        for (size_t i = 0; i < cur.count; ++i) {
            const InterpItem& it = cur.items[i];
            out.push_back(InterpOutput{it.state, it.verts, it.count, it.copy_frame, it.mesh, it.mesh_params, it.bones, it.bone_vec4, it.disp_vec4});
        }
        return false;
    }
    double world_to_screen[16];
    if (have_camera) Multiply4(to_camera, camera_to_screen, world_to_screen);

    if (!plan_.valid || plan_.have_camera != have_camera) BuildPlan(prev, cur, have_camera);
    if (!plan_.keyed) plan_.valid = false;

    if (scratch_.size() < plan_.scratch) scratch_.resize(plan_.scratch);
    if (mesh_scratch_.size() < plan_.meshes) mesh_scratch_.resize(plan_.meshes);
    if (bone_scratch_.size() < plan_.bone_floats) bone_scratch_.resize(plan_.bone_floats);
    out.resize(cur.count);
    const size_t chunks = plan_.chunks.size() - 1;
    FrameJob job{this, &prev, &cur, fraction, &out, world_to_screen};
    if (parallel_ && chunks > 1) parallel_(parallel_runner_, chunks, &RunItems, &job);
    else for (size_t k = 0; k < chunks; ++k) RunItems(&job, k);
    return true;
}

}
}
