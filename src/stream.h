/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef STREAM_H
#define STREAM_H

/*
 * Q-LiM Lite — disk streaming of clips.
 *
 * Threads:
 *   UI thread     : stream_open/close/start/stop/invalidate_preroll
 *   disk thread   : fills the ring buffer (one thread for all streams)
 *   audio thread  : ONLY stream_read / stream_position / stream_finished / stream_stop
 *                   (lock-free, no malloc, no mutex)
 *
 * All positions are in frames at the session's sample rate.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The buffer sizes depend on the disk (3 tiers, see stream.c). These values are TIER 0 (fast disk). */
#define STREAM_RING_FRAMES     65536u    /* power of 2, ~1.4 s @ 48 kHz (SSD) */
#define STREAM_PREROLL_FRAMES  24000u    /* ~0.5 s in RAM for an instant start */
#define STREAM_CHUNK_FRAMES    4096u
#define STREAM_FROM_START      ((size_t)-1)

typedef struct ClipStream ClipStream;

/* The playback region — the same semantics as in audio.c:
 *   loop off: start .. end (end_marker; 0 = end of file)
 *   loop on : start .. loop_end, back to loop_start (0 = end of file) */
typedef struct {
    size_t start;
    size_t end;
    bool   loop;
    size_t loop_start;
    size_t loop_end;
} StreamRegion;

/* Called from the disk thread — must read atomics only. */
typedef void (*StreamRegionFn)(void *ud, StreamRegion *out);

/* Source abstraction: stereo float at the session's sample rate. */
typedef struct {
    size_t (*frames)(void *ctx);
    size_t (*read)(void *ctx, size_t pos, float *out, size_t n);
    void   (*close)(void *ctx);
    void    *ctx;
} StreamSource;

/* --- global --- */
int  streams_init(void);                    /* starts the disk thread; 0 = ok */
void streams_shutdown(void);
void streams_set_trim_margin(size_t frames);/* >= 2*audio buffer + 2048     */

/* --- buffer size according to the disk ---
 * Auto (0): for every clip we look at what kind of disk the file is on (NVMe/SSD -> small buffer, USB/SD/unknown -> medium,
 *           HDD/network -> large). If the buffer still runs dry during playback, the tier of THAT disk is raised
 *           (takes effect the next time its clips are started).
 * 1/2/3   : small/medium/large for all clips. */
void     streams_set_disk_mode(int mode);
int      streams_get_disk_mode(void);
unsigned long streams_starved_total(void);          /* how many times the buffer has run dry (since start) */
void     streams_status_text(char *buf, size_t n);  /* e.g. "SSD: small, HDD: large" */

/* --- UI thread --- */
ClipStream *stream_open(const StreamSource *src, StreamRegionFn fn, void *ud);
/* like stream_open, but with the file's path: the disk is recognised from it (path may be NULL) */
ClipStream *stream_open_path(const StreamSource *src, StreamRegionFn fn, void *ud, const char *path);
int         stream_tier(const ClipStream *s);           /* 0..2 */
size_t      stream_ring_frames(const ClipStream *s);
void        stream_close(ClipStream *s);    /* the audio thread must not use it */
size_t      stream_frames(const ClipStream *s);
bool        stream_start(ClipStream *s, size_t from);   /* STREAM_FROM_START = start_marker; false = out of memory */
void        stream_rebind(ClipStream *s, StreamRegionFn fn, void *ud); /* after the Clip was moved in memory */
void        stream_invalidate_preroll(ClipStream *s);   /* call when start_marker changes */
/* changing the length (cutting the clip, step 27). ONLY while streams_lock() is held. */
void        stream_set_total_nolock(ClipStream *s, size_t total);
void        streams_trim_idle(double idle_sec);
/* UI: locks the disk thread (while clips are moved in memory); rebind_nolock — only while it is locked */
void        streams_lock(void);
void        streams_unlock(void);
void        stream_rebind_nolock(ClipStream *s, StreamRegionFn fn, void *ud);   /* UI thread, ~once per second: frees the buffers of clips not used for a long time */

/* --- audio thread (and the UI for reading the position) --- */
void     stream_stop(ClipStream *s);
size_t   stream_read(ClipStream *s, float *out, size_t frames); /* returns the real frames, the rest = 0 */
size_t   stream_position(const ClipStream *s);  /* the next frame to play */
bool     stream_finished(const ClipStream *s);  /* end reached (loop off)  */
unsigned stream_underruns(const ClipStream *s);

/* --- libsndfile source (stream_sndfile.c) ---
 * native: opens the file as it is (any sample rate). Returns 0 or -1; *file_sr = the file's rate.
 * open  : like native, but if the file's rate != target_sr it wraps it in a live resampler.
 *         Nothing is written to disk. The result is stereo float at target_sr. */
int stream_source_sndfile_native(const char *path, StreamSource *out, int *file_sr);
/* 1 if the file (MP3) has no exact seek and must always be played through a cached WAV copy */
int stream_file_needs_cache(const char *path);
int stream_source_open(const char *path, int target_sr, StreamSource *out, int *file_sr);

/* --- live resampler (stream_resample.c) ---
 * Takes ownership of *inner (stereo float at src_sr) and returns a source at dst_sr.
 * Windowed-sinc (Kaiser), with an anti-alias filter when downsampling. Random access (stateless seek):
 * every output frame depends only on its position, so loop/markers/seek give the same audio. */
int stream_source_resample(const StreamSource *inner, int src_sr, int dst_sr, StreamSource *out);
void stream_resample_shutdown(void);   /* frees the filter cache */

#endif
