#include "audio_service.h"
#include "ps2e_seam.h"

extern "C" {

ps2e_s32 ps2e_sound_initialise_remote(void) { return te::audio::Service().InitialiseRemote(); }

}
