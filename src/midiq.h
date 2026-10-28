/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef MIDIQ_H
#define MIDIQ_H
/*
 * 1.2: MIDI for playing instruments. The MIDI keyboard (RtMidi thread) and the program itself (UI thread: the
 * computer keyboard, tests) put raw messages here; the AUDIO thread takes them at the start of every block and
 * gives them to the instrument channel that is "armed" (Session.midi_target - the selected instrument channel).
 * One lock-free queue per writer, so nothing waits.
 */
#include <stdbool.h>

enum { MIDIQ_HW = 0, MIDIQ_UI = 1, MIDIQ_N };

void midiq_push(int src, unsigned char st, unsigned char d1, unsigned char d2);   /* any thread, one per src */
/* 1.3: to one channel directly (the MIDI editor's preview), not to the armed one */
void midiq_push_to(int src, int channel, unsigned char st, unsigned char d1, unsigned char d2);
/* the audio thread: out[i] = status, data1, data2, channel+1 (0 = the armed channel) */
int  midiq_pop(unsigned char (*out)[4], int max);
void midiq_clear(void);                                                           /* only while no audio runs */

#endif
