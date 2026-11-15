#pragma once

namespace title {

class RoundingGuard final {
public:
    RoundingGuard() noexcept;
    ~RoundingGuard() noexcept;
    RoundingGuard(const RoundingGuard&) = delete;
    RoundingGuard& operator=(const RoundingGuard&) = delete;
private:
    int previous_;
};

}
