/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/*
 * 1.4.13: pitch shifting = time-stretch (Signalsmith Stretch) + an exact band-limited resampler.
 *
 * Why not the pitch mode of a library directly: measured (tests/test_pitch.c, real stems), the spectral pitch
 * modes leave either a mistuned note (Signalsmith's own transpose: 2..17 cents off, depending on where the
 * note falls between FFT bins) or more artefacts on chords / bass / voice and ~10x the CPU (Rubber Band R3).
 * The classic way gives the best of both: Signalsmith only changes the LENGTH (that is what it does cleanest,
 * the frequencies stay exact), then a windowed-sinc resampler plays it back faster/slower -> the pitch moves by
 * exactly the wanted ratio (0.00 cents) and the length comes back. With "formants" Signalsmith moves the
 * spectral envelope the opposite way first, so voices keep their natural colour.
 *
 *   input --(Signalsmith: length x ratio / speed)--> fifo --(sinc resampler: read `ratio` frames per frame)--> output
 */
#include "pitchshift.h"
#include "signalsmith-stretch.h"
#include <cmath>
#include <cstring>
#include <new>
#include <vector>

namespace {

/* windowed-sinc, 32 zero crossings each side (more when it has to cut lower), Kaiser beta 10 (~ -100 dB) */
const int    RS_HW    = 32;
const int    RS_NPH   = 512;
const double RS_BETA  = 10.0;
const double RS_CUT   = 0.95;
const size_t FIFO_CAP = 1u << 16;

double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0, q = x * x * 0.25;
    for (int k = 1; k < 60; ++k) {
        term *= q / ((double)k * (double)k);
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

struct Resampler {
    double ratio = 0.0;                   /* the table is for this ratio */
    int W = 0, taps = 0;
    std::vector<float> tab;               /* (RS_NPH + 1) * taps */

    void build(double r)
    {
        double c = (r > 1.0 ? 1.0 / r : 1.0) * RS_CUT;   /* reading faster -> cut lower, nothing folds back */
        W = (int)std::ceil(RS_HW / c);
        taps = 2 * W;
        tab.assign((size_t)(RS_NPH + 1) * (size_t)taps, 0.0f);
        double i0b = bessel_i0(RS_BETA);
        for (int p = 0; p <= RS_NPH; ++p) {
            double frac = (double)p / RS_NPH;
            float *row = &tab[(size_t)p * (size_t)taps];
            for (int t = 0; t < taps; ++t) {
                double x = (double)(t - W + 1) - frac;          /* distance of this input frame from the read point */
                double s = x == 0.0 ? c : std::sin(M_PI * c * x) / (M_PI * x);
                double u = x / (double)(W + 1);
                double w = u * u < 1.0 ? bessel_i0(RS_BETA * std::sqrt(1.0 - u * u)) / i0b : 0.0;
                row[t] = (float)(s * w);
            }
        }
        ratio = r;
    }
};

}  // namespace

struct PitchShift {
    signalsmith::stretch::SignalsmithStretch<float> st;
    Resampler rs;
    double semis = 0.0, ratio = 1.0;      /* ratio = 2^(semis/12) */
    bool   fmt = false;
    /* the stretched audio waiting for the resampler; fifo[i] is absolute frame base + i */
    std::vector<float> fl, fr;
    size_t len = 0;                       /* frames in the fifo */
    double pos = 0.0;                     /* the read point (relative to the fifo start) */
    std::vector<float> sl, sr;            /* Signalsmith output of one call */
};

static void ps_apply_formants(PitchShift *p)
{
    /* the resampler moves everything by `ratio`; moving the envelope by 1/ratio first keeps it where it was */
    p->st.setFormantFactor(p->fmt ? (float)(1.0 / p->ratio) : 1.0f, false);
}

extern "C" {

PitchShift *ps_new(int sample_rate)
{
    PitchShift *p = new (std::nothrow) PitchShift();
    if (!p) return nullptr;
    try {
        p->st.presetDefault(2, (float)(sample_rate > 0 ? sample_rate : 48000));
        p->st.setFormantBase(0);                       /* 0 = find the fundamental by itself */
        p->rs.build(1.0);
        p->fl.assign(FIFO_CAP, 0.0f);
        p->fr.assign(FIFO_CAP, 0.0f);
        p->sl.assign(FIFO_CAP / 2, 0.0f);
        p->sr.assign(FIFO_CAP / 2, 0.0f);
    } catch (...) { delete p; return nullptr; }
    ps_reset(p);
    return p;
}

void ps_free(PitchShift *p) { delete p; }

void ps_reset(PitchShift *p)
{
    if (!p) return;
    p->st.reset();
    /* W zeros in front: the first output frame can already look W frames back */
    p->len = (size_t)p->rs.W;
    std::fill(p->fl.begin(), p->fl.begin() + p->len, 0.0f);
    std::fill(p->fr.begin(), p->fr.begin() + p->len, 0.0f);
    p->pos = (double)(p->rs.W - 1);
}

void ps_set(PitchShift *p, double semitones, bool formants)
{
    if (!p) return;
    if (semitones != p->semis) {
        p->semis = semitones;
        p->ratio = std::pow(2.0, semitones / 12.0);
        /* (a wider kernel looks further back: there is always enough kept behind the read point, see 3) below) */
        try { p->rs.build(p->ratio); } catch (...) { return; }
        ps_apply_formants(p);
    }
    if (formants != p->fmt) { p->fmt = formants; ps_apply_formants(p); }
}

void ps_process(PitchShift *p, const float *inL, const float *inR, int nin, float *outL, float *outR, int nout)
{
    if (!p) return;
    const double r = p->ratio;
    const int W = p->rs.W, taps = p->rs.taps;
    /* 1) how many stretched frames the resampler needs for nout output frames */
    double end = p->pos + (double)nout * r;
    long need = (long)std::floor(end) + W + 1 - (long)p->len;
    if (need < 0) need = 0;
    if ((size_t)need > p->sl.size()) need = (long)p->sl.size();
    if (p->len + (size_t)need > FIFO_CAP) {                /* (never: the fifo is compacted below) */
        need = (long)(FIFO_CAP - p->len);
    }
    {
        const float *in[2] = { inL, inR };
        float *o[2] = { p->sl.data(), p->sr.data() };
        p->st.process(in, nin, o, (int)need);
    }
    std::memcpy(&p->fl[p->len], p->sl.data(), (size_t)need * sizeof(float));
    std::memcpy(&p->fr[p->len], p->sr.data(), (size_t)need * sizeof(float));
    p->len += (size_t)need;

    /* 2) the resampler: read `r` stretched frames per output frame */
    const float *tab = p->rs.tab.data();
    for (int i = 0; i < nout; ++i) {
        double ip = std::floor(p->pos);
        double frac = p->pos - ip;
        long i0 = (long)ip - W + 1;
        double ph = frac * RS_NPH;
        int pi = (int)ph;
        if (pi >= RS_NPH) pi = RS_NPH - 1;
        float a = (float)(ph - pi);
        const float *k0 = tab + (size_t)pi * (size_t)taps, *k1 = k0 + taps;
        float yl = 0.0f, yr = 0.0f;
        if (i0 >= 0 && i0 + taps <= (long)p->len) {
            const float *xl = &p->fl[(size_t)i0], *xr = &p->fr[(size_t)i0];
            for (int t = 0; t < taps; ++t) {
                float k = k0[t] + a * (k1[t] - k0[t]);
                yl += k * xl[t];
                yr += k * xr[t];
            }
        }
        outL[i] = yl;
        outR[i] = yr;
        p->pos += r;
    }
    /* 3) drop what is no longer needed (keep W frames behind the read point) */
    long keep_from = (long)std::floor(p->pos) - W;
    if (keep_from > 4096 && keep_from < (long)p->len) {
        size_t k = (size_t)keep_from;
        std::memmove(&p->fl[0], &p->fl[k], (p->len - k) * sizeof(float));
        std::memmove(&p->fr[0], &p->fr[k], (p->len - k) * sizeof(float));
        p->len -= k;
        p->pos -= (double)k;
    }
}

int ps_latency_in(const PitchShift *p) { return p ? p->st.inputLatency() : 0; }

/* in OUTPUT frames: Signalsmith's output latency and the resampler's look-ahead, both at the stretched rate */
int ps_latency_out(const PitchShift *p)
{
    if (!p) return 0;
    return (int)std::lround((double)(p->st.outputLatency() + p->rs.W) / p->ratio);
}

}
