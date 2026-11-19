/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#define _POSIX_C_SOURCE 200809L
#include "project.h"
#include "safefile.h"
#include "fxchain.h"
#include "schedule.h"

static void write_b64_lines(FILE *f, const char *key, const void *data, size_t len);
#include "wav.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <strings.h>

// ============ Directories ============

void project_get_base_dir(char *out_path, size_t size)
{
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(out_path, size, "%s/qlim/Projects", home);
}

static int ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0;
        return 1;
    }
    if (mkdir(path, 0755) == 0) return 0;
    return 1;
}

static int ensure_dir_recursive(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);

    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (ensure_dir(tmp) != 0) return 1;
            *p = '/';
        }
    }
    return ensure_dir(tmp);
}

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) return 1;

    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return 1;
    }

    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            return 1;
        }
    }

    fclose(in);
    fclose(out);
    return 0;
}

static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* 2.0: Collect All & Save with converting: the clip whose file became e.g. "x.wav" in samples/ (looked up when the
 * collected project is written) */
typedef struct { const Clip *cl; char name[256]; char src[1024]; } CollRen;
static CollRen *g_coll_ren;
static int      g_coll_nren;
static const char *coll_name_for(const Clip *cl)
{
    for (int i = 0; i < g_coll_nren; ++i) if (g_coll_ren[i].cl == cl) return g_coll_ren[i].name;
    return NULL;
}

// ============ Save ============

// Save mode for undo/"are there changes": 0 = normal project; 1 = undo snapshot (without volume/pan/mute/solo,
// without position and zoom); 2 = snapshot for "are there unsaved changes" (with volume etc., but without position and zoom).
static int g_snap = 0;

static int save_to_file(Session *s, FILE *f, bool relative_paths)
{
    fprintf(f, "# Q-LiM project\n");
    fprintf(f, "# version:4\n\n");

    fprintf(f, "[meta]\n");
    fprintf(f, "version=4\n");   /* 4 = step 36: the new fader (0 dB = 0.8) */
    fprintf(f, "sample_rate=%d\n", s->sample_rate);
    fprintf(f, "channel_count=%d\n", s->channel_count);
    fprintf(f, "scene_count=%d\n", s->scene_count);
    fprintf(f, "tempo=%.2f\n", session_get_tempo(s));
    {   /* 1.4.8: the time signature first (the quantization is in its bars) */
        int sn, sd;
        session_get_time_sig(s, &sn, &sd);
        fprintf(f, "time_sig=%d/%d\n", sn, sd);
    }
    fprintf(f, "quantize=%g\n", (double)session_get_quantize(s));
    if (session_get_quant_mode(s) >= 0) fprintf(f, "quant_mode=%d\n", session_get_quant_mode(s));
    if (atomic_load(&s->metronome)) fprintf(f, "metronome=1\n");

    float master_gain = atomic_load(&s->master_gain);
    int master_midi_cc = atomic_load(&s->master_midi_cc);
    if (g_snap != 1) fprintf(f, "master_gain=%.4f\n", master_gain);
    if (g_snap != 1) fprintf(f, "master_pan=%.4f\n", atomic_load(&s->master_pan));
    fprintf(f, "master_muted=%d\n", atomic_load(&s->master_muted) ? 1 : 0);
    fprintf(f, "master_midi_cc=%d\n", master_midi_cc);
    fprintf(f, "master_route_l=%llu\n", (unsigned long long)session_master_route_get(s, 0));
    fprintf(f, "master_route_r=%llu\n", (unsigned long long)session_master_route_get(s, 1));
    fprintf(f, "transition_sec=%.1f\n", (double)session_get_transition_sec(s));   /* 1.4.17 */
    sched_save(f);                                                                /* 1.4.17: launches at a clock time */
    fprintf(f, "\n");

    for (int c = 0; c < s->channel_count; ++c) {
        Channel *chp = &s->channels[c];
        float gain = atomic_load(&chp->gain);
        float pan  = atomic_load(&chp->pan);
        bool muted = atomic_load(&chp->muted);
        bool soloed = atomic_load(&chp->soloed);
        int midi_cc = atomic_load(&chp->midi_cc);
        int pan_midi_cc = atomic_load(&chp->pan_midi_cc);

        fprintf(f, "[channel %d]\n", c);
        fprintf(f, "name=%s\n", chp->name);
        if (g_snap != 1) {
            fprintf(f, "gain=%.4f\n", gain);
            fprintf(f, "pan=%.4f\n", pan);
            fprintf(f, "muted=%d\n", muted ? 1 : 0);
            fprintf(f, "soloed=%d\n", soloed ? 1 : 0);
        }
        fprintf(f, "color=%d\n", chp->color_id);
        if (g_snap != 1) fprintf(f, "width=%d\n", chp->width);
        fprintf(f, "midi_cc=%d\n", midi_cc);
        fprintf(f, "pan_midi_cc=%d\n", pan_midi_cc);
        fprintf(f, "route_l=%llu\n", (unsigned long long)session_route_get(s, c, 0));
        fprintf(f, "route_r=%llu\n", (unsigned long long)session_route_get(s, c, 1));
        {   /* 1.4.19 */
            SurSetup su;
            if (session_get_surround(s, c, &su)) { char t[1600]; sur_setup_text(&su, t, sizeof t); fprintf(f, "surround=%s\n", t); }
        }
        fprintf(f, "mute_bind=%d\n", session_get_mute_bind(s, c));
        /* 1.4: the recording input and monitoring of the channel */
        fprintf(f, "rec_in=%d %d\n", atomic_load(&chp->rec_in_first), atomic_load(&chp->rec_in_stereo) ? 2 : 1);
        if (atomic_load(&chp->monitor)) fprintf(f, "monitor=1\n");
        for (int k = 0; k < chp->fx_count; ++k) {         /* the plugins: fx=FORMAT|bypass|name|path */
            const ChFx *fx = &chp->fx[k];
            char nm[96], pt[512];
            snprintf(nm, sizeof nm, "%s", fx->name); snprintf(pt, sizeof pt, "%s", fx->path);
            for (char *q = nm; *q; ++q) if (*q == '|' || *q == '\n' || *q == '\r') *q = ' ';
            for (char *q = pt; *q; ++q) if (*q == '\n' || *q == '\r') *q = ' ';
            fprintf(f, "fx=%s|%d|%s|%s\n", fx->fmt, fx->bypass ? 1 : 0, nm, pt);
            if (fx->midi_n > 0) {                           /* 1.4.14: MIDI-learned parameters: param:cc ... */
                fprintf(f, "fxmidi=");
                for (int i = 0; i < fx->midi_n; ++i) fprintf(f, "%s%d:%d", i ? " " : "", fx->midi[i].param, fx->midi[i].cc);
                fprintf(f, "\n");
            }
            if (g_snap) {                                   /* undo snapshot: only the number of the saved settings */
                if (fx->state_ref > 0) fprintf(f, "fxref=%d\n", fx->state_ref);
            } else {                                        /* file: the plugin's settings (base64, in chunks) */
                void *sd = NULL; size_t sl = 0;
                if (fxchain_state_for_save(s, c, k, &sd, &sl) == 0 && sl > 0) write_b64_lines(f, "fxstate", sd, sl);
                free(sd);
            }
        }
        fprintf(f, "solo_bind=%d\n\n", session_get_solo_bind(s, c));
    }

    for (int c = 0; c < s->channel_count; ++c) {
        for (int sc = 0; sc < s->scene_count; ++sc) {
            Clip *cl = &s->channels[c].clips[sc];
            if (!atomic_load(&cl->has_clip)) continue;

            bool is_note = atomic_load(&cl->is_note);
            bool loop_on = atomic_load(&cl->loop);
            size_t loop_start = atomic_load(&cl->loop_start);
            size_t loop_end = atomic_load(&cl->loop_end);
            size_t start_marker = atomic_load(&cl->start_marker);
            size_t end_marker = atomic_load(&cl->end_marker);

            fprintf(f, "[clip %d %d]\n", c, sc);

            if (atomic_load(&cl->is_midi)) {            /* 1.3: a MIDI clip - its notes as text */
                fprintf(f, "type=midi\n");
                fprintf(f, "name=%s\n", cl->name);
                fprintf(f, "loop=%d\n", loop_on ? 1 : 0);
                char *mt = session_midi_text(s, c, sc);
                if (mt) { fputs(mt, f); free(mt); }
                fprintf(f, "bind=%d\n\n", atomic_load(&cl->bind));
            } else if (is_note) {
                fprintf(f, "type=note\n");
                fprintf(f, "name=%s\n", cl->name);
                fprintf(f, "color=%d\n\n", cl->color);
            } else {
                fprintf(f, "type=clip\n");

                if (relative_paths) {
                    const char *cn = coll_name_for(cl);
                    fprintf(f, "path=samples/%s\n", cn ? cn : basename_of(cl->path));
                } else {
                    fprintf(f, "path=%s\n", cl->path);
                }

                fprintf(f, "name=%s\n", cl->name);
                if (!g_snap) fprintf(f, "position=%zu\n", cl->position);
                fprintf(f, "loop=%d\n", loop_on ? 1 : 0);
                fprintf(f, "loop_start=%zu\n", loop_start);
                fprintf(f, "loop_end=%zu\n", loop_end);
                fprintf(f, "start_marker=%zu\n", start_marker);
                fprintf(f, "end_marker=%zu\n", end_marker);
                if (!g_snap) {
                    fprintf(f, "wave_zoom=%.4f\n",
                            atomic_load(&cl->wave_zoom));
                    fprintf(f, "wave_scroll=%lld\n",            /* may be < 0 (step 36j) */
                            (long long)(intptr_t)atomic_load(&cl->wave_scroll_frame));
                }
                /* tempo: the detected one is kept only in the real file (it is not a user action -> not for undo) */
                float bd = atomic_load(&cl->bpm_detected), bf = atomic_load(&cl->bpm_fix), bp = atomic_load(&cl->bpm_play);
                if (!g_snap && bd > 0.0f) fprintf(f, "bpm_detected=%.2f\n", bd);
                if (bf > 0.0f) fprintf(f, "bpm_fix=%.2f\n", bf);
                if (bp > 0.0f) fprintf(f, "bpm_play=%.2f\n", bp);
                if (atomic_load(&cl->warp)) fprintf(f, "warp=1\n");
                {   /* 1.4.9: the clip's own gain / pan (only when set) */
                    float cg = atomic_load(&cl->cgain_db), cpn = atomic_load(&cl->cpan);
                    if (cg != 0.0f) fprintf(f, "clip_gain=%.2f\n", cg);
                    if (cpn != 0.0f) fprintf(f, "clip_pan=%.3f\n", cpn);
                    float pt = atomic_load(&cl->pitch_st);              /* 1.4.13 */
                    if (pt != 0.0f) fprintf(f, "clip_pitch=%.2f\n", pt);
                    if (atomic_load(&cl->pitch_fmt)) fprintf(f, "clip_formants=1\n");
                }
                if (cl->mc_mode != CLIP_MC_STEREO) {         /* 1.4.24: a surround file: how it plays */
                    fprintf(f, "mc=%d,%d\n", cl->mc_mode, atomic_load(&cl->mc_out));
                    if (cl->mc_mode == CLIP_MC_PATH && cl->mc_orig && cl->mc_orig[0])        /* 2.1: Collect: the copy in samples */
                        fprintf(f, relative_paths ? "mc_orig=samples/%s\n" : "mc_orig=%s\n", relative_paths ? basename_of(cl->mc_orig) : cl->mc_orig);
                    bool any = false;                       /* 2.0: the Speaker Map */
                    for (int k = 0; k < STREAM_MAX_CH; ++k) if (atomic_load(&cl->mc_map[k])) any = true;
                    if (any) {
                        fprintf(f, "mc_map=");
                        for (int k = 0; k < STREAM_MAX_CH; ++k) fprintf(f, "%s%llx", k ? "," : "", (unsigned long long)atomic_load(&cl->mc_map[k]));
                        fprintf(f, "\n");
                    }
                }
                {   /* 1.4.19: the surround automation: sur=t:az:spread:x:y:w ... (16 points per line) */
                    static SurPt sp[SUR_MAX_PTS];
                    char st[16 * 64];
                    int np = session_clip_get_sur_points(s, c, sc, sp, SUR_MAX_PTS);
                    for (int i = 0; i < np; i += 16) {
                        sur_points_text(sp + i, np - i < 16 ? np - i : 16, st, sizeof st);
                        fprintf(f, "sur=%s\n", st);
                    }
                }
                /* 1.4.17: separated parts, one line each: stem=name|level dB|mute|solo|MIDI CC (-1)|path */
                for (int k = 0; k < session_clip_stem_count(s, c, sc); ++k)
                    fprintf(f, relative_paths ? "stem=%s|%.2f|%d|%d|%d|samples/%s\n" : "stem=%s|%.2f|%d|%d|%d|%s\n",   /* 2.1: Collect */
                            session_clip_stem_name(s, c, sc, k), session_clip_get_stem_level(s, c, sc, k),
                            session_clip_get_stem_mute(s, c, sc, k), session_clip_get_stem_solo(s, c, sc, k),
                            session_clip_get_stem_cc(s, c, sc, k),
                            relative_paths ? basename_of(session_clip_stem_path(s, c, sc, k)) : session_clip_stem_path(s, c, sc, k));
                {   /* step 27: the cut parts (only if the clip has been cut) */
                    char *et = session_clip_edits_text(s, c, sc);
                    if (et) { fputs(et, f); free(et); }
                }
                fprintf(f, "bind=%d\n\n", atomic_load(&cl->bind));
            }
        }
    }

    return 0;
}

int project_save(Session *s, const char *path)
{
    if (!s || !path) return 1;

    char tmp[1100];
    FILE *f = safe_open(path, tmp, sizeof tmp);
    if (!f) {
        fprintf(stderr, "project_save: cannot open '%s' for writing\n", path);
        return 1;
    }

    int ret = save_to_file(s, f, false);
    if (ret != 0) { fclose(f); unlink(tmp); }
    else ret = safe_commit(f, tmp, path, 1);      /* backup copy: <project>.bak */

    if (ret == 0) {
        printf("project_save: OK '%s'\n", path);
    }

    return ret;
}

// ============ base64 (plugin settings, step 35) ============
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* 2400 characters per line (a multiple of 4 -> every line is decoded separately) */
static void write_b64_lines(FILE *f, const char *key, const void *data, size_t len)
{
    const unsigned char *b = data;
    char line[2410];
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = (unsigned)b[i] << 16 | (i + 1 < len ? (unsigned)b[i + 1] << 8 : 0) | (i + 2 < len ? b[i + 2] : 0);
        line[o++] = B64[(v >> 18) & 63];
        line[o++] = B64[(v >> 12) & 63];
        line[o++] = i + 1 < len ? B64[(v >> 6) & 63] : '=';
        line[o++] = i + 2 < len ? B64[v & 63] : '=';
        if (o >= 2400) { line[o] = 0; fprintf(f, "%s=%s\n", key, line); o = 0; }
    }
    if (o) { line[o] = 0; fprintf(f, "%s=%s\n", key, line); }
}

static int b64v(char c)
{
    const char *p = strchr(B64, c);
    return (c && p) ? (int)(p - B64) : -1;
}
static size_t b64_decode(const char *in, unsigned char *out)
{
    size_t n = 0;
    int q[4], k = 0;
    for (; *in; ++in) {
        if (*in == '=') { q[k++] = 0; }
        else { int v = b64v(*in); if (v < 0) continue; q[k++] = v; }
        if (k == 4) {
            unsigned v = (unsigned)q[0] << 18 | (unsigned)q[1] << 12 | (unsigned)q[2] << 6 | (unsigned)q[3];
            out[n++] = (unsigned char)(v >> 16);
            out[n++] = (unsigned char)(v >> 8);
            out[n++] = (unsigned char)v;
            k = 0;
        }
    }
    return n;
}
/* how many of the bytes at the end are "=" (for the exact size) */
static size_t b64_pad(const char *in) { size_t l = strlen(in), p = 0; while (l && (in[l - 1] == '\n' || in[l - 1] == '\r')) --l; while (l && in[l - 1] == '=') { --l; ++p; } return p; }

// ============ Load ============

static void resolve_path(const char *project_dir,
                         const char *stored_path,
                         char *out, size_t out_size)
{
    if (!stored_path || stored_path[0] == '\0') {
        out[0] = '\0';
        return;
    }

    if (strncmp(stored_path, "samples/", 8) == 0) {
        snprintf(out, out_size, "%s/%s", project_dir, stored_path);
        return;
    }

#ifdef _WIN32
    bool abs_path = stored_path[0] == '/' ||
                    (((stored_path[0] | 32) >= 'a' && (stored_path[0] | 32) <= 'z') && stored_path[1] == ':');
#else
    bool abs_path = stored_path[0] == '/';
#endif
    if (abs_path) {
        snprintf(out, out_size, "%s", stored_path);
        if (access(out, F_OK) == 0) return;
        /* step 35e: a project from another computer (e.g. /home/other/...) - the file is not at the old path.
         * Look for it next to the project: the tails of the old path (".../samples/x.mp3", "samples/x.mp3", "x.mp3")
         * in the project's folder and in the folder above it. */
        char parent[1100];
        snprintf(parent, sizeof parent, "%s", project_dir);
        char *sl = strrchr(parent, '/');
        if (sl && sl != parent) *sl = 0; else parent[0] = 0;
        const char *bases[2] = { project_dir, parent };
        char cand[2400];
        for (const char *t = strchr(stored_path, '/'); t; t = strchr(t + 1, '/')) {
            const char *tail = t + 1;                   /* everything after this slash */
            if (!*tail) break;
            for (int b = 0; b < 2; ++b) {
                if (!bases[b][0]) continue;
                snprintf(cand, sizeof cand, "%s/%s", bases[b], tail);
                if (access(cand, F_OK) == 0) {
                    printf("project_load:   '%s' not found - using '%s'\n", stored_path, cand);
                    snprintf(out, out_size, "%s", cand);
                    return;
                }
            }
        }
        snprintf(out, out_size, "%s", stored_path);
        return;
    }

    snprintf(out, out_size, "%s/%s", project_dir, stored_path);
}

static void get_dirname(const char *path, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s", path);
    char *slash = strrchr(out, '/');
    if (slash) {
        *slash = '\0';
    } else {
        snprintf(out, out_size, ".");
    }
}

static bool parse_kv(const char *line, const char *key, char *out, size_t out_size)
{
    size_t klen = strlen(key);
    if (strncmp(line, key, klen) != 0) return false;
    if (line[klen] != '=') return false;

    const char *val = line + klen + 1;
    size_t vlen = strlen(val);
    while (vlen > 0 && (val[vlen - 1] == '\n' || val[vlen - 1] == '\r')) {
        vlen--;
    }
    if (vlen >= out_size) vlen = out_size - 1;
    memcpy(out, val, vlen);
    out[vlen] = '\0';
    return true;
}

/* 1.3: a MIDI clip from the project */
static void load_midi(Session *s, int c, int sc, const char *name, bool loop_on, const char *text)
{
    if (c < 0 || c >= s->channel_count || sc < 0 || sc >= MAX_SCENES) return;
    if (session_midi_set_text(s, c, sc, text && text[0] ? text : "midi_len=4\n") != 0) {
        printf("project_load:   midi [%d][%d]: damaged notes, an empty clip instead\n", c, sc);
        session_new_midi_clip(s, c, sc, 4.0f, name);
    }
    if (name && name[0]) session_rename_clip(s, c, sc, name);
    atomic_store(&s->channels[c].clips[sc].loop, loop_on);
    printf("project_load:   midi [%d][%d] = '%s'\n", c, sc, s->channels[c].clips[sc].name);
}

static void load_clip(Session *s, const char *project_dir,
                      int c, int sc,
                      bool is_note,
                      const char *path, const char *name,
                      size_t position,
                      bool loop_on, size_t loop_start, size_t loop_end,
                      size_t start_marker, size_t end_marker,
                      float wave_zoom, size_t wave_scroll,
                      const char *edits)

{
    if (c < 0 || c >= s->channel_count) return;
    if (sc < 0 || sc >= MAX_SCENES) return;

    Clip *cl = &s->channels[c].clips[sc];

    if (is_note) {
        atomic_store(&cl->has_clip, true);
        atomic_store(&cl->is_note, true);
        atomic_store(&cl->playing, false);
        atomic_store(&cl->paused, false);
        atomic_store(&cl->loop, false);
        atomic_store(&cl->loop_start, 0);
        atomic_store(&cl->loop_end, 0);
        atomic_store(&cl->start_marker, 0);
        atomic_store(&cl->end_marker, 0);
        snprintf(cl->name, sizeof(cl->name), "%.255s",
                 (name && name[0]) ? name : "Note");
        cl->path[0] = '\0';
        cl->position = 0;
        printf("project_load:   note [%d][%d] = '%s'\n", c, sc, cl->name);
    } else if (path && path[0]) {
        char resolved[1024];
        resolve_path(project_dir, path, resolved, sizeof(resolved));

        if (session_set_clip(s, c, sc, resolved) == 0) {
            /* step 27: the parts BEFORE the markers (the markers depend on the clip length) */
            if (edits && edits[0] && session_clip_set_edits_text(s, c, sc, edits) != 0)
                printf("project_load:   clip [%d][%d]: cut parts could not be restored (file changed?)\n", c, sc);
            /* the values from the file may be damaged/from another version of the file -> always within the clip's bounds */
            size_t fc = cl->wav.frame_count;
            if (end_marker == 0 || end_marker > fc) end_marker = fc;
            if (start_marker >= end_marker) start_marker = 0;
            if (loop_end == 0 || loop_end > fc) loop_end = fc;
            if (loop_start >= loop_end) loop_start = 0;
            if (position > fc) position = start_marker;
            if ((intptr_t)wave_scroll > 2 * (int64_t)fc || (intptr_t)wave_scroll < -(int64_t)fc) wave_scroll = 0;
            cl->position = position;
            atomic_store(&cl->has_clip, true);
            atomic_store(&cl->is_note, false);
            atomic_store(&cl->playing, false);
            atomic_store(&cl->paused, false);
            atomic_store(&cl->loop, loop_on);
            atomic_store(&cl->loop_start, loop_start);
            atomic_store(&cl->loop_end,
                         loop_end > 0 ? loop_end : cl->wav.frame_count);
            atomic_store(&cl->start_marker,
                         start_marker > 0 ? start_marker : 0);
            atomic_store(&cl->end_marker,
                         end_marker > 0 ? end_marker : cl->wav.frame_count);
            atomic_store(&cl->wave_zoom,
                         (wave_zoom >= WAVE_ZOOM_MIN - 0.001f && wave_zoom <= WAVE_ZOOM_MAX)
                         ? (wave_zoom < WAVE_ZOOM_MIN ? WAVE_ZOOM_MIN : wave_zoom) : 1.0f);
            atomic_store(&cl->wave_scroll_frame, wave_scroll);
            stream_invalidate_preroll(cl->stream);   // start_marker is set directly

            snprintf(cl->name, sizeof(cl->name), "%s",
                     (name && name[0]) ? name : basename_of(resolved));
            snprintf(cl->path, sizeof(cl->path), "%s", resolved);

            printf("project_load:   clip [%d][%d] = '%s' (loop=%d)\n",
                   c, sc, cl->name, loop_on ? 1 : 0);
        } else {
            printf("project_load:   skipping missing file '%s'\n", resolved);
        }
    }
}

// Snapshot of the project in memory (for undo and for detecting unsaved changes). Freed with free().
char *project_snapshot(Session *s, int live)
{
#ifdef _WIN32
    /* no open_memstream on Windows: a temporary file that is deleted when closed. NOT tmpfile(): msvcrt creates it in
     * the root of C:\ where a normal user may not write -> no snapshots -> no undo and no "save changes?" question */
    FILE *f = qw_tmpfile();
    if (!f) { fprintf(stderr, "project_snapshot: cannot create a temporary file\n"); return NULL; }
    g_snap = live ? 2 : 1;
    save_to_file(s, f, false);
    g_snap = 0;
    long len = ftell(f);
    char *buf = len >= 0 ? malloc((size_t)len + 1) : NULL;
    if (buf) {
        rewind(f);
        size_t got = fread(buf, 1, (size_t)len, f);
        buf[got] = 0;
    }
    fclose(f);
    return buf;
#else
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f) return NULL;
    g_snap = live ? 2 : 1;
    save_to_file(s, f, false);
    g_snap = 0;
    fclose(f);
    return buf;
#endif
}

// Loads ONE cell (clip or note) without touching the other playing clips.
void project_load_clip_cell(Session *s, int c, int sc, bool is_note, const char *path, const char *name,
                            bool loop_on, size_t loop_start, size_t loop_end,
                            size_t start_marker, size_t end_marker, const char *edits)
{
    load_clip(s, "", c, sc, is_note, path, name, 0, loop_on, loop_start, loop_end,
              start_marker, end_marker, 1.0f, 0, edits);
}

static void apply_clip_tempo(Session *s, int c, int sc, float det, float fix, float play, bool warp)
{
    session_set_clip_bpm_fields(s, c, sc, det, fix, play);
    if (warp) session_set_clip_warp(s, c, sc, true);
}

/* 1.4.14: the stems come back if their files are still there (if they were deleted, the clip simply plays its file
 * again - separate it once more) */
typedef struct { StemSet set; float db[STEM_MAX]; int mute[STEM_MAX], solo[STEM_MAX], cc[STEM_MAX]; } LoadStems;
static void apply_stems(Session *s, int ch, int sc, const LoadStems *ls)
{
    for (int k = 0; k < ls->set.n; ++k)
        if (access(ls->set.path[k], F_OK) != 0) {
            printf("project_load: the separated parts of [%d][%d] are gone - the clip plays its file\n", ch, sc);
            return;
        }
    if (session_clip_set_stem_set(s, ch, sc, &ls->set) != 0) return;
    for (int k = 0; k < ls->set.n; ++k) {
        session_clip_set_stem_level(s, ch, sc, k, ls->db[k]);
        session_clip_set_stem_mute(s, ch, sc, k, ls->mute[k] != 0);
        session_clip_set_stem_solo(s, ch, sc, k, ls->solo[k] != 0);
        session_clip_set_stem_cc(s, ch, sc, k, ls->cc[k]);
    }
    session_clip_stems_apply(s, ch, sc);
}

int project_load(Session *s, const char *path)
{
    if (!s || !path) return 1;

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "project_load: cannot open '%s'\n", path);
        return 1;
    }
    session_midi_panic(s);                         /* 1.2: no note keeps sounding from the old project */
    session_set_midi_target(s, -1);
    sched_clear();                                 /* 1.4.17: the launches at a time belong to the project */
    s->tr_cur = -1; s->tr_hist_n = 0;

    char project_dir[1024];
    get_dirname(path, project_dir, sizeof(project_dir));

    session_release_all_clips(s);
    s->channel_count = 0;
    s->scene_count = 0;

    if (s->sample_rate <= 0) s->sample_rate = 48000;
    session_set_tempo(s, 120.0f);                  /* old projects have no tempo */
    session_set_time_sig(s, 4, 4);                 /* 1.4.8: old projects: 4/4 */
    session_set_quantize(s, 4.0f);                 /* 1.1: old projects: the next bar */
    atomic_store(&s->metronome, false);            /* 1.3.1 */
    atomic_store(&s->master_pan, 0.0f);            /* old projects have no master balance/mute */
    atomic_store(&s->master_muted, false);

    char line[4096];
    int current_ch = -1;
    int current_sc = -1;

    bool cur_is_note = false;
    bool cur_is_midi = false;                 /* 1.3 */
    char cur_path[1024] = {0};
    char cur_name[NAME_LEN] = {0};
    size_t cur_position = 0;
    bool cur_loop = false;
    size_t cur_loop_start = 0;
    size_t cur_loop_end = 0;
    size_t cur_start_marker = 0;
    size_t cur_end_marker = 0;
    float  cur_wave_zoom = 1.0f;
    size_t cur_wave_scroll = 0;
    int    cur_color = 0;
    float  cur_bpm_det = 0.0f, cur_bpm_fix = 0.0f, cur_bpm_play = 0.0f;
    bool   cur_warp = false;
    float  cur_cgain = 0.0f, cur_cpan = 0.0f;   /* 1.4.9 */
    float  cur_pitch = 0.0f;  bool cur_fmt = false;   /* 1.4.13 */
    static SurPt cur_sur[SUR_MAX_PTS];  int cur_sur_n = 0;   /* 1.4.19 */
    int cur_mc = 0, cur_mc_out = 0;  static char cur_mc_orig[1024];   /* 1.4.24 */
    uint64_t cur_mc_map[STREAM_MAX_CH] = { 0 };  bool cur_mc_has_map = false;   /* 2.0 */
    static LoadStems cur_st;                           /* 1.4.14 / 1.4.17 */
    memset(&cur_st, 0, sizeof cur_st);
    char  *cur_edits = NULL;            /* step 27: the edit_* lines of the current clip */
    size_t cur_edits_len = 0;

    bool in_channel_section = false;
    float ch_gain_pending = -1.0f;
    int file_ver = 0;                     /* below 4 = old fader -> the gains are converted (step 36) */
    float ch_pan_pending = 0.0f;
    bool  ch_has_pan = false;
    bool  ch_muted_pending = false;
    bool  ch_soloed_pending = false;
    int   ch_color_pending = -1;
    int   ch_midi_cc_pending = -2;
    int   ch_pan_midi_cc_pending = -2;
    char  ch_name_pending[NAME_LEN] = {0};
    bool  ch_has_name = false;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '#') continue;

        if (line[0] == '[') {
            if (current_ch >= 0 && current_sc >= 0) {
                if (cur_is_midi) load_midi(s, current_ch, current_sc, cur_name, cur_loop, cur_edits);
                else { if (!cur_is_note) session_load_mc_next(cur_mc, cur_mc_out, cur_mc_orig);
                      load_clip(s, project_dir,
                          current_ch, current_sc,
                          cur_is_note,
                          cur_path, cur_name,
                          cur_position,
                          cur_loop, cur_loop_start, cur_loop_end,
                          cur_start_marker, cur_end_marker,
                          cur_wave_zoom, cur_wave_scroll, cur_edits);
                      session_load_mc_next(0, 0, NULL);
                      if (cur_mc_has_map) for (int k = 0; k < STREAM_MAX_CH; ++k) session_clip_set_mc_map(s, current_ch, current_sc, k, cur_mc_map[k]); }
                if (cur_is_note && cur_color) session_set_note_color(s, current_ch, current_sc, cur_color);
                if (!cur_is_note && !cur_is_midi) apply_clip_tempo(s, current_ch, current_sc, cur_bpm_det, cur_bpm_fix, cur_bpm_play, cur_warp);
                if (!cur_is_note && !cur_is_midi) { session_set_clip_gain(s, current_ch, current_sc, cur_cgain); session_set_clip_pan(s, current_ch, current_sc, cur_cpan);
                  session_set_clip_formants(s, current_ch, current_sc, cur_fmt); session_set_clip_pitch(s, current_ch, current_sc, cur_pitch); }
                if (!cur_is_note && !cur_is_midi && cur_st.set.n > 0) apply_stems(s, current_ch, current_sc, &cur_st);
                if (!cur_is_note && cur_sur_n > 0) { sur_sort(cur_sur, cur_sur_n); session_clip_set_sur_points(s, current_ch, current_sc, cur_sur, cur_sur_n); }
            }

            if (in_channel_section && current_ch >= 0 &&
                current_ch < s->channel_count) {
                Channel *chp = &s->channels[current_ch];
                if (ch_gain_pending >= 0.0f) {
                    atomic_store(&chp->gain, ch_gain_pending);
                }
                if (ch_has_pan) {
                    atomic_store(&chp->pan, ch_pan_pending);
                }
                atomic_store(&chp->muted, ch_muted_pending);
                atomic_store(&chp->soloed, ch_soloed_pending);
                if (ch_color_pending >= 0) chp->color_id = ch_color_pending;
                if (ch_midi_cc_pending != -2) {
                    atomic_store(&chp->midi_cc, ch_midi_cc_pending);
                }
                if (ch_pan_midi_cc_pending != -2) {
                    atomic_store(&chp->pan_midi_cc, ch_pan_midi_cc_pending);
                }
                if (ch_has_name) {
                    snprintf(chp->name, sizeof(chp->name), "%s", ch_name_pending);
                }
            }

            current_ch = -1;
            current_sc = -1;
            cur_is_note = false;
            cur_is_midi = false;
            cur_path[0] = '\0';
            cur_name[0] = '\0';
            cur_position = 0;
            cur_loop = false;
            cur_loop_start = 0;
            cur_loop_end = 0;
            cur_start_marker = 0;
            cur_end_marker = 0;
            cur_wave_zoom = 1.0f;
            cur_wave_scroll = 0;
            cur_color = 0;
            cur_bpm_det = cur_bpm_fix = cur_bpm_play = 0.0f;
            cur_warp = false;
            cur_cgain = cur_cpan = 0.0f;
            cur_pitch = 0.0f; cur_fmt = false;
            cur_sur_n = 0;
            cur_mc = cur_mc_out = 0; cur_mc_orig[0] = 0;
            memset(cur_mc_map, 0, sizeof cur_mc_map); cur_mc_has_map = false;
            memset(&cur_st, 0, sizeof cur_st);
            free(cur_edits); cur_edits = NULL; cur_edits_len = 0;

            in_channel_section = false;
            ch_gain_pending = -1.0f;
            ch_pan_pending = 0.0f;
            ch_has_pan = false;
            ch_muted_pending = false;
            ch_soloed_pending = false;
            ch_color_pending = -1;
            ch_midi_cc_pending = -2;
            ch_pan_midi_cc_pending = -2;
            ch_name_pending[0] = '\0';
            ch_has_name = false;

            if (strncmp(line, "[meta]", 6) == 0) {
                // nothing
            } else if (strncmp(line, "[master]", 8) == 0) {
                // nothing
            } else if (strncmp(line, "[channel ", 9) == 0) {
                int ch = atoi(line + 9);
                if (ch >= 0 && ch < MAX_CHANNELS) {
                    while (s->channel_count <= ch) {
                        int idx = s->channel_count;
                        Channel *c = &s->channels[idx];
                        session_channel_init(c, idx, NULL);
                        s->channel_count++;
                    }
                    current_ch = ch;
                    in_channel_section = true;
                }
            } else if (strncmp(line, "[clip ", 6) == 0) {
                int c = 0, sc = 0;
                if (sscanf(line + 6, "%d %d", &c, &sc) == 2) {
                    while (s->channel_count <= c && s->channel_count < MAX_CHANNELS) {
                        int idx = s->channel_count;
                        Channel *chp = &s->channels[idx];
                        session_channel_init(chp, idx, NULL);
                        s->channel_count++;
                    }
                    current_ch = c;
                    current_sc = sc;
                }
            }
            continue;
        }

        char val[4096];

        if (current_ch < 0 && current_sc < 0) {
            if (parse_kv(line, "version", val, sizeof(val))) {
                file_ver = atoi(val);
            } else if (parse_kv(line, "master_gain", val, sizeof(val))) {
                float g = (float)atof(val);
                if (!isfinite(g)) g = FADER_UNITY_POS;      /* damaged file: NaN/inf must not reach the audio */
                if (g < 0.0f) g = 0.0f;
                if (g > 1.0f) g = 1.0f;
                if (file_ver < 4) g = session_old_pos_to_pos(g);
                atomic_store(&s->master_gain, g);
            } else if (parse_kv(line, "master_midi_cc", val, sizeof(val))) {
                atomic_store(&s->master_midi_cc, atoi(val));
            } else if (parse_kv(line, "master_pan", val, sizeof(val))) {
                float p = (float)atof(val);
                if (!isfinite(p)) p = 0.0f;
                if (p < -1.0f) p = -1.0f;
                if (p > 1.0f) p = 1.0f;
                atomic_store(&s->master_pan, p);
            } else if (parse_kv(line, "master_muted", val, sizeof(val))) {
                atomic_store(&s->master_muted, atoi(val) != 0);
            } else if (parse_kv(line, "master_route_l", val, sizeof(val))) {
                session_master_route_set(s, 0, (RouteMask)strtoull(val, NULL, 10));
            } else if (parse_kv(line, "transition_sec", val, sizeof(val))) {   /* 1.4.17 */
                session_set_transition_sec(s, (float)atof(val));
            } else if (parse_kv(line, "sched", val, sizeof(val))) {
                sched_load_line(val);
            } else if (parse_kv(line, "master_route_r", val, sizeof(val))) {
                session_master_route_set(s, 1, (RouteMask)strtoull(val, NULL, 10));
            } else if (parse_kv(line, "scene_count", val, sizeof(val))) {
                int sc = atoi(val);
                if (sc >= 0 && sc <= MAX_SCENES) {
                    s->scene_count = sc;
                }
            } else if (parse_kv(line, "sample_rate", val, sizeof(val))) {
                int sr = atoi(val);
                if (sr >= 8000 && sr <= 384000) s->sample_rate = sr;
            } else if (parse_kv(line, "tempo", val, sizeof(val))) {
                session_set_tempo(s, (float)atof(val));
            } else if (parse_kv(line, "metronome", val, sizeof(val))) {
                atomic_store(&s->metronome, atoi(val) != 0);
            } else if (parse_kv(line, "time_sig", val, sizeof(val))) {
                int sn = 0, sd = 0;
                if (sscanf(val, "%d/%d", &sn, &sd) == 2) session_set_time_sig(s, sn, sd);
            } else if (parse_kv(line, "quant_mode", val, sizeof(val))) {
                session_set_quant_mode(s, atoi(val));
            } else if (parse_kv(line, "quantize", val, sizeof(val))) {
                session_set_quantize(s, (float)atof(val));
            }
            continue;
        }

        if (in_channel_section && current_ch >= 0 && current_sc < 0) {
            if (parse_kv(line, "name", val, sizeof(val))) {
                strncpy(ch_name_pending, val, sizeof(ch_name_pending) - 1);
                ch_name_pending[sizeof(ch_name_pending) - 1] = '\0';
                ch_has_name = true;
            } else if (parse_kv(line, "gain", val, sizeof(val))) {
                float g = (float)atof(val);
                if (!isfinite(g)) g = FADER_UNITY_POS;
                if (g < 0.0f) g = 0.0f;
                if (g > 1.0f) g = 1.0f;
                if (file_ver < 4) g = session_old_pos_to_pos(g);
                ch_gain_pending = g;
            } else if (parse_kv(line, "pan", val, sizeof(val))) {
                float p = (float)atof(val);
                if (!isfinite(p)) p = 0.0f;
                if (p < -1.0f) p = -1.0f;
                if (p >  1.0f) p =  1.0f;
                ch_pan_pending = p;
                ch_has_pan = true;
            } else if (parse_kv(line, "muted", val, sizeof(val))) {
                ch_muted_pending = (atoi(val) != 0);
            } else if (parse_kv(line, "soloed", val, sizeof(val))) {
                ch_soloed_pending = (atoi(val) != 0);
            } else if (parse_kv(line, "color_id", val, sizeof(val))) {
                ch_color_pending = abs(atoi(val) % 8);      /* old format: 8 colours, the number grew with the channel */
            } else if (parse_kv(line, "color", val, sizeof(val))) {
                int cv = atoi(val);
                if (cv >= 0 && cv < N_PALETTE) ch_color_pending = cv;
            } else if (parse_kv(line, "width", val, sizeof(val))) {
                session_set_channel_width(s, current_ch, atoi(val));
            } else if (parse_kv(line, "midi_cc", val, sizeof(val))) {
                ch_midi_cc_pending = atoi(val);
            } else if (parse_kv(line, "pan_midi_cc", val, sizeof(val))) {
                ch_pan_midi_cc_pending = atoi(val);
            } else if (parse_kv(line, "route_l", val, sizeof(val))) {
                session_route_set(s, current_ch, 0, (RouteMask)strtoull(val, NULL, 10));
            } else if (parse_kv(line, "route_r", val, sizeof(val))) {
                session_route_set(s, current_ch, 1, (RouteMask)strtoull(val, NULL, 10));
            } else if (parse_kv(line, "surround", val, sizeof(val))) {    /* 1.4.19 */
                SurSetup su;
                if (sur_setup_parse(val, &su) && su.n > 0) session_set_surround(s, current_ch, &su);
            } else if (parse_kv(line, "rec_in", val, sizeof(val))) {      /* 1.4 */
                int f1 = 0, n1 = 2;
                if (sscanf(val, "%d %d", &f1, &n1) >= 1) session_set_rec_input(s, current_ch, f1, n1 != 1);
            } else if (parse_kv(line, "monitor", val, sizeof(val))) {
                session_set_monitor(s, current_ch, atoi(val) != 0);
            } else if (parse_kv(line, "mute_bind", val, sizeof(val))) {
                session_set_mute_bind(s, current_ch, atoi(val));
            } else if (parse_kv(line, "solo_bind", val, sizeof(val))) {
                session_set_solo_bind(s, current_ch, atoi(val));
            } else if (parse_kv(line, "fx", val, sizeof(val))) {
                char *a = val, *b = strchr(a, '|'), *c = b ? strchr(b + 1, '|') : NULL, *d = c ? strchr(c + 1, '|') : NULL;
                if (b && c && d) {
                    *b = *c = *d = 0;
                    int k = session_fx_add(s, current_ch, a, c + 1, d + 1);
                    if (k >= 0 && atoi(b + 1)) s->channels[current_ch].fx[k].bypass = true;
                }
            } else if (parse_kv(line, "fxmidi", val, sizeof(val))) {            /* 1.4.14 */
                Channel *chp = &s->channels[current_ch];
                if (chp->fx_count > 0) {
                    int k = chp->fx_count - 1, pa, cc, used;
                    for (const char *q = val; sscanf(q, "%d:%d%n", &pa, &cc, &used) == 2; q += used)
                        session_fx_midi_set(s, current_ch, k, pa, cc);
                }
            } else if (parse_kv(line, "fxstate", val, sizeof(val)) || parse_kv(line, "fxref", val, sizeof(val))) {
                Channel *chp = &s->channels[current_ch];
                if (chp->fx_count > 0) {
                    ChFx *fx = &chp->fx[chp->fx_count - 1];
                    if (strncmp(line, "fxref", 5) == 0) {
                        fx->state_ref = atoi(val);
                    } else {
                        if (fx->state_ref <= 0) fx->state_ref = fxstate_new();
                        unsigned char *bin = malloc(strlen(val) + 4);
                        if (bin) {
                            size_t n = b64_decode(val, bin), pad = b64_pad(val);
                            fxstate_append(fx->state_ref, bin, n >= pad ? n - pad : 0);
                            free(bin);
                        }
                    }
                }
            }
            continue;
        }

        if (current_ch >= 0 && current_sc >= 0) {
            if (parse_kv(line, "type", val, sizeof(val))) {
                cur_is_note = (strcmp(val, "note") == 0);
                cur_is_midi = (strcmp(val, "midi") == 0);
            } else if (parse_kv(line, "path", val, sizeof(val))) {
                snprintf(cur_path, sizeof(cur_path), "%s", val);
            } else if (parse_kv(line, "name", val, sizeof(val))) {
                strncpy(cur_name, val, sizeof(cur_name) - 1);
                cur_name[sizeof(cur_name) - 1] = '\0';
            } else if (parse_kv(line, "position", val, sizeof(val))) {
                cur_position = (size_t)atoll(val);
            } else if (parse_kv(line, "loop", val, sizeof(val))) {
                cur_loop = (atoi(val) != 0);
            } else if (parse_kv(line, "loop_start", val, sizeof(val))) {
                cur_loop_start = (size_t)atoll(val);
            } else if (parse_kv(line, "loop_end", val, sizeof(val))) {
                cur_loop_end = (size_t)atoll(val);
            } else if (parse_kv(line, "start_marker", val, sizeof(val))) {
                cur_start_marker = (size_t)atoll(val);
            } else if (parse_kv(line, "end_marker", val, sizeof(val))) {
                cur_end_marker = (size_t)atoll(val);
            } else if (parse_kv(line, "wave_zoom", val, sizeof(val))) {
                cur_wave_zoom = (float)atof(val);
            } else if (parse_kv(line, "color", val, sizeof(val))) {
                cur_color = atoi(val);
            } else if (parse_kv(line, "bind", val, sizeof(val))) {
                session_set_clip_bind(s, current_ch, current_sc, atoi(val));
            } else if (parse_kv(line, "wave_scroll", val, sizeof(val))) {
                cur_wave_scroll = (size_t)atoll(val);
            } else if (parse_kv(line, "bpm_detected", val, sizeof(val))) {
                cur_bpm_det = (float)atof(val);
            } else if (parse_kv(line, "bpm_fix", val, sizeof(val))) {
                cur_bpm_fix = (float)atof(val);
            } else if (parse_kv(line, "bpm_play", val, sizeof(val))) {
                cur_bpm_play = (float)atof(val);
            } else if (parse_kv(line, "warp", val, sizeof(val))) {
                cur_warp = atoi(val) != 0;
            } else if (parse_kv(line, "clip_gain", val, sizeof(val))) {
                cur_cgain = (float)atof(val);
            } else if (parse_kv(line, "clip_pan", val, sizeof(val))) {
                cur_cpan = (float)atof(val);
            } else if (parse_kv(line, "clip_pitch", val, sizeof(val))) {
                cur_pitch = (float)atof(val);
            } else if (parse_kv(line, "sur", val, sizeof(val))) {             /* 1.4.19 */
                cur_sur_n += sur_points_parse(val, cur_sur + cur_sur_n, SUR_MAX_PTS - cur_sur_n);   /* (lines add up) */
            } else if (parse_kv(line, "mc", val, sizeof(val))) {              /* 1.4.24 */
                sscanf(val, "%d,%d", &cur_mc, &cur_mc_out);
            } else if (parse_kv(line, "mc_map", val, sizeof(val))) {           /* 2.0 */
                const char *q = val;
                for (int k = 0; k < STREAM_MAX_CH && *q; ++k) { cur_mc_map[k] = (uint64_t)strtoull(q, (char **)&q, 16); if (*q != ',') break; ++q; }
                cur_mc_has_map = true;
            } else if (parse_kv(line, "mc_orig", val, sizeof(val))) {
                resolve_path(project_dir, val, cur_mc_orig, sizeof cur_mc_orig);   /* 2.1: also "samples/..." (Collect) */
            } else if (parse_kv(line, "clip_formants", val, sizeof(val))) {
                cur_fmt = atoi(val) != 0;
            } else if (parse_kv(line, "stem", val, sizeof(val))) {            /* 1.4.17: name|db|mute|solo|cc|path */
                char *f[6] = { val, NULL, NULL, NULL, NULL, NULL };
                int nf = 1;
                for (char *q = val; *q && nf < 6; ++q) if (*q == '|') { *q = 0; f[nf++] = q + 1; }
                if (nf == 6 && cur_st.set.n < STEM_MAX) {
                    int k = cur_st.set.n++;
                    snprintf(cur_st.set.name[k], sizeof cur_st.set.name[k], "%s", f[0]);
                    cur_st.db[k] = (float)atof(f[1]);
                    cur_st.mute[k] = atoi(f[2]);
                    cur_st.solo[k] = atoi(f[3]);
                    cur_st.cc[k] = atoi(f[4]);
                    resolve_path(project_dir, f[5], cur_st.set.path[k], sizeof cur_st.set.path[k]);   /* 2.1 */
                }
            } else if (parse_kv(line, "stems", val, sizeof(val))) {           /* 1.4.14 (2 parts) */
                char *bar = strchr(val, '|');
                if (bar) {
                    *bar = 0;
                    cur_st.set.n = 2;
                    cur_st.cc[0] = cur_st.cc[1] = -1;
                    resolve_path(project_dir, val, cur_st.set.path[0], sizeof cur_st.set.path[0]);
                    resolve_path(project_dir, bar + 1, cur_st.set.path[1], sizeof cur_st.set.path[1]);
                    snprintf(cur_st.set.name[0], sizeof cur_st.set.name[0], "Vocals");
                    snprintf(cur_st.set.name[1], sizeof cur_st.set.name[1], "Instrumental");
                }
            } else if (parse_kv(line, "stem_level", val, sizeof(val))) {
                sscanf(val, "%f %f", &cur_st.db[0], &cur_st.db[1]);
            } else if (parse_kv(line, "stem_mute", val, sizeof(val))) {
                sscanf(val, "%d %d", &cur_st.mute[0], &cur_st.mute[1]);
            } else if (strncmp(line, "edit_len=", 9) == 0 || strncmp(line, "edit_seg=", 9) == 0 ||
                       strncmp(line, "midi_len=", 9) == 0 || strncmp(line, "mn=", 3) == 0) {   /* 1.3: the notes too */
                size_t ll = strlen(line);
                char *ne = realloc(cur_edits, cur_edits_len + ll + 2);
                if (ne) {
                    cur_edits = ne;
                    memcpy(cur_edits + cur_edits_len, line, ll);
                    cur_edits_len += ll;
                    if (ll == 0 || line[ll - 1] != '\n') cur_edits[cur_edits_len++] = '\n';
                    cur_edits[cur_edits_len] = 0;
                }
            }
            continue;
        }
    }

    if (current_ch >= 0 && current_sc >= 0) {
        if (cur_is_midi) load_midi(s, current_ch, current_sc, cur_name, cur_loop, cur_edits);
        else { if (!cur_is_note) session_load_mc_next(cur_mc, cur_mc_out, cur_mc_orig);
               load_clip(s, project_dir,
                  current_ch, current_sc,
                  cur_is_note, cur_path, cur_name,
                  cur_position,
                cur_loop, cur_loop_start, cur_loop_end,
                          cur_start_marker, cur_end_marker,
                          cur_wave_zoom, cur_wave_scroll, cur_edits);
               session_load_mc_next(0, 0, NULL);
               if (cur_mc_has_map) for (int k = 0; k < STREAM_MAX_CH; ++k) session_clip_set_mc_map(s, current_ch, current_sc, k, cur_mc_map[k]); }
        if (cur_is_note && cur_color) session_set_note_color(s, current_ch, current_sc, cur_color);
        if (!cur_is_note && !cur_is_midi) apply_clip_tempo(s, current_ch, current_sc, cur_bpm_det, cur_bpm_fix, cur_bpm_play, cur_warp);
        if (!cur_is_note && !cur_is_midi) { session_set_clip_gain(s, current_ch, current_sc, cur_cgain); session_set_clip_pan(s, current_ch, current_sc, cur_cpan);
                  session_set_clip_formants(s, current_ch, current_sc, cur_fmt); session_set_clip_pitch(s, current_ch, current_sc, cur_pitch); }
                if (!cur_is_note && !cur_is_midi && cur_st.set.n > 0) apply_stems(s, current_ch, current_sc, &cur_st);
                if (!cur_is_note && cur_sur_n > 0) { sur_sort(cur_sur, cur_sur_n); session_clip_set_sur_points(s, current_ch, current_sc, cur_sur, cur_sur_n); }
    }

    if (in_channel_section && current_ch >= 0 &&
        current_ch < s->channel_count) {
        Channel *chp = &s->channels[current_ch];
        if (ch_gain_pending >= 0.0f) atomic_store(&chp->gain, ch_gain_pending);
        if (ch_has_pan) atomic_store(&chp->pan, ch_pan_pending);
        atomic_store(&chp->muted, ch_muted_pending);
        atomic_store(&chp->soloed, ch_soloed_pending);
        if (ch_color_pending >= 0) chp->color_id = ch_color_pending;
        if (ch_midi_cc_pending != -2) {
            atomic_store(&chp->midi_cc, ch_midi_cc_pending);
        }
        if (ch_pan_midi_cc_pending != -2) {
            atomic_store(&chp->pan_midi_cc, ch_pan_midi_cc_pending);
        }
        if (ch_has_name) {
            snprintf(chp->name, sizeof(chp->name), "%s", ch_name_pending);
        }
    }

    if (s->sample_rate <= 0) s->sample_rate = 48000;

    /* the scenes must cover all loaded cells (damaged/hand-edited file) */
    for (int c = 0; c < s->channel_count; ++c)
        for (int k = MAX_SCENES - 1; k >= s->scene_count; --k)
            if (atomic_load(&s->channels[c].clips[k].has_clip)) { s->scene_count = k + 1; break; }

    free(cur_edits);
    fclose(f);

    printf("project_load: OK '%s' (channels=%d, scenes=%d, sr=%d)\n",
           path, s->channel_count, s->scene_count, s->sample_rate);

    return 0;
}

// ============ zenity ============

/* The path is quoted for the shell, so even an apostrophe in it (e.g. "Don't") does not break the command */
static void sh_quote_dir(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 5 < n; ++p) {
        if (*p == '\'') { memcpy(out + o, "'\\''", 4); o += 4; }
        else out[o++] = *p;
    }
    out[o] = '\0';
}

/* Which folder the Save As / Open / Collect windows start in: the current project's folder (if there is one and
 * it exists), otherwise ~/qlim/Projects. It used to always be ~/qlim/Projects -> you could not find where you saved. */
static void dialog_dir(char *out, size_t n)
{
    const char *cur = project_get_current_path();
    if (cur && cur[0]) {
        snprintf(out, n, "%s", cur);
        char *sl = strrchr(out, '/');
        if (sl && sl != out) {
            *sl = '\0';
            struct stat st;
            if (stat(out, &st) == 0 && S_ISDIR(st.st_mode)) return;
        }
    }
    project_get_base_dir(out, n);
}

static int read_first_line(const char *path, char *out, size_t out_size)
{
    FILE *f = fopen(path, "r");
    if (!f) return 1;

    if (!fgets(out, out_size, f)) {
        fclose(f);
        return 1;
    }

    fclose(f);

    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) {
        out[--len] = '\0';
    }

    return (len == 0) ? 1 : 0;
}

int project_ask_name(char *out_name, size_t size)
{
    if (!out_name || size == 0) return 1;
#ifdef _WIN32
    {
        char def[256] = "Untitled";
        const char *cur = project_get_current_path();
        if (cur && cur[0]) {
            const char *b = strrchr(cur, '/');
            snprintf(def, sizeof def, "%s", b ? b + 1 : cur);
            char *dot = strstr(def, ".limproject");
            if (dot) *dot = '\0';
            if (!def[0]) snprintf(def, sizeof def, "Untitled");
        }
        if (qw_dlg_text("Q-LiM - Collect All & Save", "Name of the project folder (all audio files are copied into it):",
                        def, out_name, (int)size) != 0 || !out_name[0]) return 1;
        for (char *p = out_name; *p; ++p)
            if (*p == '/' || *p == '\\' || *p == ':' || *p == '*' || *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|') *p = '_';
        return 0;
    }
#endif

    char tmp_file[256];
    snprintf(tmp_file, sizeof(tmp_file), "/tmp/qlim_zenity_%d.txt", getpid());

    /* suggested name: the name of the current project (without .limproject) */
    char def[256] = "Untitled";
    const char *cur = project_get_current_path();
    if (cur && cur[0]) {
        const char *b = strrchr(cur, '/');
        snprintf(def, sizeof def, "%s", b ? b + 1 : cur);
        char *dot = strstr(def, ".limproject");
        if (dot) *dot = '\0';
        if (!def[0]) snprintf(def, sizeof def, "Untitled");
    }
    char qdef[600];
    sh_quote_dir(def, qdef, sizeof qdef);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "zenity --entry --title='Q-LiM - Collect All & Save' "
             "--text='Name of the project folder (all audio files are copied into it):' "
             "--entry-text='%s' "
             "> %s 2>/dev/null",
             qdef, tmp_file);

    int ret = system(cmd);
    if (ret != 0) {
        unlink(tmp_file);
        return 1;
    }

    int read_ret = read_first_line(tmp_file, out_name, size);
    unlink(tmp_file);

    if (read_ret != 0) return 1;

    for (char *p = out_name; *p; ++p) {
        if (*p == '/' || *p == '\\' || *p == ':') {
            *p = '_';
        }
    }

    return 0;
}

int project_ask_save_path(char *out_path, size_t size)
{
    if (!out_path || size == 0) return 1;

    char base_dir[512];
    dialog_dir(base_dir, sizeof(base_dir));
#ifdef _WIN32
    {
        char start[1100];
        snprintf(start, sizeof start, "%s/Untitled.limproject", base_dir);
        if (qw_dlg_save_file("Q-LiM - Save Project", start, "Q-LiM projects (*.limproject)", "*.limproject",
                             out_path, (int)size) != 0) return 1;
        size_t len = strlen(out_path), el = strlen(".limproject");
        if ((len < el || strcmp(out_path + len - el, ".limproject") != 0) && len + el + 1 < size) strcat(out_path, ".limproject");
        return 0;
    }
#endif
    char qdir[1100];
    sh_quote_dir(base_dir, qdir, sizeof qdir);

    char tmp_file[256];
    snprintf(tmp_file, sizeof(tmp_file), "/tmp/qlim_zenity_%d.txt", getpid());

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "zenity --file-selection --save "
             "--title='Q-LiM - Save Project' "
             "--filename='%s/' "
             "--file-filter='Q-LiM projects (*.limproject) | *.limproject' "
             "> %s 2>/dev/null",
             qdir, tmp_file);

    int ret = system(cmd);
    if (ret != 0) {
        unlink(tmp_file);
        return 1;
    }

    int read_ret = read_first_line(tmp_file, out_path, size);
    unlink(tmp_file);

    if (read_ret != 0) return 1;

    size_t len = strlen(out_path);
    const char *ext = ".limproject";
    size_t ext_len = strlen(ext);
    if (len < ext_len ||
        strcmp(out_path + len - ext_len, ext) != 0) {
        if (len + ext_len + 1 < size) {
            strcat(out_path, ext);
        }
    }

    return 0;
}

int project_ask_export_path(const char *start_dir, const char *def_name, const char *ext, const char *fmt_name,
                            char *out_path, size_t size)
{
    if (!out_path || size == 0 || !ext) return 1;
    char dir[1024];
    struct stat dst;
    if (start_dir && start_dir[0] && stat(start_dir, &dst) == 0 && S_ISDIR(dst.st_mode)) snprintf(dir, sizeof dir, "%s", start_dir);
    else dialog_dir(dir, sizeof dir);
    char name[300];
    snprintf(name, sizeof name, "%s", def_name && def_name[0] ? def_name : "export");
    for (char *p = name; *p; ++p)
        if (*p == '/' || *p == '\\' || *p == ':' || *p == '*' || *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|') *p = '_';
    char pat[32], filt[96];
    snprintf(pat, sizeof pat, "*.%s", ext);
    snprintf(filt, sizeof filt, "%s (*.%s)", fmt_name ? fmt_name : ext, ext);
#ifdef _WIN32
    {
        char start[1400];
        snprintf(start, sizeof start, "%s/%s.%s", dir, name, ext);
        if (qw_dlg_save_file("Q-LiM - Export Audio", start, filt, pat, out_path, (int)size) != 0) return 1;
    }
#else
    {
        char qdir[1100], qname[700], tmp_file[256], cmd[3000];
        sh_quote_dir(dir, qdir, sizeof qdir);
        sh_quote_dir(name, qname, sizeof qname);
        snprintf(tmp_file, sizeof tmp_file, "/tmp/qlim_zenity_%d.txt", getpid());
        snprintf(cmd, sizeof cmd,
                 "zenity --file-selection --save --confirm-overwrite --title='Q-LiM - Export Audio' "
                 "--filename='%s/%s.%s' --file-filter='%s | %s' > %s 2>/dev/null",
                 qdir, qname, ext, filt, pat, tmp_file);
        int ret = system(cmd);
        if (ret != 0) { unlink(tmp_file); return 1; }
        int rr = read_first_line(tmp_file, out_path, size);
        unlink(tmp_file);
        if (rr != 0) return 1;
    }
#endif
    /* the right extension (the format is chosen in Q-LiM, not by the name) */
    size_t len = strlen(out_path), el = strlen(ext);
    const char *dot = strrchr(out_path, '.'), *sl = strrchr(out_path, '/');
    bool ok = dot && (!sl || dot > sl) && strlen(dot + 1) == el && strncasecmp(dot + 1, ext, el) == 0;
    if (!ok && len + el + 2 < size) { strcat(out_path, "."); strcat(out_path, ext); }
    return 0;
}

int project_ask_open_path(char *out_path, size_t size)
{
    if (!out_path || size == 0) return 1;

    char base_dir[512];
    dialog_dir(base_dir, sizeof(base_dir));
#ifdef _WIN32
    return qw_dlg_open_file("Q-LiM - Open Project", base_dir, "Q-LiM projects (*.limproject)", "*.limproject",
                            out_path, (int)size);
#endif
    char qdir[1100];
    sh_quote_dir(base_dir, qdir, sizeof qdir);

    char tmp_file[256];
    snprintf(tmp_file, sizeof(tmp_file), "/tmp/qlim_zenity_%d.txt", getpid());

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "zenity --file-selection "
             "--title='Q-LiM - Open Project' "
             "--filename='%s/' "
             "--file-filter='Q-LiM projects (*.limproject) | *.limproject' "
             "> %s 2>/dev/null",
             qdir, tmp_file);

    int ret = system(cmd);
    if (ret != 0) {
        unlink(tmp_file);
        return 1;
    }

    int read_ret = read_first_line(tmp_file, out_path, size);
    unlink(tmp_file);

    return read_ret;
}

int project_ask_directory(char *out_path, size_t size)
{
    if (!out_path || size == 0) return 1;

    char base_dir[512];
    dialog_dir(base_dir, sizeof(base_dir));
#ifdef _WIN32
    return qw_dlg_folder("Q-LiM - Collect: choose WHERE to create the project folder", base_dir, out_path, (int)size);
#endif
    char qdir[1100];
    sh_quote_dir(base_dir, qdir, sizeof qdir);

    char tmp_file[256];
    snprintf(tmp_file, sizeof(tmp_file), "/tmp/qlim_zenity_%d.txt", getpid());

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "zenity --file-selection --directory "
             "--title='Q-LiM - Collect: choose WHERE to create the project folder' "
             "--filename='%s/' "
             "> %s 2>/dev/null",
             qdir, tmp_file);

    int ret = system(cmd);
    if (ret != 0) {
        unlink(tmp_file);
        return 1;
    }

    int read_ret = read_first_line(tmp_file, out_path, size);
    unlink(tmp_file);

    return read_ret;
}

/* 1.4.15: choose a folder (any title; start = where the window opens, NULL = the project's folder). 0 = chosen */
int project_ask_folder(const char *title, const char *start, char *out_path, size_t size)
{
    if (!out_path || size == 0) return 1;
    char base_dir[1024];
    struct stat st;
    if (start && start[0] && stat(start, &st) == 0 && S_ISDIR(st.st_mode)) snprintf(base_dir, sizeof base_dir, "%s", start);
    else dialog_dir(base_dir, sizeof base_dir);
#ifdef _WIN32
    return qw_dlg_folder(title, base_dir, out_path, (int)size);
#else
    char qdir[1100], qt[400], tmp_file[256], cmd[2048];
    sh_quote_dir(base_dir, qdir, sizeof qdir);
    sh_quote_dir(title ? title : "Q-LiM - Choose a folder", qt, sizeof qt);
    snprintf(tmp_file, sizeof tmp_file, "/tmp/qlim_zenity_%d.txt", getpid());
    snprintf(cmd, sizeof cmd, "zenity --file-selection --directory --title='%s' --filename='%s/' > %s 2>/dev/null", qt, qdir, tmp_file);
    int ret = system(cmd);
    if (ret != 0) { unlink(tmp_file); return 1; }
    int r = read_first_line(tmp_file, out_path, size);
    unlink(tmp_file);
    return r;
#endif
}

// ============ Collect All & Save ============

/* 2.0: a file that must be decoded while it plays (MP3, Ogg, Opus, FLAC, M4A ...) */
static bool is_compressed_audio(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *sl = strrchr(path, '/');
    if (dot && (!sl || dot > sl)) {
        static const char *ext[] = { "mp3", "ogg", "oga", "opus", "flac", "m4a", "aac", "mp4", "wma", "ape", "wv", "mka", NULL };
        for (int i = 0; ext[i]; ++i) if (strcasecmp(dot + 1, ext[i]) == 0) return true;
    }
    int k = audio_sniff(path, NULL, 0);
    return k == SNIFF_MP3 || k == SNIFF_FFMPEG;
}
static bool clip_convertible(const Clip *cl)
{
    return atomic_load(&cl->has_clip) && !atomic_load(&cl->is_note) && !atomic_load(&cl->is_midi) && cl->path[0] &&
           cl->mc_mode != CLIP_MC_MULTI && is_compressed_audio(cl->path);   /* (a multichannel file stays as it is) */
}

/* 2.0: progress of a running collect (it runs in its own thread; the window shows a bar) */
static volatile int g_cp_done, g_cp_total, g_cp_permille;
static char g_cp_name[256];
void project_collect_progress(int *done, int *total, int *permille, char *name, size_t n)
{
    *done = g_cp_done; *total = g_cp_total; *permille = g_cp_permille;
    if (name && n) snprintf(name, n, "%s", g_cp_name);
}

int project_collect_count_compressed(Session *s)
{
    if (!s) return 0;
    int n = 0;
    for (int c = 0; c < s->channel_count; ++c)
        for (int sc = 0; sc < s->scene_count; ++sc)
            if (clip_convertible(&s->channels[c].clips[sc])) ++n;
    return n;
}

int project_collect_and_save(Session *s, const char *project_dir,
                             const char *project_name, int fmt, int *converted)
{
    if (converted) *converted = 0;
    if (!s || !project_dir || !project_name) return 1;
    g_coll_nren = 0;
    g_cp_done = 0; g_cp_permille = 0; g_cp_name[0] = 0;
    {   int t = 0;
        for (int c = 0; c < s->channel_count; ++c)
            for (int sc = 0; sc < s->scene_count; ++sc) {
                const Clip *cl = &s->channels[c].clips[sc];
                if (atomic_load(&cl->has_clip) && !atomic_load(&cl->is_note) && cl->path[0]) ++t;
            }
        g_cp_total = t;
    }

    if (ensure_dir_recursive(project_dir) != 0) {
        fprintf(stderr, "collect: cannot create dir '%s'\n", project_dir);
        return 1;
    }

    char samples_dir[1024];
    snprintf(samples_dir, sizeof(samples_dir), "%s/samples", project_dir);

    if (ensure_dir_recursive(samples_dir) != 0) {
        fprintf(stderr, "collect: cannot create dir '%s'\n", samples_dir);
        return 1;
    }

    int copied = 0;
    int failed = 0;

    for (int c = 0; c < s->channel_count; ++c) {
        for (int sc = 0; sc < s->scene_count; ++sc) {
            Clip *cl = &s->channels[c].clips[sc];
            if (!atomic_load(&cl->has_clip)) continue;
            if (atomic_load(&cl->is_note)) continue;
            if (cl->path[0] == '\0') continue;

            snprintf(g_cp_name, sizeof g_cp_name, "%s", basename_of(cl->path));
            g_cp_permille = 0;
            /* the clip's file + 2.1: a surround path's original and the separated parts */
            const char *files[2 + STEM_MAX];
            int nf = 0;
            files[nf++] = cl->path;
            if (cl->mc_mode == CLIP_MC_PATH && cl->mc_orig && cl->mc_orig[0]) files[nf++] = cl->mc_orig;
            for (int k = 0; k < session_clip_stem_count(s, c, sc) && nf < 2 + STEM_MAX; ++k) {
                const char *sp = session_clip_stem_path(s, c, sc, k);
                if (sp && sp[0]) files[nf++] = sp;
            }
            if (fmt != WAV_OUT_KEEP && clip_convertible(cl)) {   /* 2.0: the clip's own file -> WAV / FLAC */
                char nm[256];
                snprintf(nm, sizeof nm, "%s", basename_of(cl->path));
                char *dot = strrchr(nm, '.');
                if (dot) *dot = '\0';
                char stem[240];
                snprintf(stem, sizeof stem, "%s", nm);
                const char *ext = fmt == WAV_OUT_FLAC24 ? ".flac" : ".wav";
                snprintf(nm, sizeof nm, "%s%s", stem, ext);   /* "song.mp3" -> "song.wav" */
                /* the same name from another file in this collect (song.mp3 and song.flac in two folders) -> "song (2).wav" */
                for (int k = 2; k < 100; ++k) {
                    bool taken = false;
                    for (int i = 0; i < g_coll_nren; ++i)
                        if (strcmp(g_coll_ren[i].name, nm) == 0 && strcmp(g_coll_ren[i].src, cl->path) != 0) { taken = true; break; }
                    if (!taken) break;
                    snprintf(nm, sizeof nm, "%s (%d)%s", stem, k, ext);
                }
                char dst[1024];
                int n = snprintf(dst, sizeof dst, "%s/%s", samples_dir, nm);
                struct stat st;
                bool done = false;
                for (int i = 0; i < g_coll_nren; ++i)        /* the same file already converted in this collect */
                    if (strcmp(g_coll_ren[i].src, cl->path) == 0 && strcmp(g_coll_ren[i].name, nm) == 0) done = true;
                bool self = false;                            /* collecting into the folder the file already is in */
                {   struct stat sa, sb;                       /* the same file = the same device and inode */
                    if (n > 0 && n < (int)sizeof dst && stat(cl->path, &sa) == 0 && stat(dst, &sb) == 0 &&
                        (strcmp(cl->path, dst) == 0 || (sa.st_ino != 0 && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino))) self = true; }
                if (!done && !self && n > 0 && n < (int)sizeof dst && stat(dst, &st) == 0) unlink(dst);   /* an older one: made new */
                if (!done && !self && n > 0 && n < (int)sizeof dst) {
                    StreamSource src;
                    if (session_open_clip_source(s, cl->path, &src) == 0) {
                        done = wav_write_source(&src, s->sample_rate > 0 ? s->sample_rate : 48000, fmt, dst, &g_cp_permille) == 0;
                        src.close(src.ctx);
                    }
                    if (done) { if (converted) ++*converted; printf("collect: '%s' -> '%s'\n", cl->path, dst); }
                    else fprintf(stderr, "collect: could not convert '%s' - copied as it is\n", cl->path);
                }
                if (done) {
                    CollRen *nr = realloc(g_coll_ren, sizeof *nr * (size_t)(g_coll_nren + 1));
                    if (nr) {
                        g_coll_ren = nr;
                        g_coll_ren[g_coll_nren].cl = cl;
                        snprintf(g_coll_ren[g_coll_nren].name, sizeof g_coll_ren[g_coll_nren].name, "%s", nm);
                        snprintf(g_coll_ren[g_coll_nren].src, sizeof g_coll_ren[g_coll_nren].src, "%s", cl->path);
                        ++g_coll_nren;
                    }
                    files[0] = NULL;                        /* the original is not copied */
                }
            }
            for (int i = 0; i < nf; ++i) {
                const char *src = files[i];
                if (!src) continue;
                char dst[1024];
                int n = snprintf(dst, sizeof(dst), "%s/%s", samples_dir, basename_of(src));
                if (n < 0 || n >= (int)sizeof(dst)) {
                    fprintf(stderr, "collect: path too long\n");
                    continue;
                }
                struct stat st;
                if (stat(dst, &st) == 0) continue;
                if (copy_file(src, dst) == 0) {
                    copied++;
                } else {
                    fprintf(stderr, "collect: failed to copy '%s'\n", src);
                    failed++;
                }
            }
            g_cp_permille = 1000;
            ++g_cp_done;
        }
    }

    if (failed == 0) printf("collect: copied %d files, all OK\n", copied);      /* (1.3.13: "failed 0" was shown as an error) */
    else printf("collect: copied %d files, %d failed to copy\n", copied, failed);

    char proj_file[1024];
    snprintf(proj_file, sizeof(proj_file), "%s/%s.limproject",
             project_dir, project_name);

    char tmp[1100];
    FILE *f = safe_open(proj_file, tmp, sizeof tmp);
    if (!f) {
        fprintf(stderr, "collect: cannot open '%s' for writing\n", proj_file);
        return 1;
    }

    int ret = save_to_file(s, f, true);
    g_coll_nren = 0;
    if (ret != 0) { fclose(f); unlink(tmp); }
    else ret = safe_commit(f, tmp, proj_file, 1);

    if (ret == 0) {
        printf("collect: OK project saved to '%s'\n", proj_file);
    }

    return ret;
}

// ============ Current project ============

static char g_current_project_path[1024] = {0};

const char *project_get_current_path(void)
{
    return g_current_project_path;
}

void project_set_current_path(const char *path)
{
    if (!path) {
        g_current_project_path[0] = '\0';
        return;
    }
    snprintf(g_current_project_path, sizeof(g_current_project_path),
             "%s", path);
}

bool project_has_current(void)
{
    return g_current_project_path[0] != '\0';
}

// ============ New project ============

void project_new(Session *s)
{
    if (!s) return;
    sched_clear();                                 /* 1.4.17 */
    s->tr_cur = -1; s->tr_hist_n = 0;
    session_midi_panic(s);                         /* 1.2 */
    session_set_midi_target(s, -1);

    session_release_all_clips(s);

    s->channel_count = 0;
    s->scene_count = 0;

    atomic_store(&s->master_gain, FADER_UNITY_POS);
    atomic_store(&s->master_pan, 0.0f);
    atomic_store(&s->master_muted, false);
    atomic_store(&s->master_midi_cc, -1);
    atomic_store(&s->master_route_l, MASTER_ROUTE_DEFAULT_L);
    atomic_store(&s->master_route_r, MASTER_ROUTE_DEFAULT_R);
    session_set_tempo(s, 120.0f);

    if (s->sample_rate <= 0) s->sample_rate = 48000;

    for (int i = 0; i < NEW_PROJECT_CHANNELS; ++i) {
        Channel *c = &s->channels[i];
        session_channel_init(c, i, NULL);
        s->channel_count++;
    }

    s->scene_count = NEW_PROJECT_SCENES;

    project_set_current_path("");

    printf("project_new: fresh session (%d channels, %d scenes, sr=%d)\n",
           NEW_PROJECT_CHANNELS, NEW_PROJECT_SCENES, s->sample_rate);
}
