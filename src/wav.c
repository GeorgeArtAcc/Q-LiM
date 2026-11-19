/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#define _POSIX_C_SOURCE 200809L
#include "wav.h"
#include "stream.h"
#include <sndfile.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Linear resampler: converts from src_rate to dst_rate
static float *resample_linear(const float *src, size_t src_frames,
                              int src_rate, int dst_rate,
                              size_t *out_frames)
{
    if (src_rate == dst_rate || src_frames == 0) {
        // no change
        float *copy = malloc(sizeof(float) * src_frames * 2);
        if (!copy) { *out_frames = 0; return NULL; }
        memcpy(copy, src, sizeof(float) * src_frames * 2);
        *out_frames = src_frames;
        return copy;
    }

    double ratio = (double)src_rate / (double)dst_rate;
    size_t dst_frames = (size_t)((double)src_frames / ratio);
    if (dst_frames == 0) dst_frames = 1;

    float *dst = malloc(sizeof(float) * dst_frames * 2);
    if (!dst) { *out_frames = 0; return NULL; }

    for (size_t i = 0; i < dst_frames; ++i) {
        double src_pos = (double)i * ratio;
        size_t i0 = (size_t)src_pos;
        if (i0 >= src_frames) i0 = src_frames - 1;
        size_t i1 = i0 + 1;
        if (i1 >= src_frames) i1 = src_frames - 1;
        float frac = (float)(src_pos - (double)i0);

        // linear interpolation for L and R
        float l0 = src[i0 * 2 + 0];
        float r0 = src[i0 * 2 + 1];
        float l1 = src[i1 * 2 + 0];
        float r1 = src[i1 * 2 + 1];

        dst[i * 2 + 0] = l0 + (l1 - l0) * frac;
        dst[i * 2 + 1] = r0 + (r1 - r0) * frac;
    }

    *out_frames = dst_frames;
    return dst;
}

int wav_load(const char *path, int target_sample_rate, WavFile *out)
{
    if (!path || !out) return 1;
    memset(out, 0, sizeof(*out));
    snprintf(out->path, sizeof(out->path), "%s", path);

    char what[32];
    int kind = audio_sniff(path, what, sizeof what);  /* 1.4.14: say what it really is */
    if (kind == SNIFF_MISSING) { fprintf(stderr, "wav_load: '%s' does not exist or cannot be read\n", path); return 1; }
    if (!audio_sndfile_can_open(path)) { fprintf(stderr, "wav_load: '%s' is %s - needs ffmpeg\n", path, what[0] ? what : "an unknown format"); return 1; }
    SF_INFO info;
    memset(&info, 0, sizeof(info));
    SNDFILE *sf = sf_open(path, SFM_READ, &info);
    if (!sf) {
        fprintf(stderr, "wav_load: cannot open '%s': %s\n",
                path, sf_strerror(NULL));
        return 1;
    }

    sf_count_t frames = info.frames;
    if (frames <= 0) {
        fprintf(stderr, "wav_load: empty file '%s'\n", path);
        sf_close(sf);
        return 1;
    }

    int src_channels = info.channels;
    float *raw = malloc(sizeof(float) * frames * src_channels);
    if (!raw) { sf_close(sf); return 1; }

    sf_count_t got = sf_readf_float(sf, raw, frames);
    sf_close(sf);
    if (got != frames) {
        fprintf(stderr, "wav_load: read %ld of %ld frames\n",
                (long)got, (long)frames);
        free(raw);
        return 1;
    }

    // Convert to stereo
    float *stereo = malloc(sizeof(float) * frames * 2);
    if (!stereo) { free(raw); return 1; }

    for (sf_count_t i = 0; i < frames; ++i) {
        float l, r;
        if (src_channels == 1) {
            l = r = raw[i];
        } else if (src_channels == 2) {
            l = raw[i * 2 + 0];
            r = raw[i * 2 + 1];
        } else {
            l = raw[i * src_channels + 0];
            r = raw[i * src_channels + 1];
        }
        if (!(l > -64.0f && l < 64.0f)) l = (l >= 64.0f) ? 64.0f : (l <= -64.0f ? -64.0f : 0.0f);   /* NaN/inf -> 0 */
        if (!(r > -64.0f && r < 64.0f)) r = (r >= 64.0f) ? 64.0f : (r <= -64.0f ? -64.0f : 0.0f);
        stereo[i * 2 + 0] = l;
        stereo[i * 2 + 1] = r;
    }
    free(raw);

    // Resample if needed
    size_t final_frames = (size_t)frames;
    float *final_samples = stereo;

    if (target_sample_rate != info.samplerate) {
        printf("wav_load: resampling '%s' from %d Hz to %d Hz\n",
               path, info.samplerate, target_sample_rate);
        final_samples = resample_linear(stereo, (size_t)frames,
                                        info.samplerate, target_sample_rate,
                                        &final_frames);
        free(stereo);
        if (!final_samples) return 1;
    }

    out->samples     = final_samples;
    out->frame_count = final_frames;
    out->sample_rate = target_sample_rate;   // already at the target rate
    out->channels    = 2;

    printf("wav_load: OK '%s' — %zu frames, %d Hz, src_ch=%d\n",
           path, out->frame_count, out->sample_rate, src_channels);

    return 0;
}

int wav_file_info(const char *path, int *sample_rate, int *bits, int *channels)
{
    if (!path || !*path) return -1;
    if (!audio_sndfile_can_open(path)) return -1;     /* 1.4.14 */
    SF_INFO info;
    memset(&info, 0, sizeof(info));
    SNDFILE *sf = sf_open(path, SFM_READ, &info);
    if (!sf) return -1;
    sf_close(sf);
    int b = 0;
    switch (info.format & SF_FORMAT_SUBMASK) {
    case SF_FORMAT_PCM_S8: case SF_FORMAT_PCM_U8: b = 8; break;
    case SF_FORMAT_PCM_16: b = 16; break;
    case SF_FORMAT_PCM_24: b = 24; break;
    case SF_FORMAT_PCM_32: b = 32; break;
    case SF_FORMAT_FLOAT:  b = -32; break;
    case SF_FORMAT_DOUBLE: b = -64; break;
    default: b = 0; break;                            /* MP3, Vorbis, Opus ...: no bit depth */
    }
    if (sample_rate) *sample_rate = info.samplerate;
    if (bits) *bits = b;
    if (channels) *channels = info.channels;
    return 0;
}


// ============ Cache for converted files ============

#include "stream.h"
#ifndef _WIN32
#include <sys/wait.h>
#endif
#include <fcntl.h>

#define CACHE_QUALITY_VERSION 3   // 1 = old linear resampler; 2 = windowed-sinc; 3 (1.4.7) = better sinc + MP3 without holes. Change -> new caches.

static uint64_t fnv1a(uint64_t h, const void *data, size_t len)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < len; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

static int cache_path_for(const char *path, int target_sr, const char *kind,
                          char *out_path, size_t out_size)
{
    struct stat st;
    if (stat(path, &st) != 0) return 1;

    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/.cache", home);
    mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/.cache/qlim", home);
    mkdir(dir, 0755);

    uint64_t h = 1469598103934665603ULL;
    h = fnv1a(h, path, strlen(path));
    long long sz = (long long)st.st_size, mt = (long long)st.st_mtime;
    int ver = CACHE_QUALITY_VERSION;
    h = fnv1a(h, &sz, sizeof(sz));
    h = fnv1a(h, &mt, sizeof(mt));
    h = fnv1a(h, &target_sr, sizeof(target_sr));
    h = fnv1a(h, &ver, sizeof(ver));
    h = fnv1a(h, kind, strlen(kind));

    snprintf(out_path, out_size, "%s/%016llx.wav", dir, (unsigned long long)h);
    return 0;
}

/* 1.4.24: a cache file name for something made from `path` (kind = what it is) */
int wav_cache_path(const char *path, const char *kind, char *out_path, size_t out_size)
{
    return cache_path_for(path, 0, kind, out_path, out_size);
}

// File with a different sample rate -> stereo float WAV at target_sr (streaming, through the resampler; no copy in RAM).
int wav_convert_to_cache(const char *path, int target_sample_rate,
                         char *out_path, size_t out_size)
{
    if (!path || !out_path || out_size == 0) return 1;
    if (cache_path_for(path, target_sample_rate, "sinc", out_path, out_size) != 0) return 1;
    if (access(out_path, R_OK) == 0) return 0;          // already converted

    StreamSource src;
    int file_sr = 0;
    if (stream_source_open(path, target_sample_rate, &src, &file_sr) != 0) return 1;

    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", out_path);

    SF_INFO info;
    memset(&info, 0, sizeof(info));
    info.samplerate = target_sample_rate;
    info.channels   = 2;
    info.format     = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
    SNDFILE *sf = sf_open(tmp, SFM_WRITE, &info);
    if (!sf) {
        fprintf(stderr, "wav_convert_to_cache: cannot write '%s': %s\n", tmp, sf_strerror(NULL));
        src.close(src.ctx);
        return 1;
    }

    size_t total = src.frames(src.ctx), pos = 0;
    enum { CH = 16384 };
    float *buf = malloc((size_t)CH * 2 * sizeof(float));
    int ok = buf != NULL;
    while (ok && pos < total) {
        size_t n = CH;
        if (n > total - pos) n = total - pos;
        size_t got = src.read(src.ctx, pos, buf, n);
        if (got == 0) { ok = 0; break; }
        if (sf_writef_float(sf, buf, (sf_count_t)got) != (sf_count_t)got) ok = 0;
        pos += got;
    }
    free(buf);
    sf_close(sf);
    src.close(src.ctx);

    if (!ok || rename(tmp, out_path) != 0) { unlink(tmp); return 1; }
    printf("wav_convert_to_cache: '%s' %d Hz -> %d Hz (%zu frames)\n", path, file_sr, target_sample_rate, total);
    return 0;
}

/* 2.0: see wav.h */
int wav_write_source(StreamSource *src, int sr, int fmt, const char *out_path, volatile int *permille)
{
    if (!src || !out_path || sr <= 0) return 1;
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", out_path);
    SF_INFO info;
    memset(&info, 0, sizeof info);
    info.samplerate = sr;
    info.channels = 2;
    info.format = fmt == WAV_OUT_FLAC24 ? (SF_FORMAT_FLAC | SF_FORMAT_PCM_24)
                : fmt == WAV_OUT_PCM24  ? (SF_FORMAT_WAV | SF_FORMAT_PCM_24)
                :                         (SF_FORMAT_WAV | SF_FORMAT_FLOAT);
    SNDFILE *sf = sf_open(tmp, SFM_WRITE, &info);
    if (!sf) { fprintf(stderr, "wav_write_source: cannot write '%s': %s\n", tmp, sf_strerror(NULL)); return 1; }
    sf_command(sf, SFC_SET_CLIPPING, NULL, SF_TRUE);    /* integer formats: an over is clipped, never wrapped */
    size_t total = src->frames(src->ctx), pos = 0;
    enum { CH = 16384 };
    float *buf = malloc((size_t)CH * 2 * sizeof(float));
    int ok = buf != NULL;
    while (ok && pos < total) {
        size_t n = CH;
        if (n > total - pos) n = total - pos;
        size_t got = src->read(src->ctx, pos, buf, n);
        if (got == 0) { ok = 0; break; }
        if (sf_writef_float(sf, buf, (sf_count_t)got) != (sf_count_t)got) ok = 0;
        pos += got;
        if (permille && total) *permille = (int)((double)pos * 1000.0 / (double)total);
    }
    free(buf);
    if (sf_close(sf) != 0) ok = 0;
    if (!ok || rename(tmp, out_path) != 0) { unlink(tmp); return 1; }
    return 0;
}

// Formats that libsndfile cannot read (m4a, aac, wma, ape ...): ffmpeg, if installed.
// No shell (fork + execvp), so file names cannot be "injected".
/* 2.1: keep = 1 keeps all the file's channels (a surround M4A ...), 0 = stereo */
static int ffmpeg_decode(const char *path, int target_sample_rate, int keep, char *out_path, size_t out_size)
{
    if (!path || !out_path || out_size == 0) return 1;
    if (cache_path_for(path, target_sample_rate, keep ? "ffmpegch" : "ffmpeg", out_path, out_size) != 0) return 1;
    if (access(out_path, R_OK) == 0) return 0;

    /* 1.4.7: decoded at the file's own rate (32-bit float, nothing lost); a different rate is then converted by our
     * resampler (better than ffmpeg's default one) */
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp.wav", out_path);

#ifdef _WIN32
    {
        /* ffmpeg.exe next to qlim.exe, or anywhere in PATH */
        char exe[2048];
        snprintf(exe, sizeof exe, "%s/ffmpeg.exe", qw_exe_dir());
        const char *prog = access(exe, F_OK) == 0 ? exe : "ffmpeg";
        const char *const argv2[] = { prog, "-v", "error", "-nostdin", "-y", "-i", path,
                                      "-vn", "-ac", "2", "-c:a", "pcm_f32le", "-f", "wav", tmp, NULL };
        const char *const argvk[] = { prog, "-v", "error", "-nostdin", "-y", "-i", path,
                                      "-vn", "-c:a", "pcm_f32le", "-f", "wav", tmp, NULL };
        const char *const *argv = keep ? argvk : argv2;
        int rc = qw_run_wait(argv, 1);
        if (rc != 0) {
            unlink(tmp);
            if (rc < 0) fprintf(stderr, "wav_convert_with_ffmpeg: ffmpeg is not installed\n");
            return 1;
        }
    }
#else
    pid_t pid = fork();
    if (pid < 0) return 1;
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); if (dn > 2) close(dn); }
        char *argv2[] = { "ffmpeg", "-v", "error", "-nostdin", "-y", "-i", (char *)path,
                          "-vn", "-ac", "2", "-c:a", "pcm_f32le", "-f", "wav", tmp, NULL };
        char *argvk[] = { "ffmpeg", "-v", "error", "-nostdin", "-y", "-i", (char *)path,
                          "-vn", "-c:a", "pcm_f32le", "-f", "wav", tmp, NULL };
        execvp("ffmpeg", keep ? argvk : argv2);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        unlink(tmp);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
            fprintf(stderr, "wav_convert_with_ffmpeg: ffmpeg is not installed\n");
        return 1;
    }
#endif
    if (rename(tmp, out_path) != 0) { unlink(tmp); return 1; }
    printf("wav_convert_with_ffmpeg: '%s' decoded (for %d Hz%s)\n", path, target_sample_rate, keep ? ", all channels" : "");
    return 0;
}

int wav_convert_with_ffmpeg(const char *path, int target_sample_rate, char *out_path, size_t out_size)
{
    return ffmpeg_decode(path, target_sample_rate, 0, out_path, out_size);
}

const char *wav_readable_all_channels(const char *path, int target_sample_rate, char *buf, size_t n)
{
    if (!path || !path[0] || audio_sndfile_can_open(path) || access(path, R_OK) != 0) return path;
    return ffmpeg_decode(path, target_sample_rate, 1, buf, n) == 0 ? buf : path;
}
