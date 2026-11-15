#include <cstdio>

#include "engine/cdvd/iso_fs.h"
#include "ps2e_seam.h"
#include "te_services.h"
#include "stream_service.h"

namespace te {

Disc::Disc(const Settings& settings) : iso_(new IsoFs), path_(settings.iso_path) {}
Disc::~Disc() { stream::Service().Reset(); }

int32_t Disc::Initialise() {
    if (opened_) return 1;
    if (tried_) return 0;
    tried_ = true;
    if (path_.empty()) { error_ = "no disc image (te::Settings::iso_path empty)"; return 0; }
    opened_ = iso_->Open(path_, &error_);
    if (!opened_) std::fprintf(stderr, "[reNsanity] disc: %s\n", error_.c_str());
    return opened_ ? 1 : 0;
}

int32_t Disc::SetMedia(int32_t media) {

    if (media != 1 && media != 2) return 0;
    media_ = media;
    return 1;
}

int32_t Disc::WaitReady() const { return opened_ ? 2 : 6; }

bool Disc::IsOpen() const { return opened_; }

}
