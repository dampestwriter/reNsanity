#ifndef PS2E_MOVIE_H
#define PS2E_MOVIE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps2e_movie ps2e_movie;

typedef struct ps2e_movie_config {
    int audio_channel;
    int max_pictures;
    int yuv420;
} ps2e_movie_config;

ps2e_movie* ps2e_movie_create(const ps2e_movie_config* config);
void ps2e_movie_destroy(ps2e_movie* movie);

int ps2e_movie_feed(ps2e_movie* movie, const void* bytes, size_t size);

void ps2e_movie_end_input(ps2e_movie* movie);

typedef struct ps2e_movie_picture {
    int width;
    int height;
    const uint8_t* rgba8;
    const uint8_t* yuv420;
} ps2e_movie_picture;

int ps2e_movie_pictures_ready(const ps2e_movie* movie);

int ps2e_movie_take_picture(ps2e_movie* movie, ps2e_movie_picture* picture);

int ps2e_movie_ended(const ps2e_movie* movie);

double ps2e_movie_fps(const ps2e_movie* movie);

#define PS2E_MOVIE_PCM_RATE 48000

int ps2e_movie_audio_disabled(const ps2e_movie* movie);

uint32_t ps2e_movie_pcm_queued(const ps2e_movie* movie);

uint32_t ps2e_movie_pcm_read(ps2e_movie* movie, int16_t* stereo, uint32_t frames);

size_t ps2e_movie_yuv420_bytes(int width, int height);
int ps2e_movie_yuv_to_rgba(const uint8_t* yuv420, int width, int height, uint8_t* rgba8);

typedef struct ps2e_movie_yuv_tables_info {
    const uint8_t* rb;
    const uint8_t* g;
    uint32_t serial;
    double probe_ms;
} ps2e_movie_yuv_tables_info;
int ps2e_movie_yuv_tables(int width, int height, ps2e_movie_yuv_tables_info* out);

void ps2e_movie_yuv_tables_apply(const ps2e_movie_yuv_tables_info* tables, const uint8_t* yuv420, int width, int height,
                                 uint8_t* rgba8);

#ifdef __cplusplus
}
#endif

#endif
