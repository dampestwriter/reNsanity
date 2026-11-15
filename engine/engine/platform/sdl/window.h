#pragma once

namespace ps2r {
namespace platform {

class Window {
public:
    Window() = default;
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool Create();
    void Destroy();

    void DrawableSize(int* w, int* h) const;

    int Width() const { return width_; }
    int Height() const { return height_; }

private:
    bool video_inited_ = false;
    int width_ = 0;
    int height_ = 0;
};

}
}
