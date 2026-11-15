#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "engine/platform/ps2e/ps2e_draw.h"

namespace ps2r {
namespace ps2e {

enum class InterpKind : uint8_t { Newest = 0, Rigid = 1, Vertices = 2 };

struct InterpTag {
    uint64_t id = 0;
    uint32_t part = 0;
    InterpKind kind = InterpKind::Newest;
};

struct InterpTransform {
    float world[16];
    float screen[16];
};

struct InterpCamera {
    bool valid = false;
    float to_camera[16];
    float camera_to_screen[16];
};

struct InterpItem {
    const ps2e_draw_state* state = nullptr;
    const ps2e_draw_vertex* verts = nullptr;
    uint32_t count = 0;
    bool copy_frame = false;
    InterpTag tag;
    const InterpTransform* transform = nullptr;
    uint32_t mesh = 0;
    const ps2e_draw_mesh_params* mesh_params = nullptr;
    const float* bones = nullptr;
    uint32_t bone_vec4 = 0;
    uint32_t disp_vec4 = 0;
};

struct InterpScene {
    const InterpItem* items = nullptr;
    size_t count = 0;
    InterpCamera camera;
    float off_x = 0, off_y = 0;
    float screen_scale = 1;

    bool cut = false;
    uint64_t ordinal = 0;
};

struct InterpOutput {
    const ps2e_draw_state* state = nullptr;
    const ps2e_draw_vertex* verts = nullptr;
    uint32_t count = 0;
    bool copy_frame = false;
    uint32_t mesh = 0;
    const ps2e_draw_mesh_params* mesh_params = nullptr;
    const float* bones = nullptr;
    uint32_t bone_vec4 = 0;
    uint32_t disp_vec4 = 0;
};

class SceneInterpolator {
public:

    using ParallelRun = void (*)(void* runner, size_t chunks, void (*job)(void* ctx, size_t chunk), void* ctx);
    struct Options {
        double camera_cut_travel = 2.0;
        double camera_cut_angle_degrees = 30;
        double object_snap_travel = 4.0;
        double object_snap_angle_degrees = 60;

        double vertex_run_max_step_px = 64.0;
    };

    SceneInterpolator() = default;
    void SetOptions(const Options& options) { options_ = options; plan_.valid = false; plan_.keyed = false; }

    void SetParallel(ParallelRun run, void* runner, unsigned participants) {
        parallel_ = run;
        parallel_runner_ = runner;
        participants_ = participants ? participants : 1;
    }
    const Options& GetOptions() const { return options_; }

    bool Interpolate(const InterpScene& prev, const InterpScene& cur, double fraction, std::vector<InterpOutput>& out);


    void ResetHistory();

    static void Multiply4(const double a[16], const double b[16], double out[16]);
    static bool Invert4(const double m[16], double out[16]);

    static bool DecomposeTrs(const double m[16], double t[3], double q[4], double s[3]);
    static void ComposeTrs(const double t[3], const double q[4], const double s[3], double out[16]);
    static void Slerp(const double a[4], const double b[4], double f, double out[4]);

    struct SlerpArc { double b[4]; bool near = true; double omega = 0, sinom = 0; };
    static void SlerpPrepare(const double a[4], const double b[4], SlerpArc& arc);
    static void SlerpAt(const double a[4], const SlerpArc& arc, double f, double out[4]);
    static double AngleDegrees(const double a[4], const double b[4]);

private:

    static uint64_t Key(const InterpTag& tag);

    static uint64_t InstanceKey(const InterpTag& tag, const float world[16]);

    static bool ReprojectRigid(const InterpItem& item, const double K[16], float off_x, float off_y, float scale, ps2e_draw_vertex* out);

    struct Decomp { bool ok = false; double t[3], q[4], s[3]; };

    struct PictureDecomps {
        const InterpItem* items = nullptr;
        size_t count = 0;
        uint64_t ordinal = 0;
        std::vector<uint8_t> have;
        std::vector<Decomp> decomps;
    };
    PictureDecomps decomps_[2];
    static const Decomp& DecompOf(PictureDecomps& pic, size_t item);

    struct CameraPair {
        bool usable = false, cut = false;
        double vb[16], pa[16], pb[16], ta[3], tb[3], qa[4], qb[4], sa[3], sb[3];
    };
    static void PrepareCamera(const InterpCamera& a, const InterpCamera& b, const Options& options, CameraPair& pair);
    static bool CameraAt(const CameraPair& pair, double fraction, double to_camera[16], double camera_to_screen[16], bool* cut);

    struct PlanObject {
        bool ok = false;
        bool have_prev = false;
        bool has_residual = false;
        uint64_t prev_key = 0;
        size_t prev_item = 0, cur_item = 0;
        double ta[3], qa[4], sa[3], tb[3], sb[3];
        SlerpArc arc;
        double residual[16], screen_inv[16];
        double k[16];
    };
    enum class PlanOp : uint8_t { Newest, RigidMesh, RigidVerts, Lerp, SkinLerp };
    struct PlanItem {
        PlanOp op = PlanOp::Newest;
        uint8_t k_role = 0;

        uint32_t object = UINT32_MAX;
        size_t prev_item = 0;
        size_t at = 0;
        size_t bones_at = 0;
        bool vertex_prev = false;
    };
    struct PairPlan {
        bool keyed = false;
        bool valid = false;
        const InterpItem* prev_items = nullptr;
        const InterpItem* cur_items = nullptr;
        size_t prev_count = 0, cur_count = 0;
        uint64_t prev_ordinal = 0, cur_ordinal = 0;
        bool have_camera = false;
        CameraPair camera;
        std::vector<PlanObject> objects;
        std::vector<PlanItem> items;
        std::vector<size_t> chunks;
        size_t scratch = 0, meshes = 0, bone_floats = 0;
    };
    PairPlan plan_;
    int cache_rounding_ = -1;
    bool PairFits(const InterpScene& prev, const InterpScene& cur) const;
    void BuildPlan(const InterpScene& prev, const InterpScene& cur, bool have_camera);

    struct FrameJob {
        SceneInterpolator* self;
        const InterpScene* prev;
        const InterpScene* cur;
        double fraction;
        std::vector<InterpOutput>* out;
        const double* world_to_screen;
    };

    struct RunCheck { size_t item, prev; bool steady; };
    std::vector<RunCheck> run_checks_;
    struct PlanTask { uint32_t objects_begin, objects_end, runs_begin, runs_end; };
    std::vector<PlanTask> plan_tasks_;
    struct PlanJob { SceneInterpolator* self; const InterpScene* prev; const InterpScene* cur; PictureDecomps* dp; PictureDecomps* dc; };
    static void PlanObjects(void* ctx, size_t chunk);
    static void ComputeK(const PlanObject& ob, double fraction, const double world_to_screen[16], double k[16]);
    static void RunItems(void* ctx, size_t chunk);
    ParallelRun parallel_ = nullptr;
    void* parallel_runner_ = nullptr;
    unsigned participants_ = 1;

    Options options_;
    std::vector<ps2e_draw_vertex> scratch_;
    std::vector<ps2e_draw_mesh_params> mesh_scratch_;
    std::vector<float> bone_scratch_;

    struct FlatIndex {
        std::vector<uint64_t> keys;
        std::vector<uint32_t> values, stamps;
        uint32_t stamp = 0;
        int shift = 64;
        void Reset(size_t entries);
        const uint32_t* Find(uint64_t key) const;

        uint32_t& Insert(uint64_t key, uint32_t value, bool* inserted);
    };
    FlatIndex prev_by_key_;
    FlatIndex object_index_;

    struct PrevInstance { size_t item; uint64_t key; double t[3]; bool taken = false; uint32_t next = UINT32_MAX; };
    std::vector<PrevInstance> prev_instances_;
    struct PrevGroup { uint32_t head = UINT32_MAX, tail = UINT32_MAX, size = 0; };
    std::vector<PrevGroup> prev_groups_;
    FlatIndex prev_rigid_;

    PrevInstance* NearestPrev(uint64_t id, const double t[3], double* distance);
};

}
}
