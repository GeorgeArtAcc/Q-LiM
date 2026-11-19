/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef SEPARATE_H
#define SEPARATE_H

/*
 * 1.4.14 / 1.4.17: separation of a clip into parts - the vocals, and (1.4.17) the drums, bass, guitar and piano,
 * chosen by the user; what is not chosen stays together as "Other" ("Instrumental" when only the vocals are taken),
 * so the parts always add up to the original.
 *
 * "Invisible" until it is used: nothing here runs, loads or allocates while nobody asks for it. The neural networks
 * run in a SEPARATE program (qlim-separate, next to qlim) that Q-LiM starts only for a separation, so Q-LiM itself
 * never links ONNX Runtime and never holds a model in memory. A model is downloaded once, the first time it is
 * needed, after the user agrees, and can be removed again (File -> Separation...). 64-bit Linux / Windows only.
 *
 * The models:
 *   SEP_MEL     Mel-Band RoFormer - the vocals (the best there is for them). 443 MB, ~3 GB RAM.
 *   SEP_BSRSW   BS-RoFormer SW - 6 parts, the best quality. 336 MB, ~3.5 GB RAM, slow.
 *   SEP_DEMUCS  HTDemucs 6s - 6 parts, lighter and ~5x faster, a little less clean. 130 MB, ~2 GB RAM.
 * Only the vocals -> Mel-Band RoFormer. Other parts -> the chosen 6-part model; with "best vocals" the vocals come
 * from Mel-Band RoFormer first and the 6-part model works on the rest.
 *
 * The result is kept (one folder per song, in the chosen format), so the same choice for the same file is separated
 * only once; then it plays like any other audio (session_clip_set_stem_set).
 */

#include "session.h"
#include <stdbool.h>
#include <stddef.h>

enum { SEP_MEL, SEP_BSRSW, SEP_DEMUCS, SEP_NMODEL };
enum { SEP_P_VOCALS = 1, SEP_P_DRUMS = 2, SEP_P_BASS = 4, SEP_P_GUITAR = 8, SEP_P_PIANO = 16 };
#define SEP_NPART 5                                   /* the parts that can be chosen (+ the rest) */
#define SEP_P_ALL 31u
const char *sep_part_name(int i);                     /* 0..4: "Vocals", "Drums", "Bass", "Guitar", "Piano" */

typedef struct {
    unsigned parts;           /* SEP_P_* */
    bool     best_vocals;     /* vocals + something else: the vocals by Mel-Band RoFormer (better, takes longer) */
    int      multi;           /* SEP_BSRSW or SEP_DEMUCS: the 6-part model */
} SepRequest;

bool sep_supported(void);                         /* this build can do it (64-bit) and qlim-separate is there */
void sep_data_dir(char *out, size_t n);           /* where the models are kept */

/* the models: download size, on disk, remove (a removed one is downloaded again when it is needed) */
const char *sep_model_title(int m);
const char *sep_model_info(int m);                /* a short line: what it is good at, speed, licence */
double sep_model_download_mb(int m);
bool   sep_model_installed(int m);
double sep_model_disk_mb(int m);                  /* 0 = not there */
int    sep_model_remove(int m);                   /* 0 = removed */
bool   sep_runtime_installed(void);               /* ONNX Runtime */
double sep_runtime_disk_mb(void);
int    sep_runtime_remove(void);
bool   sep_busy_with_models(void);                /* a separation runs: nothing can be removed now */

int    sep_request_models(const SepRequest *r, int out[2]);   /* the models it runs, in order: how many (0 = nothing chosen) */
double sep_need_mb(const SepRequest *r);          /* what it must download first (0 = all there) */
int    sep_ram_need_mb(const SepRequest *r);      /* about how much RAM it takes */
int    sep_ram_total_mb(void);                    /* this computer's RAM (0 = unknown) */
int    sep_request_parts(const SepRequest *r, char names[][STEM_NAME_LEN]);  /* the names of the parts it makes, in order */

/* 1.4.15: where the separated tracks are kept and in what format (File -> Separation...);
 * saved in ~/.config/qlim/separation.conf. dir "" = the default (~/Music/Q-LiM Stems).
 * Every song gets its own folder: "<song>/<song> (Vocals).flac" ... (+ a hidden .qlim-source). */
typedef struct {
    char dir[1024];
    bool asked;               /* the folder was chosen once (asked on the first separation) */
    int  format, bits, rate, quality;   /* ExportOpts (export.h); rate 0 = 44.1 kHz, as the models make it */
    SepRequest last;          /* 1.4.17: the last choice (offered again) */
} SepPrefs;
void sep_prefs_get(SepPrefs *p);
void sep_prefs_set(const SepPrefs *p);
void sep_default_dir(char *out, size_t n);
void sep_stems_dir(char *out, size_t n);          /* the folder in use now */
void sep_usage(double *bytes, int *songs);        /* the space the separated tracks take */
int  sep_delete_all(void);                        /* deletes them (only the song folders Q-LiM made); returns how many */

/* the parts of this audio file for this choice, if it was separated so before (0 = found) */
int  sep_cached(const char *audio_path, const SepRequest *r, StemSet *out);

enum { SEP_DOWNLOADING = 1, SEP_PREPARING, SEP_SEPARATING, SEP_DONE, SEP_FAILED, SEP_CANCELLED };

typedef struct SepJob SepJob;
/* UI thread: starts downloading (if needed) and separating the clip's file on a background thread */
SepJob *sep_start(Session *s, int ch, int scene, const SepRequest *r, char *err, size_t err_size);
int     sep_state(SepJob *j, float *progress, char *text, size_t text_size);   /* SEP_* + a line for the user */
int     sep_step(SepJob *j, int *nsteps);         /* which model runs now (1..n) */
bool    sep_result(SepJob *j, StemSet *out);      /* after SEP_DONE */
void    sep_target(SepJob *j, int *ch, int *scene, char *path, size_t n);    /* which clip it was for */
void    sep_cancel(SepJob *j);
void    sep_free(SepJob *j);                      /* cancels if still running, waits, frees */

/* for tests: the helper program, the data folder and the stems folder can be overridden (NULL = the defaults) */
void sep_set_paths(const char *helper, const char *data_dir, const char *cache_dir);

#endif
