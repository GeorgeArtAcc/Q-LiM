/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef MIXER_H
#define MIXER_H

#include "session.h"

/* Mixes `frames` frames from the session into `out` (interleaved float, out_ch channels).
 * Called from the audio callback, but does not depend on PortAudio — can be tested without a sound card.
 * Only the audio thread (or a test) may call it; it allocates no memory and locks nothing. */
void mixer_render(Session *s, float *out, unsigned long frames, int out_ch);

/* Graph mode (PipeWire, step 28): the routing and the master are links/nodes in the PipeWire graph.
 * mixer_render_channels: every channel (after volume/pan/mute/solo) -> outs[2c] (L) and outs[2c+1] (R).
 *   outs[k] may be NULL; channels without a buffer are not written. Also runs the queue and meters the channels.
 * mixer_render_master: the master input (the sum PipeWire made from the links) -> volume, balance,
 *   mute, metering -> output. in_l/in_r may be NULL (= silence). Called AFTER the channels in the same cycle. */
void mixer_render_channels(Session *s, float *const *outs, int nouts, unsigned long frames);
void mixer_render_master(Session *s, const float *in_l, const float *in_r, float *out_l, float *out_r,
                         unsigned long frames);

/* 1.4: the sound card's input for the NEXT mixer_render / mixer_render_channels call (same thread, same frame
 * count; interleaved, in_ch channels). in = NULL or in_ch = 0: no input (recording records silence, no monitoring). */
void mixer_set_input(const float *in, int in_ch, unsigned long frames);

#endif
