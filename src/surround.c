/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.4.19: surround panning and automation (see surround.h) */
#include "surround.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *const LAYOUT_NAMES[SUR_L_N] = { "Quad", "5.0", "5.1", "6 ring", "7.0", "7.1", "8 ring" };
const char *sur_layout_name(int l) { return l >= 0 && l < SUR_L_N ? LAYOUT_NAMES[l] : "Custom"; }

/* the speakers of each layout, in the order of the card outputs (offsets from `first`; -1 = skipped (LFE)) */
typedef struct { int n, outs; int off[SUR_MAX_SPK]; float az[SUR_MAX_SPK]; const char *nm[SUR_MAX_SPK]; } Lay;
static const Lay LAY[SUR_L_N] = {
    { 4, 4, { 0, 1, 2, 3 }, { -45, 45, -135, 135 }, { "FL", "FR", "RL", "RR" } },
    { 5, 5, { 0, 1, 2, 3, 4 }, { -30, 30, 0, -110, 110 }, { "L", "R", "C", "Ls", "Rs" } },
    { 5, 6, { 0, 1, 2, 4, 5 }, { -30, 30, 0, -110, 110 }, { "L", "R", "C", "Ls", "Rs" } },
    { 6, 6, { 0, 1, 2, 3, 4, 5 }, { -30, 30, 90, 150, -150, -90 }, { "1", "2", "3", "4", "5", "6" } },
    { 7, 7, { 0, 1, 2, 3, 4, 5, 6 }, { -30, 30, 0, -90, 90, -150, 150 }, { "L", "R", "C", "Ls", "Rs", "Lb", "Rb" } },
    { 7, 8, { 0, 1, 2, 4, 5, 6, 7 }, { -30, 30, 0, -90, 90, -150, 150 }, { "L", "R", "C", "Ls", "Rs", "Lb", "Rb" } },
    { 8, 8, { 0, 1, 2, 3, 4, 5, 6, 7 }, { -22.5f, 22.5f, 67.5f, 112.5f, 157.5f, -157.5f, -112.5f, -67.5f },
      { "1", "2", "3", "4", "5", "6", "7", "8" } },
};

void sur_layout(SurSetup *s, int layout, int first)
{
    if (layout < 0 || layout >= SUR_L_N) layout = SUR_L_QUAD;
    if (first < 0) first = 0;
    float w = s->width, a0 = s->az0, sp = s->spread0;
    bool keep = s->n > 0;
    memset(s, 0, sizeof *s);
    const Lay *L = &LAY[layout];
    s->n = L->n;
    for (int k = 0; k < L->n; ++k) { s->out[k] = first + L->off[k]; s->az[k] = L->az[k]; }
    s->layout = layout;
    if (keep) { s->width = w; s->az0 = a0; s->spread0 = sp; }
    /* 1.4.22: the same speakers on a room plan (if the user switches to the room mode) */
    s->mode = SUR_MODE_CIRCLE;
    s->room_w = 16; s->room_d = 26; s->stage_d = 7; s->x0 = 0; s->y0 = 10; s->w0 = 0;
    for (int k = 0; k < L->n; ++k) {
        float a = L->az[k] * (float)M_PI / 180.0f;
        s->x[k] = 7.0f * sinf(a); s->y[k] = 10.0f + 9.0f * cosf(a);
    }
}
const char *sur_speaker_name(int l, int k) { return l >= 0 && l < SUR_L_N && k >= 0 && k < LAY[l].n ? LAY[l].nm[k] : ""; }

/* 1.4.22: the room layouts (metres: x from the middle, y from the back wall; the hall 16 x 26, the stage the top 7 m) */
typedef struct { int n; float x[SUR_MAX_SPK], y[SUR_MAX_SPK]; const char *nm[SUR_MAX_SPK]; } RLay;
static const char *const RLAY_NAMES[SUR_R_N] = { "Theater (9)", "Quad", "Theater (6)" };
static const RLay RLAY[SUR_R_N] = {
    { 9, { -6, 6, -7.5f, 7.5f, -6, 6, -5, 5, 0 }, { 19.5f, 19.5f, 10, 10, 0.5f, 0.5f, 25, 25, 25.5f },
      { "Front L", "Front R", "House L", "House R", "Back L", "Back R", "Stage L", "Stage R", "Stage C" } },
    { 4, { -7, 7, -7, 7 }, { 18, 18, 1, 1 }, { "FL", "FR", "BL", "BR" } },
    { 6, { -6, 6, -7.5f, 7.5f, -6, 6 }, { 19.5f, 19.5f, 10, 10, 0.5f, 0.5f },
      { "Front L", "Front R", "House L", "House R", "Back L", "Back R" } },
};
const char *sur_room_layout_name(int l) { return l >= 0 && l < SUR_R_N ? RLAY_NAMES[l] : "Custom"; }
const char *sur_room_speaker_name(int l, int k) { return l >= 0 && l < SUR_R_N && k >= 0 && k < RLAY[l].n ? RLAY[l].nm[k] : ""; }
void sur_room_layout(SurSetup *s, int layout, int first)
{
    if (layout < 0 || layout >= SUR_R_N) layout = SUR_R_THEATER9;
    if (first < 0) first = 0;
    const RLay *L = &RLAY[layout];
    s->mode = SUR_MODE_ROOM;
    s->room_w = 16; s->room_d = 26; s->stage_d = 7;
    s->n = L->n;
    for (int k = 0; k < L->n; ++k) {
        s->out[k] = first + k; s->x[k] = L->x[k]; s->y[k] = L->y[k];
        s->az[k] = sur_wrap(atan2f(L->x[k], L->y[k] - 10.0f) * 180.0f / (float)M_PI);   /* (for the circle view) */
    }
    s->layout = layout;
    if (!(s->y0 > 0)) { s->x0 = 0; s->y0 = 10; }
}

float sur_wrap(float a)
{
    if (!(a == a)) return 0.0f;
    a = fmodf(a, 360.0f);
    if (a <= -180.0f) a += 360.0f;
    if (a > 180.0f) a -= 360.0f;
    return a;
}

/* 0..360 from `from` clockwise to `to` */
static float cw(float from, float to)
{
    float d = fmodf(to - from, 360.0f);
    if (d < 0) d += 360.0f;
    return d;
}

void sur_gains(const SurSetup *s, float az, float spread, float *g)
{
    int n = s->n;
    if (n <= 0) return;
    for (int k = 0; k < n; ++k) g[k] = 0.0f;
    if (n == 1) { g[0] = 1.0f; return; }
    az = sur_wrap(az);
    if (!(spread == spread) || spread < 0.0f) spread = 0.0f;
    if (spread > 1.0f) spread = 1.0f;
    /* the speaker just left of az (counter-clockwise) and the one just right of it */
    int a = -1, b = -1;
    float da = 1e9f, db = 1e9f;
    for (int k = 0; k < n; ++k) {
        float back = cw(s->az[k], az), fwd = cw(az, s->az[k]);
        if (back < da) { da = back; a = k; }
        if (fwd < db || (fwd == db && k != a)) { db = fwd; b = k; }
    }
    if (a < 0 || b < 0) return;
    if (a == b || da + db < 1e-4f) g[a] = 1.0f;                 /* right on a speaker */
    else {
        float f = da / (da + db);                              /* 0 at a, 1 at b */
        g[a] = cosf(f * (float)(M_PI * 0.5));
        g[b] = sinf(f * (float)(M_PI * 0.5));
    }
    if (spread > 0.0f) {                                       /* blend in all speakers, the power stays 1 */
        float u = 1.0f / (float)n;
        for (int k = 0; k < n; ++k) g[k] = sqrtf((1.0f - spread) * g[k] * g[k] + spread * u);
    }
}

/* DBAP: 1 / d^1.5 (9 dB per doubling of the distance), d with the blur radius; the powers summed = 1 */
void sur_gains_room(const SurSetup *s, float x, float y, float spread, float *g)
{
    int n = s->n;
    if (n <= 0) return;
    if (!(spread == spread) || spread < 0.0f) spread = 0.0f;
    if (spread > 1.0f) spread = 1.0f;
    float big = (s->room_w > s->room_d ? s->room_w : s->room_d);
    if (!(big > 0)) big = 20.0f;
    float rs = 0.3f + spread * big * 0.6f;
    double sum = 0.0;
    for (int k = 0; k < n; ++k) {
        float dx = x - s->x[k], dy = y - s->y[k];
        double d2 = (double)dx * dx + (double)dy * dy + (double)rs * rs;
        double gk = pow(d2, -0.75);                            /* (d^2)^-0.75 = d^-1.5 */
        g[k] = (float)gk;
        sum += gk * gk;
    }
    float nrm = sum > 0 ? (float)(1.0 / sqrt(sum)) : 0.0f;
    for (int k = 0; k < n; ++k) g[k] *= nrm;
}

void sur_eval_room(const SurPt *p, int n, float t, const SurSetup *s, float *x, float *y, float *w, float *spread)
{
    if (n <= 0 || !p) { *x = s->x0; *y = s->y0; *w = s->w0; *spread = s->spread0; return; }
    if (t <= p[0].t) { *x = p[0].x; *y = p[0].y; *w = p[0].w; *spread = p[0].spread; return; }
    if (t >= p[n - 1].t) { *x = p[n - 1].x; *y = p[n - 1].y; *w = p[n - 1].w; *spread = p[n - 1].spread; return; }
    int lo = 0, hi = n - 1;
    while (hi - lo > 1) { int m = (lo + hi) / 2; if (p[m].t <= t) lo = m; else hi = m; }
    float dt = p[hi].t - p[lo].t;
    float f = dt > 1e-6f ? (t - p[lo].t) / dt : 1.0f;
    *x = p[lo].x + (p[hi].x - p[lo].x) * f;
    *y = p[lo].y + (p[hi].y - p[lo].y) * f;
    *w = p[lo].w + (p[hi].w - p[lo].w) * f;
    *spread = p[lo].spread + (p[hi].spread - p[lo].spread) * f;
}

/* 1.4.23: the gains of the channel's left (gl) and right (gr) for every speaker at time t - the mixer and the export
 * use exactly this. vis (optional): az, spread, x, y, width of the position. */
void sur_pair_gains(const SurSetup *su, const SurPt *p, int n, float t, float *gl, float *gr, float *vis)
{
    float az = 0, sp = 0, rx = 0, ry = 0, rw = 0;
    if (su->mode == SUR_MODE_ROOM) {                   /* the room plan (DBAP); stereo towards the walls */
        sur_eval_room(p, n, t, su, &rx, &ry, &rw, &sp);
        if (rw >= 0.05f) {
            sur_gains_room(su, rx - rw * 0.5f, ry, sp, gl);
            sur_gains_room(su, rx + rw * 0.5f, ry, sp, gr);
        } else {
            sur_gains_room(su, rx, ry, sp, gl);
            for (int k = 0; k < su->n; ++k) { gl[k] *= 0.5f; gr[k] = gl[k]; }
        }
    } else {
        sur_eval(p, n, t, su->az0, su->spread0, &az, &sp);
        float w = su->width;
        if (w >= 1.0f) {                               /* left and right: two sources `width` degrees apart */
            sur_gains(su, az - w * 0.5f, sp, gl);
            sur_gains(su, az + w * 0.5f, sp, gr);
        } else {                                       /* one point: (L + R) / 2 */
            sur_gains(su, az, sp, gl);
            for (int k = 0; k < su->n; ++k) { gl[k] *= 0.5f; gr[k] = gl[k]; }
        }
    }
    if (vis) { vis[0] = az; vis[1] = sp; vis[2] = rx; vis[3] = ry; vis[4] = rw; }
}

/* 1.4.23: what a speaker is called (for file names): "L", "Stage C" ... or "Speaker 3" */
void sur_speaker_label(const SurSetup *su, int k, char *out, size_t n)
{
    const char *nm = "";
    if (su->mode == SUR_MODE_ROOM) nm = sur_room_speaker_name(su->layout, k);
    else nm = sur_speaker_name(su->layout, k);
    if (nm && nm[0]) snprintf(out, n, "%s", nm);
    else snprintf(out, n, "Speaker %d", k + 1);
}

void sur_sort(SurPt *p, int n)
{
    for (int i = 1; i < n; ++i) {
        SurPt v = p[i];
        int j = i;
        while (j > 0 && p[j - 1].t > v.t) { p[j] = p[j - 1]; --j; }
        p[j] = v;
    }
}

void sur_eval(const SurPt *p, int n, float t, float az0, float spread0, float *az, float *spread)
{
    if (n <= 0 || !p) { *az = az0; *spread = spread0; return; }
    if (t <= p[0].t) { *az = p[0].az; *spread = p[0].spread; return; }
    if (t >= p[n - 1].t) { *az = p[n - 1].az; *spread = p[n - 1].spread; return; }
    int lo = 0, hi = n - 1;                                    /* p[lo].t <= t < p[hi].t */
    while (hi - lo > 1) { int m = (lo + hi) / 2; if (p[m].t <= t) lo = m; else hi = m; }
    float dt = p[hi].t - p[lo].t;
    float f = dt > 1e-6f ? (t - p[lo].t) / dt : 1.0f;
    float d = sur_wrap(p[hi].az - p[lo].az);                   /* the short way round */
    *az = sur_wrap(p[lo].az + d * f);
    *spread = p[lo].spread + (p[hi].spread - p[lo].spread) * f;
}

static bool sur_setup_parse_(const char *txt, SurSetup *s);

int sur_mask_outputs(uint64_t mask, int def, int *o)
{
    if (mask == 0) { o[0] = def; return 1; }
    if (mask & SUR_MUTED) return 0;
    int c = 0;
    for (int b = 0; b < 62; ++b) if (mask & (1ull << b)) o[c++] = b;
    return c;
}

void sur_setup_text(const SurSetup *s, char *out, size_t n)
{
    size_t l = 0;
    #define P(...) do { if (l < n) l += (size_t)snprintf(out + l, n - l, __VA_ARGS__); } while (0)
    P("%d|", s->n);
    for (int k = 0; k < s->n; ++k) P("%s%d", k ? "," : "", s->out[k]);
    P("|");
    for (int k = 0; k < s->n; ++k) P("%s%.1f", k ? "," : "", (double)s->az[k]);
    P("|%.1f|%.1f|%.3f|%d", (double)s->width, (double)s->az0, (double)s->spread0, s->layout);
    /* 1.4.22: the room */
    P("|%d|%.2f|%.2f|%.2f|", s->mode, (double)s->room_w, (double)s->room_d, (double)s->stage_d);
    for (int k = 0; k < s->n; ++k) P("%s%.2f", k ? "," : "", (double)s->x[k]);
    P("|");
    for (int k = 0; k < s->n; ++k) P("%s%.2f", k ? "," : "", (double)s->y[k]);
    P("|%.2f|%.2f|%.2f", (double)s->x0, (double)s->y0, (double)s->w0);
    bool any = false;                                          /* 2.0: the Speaker Map (only when used) */
    for (int k = 0; k < s->n; ++k) if (s->omask[k]) any = true;
    if (any) { P("|om:"); for (int k = 0; k < s->n; ++k) P("%s%llx", k ? "," : "", (unsigned long long)s->omask[k]); }
    #undef P
}

bool sur_setup_parse(const char *txt, SurSetup *s)
{
    memset(s, 0, sizeof *s);
    if (!txt) return false;
    const char *om = strstr(txt, "|om:");                      /* 2.0: the Speaker Map */
    uint64_t omask[SUR_MAX_SPK] = { 0 };
    if (om) {
        const char *q = om + 4;
        for (int k = 0; k < SUR_MAX_SPK && *q; ++k) {
            omask[k] = (uint64_t)strtoull(q, (char **)&q, 16);
            if (*q != ',') break;
            ++q;
        }
    }
    bool ok = sur_setup_parse_(txt, s);
    if (ok) memcpy(s->omask, omask, sizeof omask);
    return ok;
}

static bool sur_setup_parse_(const char *txt, SurSetup *s)
{
    if (!txt) return false;
    const char *p = txt;
    s->n = atoi(p);
    if (s->n < 0 || s->n > SUR_MAX_SPK) { s->n = 0; return false; }
    if (!(p = strchr(p, '|'))) return false;
    ++p;
    for (int k = 0; k < s->n; ++k) { s->out[k] = atoi(p); while (*p && *p != ',' && *p != '|') ++p; if (*p == ',') ++p; }
    if (*p != '|') return false;
    ++p;
    for (int k = 0; k < s->n; ++k) { s->az[k] = sur_wrap((float)atof(p)); while (*p && *p != ',' && *p != '|') ++p; if (*p == ',') ++p; }
    if (*p != '|') return false;
    float w = 0, a0 = 0, sp = 0;
    int lay = -1;
    sscanf(p + 1, "%f|%f|%f|%d", &w, &a0, &sp, &lay);
    s->width = w < 0 ? 0 : w > 180 ? 180 : w;
    s->az0 = sur_wrap(a0);
    s->spread0 = sp < 0 ? 0 : sp > 1 ? 1 : sp;
    s->layout = lay;
    for (int k = 0; k < s->n; ++k) if (s->out[k] < 0 || s->out[k] > 61) return false;
    /* 1.4.22: the room (missing in older files -> circle) */
    s->room_w = 16; s->room_d = 26; s->stage_d = 7; s->x0 = 0; s->y0 = 10; s->w0 = 0;
    for (int k = 0; k < s->n; ++k) {                          /* a sensible plan from the angles */
        float a = s->az[k] * (float)M_PI / 180.0f;
        s->x[k] = 7.0f * sinf(a); s->y[k] = 10.0f + 9.0f * cosf(a);
    }
    for (int f = 0; f < 4 && p; ++f) p = strchr(p + 1, '|');   /* past width|az0|spread0|layout */
    if (!p) return true;
    int mode = 0;
    float rw = 0, rd = 0, sd = 0;
    if (sscanf(p + 1, "%d|%f|%f|%f", &mode, &rw, &rd, &sd) != 4) return true;
    s->mode = mode == SUR_MODE_ROOM ? SUR_MODE_ROOM : SUR_MODE_CIRCLE;
    if (rw >= 2 && rw <= 500) s->room_w = rw;
    if (rd >= 2 && rd <= 500) s->room_d = rd;
    s->stage_d = sd >= 0 && sd < s->room_d ? sd : 0;
    for (int f = 0; f < 4 && p; ++f) p = strchr(p + 1, '|');
    if (!p) return true;
    ++p;
    for (int k = 0; k < s->n; ++k) { s->x[k] = (float)atof(p); while (*p && *p != ',' && *p != '|') ++p; if (*p == ',') ++p; }
    if (*p != '|') return true;
    ++p;
    for (int k = 0; k < s->n; ++k) { s->y[k] = (float)atof(p); while (*p && *p != ',' && *p != '|') ++p; if (*p == ',') ++p; }
    if (*p != '|') return true;
    float x0 = 0, y0 = 10, w0 = 0;
    sscanf(p + 1, "%f|%f|%f", &x0, &y0, &w0);
    s->x0 = x0; s->y0 = y0; s->w0 = w0 < 0 ? 0 : w0;
    return true;
}

void sur_points_text(const SurPt *p, int n, char *out, size_t sz)
{
    size_t l = 0;
    out[0] = 0;
    for (int i = 0; i < n && l < sz; ++i)
        l += (size_t)snprintf(out + l, sz - l, "%s%.4f:%.1f:%.3f:%.2f:%.2f:%.2f", i ? " " : "", (double)p[i].t, (double)p[i].az,
                              (double)p[i].spread, (double)p[i].x, (double)p[i].y, (double)p[i].w);
}

int sur_points_parse(const char *txt, SurPt *p, int max)
{
    int n = 0;
    const char *q = txt;
    while (q && *q && n < max) {
        while (*q == ' ') ++q;
        float t, a, s, x = 0, y = 0, w = 0;
        int got = sscanf(q, "%f:%f:%f:%f:%f:%f", &t, &a, &s, &x, &y, &w);
        if (got >= 3 && t == t && t >= 0) {
            p[n].t = t; p[n].az = sur_wrap(a); p[n].spread = s < 0 ? 0 : s > 1 ? 1 : s;
            p[n].x = got >= 6 ? x : 0; p[n].y = got >= 6 ? y : 0; p[n].w = got >= 6 && w > 0 ? w : 0;
            ++n;
        }
        q = strchr(q, ' ');
    }
    sur_sort(p, n);
    return n;
}
