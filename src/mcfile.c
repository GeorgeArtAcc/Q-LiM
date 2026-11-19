/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.4.24: a surround file -> a stereo mix + Q-LiM surround points (see mcfile.h) */
#define _POSIX_C_SOURCE 200809L
#include "mcfile.h"
#include "wav.h"
#include "thread_prio.h"
#include <math.h>
#include <pthread.h>
#include <sndfile.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define WIN_SEC   0.1        /* one measurement every 0.1 s */
#define TOL_AZ    6.0f       /* thinning: a point is left out when the line through its neighbours misses it by less */
#define TOL_SP    0.08f
#define SILENT    1e-7       /* mean square below this (-70 dBFS) = silence: the position holds */

struct McJob {
    char        in[1024], out[1100];
    pthread_t   thr;
    bool        started;
    atomic_bool done, cancel;
    _Atomic float prog;
    int         rc;
    char        err[200];
    McResult    res;
};

/* the usual layouts by the number of channels (WAV order) */
static int dirs_by_count(int n, float *az, int *layout)
{
    static const float d4[4] = { -45, 45, -135, 135 };
    static const float d5[5] = { -30, 30, 0, -110, 110 };
    static const float d6[6] = { -30, 30, 0, NAN, -110, 110 };
    static const float d7[7] = { -30, 30, 0, -150, 150, -90, 90 };
    static const float d8[8] = { -30, 30, 0, NAN, -150, 150, -90, 90 };
    const float *d = NULL;
    int lay = SUR_L_RING8;
    switch (n) {
    case 4: d = d4; lay = SUR_L_QUAD; break;
    case 5: d = d5; lay = SUR_L_50; break;
    case 6: d = d6; lay = SUR_L_51; break;
    case 7: d = d7; lay = SUR_L_70; break;
    case 8: d = d8; lay = SUR_L_71; break;
    default: break;
    }
    for (int k = 0; k < n; ++k) az[k] = d ? d[k] : sur_wrap(360.0f * (float)k / (float)n);
    if (layout) *layout = lay;
    return n;
}

int mc_file_directions(const char *path, float *az, int max, int *layout)
{
    SF_INFO si;
    memset(&si, 0, sizeof si);
    SNDFILE *f = sf_open(path, SFM_READ, &si);
    if (!f) return 0;
    int n = si.channels;
    if (n < 1 || n > max) { sf_close(f); return 0; }
    dirs_by_count(n, az, layout);
    int map[64];
    if (n <= 64 && sf_command(f, SFC_GET_CHANNEL_MAP_INFO, map, (int)(sizeof(int) * (size_t)n)) == SF_TRUE) {
        bool side = false, back = false;
        for (int k = 0; k < n; ++k) {
            if (map[k] == SF_CHANNEL_MAP_SIDE_LEFT || map[k] == SF_CHANNEL_MAP_SIDE_RIGHT) side = true;
            if (map[k] == SF_CHANNEL_MAP_REAR_LEFT || map[k] == SF_CHANNEL_MAP_REAR_RIGHT) back = true;
        }
        float rear = side && back ? 150.0f : 110.0f, sd = side && back ? 90.0f : 110.0f;
        bool known = true;
        float a[64];
        for (int k = 0; k < n && known; ++k) {
            switch (map[k]) {
            case SF_CHANNEL_MAP_MONO: case SF_CHANNEL_MAP_CENTER: a[k] = 0; break;
            case SF_CHANNEL_MAP_LEFT: case SF_CHANNEL_MAP_FRONT_LEFT: a[k] = -30; break;
            case SF_CHANNEL_MAP_RIGHT: case SF_CHANNEL_MAP_FRONT_RIGHT: a[k] = 30; break;
            case SF_CHANNEL_MAP_FRONT_CENTER: a[k] = 0; break;
            case SF_CHANNEL_MAP_FRONT_LEFT_OF_CENTER: a[k] = -15; break;
            case SF_CHANNEL_MAP_FRONT_RIGHT_OF_CENTER: a[k] = 15; break;
            case SF_CHANNEL_MAP_LFE: a[k] = NAN; break;
            case SF_CHANNEL_MAP_REAR_LEFT: a[k] = -rear; break;
            case SF_CHANNEL_MAP_REAR_RIGHT: a[k] = rear; break;
            case SF_CHANNEL_MAP_REAR_CENTER: a[k] = 180; break;
            case SF_CHANNEL_MAP_SIDE_LEFT: a[k] = -sd; break;
            case SF_CHANNEL_MAP_SIDE_RIGHT: a[k] = sd; break;
            default: known = false; break;          /* height / ambisonic / unknown: the usual order */
            }
        }
        if (known) for (int k = 0; k < n; ++k) az[k] = a[k];
    }
    sf_close(f);
    return n;
}

const char *mc_channel_name(int n, int k)
{
    static const char *const n4[4] = { "FL", "FR", "BL", "BR" };
    static const char *const n5[5] = { "L", "R", "C", "Ls", "Rs" };
    static const char *const n6[6] = { "L", "R", "C", "LFE", "Ls", "Rs" };
    static const char *const n7[7] = { "L", "R", "C", "Lb", "Rb", "Ls", "Rs" };
    static const char *const n8[8] = { "L", "R", "C", "LFE", "Lb", "Rb", "Ls", "Rs" };
    static char t[16];
    if (k >= 0 && k < n) {
        if (n == 4) return n4[k];
        if (n == 5) return n5[k];
        if (n == 6) return n6[k];
        if (n == 7) return n7[k];
        if (n == 8) return n8[k];
    }
    snprintf(t, sizeof t, "Ch %d", k + 1);
    return t;
}

void mc_mix_path(const char *in, char *out, size_t n)
{
    if (wav_cache_path(in, "mcmix", out, n) != 0) snprintf(out, n, "%s.qlim-mix.wav", in);
}

/* where the sound is: the energy-weighted directions; the spread compared with a point between two speakers there */
static void locate(const double *e, const float *az, int n, const SurSetup *ref, float *o_az, float *o_sp)
{
    double vx = 0, vy = 0, sum = 0;
    for (int k = 0; k < n; ++k) {
        if (!(az[k] == az[k])) continue;
        double a = az[k] * M_PI / 180.0;
        vx += e[k] * sin(a); vy += e[k] * cos(a); sum += e[k];
    }
    if (sum <= 0) { *o_az = 0; *o_sp = 1; return; }
    double r = sqrt(vx * vx + vy * vy) / sum;
    float a = (float)(atan2(vx, vy) * 180.0 / M_PI);
    /* the same measure for an ideal point at a (what the panning would give) */
    float g[SUR_MAX_SPK];
    sur_gains(ref, a, 0.0f, g);
    double px = 0, py = 0, ps = 0;
    for (int k = 0; k < ref->n; ++k) {
        double w = (double)g[k] * g[k], b = ref->az[k] * M_PI / 180.0;
        px += w * sin(b); py += w * cos(b); ps += w;
    }
    double rp = ps > 0 ? sqrt(px * px + py * py) / ps : 1.0;
    double sp = rp > 1e-6 ? 1.0 - r / rp : 1.0;
    if (sp < 0.03) sp = 0;
    if (sp > 1) sp = 1;
    *o_az = sur_wrap(a);
    *o_sp = (float)sp;
}

/* thins the points out: keeps a point only where the line from the last kept one to the next would miss one by more
 * than the tolerance (angles unwrapped; never more than 170 degrees between two kept points - the short way round) */
static int thin(const float *t, const float *a, const float *sp, int n, SurPt *out, int max, float tol_a, float tol_s)
{
    if (n <= 0) return 0;
    int m = 0, i0 = 0;
    out[m++] = (SurPt){ t[0], sur_wrap(a[0]), sp[0], 0, 10, 0 };
    while (i0 < n - 1 && m < max) {
        int best = i0 + 1;
        for (int j = i0 + 2; j < n; ++j) {
            if (fabsf(a[j] - a[i0]) > 170.0f) break;
            bool ok = true;
            for (int q = i0 + 1; q < j && ok; ++q) {
                float u = (t[q] - t[i0]) / (t[j] - t[i0]);
                float la = a[i0] + u * (a[j] - a[i0]), ls = sp[i0] + u * (sp[j] - sp[i0]);
                if (fabsf(la - a[q]) > tol_a || fabsf(ls - sp[q]) > tol_s) ok = false;
            }
            if (!ok) break;
            best = j;
        }
        out[m++] = (SurPt){ t[best], sur_wrap(a[best]), sp[best], 0, 10, 0 };
        i0 = best;
    }
    return m;
}

static void fail(McJob *j, const char *msg) { snprintf(j->err, sizeof j->err, "%s", msg); j->rc = 1; }

static void *job_main(void *arg)
{
    McJob *j = arg;
    thread_prio_low();
    SF_INFO si;
    memset(&si, 0, sizeof si);
    SNDFILE *f = sf_open(j->in, SFM_READ, &si);
    float *buf = NULL, *mix = NULL, *ta = NULL, *aa = NULL, *sa = NULL;
    SNDFILE *o = NULL;
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", j->out);
    if (!f) { fail(j, "Cannot read the file"); goto done; }
    int n = si.channels;
    if (n < 3 || n > SUR_MAX_SPK) { fail(j, n < 3 ? "The file is not multichannel" : "Too many channels (16 at most)"); goto done; }
    McResult *r = &j->res;
    memset(r, 0, sizeof *r);
    r->nch = n;
    sf_close(f); f = NULL;
    mc_file_directions(j->in, r->az, SUR_MAX_SPK, &r->layout);
    f = sf_open(j->in, SFM_READ, &si);
    if (!f) { fail(j, "Cannot read the file"); goto done; }
    /* the reference speakers for the spread: the file's own directions (no LFE) */
    SurSetup ref;
    memset(&ref, 0, sizeof ref);
    for (int k = 0; k < n; ++k) if (r->az[k] == r->az[k]) { ref.az[ref.n] = r->az[k]; ref.out[ref.n] = ref.n; ref.n++; }
    if (ref.n < 2) { fail(j, "The file's channels have no directions"); goto done; }
    /* the stereo mix: left side -> L, right side -> R, the middle into both, the back -3 dB, no LFE */
    float gl[SUR_MAX_SPK], gr[SUR_MAX_SPK];
    for (int k = 0; k < n; ++k) {
        if (!(r->az[k] == r->az[k])) { gl[k] = gr[k] = 0; continue; }
        float x = sinf(r->az[k] * (float)M_PI / 180.0f);
        float l = 0.5f - x, rr = 0.5f + x;
        gl[k] = sqrtf(l < 0 ? 0 : l > 1 ? 1 : l);
        gr[k] = sqrtf(rr < 0 ? 0 : rr > 1 ? 1 : rr);
        if (fabsf(r->az[k]) > 90.0f) { gl[k] *= 0.7071f; gr[k] *= 0.7071f; }
    }
    SF_INFO so;
    memset(&so, 0, sizeof so);
    so.samplerate = si.samplerate; so.channels = 2;
    so.format = (si.frames > 250000000L ? SF_FORMAT_RF64 : SF_FORMAT_WAV) | SF_FORMAT_FLOAT;
    o = sf_open(tmp, SFM_WRITE, &so);
    if (!o) { fail(j, "Cannot write the stereo mix"); goto done; }
    long win = (long)(si.samplerate * WIN_SEC + 0.5);
    if (win < 64) win = 64;
    long nwin = (long)(si.frames / win) + 1;
    buf = malloc(sizeof(float) * (size_t)win * (size_t)n);
    mix = malloc(sizeof(float) * (size_t)win * 2);
    ta = malloc(sizeof(float) * (size_t)nwin);
    aa = malloc(sizeof(float) * (size_t)nwin);
    sa = malloc(sizeof(float) * (size_t)nwin);
    if (!buf || !mix || !ta || !aa || !sa) { fail(j, "Out of memory"); goto done; }
    int np = 0;
    long pos = 0;
    float last_a = 0, last_s = 0;
    bool any = false;
    double peak = 0;
    while (pos < si.frames) {
        if (atomic_load(&j->cancel)) { fail(j, "Cancelled"); goto done; }
        sf_count_t got = sf_readf_float(f, buf, win);
        if (got <= 0) break;
        double e[SUR_MAX_SPK] = { 0 }, tot = 0;
        for (sf_count_t i = 0; i < got; ++i) {
            const float *fr = buf + (size_t)i * (size_t)n;
            float L = 0, R = 0;
            for (int k = 0; k < n; ++k) { float v = fr[k]; if (!(v > -64 && v < 64)) v = 0; e[k] += (double)v * v; L += v * gl[k]; R += v * gr[k]; }
            mix[i * 2] = L; mix[i * 2 + 1] = R;
            if (fabs(L) > peak) peak = fabs(L);
            if (fabs(R) > peak) peak = fabs(R);
        }
        sf_writef_float(o, mix, got);
        for (int k = 0; k < n; ++k) if (r->az[k] == r->az[k]) tot += e[k] / (double)got;
        /* the energies of the channels with a direction, in the order of ref */
        double ee[SUR_MAX_SPK];
        float az2[SUR_MAX_SPK];
        int m = 0;
        for (int k = 0; k < n; ++k) if (r->az[k] == r->az[k]) { ee[m] = e[k]; az2[m] = r->az[k]; m++; }
        if (tot > SILENT) {
            float a, s2;
            locate(ee, az2, m, &ref, &a, &s2);
            if (any) {                                   /* unwrapped (no jump through +-180) */
                while (a - last_a > 180.0f) a -= 360.0f;
                while (a - last_a < -180.0f) a += 360.0f;
            }
            if (!any && np == 0 && pos > 0) {            /* silence before the first sound: it starts there */
                ta[np] = 0; aa[np] = a; sa[np] = s2; np++;
            }
            ta[np] = (float)((double)(pos + got / 2) / si.samplerate);
            aa[np] = a; sa[np] = s2; np++;
            last_a = a; last_s = s2; any = true;
        }
        pos += got;
        atomic_store(&j->prog, (float)((double)pos / (double)(si.frames > 0 ? si.frames : 1)));
    }
    (void)last_s;
    if (!any) { fail(j, "The file is silent"); goto done; }
    if (np >= 2) ta[np - 1] = (float)((double)si.frames / si.samplerate);   /* the last point at the end */
    /* a short moving average (3 windows) against the jitter of the measurement */
    if (np >= 3) {
        float *a2 = malloc(sizeof(float) * (size_t)np), *s2 = malloc(sizeof(float) * (size_t)np);
        if (a2 && s2) {
            for (int i = 0; i < np; ++i) {
                int i0 = i > 0 ? i - 1 : i, i1 = i + 1 < np ? i + 1 : i;
                float sa_ = 0, ss = 0; int c = 0;
                for (int q = i0; q <= i1; ++q) { sa_ += aa[q]; ss += sa[q]; c++; }
                a2[i] = sa_ / c; s2[i] = ss / c;
            }
            memcpy(aa, a2, sizeof(float) * (size_t)np);
            memcpy(sa, s2, sizeof(float) * (size_t)np);
        }
        free(a2); free(s2);
    }
    float ta_ = TOL_AZ, ts = TOL_SP;
    for (int tries = 0; tries < 8; ++tries) {
        r->npts = thin(ta, aa, sa, np, r->pts, SUR_MAX_PTS, ta_, ts);
        if (r->npts < SUR_MAX_PTS) break;
        ta_ *= 1.6f; ts *= 1.6f;
    }
    sf_close(o); o = NULL;
    remove(j->out);                                      /* (Windows: rename does not replace) */
    if (rename(tmp, j->out) != 0) { fail(j, "Cannot write the stereo mix"); goto done; }
    printf("mcfile: '%s' -> %d channels (%s), %d points, the mix peak %.1f dBFS\n", j->in, n, sur_layout_name(r->layout),
           r->npts, peak > 0 ? 20.0 * log10(peak) : -180.0);
    j->rc = 0;
done:
    if (f) sf_close(f);
    if (o) { sf_close(o); remove(tmp); }
    free(buf); free(mix); free(ta); free(aa); free(sa);
    atomic_store(&j->prog, 1.0f);
    atomic_store(&j->done, true);
    return NULL;
}

McJob *mc_job_start(const char *in, const char *mix_out)
{
    if (!in || !mix_out) return NULL;
    McJob *j = calloc(1, sizeof *j);
    if (!j) return NULL;
    snprintf(j->in, sizeof j->in, "%s", in);
    snprintf(j->out, sizeof j->out, "%s", mix_out);
    j->rc = 1;
    if (pthread_create(&j->thr, NULL, job_main, j) != 0) { free(j); return NULL; }
    j->started = true;
    return j;
}

bool  mc_job_done(McJob *j) { return !j || atomic_load(&j->done); }
float mc_job_progress(McJob *j) { return j ? atomic_load(&j->prog) : 1.0f; }
void  mc_job_cancel(McJob *j) { if (j) atomic_store(&j->cancel, true); }
const char *mc_job_error(McJob *j) { return j ? j->err : "No job"; }
int mc_job_result(McJob *j, McResult *r)
{
    if (!j || !atomic_load(&j->done)) return 1;
    if (j->rc == 0 && r) *r = j->res;
    return j->rc;
}
void mc_job_free(McJob *j)
{
    if (!j) return;
    if (j->started) pthread_join(j->thr, NULL);
    free(j);
}
