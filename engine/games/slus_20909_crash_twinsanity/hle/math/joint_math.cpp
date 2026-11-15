#include "games/slus_20909_crash_twinsanity/hle/math/joint_math.h"

#include <cfenv>
#include <cmath>
#include <cstring>

#include "engine/vu/vu_float.h"

#include <arm_neon.h>

namespace title {

class JointMath::Rounding {
public:
    Rounding() : previous_(std::fegetround()) {
        if (previous_ != FE_TOWARDZERO) std::fesetround(FE_TOWARDZERO);
    }
    ~Rounding() {
        if (previous_ != FE_TOWARDZERO) std::fesetround(previous_);
    }
private:
    int previous_;
};

float JointMath::Float(uint32_t bits) {
    float result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}
float JointMath::Finite(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const auto magnitude = bits & 0x7fffffff;
    if (magnitude >= 0x7f800000) bits = (bits & 0x80000000) | 0x7f7fffff;
    else if (magnitude < 0x00800000) bits &= 0x80000000;
    return Float(bits);
}
float JointMath::Product(float a, float b) {
    volatile float left = Finite(a), right = Finite(b), result = left * right;
    return Finite(result);
}
float JointMath::Sum(float a, float b) {
    volatile float left = Finite(a), right = Finite(b), result = left + right;
    return Finite(result);
}
float JointMath::Difference(float a, float b) {
    volatile float left = Finite(a), right = Finite(b), result = left - right;
    return Finite(result);
}
float JointMath::MultiplyAdd(float a, float b, float sum) {
    volatile float left = Finite(a), right = Finite(b), accumulated = Finite(sum);

    return Finite(::VuFloat::Fma(left, right, accumulated));
}
float JointMath::Reciprocal(float value) {
    volatile float divisor = Finite(value);
    if (divisor == 0) return Float(std::signbit(divisor) ? 0xff7fffff : 0x7f7fffff);
    volatile float result = 1.0f / divisor;
    return Finite(result);
}

float JointMath::Fold(float angle) {
    const float magnitude = std::fabs(angle);
    const float scale = Float(0xbe22f987);
    const float magic = Float(0x4b400000);
    float residual = Product(magnitude, scale);
    residual = MultiplyAdd(-1, magic, residual);
    residual = MultiplyAdd(1, magic, residual);
    residual = MultiplyAdd(-magnitude, scale, residual);
    residual = MultiplyAdd(-1, 0.5f, residual);
    return Difference(std::fabs(residual), 0.25f);
}

float JointMath::Polynomial(float folded) {
    const float squared = Product(folded, folded);
    const float fourth = Product(squared, squared);
    const float eighth = Product(fourth, fourth);
    const float seventh = Product(Product(folded, Float(0xc2992661)), squared);
    float sum = Product(Product(folded, Float(0xc2255de0)), squared);
    sum = MultiplyAdd(seventh, fourth, sum);
    sum = MultiplyAdd(Product(folded, Float(0x42a33457)), fourth, sum);
    sum = MultiplyAdd(folded, Float(0x40c90fda), sum);
    return MultiplyAdd(Product(folded, Float(0x421ed7b7)), eighth, sum);
}

float JointMath::Arc(float cosine) {
    const float half_pi = Float(0x3fc90fdb);
    const float delta = Difference(Product(cosine, half_pi), half_pi);
    const float correction = Polynomial(Fold(delta));
    const float base = Difference(Product(cosine, -half_pi), -half_pi);
    const float adjusted = Difference(Product(correction, -half_pi), -half_pi);
    return Difference(Sum(base, base), adjusted);
}

float JointMath::Sine(float angle) {
    return Polynomial(Fold(Difference(angle, Float(0x3fc90fd8))));
}

namespace {
using Lanes = float __attribute__((vector_size(16)));
using LaneBits = uint32_t __attribute__((vector_size(16)));
constexpr LaneBits kAll{~0u, ~0u, ~0u, ~0u}, kXyz{~0u, ~0u, ~0u, 0}, kXy{~0u, ~0u, 0, 0};

float Bits2F(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
Lanes Load(const JointMath::Vector& v) {
    Lanes lanes;
    std::memcpy(&lanes, v.data(), sizeof(lanes));
    return lanes;
}
JointMath::Vector Store(Lanes lanes) {
    JointMath::Vector v;
    std::memcpy(v.data(), &lanes, sizeof(lanes));
    return v;
}
Lanes Splat(float value) { return Lanes{value, value, value, value}; }
Lanes Abs(Lanes value) { return (Lanes)((LaneBits)value & 0x7fffffffu); }
Lanes Fused(Lanes a, Lanes b, Lanes c) {
    return (Lanes)vfmaq_f32((float32x4_t)c, (float32x4_t)a, (float32x4_t)b);
}

template <typename T> void Pin(T& value) {
    asm volatile("" : "+w"(value) : : "memory");
}

class Tiny {
public:
    Lanes operator()(Lanes value, LaneBits used = kAll) {
        const LaneBits key = (((LaneBits)value << 1) - 1) | ~used;
        least_ = key < least_ ? key : least_;
        return value;
    }
    void Operand(Lanes value, LaneBits used = kAll) {
        const LaneBits doubled = (LaneBits)value << 1;
        const LaneBits key = ((doubled - 1) & ~(LaneBits)(doubled >= 0xff000000u)) | ~used;
        least_ = key < least_ ? key : least_;
    }
    bool Any() const {
        return vminvq_u32((uint32x4_t)least_) < 0xffffffu;
    }
private:
    LaneBits least_ = kAll;
};

Lanes Fold(Lanes angle, Tiny& tiny) {
    const Lanes magnitude = Abs(angle);
    const Lanes scale = Splat(Bits2F(0xbe22f987)), magic = Splat(Bits2F(0x4b400000));
    Lanes residual = tiny(magnitude * scale);
    residual = tiny(Fused(Splat(-1), magic, residual));
    residual = tiny(Fused(Splat(1), magic, residual));
    residual = tiny(Fused(-magnitude, scale, residual));
    residual = tiny(Fused(Splat(-1), Splat(0.5f), residual));
    return tiny(Abs(residual) - 0.25f);
}
Lanes Polynomial(Lanes folded, Tiny& tiny) {
    const Lanes squared = tiny(folded * folded);
    const Lanes fourth = tiny(squared * squared);
    const Lanes eighth = tiny(fourth * fourth);
    const Lanes seventh = tiny(tiny(folded * Bits2F(0xc2992661)) * squared);
    Lanes sum = tiny(tiny(folded * Bits2F(0xc2255de0)) * squared);
    sum = tiny(Fused(seventh, fourth, sum));
    sum = tiny(Fused(tiny(folded * Bits2F(0x42a33457)), fourth, sum));
    sum = tiny(Fused(folded, Splat(Bits2F(0x40c90fda)), sum));
    return tiny(Fused(tiny(folded * Bits2F(0x421ed7b7)), eighth, sum));
}
Lanes Sine(Lanes angle, Tiny& tiny) { return Polynomial(Fold(tiny(angle - Bits2F(0x3fc90fd8)), tiny), tiny); }
Lanes Arc(Lanes cosine, Tiny& tiny) {
    const float half_pi = Bits2F(0x3fc90fdb);
    const Lanes correction = Polynomial(Fold(tiny(tiny(cosine * half_pi) - half_pi), tiny), tiny);
    const Lanes base = tiny(tiny(cosine * -half_pi) - -half_pi);
    const Lanes adjusted = tiny(tiny(correction * -half_pi) - -half_pi);
    return tiny(tiny(base + base) - adjusted);
}

Lanes Reciprocal(Lanes divisor, Tiny& tiny, LaneBits used) {
    const LaneBits bits = (LaneBits)divisor;
    const LaneBits zero = (LaneBits)((bits << 1) == 0);
    const Lanes quotient = 1.0f / divisor;
    return tiny((Lanes)((zero & ((bits & 0x80000000u) | 0x7f7fffffu)) | (~zero & (LaneBits)quotient)), used);
}

bool Interpolate(const JointMath::Vector& from, const JointMath::Vector& to, float share,
                 JointMath::Interpolation& result) {
    Tiny tiny;
    const Lanes f = Load(from);
    Lanes target = Load(to);
    tiny.Operand(f);
    tiny.Operand(target);
    tiny.Operand(Splat(share));
    const Lanes products = tiny(f * target);
    Lanes cosine = Splat(products[0]);
    for (int lane = 1; lane < 4; ++lane) cosine = tiny(cosine + products[lane]);
    if (std::signbit(cosine[0])) {
        cosine = -cosine;
        target = -target;
    }
    result.curved = std::signbit(tiny(cosine - Bits2F(0x3f7fbe77))[0]);
    if (result.curved) {
        const Lanes angle = Arc(cosine, tiny);
        const float rest = tiny(Splat(1.0f) - share)[0];
        const Lanes scaled = tiny(Lanes{share, rest, 0, 0} * angle, kXy);
        const Lanes sines = Sine(Lanes{angle[0], scaled[0], scaled[1], angle[0]}, tiny);
        const float inverse = Reciprocal(Splat(sines[0]), tiny, kAll)[0];
        const Lanes weights = tiny(Lanes{sines[1], sines[2], 0, 0} * inverse, kXy);
        result.to_weight = weights[0];
        result.from_weight = weights[1];
    } else {
        result.to_weight = share;
        result.from_weight = tiny(Splat(1.0f) - share)[0];
    }
    result.rotation = Store(tiny(Fused(target, Splat(result.to_weight), tiny(f * result.from_weight))));
    return !tiny.Any();
}

bool RotationBasis(const JointMath::Vector& rotation, const JointMath::Vector& parent_scale,
                   const JointMath::Vector* own_scale, JointMath::Basis& result) {
    Tiny tiny;
    const Lanes r = Load(rotation), parent = Load(parent_scale);
    tiny.Operand(r);
    tiny.Operand(parent, kXyz);
    Lanes own{};
    if (own_scale) {
        own = Load(*own_scale);
        tiny.Operand(own, kXyz);
    }
    const Lanes scaled = tiny(r * Bits2F(0x3fb504f3));
    const Lanes doubled = tiny(Fused(r, r, tiny(r * r)));
    const Lanes cross = tiny(__builtin_shufflevector(scaled, scaled, 1, 2, 0, 3) *
                             __builtin_shufflevector(scaled, scaled, 2, 0, 1, 3));
    const Lanes w = __builtin_shufflevector(scaled, scaled, 3, 3, 3, 3);
    const Lanes positive = tiny(Fused(scaled, w, cross)), negative = tiny(Fused(-scaled, w, cross));
    const Lanes diagonal = tiny(Fused(Splat(-1), __builtin_shufflevector(doubled, doubled, 2, 2, 1, 3),
                                      tiny(1.0f - __builtin_shufflevector(doubled, doubled, 1, 0, 0, 3))));
    const Lanes moved = tiny(0.0f + negative);
    const Lanes inverse = tiny(0.0f + Reciprocal(parent, tiny, kXyz), kXyz);
    const Lanes x{inverse[0], 0, 0, 0}, y{0, inverse[1], 0, 0}, z{0, 0, inverse[2], 0};
    const auto row = [&](size_t index, Lanes coefficients) {
        if (own_scale) coefficients = tiny(coefficients * own[index]);
        Lanes sum = tiny(x * coefficients[0]);
        sum = tiny(Fused(y, Splat(coefficients[1]), sum));
        sum = tiny(Fused(z, Splat(coefficients[2]), sum));
        result[index] = Store(tiny(Fused(Splat(0), Splat(coefficients[3]), sum)));
    };
    row(0, Lanes{diagonal[0], positive[2], moved[1], 0});
    row(1, Lanes{negative[2], diagonal[1], positive[0], 0});
    row(2, Lanes{positive[1], moved[0], diagonal[2], 0});
    return !tiny.Any();
}

bool MultiplyByParent(const JointMath::Matrix& matrix, const JointMath::Matrix& parent, JointMath::Matrix& result) {
    Tiny tiny;
    const Lanes p0 = Load(parent[0]), p1 = Load(parent[1]), p2 = Load(parent[2]), p3 = Load(parent[3]);
    tiny.Operand(p0);
    tiny.Operand(p1);
    tiny.Operand(p2);
    tiny.Operand(p3);
    const auto row = [&](size_t index, LaneBits used) {
        Lanes m = Load(matrix[index]);
        tiny.Operand(m, used);
        if (index == 3) m[3] = 1;
        Lanes sum = tiny(p0 * m[0]);
        sum = tiny(Fused(p1, Splat(m[1]), sum));
        sum = tiny(Fused(p2, Splat(m[2]), sum));
        result[index] = Store(tiny(Fused(p3, Splat(m[3]), sum), used));
    };
    row(0, kAll);
    row(1, kAll);
    row(2, kAll);
    row(3, kXyz);
    result[3][3] = 1;
    return !tiny.Any();
}

bool Turn(const JointMath::Vector& by, const JointMath::Vector& rotation, Lanes& result) {
    Tiny tiny;
    const Lanes b = Load(by), r = Load(rotation);
    tiny.Operand(b);
    tiny.Operand(r);
    const Lanes products = tiny(b * r);
    Lanes accumulator = tiny(__builtin_shufflevector(b, b, 1, 2, 0, 3) * __builtin_shufflevector(r, r, 2, 0, 1, 3), kXyz);
    accumulator = tiny(Fused(b, __builtin_shufflevector(r, r, 3, 3, 3, 3), accumulator), kXyz);
    accumulator = tiny(Fused(r, __builtin_shufflevector(b, b, 3, 3, 3, 3), accumulator), kXyz);
    const Lanes xyz = tiny(Fused(-__builtin_shufflevector(r, r, 1, 2, 0, 3), __builtin_shufflevector(b, b, 2, 0, 1, 3),
                                 accumulator), kXyz);
    Lanes w = tiny(Splat(products[3]) - products[2]);
    w = tiny(Fused(Splat(-1), Splat(products[1]), w));
    w = tiny(Fused(Splat(-1), Splat(products[0]), w));
    result = Lanes{xyz[0], xyz[1], xyz[2], w[0]};
    return !tiny.Any();
}

bool EulerRotation(const JointMath::Vector& angles_now, const JointMath::Vector& angles_next,
                   const JointMath::Vector& move_now, const JointMath::Vector& move_next, JointMath::EulerResult& result) {
    Tiny tiny;
    const Lanes now = Load(angles_now), next = Load(angles_next), from = Load(move_now), to = Load(move_next);
    tiny.Operand(now, kXyz);
    tiny.Operand(next);
    tiny.Operand(from, kXyz);
    tiny.Operand(to, kXyz);
    const Lanes share = Splat(next[3]);
    const Lanes angles = tiny(Fused(next, share, tiny(Fused(-now, share, tiny(now + 0.0f, kXyz)), kXyz)), kXyz);
    const Lanes move = tiny(Fused(to, share, tiny(Fused(-from, share, tiny(from + 0.0f, kXyz)), kXyz)), kXyz);
    result.move = Store(Lanes{move[0], move[1], move[2], 1});
    const Lanes half = tiny(angles * 0.5f, kXyz);

    const Lanes moved = tiny(half - Bits2F(0x3fc90fd8), kXyz), added = tiny(0.0f + half, kXyz);
    const Lanes shifted = Polynomial(Fold(__builtin_shufflevector(moved, moved, 0, 1, 2, 0), tiny), tiny);
    const Lanes plain = Polynomial(Fold(__builtin_shufflevector(added, half * 1.0f, 0, 1, 6, 0), tiny), tiny);

    const Lanes pairs = tiny(Lanes{shifted[0], plain[0], shifted[0], plain[0]} *
                             Lanes{shifted[2], shifted[2], plain[2], plain[2]});
    const Lanes by_plain = tiny(pairs * plain[1]), by_shifted = tiny(pairs * shifted[1]);

    const Lanes terms = tiny(__builtin_shufflevector(by_plain, by_plain, 2, 0, 1, 3) +
                             (Lanes)((LaneBits)__builtin_shufflevector(by_shifted, by_shifted, 1, 3, 2, 0) ^
                                     LaneBits{0x80000000u, 0, 0x80000000u, 0}));
    const Lanes summed = tiny(0.0f + terms);
    result.rotation = Store(Lanes{terms[0], summed[1], summed[2], summed[3]});
    return !tiny.Any();
}
}

JointMath::Interpolation JointMath::Interpolate(const Vector& from, const Vector& to, float share) {
    Rounding rounding;
    Pin(share);
    Interpolation lanes;
    if (title::Interpolate(from, to, share, lanes)) return lanes;
    float cosine = Product(from[0], to[0]);
    for (size_t lane = 1; lane < from.size(); ++lane)
        cosine = Sum(cosine, Product(from[lane], to[lane]));
    auto target = to;
    if (std::signbit(cosine)) {
        cosine = Product(cosine, -1);
        for (auto& lane : target) lane = Product(lane, -1);
    }
    Interpolation result{};
    result.curved = std::signbit(Difference(cosine, Float(0x3f7fbe77)));
    if (result.curved) {
        const float angle = Arc(cosine);
        const float inverse = Reciprocal(Sine(angle));
        result.to_weight = Product(Sine(Product(share, angle)), inverse);
        result.from_weight = Product(Sine(Product(Difference(1, share), angle)), inverse);
    } else {
        result.to_weight = share;
        result.from_weight = Difference(1, share);
    }
    for (size_t lane = 0; lane < from.size(); ++lane)
        result.rotation[lane] = MultiplyAdd(target[lane], result.to_weight,
            Product(from[lane], result.from_weight));
    return result;
}

JointMath::Basis JointMath::RotationBasis(const Vector& rotation, const Vector& parent_scale,
                                        const Vector* own_scale) {
    Rounding rounding;
    Basis lanes;
    if (title::RotationBasis(rotation, parent_scale, own_scale, lanes)) return lanes;
    Vector scaled{}, doubled_square{}, positive_cross{}, negative_cross{};
    for (size_t lane = 0; lane < 4; ++lane)
        scaled[lane] = Product(rotation[lane], Float(0x3fb504f3));
    for (size_t lane = 0; lane < 3; ++lane) {
        doubled_square[lane] = MultiplyAdd(rotation[lane], rotation[lane],
            Product(rotation[lane], rotation[lane]));
        const float cross = Product(scaled[(lane + 1) % 3], scaled[(lane + 2) % 3]);
        positive_cross[lane] = MultiplyAdd(scaled[lane], scaled[3], cross);
        negative_cross[lane] = MultiplyAdd(-scaled[lane], scaled[3], cross);
    }
    const float diagonal_x = MultiplyAdd(-1, doubled_square[2], Difference(1, doubled_square[1]));
    const float diagonal_y = MultiplyAdd(-1, doubled_square[2], Difference(1, doubled_square[0]));
    const float diagonal_z = MultiplyAdd(-1, doubled_square[1], Difference(1, doubled_square[0]));
    Basis coefficients{{
        {diagonal_x, positive_cross[2], Sum(0, negative_cross[1]), 0},
        {negative_cross[2], diagonal_y, positive_cross[0], 0},
        {positive_cross[1], Sum(0, negative_cross[0]), diagonal_z, 0}}};
    if (own_scale)
        for (size_t row = 0; row < 3; ++row)
            for (auto& value : coefficients[row]) value = Product(value, (*own_scale)[row]);
    Vector inverse{};
    for (size_t lane = 0; lane < 3; ++lane)
        inverse[lane] = Sum(0, Reciprocal(parent_scale[lane]));
    Basis result{};
    for (size_t row = 0; row < 3; ++row) {
        for (size_t lane = 0; lane < 4; ++lane) {
            float sum = Product(lane == 0 ? inverse[0] : 0, coefficients[row][0]);
            sum = MultiplyAdd(lane == 1 ? inverse[1] : 0, coefficients[row][1], sum);
            sum = MultiplyAdd(lane == 2 ? inverse[2] : 0, coefficients[row][2], sum);
            result[row][lane] = MultiplyAdd(0, coefficients[row][3], sum);
        }
    }
    return result;
}

JointMath::Matrix JointMath::MultiplyByParent(const Matrix& matrix, const Matrix& parent) {
    Rounding rounding;
    Matrix lanes;
    if (title::MultiplyByParent(matrix, parent, lanes)) return lanes;
    Matrix result{};
    for (size_t row = 0; row < 4; ++row) {
        const size_t lanes = row == 3 ? 3 : 4;
        for (size_t lane = 0; lane < lanes; ++lane) {
            float sum = Product(parent[0][lane], matrix[row][0]);
            sum = MultiplyAdd(parent[1][lane], matrix[row][1], sum);
            sum = MultiplyAdd(parent[2][lane], matrix[row][2], sum);
            result[row][lane] = MultiplyAdd(parent[3][lane], row == 3 ? 1 : matrix[row][3], sum);
        }
    }
    result[3][3] = 1;
    return result;
}

JointMath::Vector JointMath::Turn(const Vector& by, const Vector& rotation) {

    Rounding rounding;
    Lanes lanes;
    if (title::Turn(by, rotation, lanes)) {
        Pin(lanes);
        return Store(lanes);
    }
    Vector products, accumulator, result;
    for (size_t lane = 0; lane < 4; ++lane) products[lane] = Product(by[lane], rotation[lane]);
    accumulator[0] = Product(by[1], rotation[2]);
    accumulator[1] = Product(by[2], rotation[0]);
    accumulator[2] = Product(by[0], rotation[1]);
    for (size_t lane = 0; lane < 3; ++lane) {
        accumulator[lane] = MultiplyAdd(by[lane], rotation[3], accumulator[lane]);
        accumulator[lane] = MultiplyAdd(rotation[lane], by[3], accumulator[lane]);
    }
    accumulator[3] = Difference(products[3], products[2]);
    accumulator[3] = MultiplyAdd(-1.0f, products[1], accumulator[3]);
    result[0] = MultiplyAdd(-rotation[1], by[2], accumulator[0]);
    result[1] = MultiplyAdd(-rotation[2], by[0], accumulator[1]);
    result[2] = MultiplyAdd(-rotation[0], by[1], accumulator[2]);
    result[3] = MultiplyAdd(-1.0f, products[0], accumulator[3]);
    return result;
}

JointMath::EulerResult JointMath::EulerRotation(const Vector& angles_now, const Vector& angles_next,
                                                const Vector& move_now, const Vector& move_next) {
    Rounding rounding;
    EulerResult lanes;
    if (title::EulerRotation(angles_now, angles_next, move_now, move_next, lanes)) return lanes;
    const float share = angles_next[3];

    Vector angles{}, move{};
    for (size_t lane = 0; lane < 3; ++lane) {
        float accumulated = Sum(angles_now[lane], 0.0f);
        accumulated = MultiplyAdd(-angles_now[lane], share, accumulated);
        angles[lane] = MultiplyAdd(angles_next[lane], share, accumulated);
        accumulated = Sum(move_now[lane], 0.0f);
        accumulated = MultiplyAdd(-move_now[lane], share, accumulated);
        move[lane] = MultiplyAdd(move_next[lane], share, accumulated);
    }
    move[3] = 1;

    const float half_x = Product(angles[0], 0.5f), half_y = Product(angles[1], 0.5f),
                half_z = Product(angles[2], 0.5f);
    const float shifted_x = Sine(half_x), plain_x = Polynomial(Fold(Sum(0.0f, half_x)));
    const float shifted_z = Sine(half_z), plain_z = Polynomial(Fold(Product(1.0f, half_z)));
    const float shifted_y = Sine(half_y), plain_y = Polynomial(Fold(Sum(0.0f, half_y)));

    const float ss = Product(shifted_x, shifted_z), ps = Product(plain_x, shifted_z);
    const float sp = Product(shifted_x, plain_z), pp = Product(plain_x, plain_z);
    const float ss_p = Product(ss, plain_y), ps_p = Product(ps, plain_y);
    const float sp_s = Product(sp, shifted_y), pp_s = Product(pp, shifted_y);
    const float ss_s = Product(ss, shifted_y), ps_s = Product(ps, shifted_y);
    const float sp_p = Product(sp, plain_y), pp_p = Product(pp, plain_y);
    const float zero = Difference(0.0f, 0.0f);
    EulerResult result{};
    result.move = move;
    result.rotation[0] = Difference(sp_p, ps_s);
    result.rotation[1] = Sum(zero, Sum(ss_p, pp_s));
    result.rotation[2] = Sum(zero, Difference(ps_p, sp_s));
    result.rotation[3] = Sum(zero, Sum(pp_p, ss_s));
    return result;
}

namespace {

float MacroClamp(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    if (((bits >> 23) & 0xffu) == 0xffu) bits = (bits & 0x80000000u) | 0x7f7fffffu;
    float result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}
}

float JointMath::ScalarSum(float a, float b) { volatile float r = MacroClamp(a) + MacroClamp(b); return MacroClamp(r); }
float JointMath::ScalarDifference(float a, float b) {
    volatile float r = MacroClamp(a) - MacroClamp(b);
    return MacroClamp(r);
}
float JointMath::ScalarProduct(float a, float b) { volatile float r = MacroClamp(a) * MacroClamp(b); return MacroClamp(r); }

JointMath::Vector JointMath::Lerp(const Vector& from, const Vector& to, float share) {
    Vector result{};
    for (size_t lane = 0; lane < 3; ++lane) {
        float accumulated = ScalarSum(from[lane], 0.0f);
        accumulated = ScalarDifference(accumulated, ScalarProduct(from[lane], share));
        result[lane] = ScalarSum(accumulated, ScalarProduct(to[lane], share));
    }
    result[3] = 1;
    return result;
}

}
