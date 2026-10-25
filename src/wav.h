/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef WAV_H
#define WAV_H

#include <stddef.h>

typedef struct {
    float  *samples;
    size_t  frame_count;
    int     sample_rate;
    int     channels;
    char    path[1024];
} WavFile;

int wav_load(const char *path, int target_sample_rate, WavFile *out);
void wav_free(WavFile *w);

// File with a different sample rate -> stereo float WAV at target_sample_rate in ~/.cache/qlim/
// (windowed-sinc, streaming). If the cache already exists — returns its path. 0 = success.
int wav_convert_to_cache(const char *path, int target_sample_rate,
                         char *out_path, size_t out_size);

// Format that libsndfile cannot read (m4a, aac, wma ...): converts it with ffmpeg (if installed).
int wav_convert_with_ffmpeg(const char *path, int target_sample_rate,
                            char *out_path, size_t out_size);

#endif
