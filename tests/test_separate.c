/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.4.14: vocal separation end to end (src/separate.c + the qlim-separate program + the real model):
 * first-use install (download from file:// copies, checksum, unpack), the separation of a clip with a known
 * synthetic voice over real music, the stems playing in the clip (levels, mute, through warp), cancel, the cache,
 * project save / load, export of one stem, "split to clips".
 * Arguments: <qlim-separate> <folder with mel-roformer.fp16.onnx + microsoft.ml.onnxruntime.1.22.0.nupkg> <mix.wav> <voice.wav>
 *   (mix.wav / voice.wav: 44.1 kHz; make them with tests/gen_sep_audio.py) */
#define _POSIX_C_SOURCE 200809L
#include "../src/session.h"
#include "../src/mixer.h"
#include "../src/separate.h"
#include "../src/export.h"
#include "../src/project.h"
#include <math.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void nap(long ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
#define SR 44100                 /* = the files: no resampling, so the stems can be compared sample for sample */

static float *load(const char *p, long *n, int *sr)
{
    SF_INFO i = { 0 };
    SNDFILE *f = sf_open(p, SFM_READ, &i);
    if (!f) return NULL;
    float *b = malloc((size_t)i.frames * 2 * sizeof(float));
    *n = (long)sf_readf_float(f, b, i.frames);
    if (sr) *sr = i.samplerate;
    sf_close(f);
    return b;
}

/* renders the clip (playing from its start) for `secs`, returns interleaved stereo */
static float *render(Session *s, int ch, double secs, long *n)
{
    session_stop_all(s);
    for (int i = 0; i < 20; ++i) { static float z[512]; mixer_render(s, z, 256, 2); }
    session_play_clip_from(s, ch, 0, 0);
    long total = (long)(secs * SR);
    float *out = calloc((size_t)(total + 256) * 2, sizeof(float));   /* (whole blocks of 256) */
    for (long p = 0; p < total; p += 256) {
        nap(4);                                    /* the disk thread fills the ring */
        mixer_render(s, out + p * 2, 256, 2);
    }
    *n = total;
    return out;
}

/* best alignment (the mixer's start fades / latency) and the residual energy of a - g*b in dB below a */
static double resid_db(const float *a, long na, const float *b, long nb, long from, long len, int ch, int *lag_out)
{
    double best = 1e9; int bl = 0;
    for (int lag = -400; lag <= 4000; lag += 1) {
        double e = 0, r = 0;
        for (long i = from; i < from + len; i += 7) {
            long j = i - lag;
            if (j < 0 || j >= nb || i >= na) continue;
            double d = a[i * 2 + ch] - b[j * 2 + ch];
            e += d * d; r += (double)a[i * 2 + ch] * a[i * 2 + ch];
        }
        double v = r > 0 ? 10 * log10(e / r + 1e-30) : 0;
        if (v < best) { best = v; bl = lag; }
    }
    if (lag_out) *lag_out = bl;
    return best;
}

int main(int argc, char **argv)
{
    if (argc < 5) { printf("usage: test_separate <qlim-separate> <dir with model + ort nupkg> <mix.wav> <voice.wav>\n"); return 2; }
    const char *helper = argv[1], *assets = argv[2], *mixp = argv[3], *voicep = argv[4];
    if (access(helper, X_OK) != 0) { printf("FAIL: no helper program %s (make qlim-separate)\nSEPARATE: FAILED\n", helper); return 1; }
    if (system("rm -rf /tmp/qlim_sep_data /tmp/qlim_sep_cache /tmp/qlim_sep_home && mkdir -p /tmp/qlim_sep_data /tmp/qlim_sep_home") != 0) return 2;
    setenv("HOME", "/tmp/qlim_sep_home", 1);                 /* its own separation.conf */
    {   /* 1.4.15: the settings (folder, format) - 32-bit float WAV here, so the stems can be compared exactly */
        SepPrefs pr;
        sep_prefs_get(&pr);
        CHECK(!pr.asked && pr.format == EXP_FLAC && pr.bits == 24, "default settings: not asked yet, FLAC 24-bit");
        char dd[1024];
        sep_default_dir(dd, sizeof dd);
        CHECK(strcmp(dd, "/tmp/qlim_sep_home/Music/Q-LiM Stems") == 0, "the default folder: ~/Music/Q-LiM Stems");
        snprintf(pr.dir, sizeof pr.dir, "/tmp/qlim_sep_cache");
        pr.asked = true; pr.format = EXP_WAV; pr.bits = 32; pr.rate = 0;
        sep_prefs_set(&pr);
        SepPrefs back;
        memset(&back, 0, sizeof back);
        sep_prefs_get(&back);
        char sd[1024];
        sep_stems_dir(sd, sizeof sd);
        CHECK(back.asked && back.format == EXP_WAV && back.bits == 32 && strcmp(sd, "/tmp/qlim_sep_cache") == 0,
              "a chosen folder and format are kept (%s)", sd);
    }
    sep_set_paths(helper, "/tmp/qlim_sep_data", NULL);
    char url[1200];
    snprintf(url, sizeof url, "file://%s/mel-roformer.fp16.onnx", assets); setenv("QLIM_SEP_MODEL_URL", url, 1);
    snprintf(url, sizeof url, "file://%s/microsoft.ml.onnxruntime.1.22.0.nupkg", assets); setenv("QLIM_SEP_ORT_URL", url, 1);
    snprintf(url, sizeof url, "file://%s/sep2/htdemucs_6s_fp16weights.onnx", assets); setenv("QLIM_SEP_DEMUCS_URL", url, 1);
    snprintf(url, sizeof url, "file://%s/sep2/bs_roformer_sw_6stem_fp16.onnx", assets); setenv("QLIM_SEP_BSRSW_URL", url, 1);
    const SepRequest VO = { SEP_P_VOCALS, false, SEP_BSRSW };
    streams_init();

    CHECK(sep_supported(), "supported (64-bit, the helper is there)");
    CHECK(!sep_model_installed(SEP_MEL) && sep_need_mb(&VO) > 400, "not installed yet: %.0f MB to download", sep_need_mb(&VO));

    static Session S; Session *s = &S;
    session_init(s, SR);
    session_add_channel(s, "Song");
    session_add_scene(s);
    atomic_store(&s->audio_running, false);
    CHECK(session_set_clip(s, 0, 0, mixp) == 0, "the song is a clip");

    /* 1) first use: install + separate; cancel once in the middle of the separation */
    char err[300] = "", text[300];
    SepJob *j = sep_start(s, 0, 0, &VO, err, sizeof err);
    CHECK(j != NULL, "the separation starts (%s)", err);
    int st = 0; float p = 0; int saw_dl = 0, saw_run = 0;
    time_t t0 = time(NULL);
    while (j && (st = sep_state(j, &p, text, sizeof text)) != SEP_DONE && st != SEP_FAILED && st != SEP_CANCELLED) {
        if (st == SEP_DOWNLOADING) saw_dl = 1;
        if (st == SEP_SEPARATING && p > 0.0f) { saw_run = 1; sep_cancel(j); }
        nap(50);
        if (time(NULL) - t0 > 900) break;
    }
    CHECK(saw_dl && sep_model_installed(SEP_MEL) && sep_runtime_installed(), "downloaded, checked and unpacked (model + ONNX Runtime)");
    CHECK(st == SEP_CANCELLED && saw_run, "cancel during the separation stops it (%s)", text);
    sep_free(j);
    char v[1100], in[1100];
    static StemSet SS;
    CHECK(sep_cached(mixp, &VO, &SS) != 0 && system("test -e '/tmp/qlim_sep_cache/mix'") != 0, "a cancelled separation leaves no stems (and no folder)");
    CHECK(system("ps -eo comm | grep -q qlim-separat") != 0, "and the qlim-separate process is gone");

    /* 2) the real run */
    j = sep_start(s, 0, 0, &VO, err, sizeof err);
    t0 = time(NULL);
    float lastp = -1; int steps = 0;
    while (j && (st = sep_state(j, &p, text, sizeof text)) != SEP_DONE && st != SEP_FAILED && st != SEP_CANCELLED) {
        if (p != lastp) { lastp = p; ++steps; if (st == SEP_SEPARATING) printf("      %s\n", text); }
        nap(100);
    }
    double took = difftime(time(NULL), t0);
    CHECK(st == SEP_DONE, "separated in %.0f s (%s)", took, text);
    CHECK(sep_result(j, &SS) && SS.n == 2 && !strcmp(SS.name[1], "Instrumental"), "the stems: %s", SS.path[0]);
    snprintf(v, sizeof v, "%s", SS.path[0]); snprintf(in, sizeof in, "%s", SS.path[1]);
    sep_free(j);
    memset(&SS, 0, sizeof SS);
    CHECK(sep_cached(mixp, &VO, &SS) == 0 && !strcmp(SS.path[0], v), "they are found again (the next time: instant)");
    CHECK(strcmp(v, "/tmp/qlim_sep_cache/mix/mix (Vocals).wav") == 0 && strcmp(in, "/tmp/qlim_sep_cache/mix/mix (Instrumental).wav") == 0,
          "in a folder named after the track: %s", v);
    {
        double b = 0; int n = 0;
        sep_usage(&b, &n);
        CHECK(n == 1 && b > 2.0 * 441000 * 8, "the space they take is counted (%d song, %.1f MB)", n, b / 1048576.0);
    }

    /* 3) the quality: the known voice vs the vocal stem (both 44.1 kHz) */
    long nv = 0, nk = 0, nm = 0;
    float *ve = load(v, &nv, NULL), *vk = load(voicep, &nk, NULL), *mx = load(mixp, &nm, NULL), *ie = load(in, &nk, NULL);
    double e = 0, r = 0, sum = 0;
    for (long i = 0; i < nv * 2 && i < nk * 2; ++i) { double d = vk[i] - ve[i]; e += d * d; r += (double)vk[i] * vk[i]; }
    for (long i = 0; i < nv * 2 && i < nm * 2; ++i) { double d = mx[i] - ve[i] - ie[i]; if (fabs(d) > sum) sum = fabs(d); }
    double sdr = 10 * log10(r / e);
    /* how much of the MUSIC got into the vocal stem (the voice itself may be judged partly "synth" by the model:
     * a synthetic singer is not a real one - its quality on real songs is measured separately) */
    double mm = 0, me = 0;
    for (long i = 0; i < nv * 2 && i < nk * 2 && i < nm * 2; ++i) { double mu = mx[i] - vk[i]; mm += mu * mu; me += (ve[i] - vk[i]) * mu; }
    double leak = me / mm;
    printf("      (vocals SDR %.1f dB against the synthetic singer)\n", sdr);
    CHECK(fabs(leak) < 0.03, "almost none of the music leaks into the vocal stem (%.4f)", leak);
    CHECK(sum < 1e-5, "vocals + instrumental = the original exactly (max diff %.1e)", sum);

    /* 4) the stems play in the clip: both at 0 dB = the original; vocals muted = the instrumental */
    long n0, n1;
    float *orig = render(s, 0, 4.0, &n0);
    CHECK(session_clip_set_stems(s, 0, 0, v, in) == 0 && session_clip_has_stems(s, 0, 0), "the stems are put into the clip");
    float *both = render(s, 0, 4.0, &n1);
    int lag;
    double d0 = resid_db(orig, n0, both, n1, SR, SR * 2, 0, &lag);
    CHECK(d0 < -40.0, "both stems at 0 dB sound like the original (difference %.1f dB)", d0);
    session_clip_set_stem_mute(s, 0, 0, STEM_VOCALS, true);
    session_clip_stems_apply(s, 0, 0);
    float *inst = render(s, 0, 4.0, &n1);
    double di = resid_db(ie, nk, inst, n1, SR, SR * 2, 0, NULL);
    /* the instrumental the model made, through the session's rate */
    ClipSnapshot cs;
    session_clip_snapshot(s, 0, 0, &cs);
    CHECK(cs.stems && cs.stem_gain[0] == 0.0f && cs.stem_gain[1] == 1.0f, "the snapshot knows: vocals muted");
    edit_free(cs.edits);
    CHECK(di < -40.0, "vocals muted: the clip plays exactly the instrumental stem (difference %.1f dB)", di);
    session_clip_set_stem_mute(s, 0, 0, STEM_VOCALS, false);
    session_clip_set_stem_level(s, 0, 0, STEM_INST, CLIP_GAIN_MIN_DB);
    session_clip_stems_apply(s, 0, 0);
    float *voc = render(s, 0, 4.0, &n1);
    double dv = resid_db(ve, nv, voc, n1, SR, SR * 2, 0, NULL);
    CHECK(dv < -40.0, "instrumental at -inf: the clip plays exactly the vocal stem (difference %.1f dB)", dv);
    session_clip_set_stem_level(s, 0, 0, STEM_INST, -3.0f);
    /* with warp at once (the stems go through the Stretch like the file) */
    session_set_clip_bpm_fields(s, 0, 0, 120.0f, 120.0f, 132.0f);
    session_set_clip_warp(s, 0, 0, true);
    float *wp = render(s, 0, 3.0, &n1);
    double we = 0;
    for (long i = SR; i < SR * 2; ++i) we += (double)wp[i * 2] * wp[i * 2];
    CHECK(we > 0.001, "stems + warp play (energy %.3f)", we);
    session_set_clip_warp(s, 0, 0, false);

    /* 5) export of one stem */
    ExportOpts o = { EXP_WAV, 32, 44100, 0, 1 };
    ExportJob *ej = export_clip_start(s, 0, 0, &o, "/tmp/qlim_sep_vocals.wav", err, sizeof err);
    while (ej && !export_finished(ej)) nap(20);
    int er = export_result(ej, text, sizeof text);
    export_free(ej);
    long nx = 0;
    float *xv = load("/tmp/qlim_sep_vocals.wav", &nx, NULL);
    double xe = 0, xr = 0;
    for (long i = 0; i < nx * 2 && i < nv * 2; ++i) { double d = ve[i] - xv[i]; xe += d * d; xr += (double)ve[i] * ve[i]; }
    CHECK(er == 0 && xv && nx == nv && xe <= xr * 1e-10, "\"Export vocals\" writes the vocal stem alone, exactly (%s)", text);

    /* 6) the project keeps them */
    CHECK(project_save(s, "/tmp/qlim_sep.limproject") == 0, "saved");
    static Session T;
    session_init(&T, SR);
    atomic_store(&T.audio_running, false);
    CHECK(project_load(&T, "/tmp/qlim_sep.limproject") == 0 && session_clip_has_stems(&T, 0, 0) &&
          fabsf(session_clip_get_stem_level(&T, 0, 0, STEM_INST) + 3.0f) < 0.01f && !session_clip_get_stem_mute(&T, 0, 0, STEM_VOCALS),
          "loaded: the clip plays the stems again, levels kept");

    /* 7) split to clips */
    {   /* (1.4.17: the parts are carried by the mouse and dropped one under another into empty cells) */
        ClipPart pv, pi;
        CHECK(session_clip_capture_part(&T, 0, 0, 0, &pv) == 0 && session_clip_capture_part(&T, 0, 0, 1, &pi) == 0, "split: both parts taken");
        if (T.channel_count < 2) session_add_channel(&T, "Parts");
        int r1 = session_place_part(&T, 1, 0, &pv), r2 = session_place_part(&T, 1, 1, &pi);
        CHECK(r1 == 0 && r2 == 0, "and dropped into channel 2, one under another");
        CHECK(session_place_part(&T, 1, 0, &pv) == 2, "a full cell is refused");
        CHECK(atomic_load(&T.channels[1].clips[0].has_clip) && atomic_load(&T.channels[1].clips[1].has_clip) &&
              strstr(T.channels[1].clips[0].path, "(Vocals)") && strstr(T.channels[1].clips[1].path, "(Instrumental)"),
              "with the vocals and the instrumental as clips (%s / %s)", T.channels[1].clips[0].name, T.channels[1].clips[1].name);
        CHECK(fabsf(session_get_clip_gain(&T, 1, 1) + 3.0f) < 0.01f, "the instrumental clip got the -3 dB level");
        clip_part_free(&pv); clip_part_free(&pi);
    }

    /* 8) the format setting: the same tracks as FLAC 24-bit (what a separation saves by default) */
    {
        ExportOpts fo = { EXP_FLAC, 24, 44100, 0, 0 };
        CHECK(export_file_now(v, &fo, "/tmp/qlim_sep_v.flac", err, sizeof err) == 0, "saving as FLAC 24-bit works (%s)", err);
        long nf = 0;
        float *fv = load("/tmp/qlim_sep_v.flac", &nf, NULL);
        double fe = 0, fr = 0;
        for (long i = 0; fv && i < nf * 2 && i < nv * 2; ++i) { double d = fv[i] - ve[i]; fe += d * d; fr += (double)ve[i] * ve[i]; }
        CHECK(fv && nf == nv && 10 * log10(fr / (fe + 1e-30)) > 100.0, "FLAC 24-bit = the float stem to %.0f dB", 10 * log10(fr / (fe + 1e-30)));
    }
    /* 8) back to the file */
    CHECK(session_clip_set_stems(s, 0, 0, NULL, NULL) == 0 && !session_clip_has_stems(s, 0, 0), "remove: the clip plays its file again");
    float *back = render(s, 0, 4.0, &n1);
    double db = resid_db(orig, n0, back, n1, SR, SR * 2, 0, NULL);
    CHECK(db < -60.0, "exactly as before (difference %.1f dB)", db);
    /* 9) delete all */
    StemSet gone;
    CHECK(sep_delete_all() == 1 && sep_cached(mixp, &VO, &gone) != 0 && system("test -e '/tmp/qlim_sep_cache/mix'") != 0,
          "\"Delete all\" removes the song folder");

    /* 10) 1.4.17: several parts, chosen. Drums + bass + vocals with HTDemucs; the rest is "Other";
     *     everything adds up to the mix exactly (32-bit float WAV here) */
    {
        SepRequest rq = { SEP_P_VOCALS | SEP_P_DRUMS | SEP_P_BASS, false, SEP_DEMUCS };
        int mods[2];
        CHECK(sep_request_models(&rq, mods) == 1 && mods[0] == SEP_DEMUCS, "drums + bass + vocals: one model (HTDemucs)");
        CHECK(sep_need_mb(&rq) > 100 && sep_ram_need_mb(&rq) > 1000 && sep_ram_total_mb() > 0,
              "it must download %.0f MB first, needs %d MB RAM (this computer: %d MB)", sep_need_mb(&rq), sep_ram_need_mb(&rq), sep_ram_total_mb());
        j = sep_start(s, 0, 0, &rq, err, sizeof err);
        bool loading = false, separating = false;
        while (j && (st = sep_state(j, &p, text, sizeof text)) != SEP_DONE && st != SEP_FAILED && st != SEP_CANCELLED) {
            if (strstr(text, "Loading the AI model")) loading = true;
            if (strstr(text, "Separating the parts")) separating = true;
            nap(100);
        }
        CHECK(st == SEP_DONE, "separated (%s)", text);
        CHECK(loading && separating, "the text says \"Loading the AI model\" first, then \"Separating the parts\"");
        CHECK(sep_result(j, &SS) && SS.n == 4 && !strcmp(SS.name[0], "Vocals") && !strcmp(SS.name[1], "Drums") &&
              !strcmp(SS.name[2], "Bass") && !strcmp(SS.name[3], "Other"), "4 parts: %s, %s, %s, %s", SS.name[0], SS.name[1], SS.name[2], SS.name[3]);
        sep_free(j);
        CHECK(strstr(SS.path[1], "/mix/mix (Drums).wav") != NULL, "in their own song folder (%s)", SS.path[1]);
        long nmx = 0, np = 0;
        float *mx2 = load(mixp, &nmx, NULL);
        double *acc = calloc((size_t)nmx * 2, sizeof(double)), er = 0, rr = 0, e_d = 0;
        for (int k = 0; k < SS.n; ++k) {
            float *q = load(SS.path[k], &np, NULL);
            for (long i = 0; q && i < np * 2 && i < nmx * 2; ++i) { acc[i] += q[i]; if (k == 1) e_d += (double)q[i] * q[i]; }
            free(q);
        }
        for (long i = 0; i < nmx * 2; ++i) { double d = acc[i] - mx2[i]; er += d * d; rr += (double)mx2[i] * mx2[i]; }
        CHECK(np == nmx && er <= rr * 1e-10, "all the parts together = the song exactly (%.0f dB)", 10 * log10(rr / (er + 1e-30)));
        CHECK(e_d / nmx > 1e-5, "the drums are there (the test song has drums: rms %.3f)", sqrt(e_d / (nmx * 2)));
        free(acc); free(mx2);
        StemSet again;
        CHECK(sep_cached(mixp, &rq, &again) == 0 && again.n == 4 && !strcmp(again.name[3], "Other"), "the same choice is found again (instant)");
        SepRequest other = rq;
        other.parts = SEP_P_DRUMS;
        CHECK(sep_cached(mixp, &other, &again) != 0, "another choice is not");
        CHECK(session_clip_set_stem_set(s, 0, 0, &SS) == 0 && session_clip_stem_count(s, 0, 0) == 4, "the clip plays the 4 parts");
        long nn = 0;
        float *all = render(s, 0, 4.0, &nn);
        double dd = resid_db(orig, n0, all, nn, SR, SR * 2, 0, NULL);
        CHECK(dd < -40.0, "all at 0 dB = the original (difference %.1f dB)", dd);
        free(all);
    }
    /* 11) the best vocals: Mel-Band RoFormer first, then the instruments from the rest; and the model removal */
    {
        SepRequest rq = { SEP_P_VOCALS | SEP_P_GUITAR, true, SEP_DEMUCS };
        int mods[2];
        CHECK(sep_request_models(&rq, mods) == 2 && mods[0] == SEP_MEL && mods[1] == SEP_DEMUCS && sep_need_mb(&rq) == 0.0,
              "vocals (best) + guitar: Mel-Band RoFormer, then HTDemucs - both here already");
        j = sep_start(s, 0, 0, &rq, err, sizeof err);
        bool step2 = false;
        while (j && (st = sep_state(j, &p, text, sizeof text)) != SEP_DONE && st != SEP_FAILED && st != SEP_CANCELLED) {
            int ns = 0;
            if (sep_step(j, &ns) == 2 && ns == 2 && strstr(text, "Step 2 of 2")) step2 = true;
            nap(100);
        }
        CHECK(st == SEP_DONE && step2, "both steps ran (%s)", text);
        StemSet b;
        CHECK(sep_result(j, &b) && b.n == 3 && !strcmp(b.name[1], "Guitar") && !strcmp(b.name[2], "Other"), "3 parts: Vocals, Guitar, Other");
        sep_free(j);
        long n1v = 0, n2v = nv;
        float *va = load(b.path[0], &n1v, NULL), *vb = ve;                   /* the vocals = the ones of the vocals-only run */
        double de = 0, dr = 0;
        for (long i = 0; va && vb && i < n1v * 2 && i < n2v * 2; ++i) { double d = va[i] - vb[i]; de += d * d; dr += (double)vb[i] * vb[i]; }
        CHECK(va && vb && de <= dr * 1e-8 + 1e-12, "the vocals are Mel-Band RoFormer's (the same as before)");
        free(va);

        double mb = sep_model_disk_mb(SEP_DEMUCS);
        CHECK(mb > 100 && sep_model_remove(SEP_DEMUCS) == 0 && !sep_model_installed(SEP_DEMUCS) && sep_model_disk_mb(SEP_DEMUCS) == 0.0,
              "a model can be removed (%.0f MB freed)", mb);
        CHECK(sep_need_mb(&rq) > 100, "then it is needed again (%.0f MB)", sep_need_mb(&rq));
        CHECK(sep_runtime_disk_mb() > 1 && sep_model_disk_mb(SEP_MEL) > 400, "the others stay (runtime %.0f MB, Mel %.0f MB)", sep_runtime_disk_mb(), sep_model_disk_mb(SEP_MEL));
    }
    /* 12) BS-RoFormer SW (the best 6-part model) - drums only; QLIM_SEP_SKIP_BSRSW=1 skips it (slow) */
    if (!getenv("QLIM_SEP_SKIP_BSRSW")) {
        SepRequest rq = { SEP_P_DRUMS, false, SEP_BSRSW };
        j = sep_start(s, 0, 0, &rq, err, sizeof err);
        t0 = time(NULL);
        while (j && (st = sep_state(j, &p, text, sizeof text)) != SEP_DONE && st != SEP_FAILED && st != SEP_CANCELLED) nap(200);
        StemSet b;
        CHECK(st == SEP_DONE && sep_result(j, &b) && b.n == 2 && !strcmp(b.name[0], "Drums") && !strcmp(b.name[1], "Other"),
              "BS-RoFormer SW: Drums + Other in %.0f s (%s)", difftime(time(NULL), t0), text);
        sep_free(j);
        long a1 = 0, a2 = 0;
        float *d1 = load(b.path[0], &a1, NULL), *d2 = load(SS.path[1], &a2, NULL);   /* vs the HTDemucs drums */
        double c12 = 0, c11 = 0, c22 = 0;
        for (long i = 0; d1 && d2 && i < a1 * 2 && i < a2 * 2; ++i) { c12 += (double)d1[i] * d2[i]; c11 += (double)d1[i] * d1[i]; c22 += (double)d2[i] * d2[i]; }
        CHECK(c11 > 0 && c12 / sqrt(c11 * c22 + 1e-30) > 0.9, "its drums agree with HTDemucs' drums (correlation %.3f)", c12 / sqrt(c11 * c22 + 1e-30));
        free(d1); free(d2);
    }
    CHECK(sep_delete_all() >= 3, "\"Delete all\" removes them all");

    printf("SEPARATE: %s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
