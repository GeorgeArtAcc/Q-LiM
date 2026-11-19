/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef QLIM_MCFILE_H
#define QLIM_MCFILE_H
/*
 * 1.4.24: a surround (multichannel) file as Q-LiM's own surround path.
 *
 * In the background: every 0.1 s the loudness of each channel is measured; the channels' directions (from the file's
 * channel layout - L, R, C, Ls ... - or the usual order for its number of channels) weighted by their energy give where
 * the sound is (an angle) and how much it is spread (a point between two speakers = 0, the same in all = 1). The points
 * are then thinned out (only where the movement changes) - they are the clip's surround points, which can be edited.
 * The file is also mixed down to stereo (left side -> L, right side -> R, the middle into both, the back a little
 * quieter, the LFE left out): the clip plays that through the path. It is an estimate: a sound coming from two
 * places at once becomes one place. The original stays where it is (the clip can go back to it).
 */
#include "surround.h"
#include <stdbool.h>

typedef struct McJob McJob;

typedef struct {
    int    nch;                         /* the file's channels */
    int    layout;                      /* SUR_L_* that fits it (for the channel's speakers) */
    float  az[SUR_MAX_SPK];             /* the direction of every channel (degrees), LFE = NAN */
    int    npts;
    SurPt  pts[SUR_MAX_PTS];            /* the path (time in seconds of the file) */
} McResult;

/* in: the multichannel file; mix_out: the stereo WAV to write. NULL = could not start. */
McJob *mc_job_start(const char *in, const char *mix_out);
bool   mc_job_done(McJob *j);
float  mc_job_progress(McJob *j);        /* 0..1 */
void   mc_job_cancel(McJob *j);
int    mc_job_result(McJob *j, McResult *r);   /* 0 = ok; after done */
const char *mc_job_error(McJob *j);
void   mc_job_free(McJob *j);            /* waits for the thread */

/* the directions of a file's channels (from its channel layout or the usual order); returns the channels (0 = cannot read) */
int    mc_file_directions(const char *path, float *az, int max, int *layout);
/* the usual name of channel k of a file with n channels ("L", "C", "LFE", "Ls" ...; else "Ch k+1") */
const char *mc_channel_name(int n, int k);
/* the cache file name for the stereo mix of `in` (in Q-LiM's cache folder) */
void   mc_mix_path(const char *in, char *out, size_t n);

#endif
