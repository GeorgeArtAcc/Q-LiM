/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef EXPORT_H
#define EXPORT_H

/*
 * 1.4.14: audio export of a clip, from its start marker to its stop marker, the way it sounds:
 * cuts and fades, warp (tempo), pitch (+ formants), the clip's gain, pan and loudness normalization.
 *
 * It is rendered OFFLINE on its own thread from a fresh copy of the source (the clip can keep playing),
 * so it is as exact as possible and never "can't keep up". Nothing runs and nothing is allocated
 * while no export is in progress.
 *
 * Formats (only those this build's libsndfile can write are offered):
 *   WAV, AIFF  16 / 24 bit, 32 bit float      FLAC  16 / 24 bit
 *   Ogg Vorbis, Opus (48 kHz), MP3 (32 / 44.1 / 48 kHz)  - quality High / Medium / Small
 * The sample rate: like the session or 44.1 / 48 / 88.2 / 96 / 192 kHz (the band-limited converter of
 * stream_resample.c). 16 bit gets TPDF dither.
 */

#include "session.h"
#include <stdbool.h>
#include <stddef.h>

enum { EXP_WAV, EXP_AIFF, EXP_FLAC, EXP_OGG, EXP_OPUS, EXP_MP3, EXP_NFMT };
enum { EXP_Q_HIGH, EXP_Q_MEDIUM, EXP_Q_SMALL };

typedef struct {
    int format;          /* EXP_* */
    int bits;            /* 16, 24 or 32 (= 32-bit float); only for WAV / AIFF / FLAC */
    int sample_rate;     /* 0 = like the session */
    int quality;         /* EXP_Q_*; only for Ogg / Opus / MP3 */
    int stem;            /* a separated clip: 0 = as it is heard, k + 1 = only part k (at full level) */
    int sur;             /* 1.4.23: EXP_SUR_* - a clip through its surround path / a scene with all its outputs */
} ExportOpts;
/* 1.4.23: surround. A clip: one channel per speaker (5.1 / 7.1 with an empty LFE and the standard channel layout);
 * a scene: outputs 1 .. the highest one in use. ONE = one multi-channel file, SPLIT = a mono file for each
 * ("name - Stage C.wav", "name - Out 5.wav"). At the session's sample rate. */
enum { EXP_SUR_OFF = 0, EXP_SUR_ONE = 1, EXP_SUR_SPLIT = 2 };
bool        export_format_available_ch(int fmt, int channels);
int         export_scene_outputs(Session *s);    /* how many outputs a scene export in surround records */

const char *export_format_name(int fmt);          /* "WAV" ... */
const char *export_format_ext(int fmt);           /* "wav" ... */
bool        export_format_available(int fmt);     /* this build can write it */
bool        export_format_lossy(int fmt);
bool        export_format_has_bits(int fmt);
int         export_fix_rate(int fmt, int sr);     /* the nearest rate the format allows (Opus: 48 kHz ...) */
int         export_fix_bits(int fmt, int bits);

typedef struct ExportJob ExportJob;

/* UI thread. out_path: where to write (an existing file is replaced). NULL + err on failure. */
ExportJob *export_clip_start(Session *s, int ch, int scene, const ExportOpts *o, const char *out_path,
                             char *err, size_t err_size);
/* The same for a whole file that already is the final sound (e.g. a separated stem): only converted. */
ExportJob *export_file_start(const char *in_path, const ExportOpts *o, const char *out_path,
                             char *err, size_t err_size);
/* 1.4.17: Export Scene - stops everything, plays the scene at once and records the master (the faders, plugins and
 * master as they are, without the metronome) for `secs` from its first sound, then saves it in the format.
 * Real time: it takes as long as the scene. The UI stops the scene when it is done. */
ExportJob *export_scene_start(Session *s, int scene, double secs, const ExportOpts *o, const char *out_path,
                              char *err, size_t err_size);
int   export_phase(const ExportJob *j);              /* scene: 1 = recording, 2 = saving; 0 = a clip / file */
float export_progress(const ExportJob *j);         /* 0..1 */
bool  export_finished(const ExportJob *j);
/* after export_finished: 0 = ok; msg = a line for the user ("Exported ... (peak -0.3 dBFS)" / the error) */
int   export_result(const ExportJob *j, char *msg, size_t msg_size);
void  export_cancel(ExportJob *j);                 /* asks it to stop (the half-written file is removed) */
void  export_free(ExportJob *j);                   /* waits for the thread; then frees everything */

/* converts a finished file synchronously (1.4.15: the separated tracks). 0 = ok */
int   export_file_now(const char *in_path, const ExportOpts *o, const char *out_path, char *err, size_t err_size);
/* For tests and the separation helper: renders synchronously (no thread). 0 = ok. */
int   export_clip_now(const ClipSnapshot *c, StreamSource *src, const ExportOpts *o, const char *out_path,
                      float *peak_db, char *err, size_t err_size);

#endif
