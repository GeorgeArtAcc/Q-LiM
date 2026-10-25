/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#define _DEFAULT_SOURCE
#include <sndfile.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void mk(const char *name, int fmt, int sr, int ch, long frames, int kind)
{
    SF_INFO in = { 0 }; in.samplerate = sr; in.channels = ch; in.format = fmt;
    SNDFILE *f = sf_open(name, SFM_WRITE, &in);
    if (!f) { printf("skip %s: %s\n", name, sf_strerror(NULL)); return; }
    double *b = malloc(sizeof(double) * (size_t)(frames > 0 ? frames : 1) * ch);
    for (long i = 0; i < frames; ++i) for (int c = 0; c < ch; ++c) {
        double v = 0.4 * sin(2 * M_PI * 220.0 * (c + 1) * i / sr) * ((i / (sr / 2 + 1)) % 2 ? 1 : 0.3);
        if (kind == 1) v = 0;                                  /* silence */
        if (kind == 2) v = (i % 100 == 0) ? NAN : v;           /* NaN samples */
        if (kind == 3) v = (i % 50 == 0) ? INFINITY : 8.0 * v; /* inf and far above 1.0 */
        if (kind == 4) v = 1.0;                                /* DC */
        b[i * ch + c] = v;
    }
    if (frames > 0) sf_writef_double(f, b, frames);
    sf_close(f); free(b);
    printf("made %s\n", name);
}
int main(void)
{
    int W = SF_FORMAT_WAV;
    mk("mono_u8_8k.wav", W | SF_FORMAT_PCM_U8, 8000, 1, 8000 * 3, 0);
    mk("st_24_96k.wav", W | SF_FORMAT_PCM_24, 96000, 2, 96000 * 4, 0);
    mk("st_32_192k.wav", W | SF_FORMAT_PCM_32, 192000, 2, 192000 * 3, 0);
    mk("st_float_44k.wav", W | SF_FORMAT_FLOAT, 44100, 2, 44100 * 5, 0);
    mk("st_double_48k.wav", W | SF_FORMAT_DOUBLE, 48000, 2, 48000 * 2, 0);
    mk("six_ch_48k.wav", W | SF_FORMAT_PCM_16, 48000, 6, 48000 * 3, 0);
    mk("one_frame.wav", W | SF_FORMAT_PCM_16, 48000, 2, 1, 0);
    mk("ten_frames.wav", W | SF_FORMAT_PCM_16, 48000, 2, 10, 0);
    mk("zero_frames.wav", W | SF_FORMAT_PCM_16, 48000, 2, 0, 0);
    mk("short_10ms.wav", W | SF_FORMAT_PCM_16, 48000, 2, 480, 0);
    mk("silence_20s.wav", W | SF_FORMAT_PCM_16, 48000, 2, 48000 * 20, 1);
    mk("nan_float.wav", W | SF_FORMAT_FLOAT, 48000, 2, 48000 * 3, 2);
    mk("inf_loud_float.wav", W | SF_FORMAT_FLOAT, 48000, 2, 48000 * 3, 3);
    mk("dc_full.wav", W | SF_FORMAT_PCM_16, 48000, 2, 48000 * 3, 4);
    mk("odd_rate_11025.wav", W | SF_FORMAT_PCM_16, 11025, 2, 11025 * 3, 0);
    mk("odd_rate_7999.wav", W | SF_FORMAT_PCM_16, 7999, 1, 7999 * 2, 0);
    mk("aiff_16.aiff", SF_FORMAT_AIFF | SF_FORMAT_PCM_16, 44100, 2, 44100 * 3, 0);
    mk("flac_16.flac", SF_FORMAT_FLAC | SF_FORMAT_PCM_16, 44100, 2, 44100 * 3, 0);
    mk("ogg.ogg", SF_FORMAT_OGG | SF_FORMAT_VORBIS, 44100, 2, 44100 * 3, 0);
    mk("long_10min.wav", W | SF_FORMAT_PCM_16, 48000, 2, 48000L * 600, 0);
    return 0;
}
