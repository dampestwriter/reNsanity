#pragma once

class VuChopRoundGuard {
public:
    VuChopRoundGuard();
    ~VuChopRoundGuard();
    VuChopRoundGuard(const VuChopRoundGuard&) = delete;
    VuChopRoundGuard& operator=(const VuChopRoundGuard&) = delete;

private:
    int prev_;
};
