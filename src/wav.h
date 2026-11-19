/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef WAV_H
#define WAV_H

#include <stddef.h>
#include "stream.h"                     /* 2.0: StreamSource (wav_write_source) */

typedef struct {
    float  *samples;
    size_t  frame_count;
    int     sample_rate;
    int     channels;
    char    path[1024];
} WavFile;

int wav_load(const char *path, int target_sample_rate, WavFile *out);

/* 1.4.8: what the file itself is (only its header is read): sample rate, bits (0 = not PCM / not known, -32 / -64 =
 * floating point), channels. Returns 0, or -1 if libsndfile cannot read it (e.g. m4a - then nothing is shown). */
int wav_file_info(const char *path, int *sample_rate, int *bits, int *channels);

// File with a different sample rate -> stereo float WAV at target_sample_rate in ~/.cache/qlim/
// (windowed-sinc, streaming). If the cache already exists — returns its path. 0 = success.
int wav_convert_to_cache(const char *path, int target_sample_rate,
                         char *out_path, size_t out_size);

/* 2.1: a file libsndfile cannot read (a surround M4A ...): its ffmpeg copy in the cache with ALL its channels (made once,
 * buf holds the name); a readable file, or one ffmpeg cannot decode: `path` itself */
const char *wav_readable_all_channels(const char *path, int target_sample_rate, char *buf, size_t n);
// Format that libsndfile cannot read (m4a, aac, wma ...): converts it with ffmpeg (if installed).
int wav_convert_with_ffmpeg(const char *path, int target_sample_rate,
                            char *out_path, size_t out_size);

int wav_cache_path(const char *path, const char *kind, char *out_path, size_t out_size);   /* 1.4.24 */

/* 2.0: Collect All & Save can turn compressed files into files that play without decoding */
enum { WAV_OUT_KEEP = 0, WAV_OUT_FLOAT = 1, WAV_OUT_PCM24 = 2, WAV_OUT_FLAC24 = 3 };
/* writes the whole (already opened, stereo) source to out_path at sample rate sr in the given format; the source is
 * NOT closed. Through a temporary file, so a half written file never stays. 0 = success. */
int wav_write_source(StreamSource *src, int sr, int fmt, const char *out_path, volatile int *permille);   /* progress 0..1000 */

#endif
