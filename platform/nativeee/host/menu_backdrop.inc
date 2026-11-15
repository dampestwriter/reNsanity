struct MenuBackdrop {
    static constexpr size_t kLimit = 8 * 1024 * 1024;
    std::vector<uint8_t> previous, changed;
    size_t cursor = 0;
    bool valid = false, have_key = false, equal = false, overflow = false, independent = false, admit = false;
    int key_w = 0, key_h = 0; uint64_t key_res = 0;
    uint64_t considered = 0;
    void Add(const void* p, size_t n) {
        if (overflow) return;
        if (n > kLimit - cursor) { overflow = true; return; }
        if (equal && (cursor + n > previous.size() || (n && std::memcmp(previous.data() + cursor, p, n)))) {
            changed.assign(previous.begin(), previous.begin() + cursor);
            equal = false;
        }
        if (!equal && n) {
            const auto* bytes = static_cast<const uint8_t*>(p);
            changed.insert(changed.end(), bytes, bytes + n);
        }
        cursor += n;
    }
    template<class T> void Value(const T& t) { Add(&t, sizeof t); }

    static bool Covers(const Draw& d, int w, int h) {
        const auto& s = d.state;
        if (d.pending || d.mesh || d.copy_frame || s.blend || s.alpha_test || s.stencil ||
            s.dest_alpha_test || s.no_colour_write || s.depth_write ||
            s.depth_test != PS2E_DRAW_DEPTH_ALWAYS || s.scissor_x0 || s.scissor_y0 ||
            s.scissor_x1 || s.scissor_y1 || d.verts.empty() || d.verts.size() % 3 ||
            d.verts.size() > 4095 || w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
        uint64_t census[3];
        if (s.texture && !ps2e_draw_texture_census(g.dev, s.texture, census)) return false;
        using Point = std::array<int32_t, 2>;
        using Edge = std::array<Point, 2>;
        thread_local std::vector<Edge> edges;
        edges.clear();
        edges.reserve(d.verts.size());
        const auto cross = [](Point a, Point b, Point p) {
            return int64_t(b[0] - a[0]) * (p[1] - a[1]) - int64_t(b[1] - a[1]) * (p[0] - a[0]);
        };
        for (size_t i = 0; i < d.verts.size(); i += 3) {
            Point p[3];
            for (int j = 0; j < 3; ++j) {
                const auto& v = d.verts[i + size_t(j)];
                if (!std::isfinite(v.x) || !std::isfinite(v.y) || std::abs(v.x) > 32768.f ||
                    std::abs(v.y) > 32768.f || !(v.z >= 0.f && v.z <= 16777215.f) || !std::isfinite(v.q) ||
                    !std::isfinite(v.u) || !std::isfinite(v.v)) return false;
                p[j] = {int32_t(v.x * 16.f), int32_t(v.y * 16.f)};
                if (float(p[j][0]) != v.x * 16.f || float(p[j][1]) != v.y * 16.f) return false;
            }
            const auto area = cross(p[0], p[1], p[2]);
            if (!area) continue;
            if (area < 0) std::swap(p[1], p[2]);
            for (int j = 0; j < 3; ++j) {
                Edge edge{p[j], p[(j + 1) % 3]}, reverse{edge[1], edge[0]};
                auto it = std::find(edges.begin(), edges.end(), reverse);
                if (it == edges.end()) edges.push_back(edge);
                else edges.erase(it);
            }
        }
        if (edges.empty()) return false;
        int winding = 0;
        const Point centre{w * 8, h * 8};
        for (const auto& e : edges) {
            const auto& a = e[0]; const auto& b = e[1];
            if (!(std::max(a[0], b[0]) < -16 || std::min(a[0], b[0]) > (w + 1) * 16 ||
                  std::max(a[1], b[1]) < -16 || std::min(a[1], b[1]) > (h + 1) * 16)) return false;
            if (a[1] <= centre[1] && b[1] > centre[1] && cross(a, b, centre) > 0) ++winding;
            if (b[1] <= centre[1] && a[1] > centre[1] && cross(a, b, centre) < 0) --winding;
        }
        return winding > 0;
    }

    bool Observe(Gfx::Bucket* buckets, int w, int h, bool sky_late, size_t split) {
        ++considered;
        if (considered == 1) {
            previous.reserve(kLimit); changed.reserve(kLimit);
        }
        cursor = 0; equal = have_key; overflow = false; changed.clear();
        const auto stamp = ps2e_draw_get_reuse_stamp(g.dev);
        key_w = w; key_h = h; key_res = stamp.resources;
        Value(w); Value(h); Value(stamp.resources); Value(sky_late); Value(split);
        const auto fp = te::ExactFp::Read();
        if (!fp.valid) { valid = have_key = independent = admit = false; return false; }
        Value(fp.bytes);
        bool permitted = true, stencil_reset = false;
        for (int b = 0; b < TE_GFX_BUCKET_UI; ++b) {
            Value(b); Value(buckets[b].used);
            for (size_t i = 0; i < buckets[b].used; ++i) {
                const Draw& d = buckets[b].draws[i];
                const auto& s = d.state;
                if (d.pending || d.copy_frame || s.texture == PS2E_DRAW_TEXTURE_FRAME_COPY) permitted = false;
                if (s.stencil == PS2E_DRAW_STENCIL_CLEAR) stencil_reset = true;
                else if ((s.stencil || s.dest_alpha_test) && !stencil_reset) permitted = false;
                uint8_t state[sizeof(ps2e_draw_state)], params[sizeof(ps2e_draw_mesh_params)];
                HashState(s, state); Add(state, PS2E_DRAW_STATE_BASE_BYTES);
                Value(s.texture); Value(d.mesh); Value(d.copy_frame); Value(d.verts.size());
                Add(d.verts.data(), d.verts.size() * sizeof(ps2e_draw_vertex));
                if (d.mesh) { HashParams(d.mesh_params, params); Add(params, sizeof params); Value(d.mesh_box); }
                if (!d.bones.empty()) Add(d.bones.data(), d.bones.size() * sizeof(float));
            }
        }
        if (equal && cursor != previous.size()) {
            changed.assign(previous.begin(), previous.begin() + cursor); equal = false;
        }
        int actual_w = 0, actual_h = 0;
        ps2e_draw_frame_size(g.dev, &actual_w, &actual_h);
        permitted = permitted && actual_w == w && actual_h == h;
        const bool same = equal && !overflow;
        if (!same) {

            independent = !overflow && permitted && stencil_reset && sky_late &&
                buckets[0].used && Covers(buckets[0].draws[0], w, h);
            if (!overflow) previous.swap(changed);
        }

        have_key = !overflow;
        admit = same && independent;
        const bool hit = valid && admit && ps2e_draw_backdrop(g.dev, 1);
        valid = hit;
        return hit;
    }

    bool SkipUsable(int w, int h) const {
        if (!valid || !have_key || w != key_w || h != key_h) return false;
        return ps2e_draw_get_reuse_stamp(g.dev).resources == key_res;
    }
    void Save() {
        if (admit && !overflow && ps2e_draw_backdrop(g.dev, 0)) valid = true;
    }
};
