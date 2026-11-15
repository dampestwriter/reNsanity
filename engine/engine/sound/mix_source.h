#pragma once

namespace ps2r::sound {

class MixSource {
public:
    virtual ~MixSource() = default;
    virtual void MixInto(float* stereo, int frames) = 0;
};

}
