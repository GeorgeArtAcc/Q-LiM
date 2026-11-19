/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/*
 * qlim-separate - 1.4.14 / 1.4.17: source separation, a SEPARATE program.
 *
 * Q-LiM itself never loads any of this: no neural network, no ONNX Runtime, no extra thread or memory while it is
 * not used. Only when a separation is asked for, Q-LiM starts this program, which loads ONNX Runtime (by dlopen -
 * downloaded on first use, not part of Q-LiM), runs one model over the whole song in overlapping pieces with
 * cross-fades, writes the parts as 32-bit float WAV files and ends.
 *
 * The models (--kind):
 *   mel     Mel-Band RoFormer vocals (Kimberley Jensen, MIT; ONNX by audiojs). [1,4,1025,801] = L re, L im, R re, R im
 *           of a 2048-point STFT, hop 441, of 352800 samples -> the same for the voice.
 *           Writes --vocals (the model) and --inst (mix - vocals: the two always add up to the mix).
 *   bsrsw   BS-RoFormer SW 6 stems (ONNX by elicwhite). spec_real / spec_imag [1,2,1025,345] of a 2048-point STFT,
 *           hop 512, of 176400 samples -> [1,6,2,1025,345]: bass, drums, other, vocals, guitar, piano.
 *   demucs  HTDemucs 6s (Meta, MIT; ONNX by adowu). The waveform [1,2,343980] -> [1,6,2,343980]:
 *           drums, bass, other, vocals, guitar, piano.
 *   bsrsw / demucs write <outdir>/<name>.wav for each name in --want (default: all 6).
 * All STFTs: periodic Hann, centred with reflection, not normalized (= torch.stft / istft).
 *
 *   qlim-separate --ort LIB --model M.onnx --in MIX.wav(44.1 kHz) [--kind mel] --vocals V.wav --inst I.wav
 *   qlim-separate --ort LIB --model M.onnx --in MIX.wav --kind bsrsw|demucs --outdir DIR [--want vocals,drums,...]
 *                 [--threads N] [--overlap 1..8 (pieces per sample, may be fractional)]
 *   qlim-separate --sha256 FILE
 * stdout: "PROGRESS <done> <total>" lines, at the end "DONE" (exit 0) or "ERROR <text>" (exit 1).
 * Memory: the mix + one piece + a sliding sum of two pieces; the parts are written while it runs.
 */
#define _POSIX_C_SOURCE 200809L
#include "onnxruntime_c_api.h"
#include <sndfile.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
SNDFILE *sf_wchar_open(const wchar_t *wpath, int mode, SF_INFO *sfinfo);   /* (libsndfile, Windows) */
#else
#include <dlfcn.h>
#include <unistd.h>
#endif
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SR      44100
#define NFFT    2048
#define NBIN    (NFFT / 2 + 1)                 /* 1025 */
#define CHUNK_MAX 352800
static long g_hop = 441, g_chunk = 352800, g_nfrm = 801;   /* set by the model kind */
#define HOP   g_hop
#define CHUNK g_chunk
#define NFRM  g_nfrm

static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("ERROR ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fflush(stdout);
    exit(1);
}

/* file names are UTF-8 everywhere (Windows: through the wide functions) */
static SNDFILE *sfopen(const char *path, int mode, SF_INFO *info)
{
#ifdef _WIN32
    wchar_t wp[2048];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 2048);
    return sf_wchar_open(wp, mode, info);
#else
    return sf_open(path, mode, info);
#endif
}

/* ---------------- FFT (radix 2, in place) ---------------- */

static double g_cos[NFFT / 2], g_sin[NFFT / 2], g_win[NFFT];
static int    g_rev[NFFT];

static void fft_init(void)
{
    for (int i = 0; i < NFFT / 2; ++i) { g_cos[i] = cos(2 * M_PI * i / NFFT); g_sin[i] = sin(2 * M_PI * i / NFFT); }
    for (int i = 0; i < NFFT; ++i) g_win[i] = 0.5 - 0.5 * cos(2 * M_PI * i / NFFT);     /* periodic Hann */
    int bits = 0;
    while ((1 << bits) < NFFT) ++bits;
    for (int i = 0; i < NFFT; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        g_rev[i] = r;
    }
}

static void fft(double *re, double *im, bool inverse)
{
    for (int i = 0; i < NFFT; ++i) {
        int j = g_rev[i];
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= NFFT; len <<= 1) {
        int step = NFFT / len;
        for (int i = 0; i < NFFT; i += len)
            for (int k = 0; k < len / 2; ++k) {
                double wr = g_cos[k * step], wi = inverse ? g_sin[k * step] : -g_sin[k * step];
                int a = i + k, b = a + len / 2;
                double xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr; im[a] += xi;
            }
    }
}

/* reflect index into [0, n) (like torch / numpy "reflect": no edge repeat) */
static inline long reflect(long i, long n)
{
    if (n == 1) return 0;
    while (i < 0 || i >= n) {
        if (i < 0) i = -i;
        if (i >= n) i = 2 * (n - 1) - i;
    }
    return i;
}

/* STFT of one channel of a chunk (CHUNK samples) -> spec[2][NBIN][NFRM] (re, im) as float in the model's layout */
static void stft(const float *x, int stride, float *out_re, float *out_im)
{
    static double re[NFFT], im[NFFT];
    for (int t = 0; t < NFRM; ++t) {
        long c0 = (long)t * HOP - NFFT / 2;
        for (int n = 0; n < NFFT; ++n) { re[n] = (double)x[reflect(c0 + n, CHUNK) * stride] * g_win[n]; im[n] = 0.0; }
        fft(re, im, false);
        for (int f = 0; f < NBIN; ++f) { out_re[(size_t)f * NFRM + t] = (float)re[f]; out_im[(size_t)f * NFRM + t] = (float)im[f]; }
    }
}

/* inverse (torch.istft, center, length CHUNK): overlap-add of windowed frames / the sum of the squared windows */
static void istft(const float *in_re, const float *in_im, float *y, int stride)
{
    static double re[NFFT], im[NFFT];
    static double acc[CHUNK_MAX + NFFT], wsum[CHUNK_MAX + NFFT];
    memset(acc, 0, (size_t)(CHUNK + NFFT) * sizeof acc[0]);
    memset(wsum, 0, (size_t)(CHUNK + NFFT) * sizeof wsum[0]);
    for (int t = 0; t < NFRM; ++t) {
        for (int f = 0; f < NBIN; ++f) { re[f] = in_re[(size_t)f * NFRM + t]; im[f] = in_im[(size_t)f * NFRM + t]; }
        im[0] = 0.0; im[NBIN - 1] = 0.0;
        for (int f = NBIN; f < NFFT; ++f) { re[f] = re[NFFT - f]; im[f] = -im[NFFT - f]; }
        fft(re, im, true);
        for (int n = 0; n < NFFT; ++n) {
            long p = (long)t * HOP + n;                         /* in the padded signal */
            acc[p] += re[n] / NFFT * g_win[n];
            wsum[p] += g_win[n] * g_win[n];
        }
    }
    for (long i = 0; i < CHUNK; ++i) {
        long p = i + NFFT / 2;
        y[i * stride] = wsum[p] > 1e-11 ? (float)(acc[p] / wsum[p]) : 0.0f;
    }
}

/* ---------------- ONNX Runtime ---------------- */

static const OrtApi *g_ort;
#define ORT(call) do { OrtStatus *st_ = (call); if (st_) { const char *m_ = g_ort->GetErrorMessage(st_); fail("onnxruntime: %s", m_); } } while (0)

static void load_ort(const char *path)
{
    typedef const OrtApiBase *(*GetBase)(void);
    GetBase gb = NULL;
#ifdef _WIN32
    wchar_t wp[2048];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 2048);
    for (wchar_t *c = wp; *c; ++c) if (*c == L'/') *c = L'\\';   /* LoadLibrary wants backslashes */
    /* (ALTERED_SEARCH_PATH: what the DLL needs is also looked for next to it) */
    HMODULE h = LoadLibraryExW(wp, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) {
        DWORD e = GetLastError();
        if (GetFileAttributesW(wp) == INVALID_FILE_ATTRIBUTES) fail("%s is missing (it is downloaded again next time)", path);
        if (e == ERROR_MOD_NOT_FOUND)          /* 126: the DLL is there, something it needs is not */
            fail("ONNX Runtime needs the Microsoft Visual C++ Redistributable (x64): install it from "
                 "https://aka.ms/vs/17/release/vc_redist.x64.exe and try again");
        if (e == ERROR_DLL_INIT_FAILED)        /* 1114: ORT 1.2x with an OLD msvcp140.dll (it needs 14.40+) */
            fail("ONNX Runtime needs a newer Microsoft Visual C++ Redistributable (x64, 2015-2022): install the "
                 "latest from https://aka.ms/vs/17/release/vc_redist.x64.exe and try again");
        if (e == ERROR_BAD_EXE_FORMAT)         /* 193 */
            fail("%s is not a 64-bit DLL (download it again: delete the separation folder)", path);
        if (e == ERROR_PROC_NOT_FOUND)         /* 127: Windows too old */
            fail("ONNX Runtime does not run on this Windows version (Windows 10 or newer is needed)");
        fail("cannot load %s (Windows error %lu)", path, (unsigned long)e);
    }
    gb = (GetBase)(void *)GetProcAddress(h, "OrtGetApiBase");
#else
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) fail("cannot load %s: %s", path, dlerror());
    gb = (GetBase)dlsym(h, "OrtGetApiBase");
#endif
    if (!gb) fail("%s is not ONNX Runtime", path);
    const OrtApiBase *base = gb();
    g_ort = base ? base->GetApi(ORT_API_VERSION) : NULL;
    if (!g_ort) fail("this ONNX Runtime is too old (need API %d)", ORT_API_VERSION);
}

/* ---------------- SHA-256 (to check the downloads) ---------------- */

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(uint32_t h[8], const unsigned char *b)
{
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = (uint32_t)b[i * 4] << 24 | (uint32_t)b[i * 4 + 1] << 16 | (uint32_t)b[i * 4 + 2] << 8 | b[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
static int sha256_file(const char *path)
{
#ifdef _WIN32
    wchar_t wp[2048];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 2048);
    FILE *f = _wfopen(wp, L"rb");
#else
    FILE *f = fopen(path, "rb");
#endif
    if (!f) { printf("ERROR cannot read %s\n", path); return 1; }
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    static unsigned char buf[1 << 20];
    uint64_t total = 0;
    size_t n, keep = 0;
    while ((n = fread(buf + keep, 1, sizeof buf - keep, f)) > 0 || keep) {
        n += keep;
        size_t full = n / 64 * 64;
        for (size_t i = 0; i < full; i += 64) sha_block(h, buf + i);
        total += full;
        keep = n - full;
        memmove(buf, buf + full, keep);
        if (feof(f)) break;
    }
    fclose(f);
    total += keep;
    unsigned char tail[128];
    memset(tail, 0, sizeof tail);
    memcpy(tail, buf, keep);
    tail[keep] = 0x80;
    size_t tl = keep + 1 + 8 <= 64 ? 64 : 128;
    uint64_t bits = total * 8;
    for (int i = 0; i < 8; ++i) tail[tl - 1 - i] = (unsigned char)(bits >> (8 * i));
    for (size_t i = 0; i < tl; i += 64) sha_block(h, tail + i);
    for (int i = 0; i < 8; ++i) printf("%08x", h[i]);
    printf("\n");
    return 0;
}

/* ---------------- the models ---------------- */

enum { K_MEL, K_BSRSW, K_DEMUCS };
static const char *const BSRSW_NAMES[6]  = { "bass", "drums", "other", "vocals", "guitar", "piano" };
static const char *const DEMUCS_NAMES[6] = { "drums", "bass", "other", "vocals", "guitar", "piano" };

typedef struct {
    int kind, nstem;
    OrtSession *sess;
    OrtMemoryInfo *mi;
    char *in_name[2], *out_name[2];
    float *buf, *buf2;                 /* the model's input */
} Model;

static void find_names(Model *m, OrtAllocator *al, int nin, int nout, const char *const *want_in, const char *const *want_out)
{
    size_t ni = 0, no = 0;
    ORT(g_ort->SessionGetInputCount(m->sess, &ni));
    ORT(g_ort->SessionGetOutputCount(m->sess, &no));
    if ((int)ni < nin || (int)no < nout) fail("this is not the expected model (%d inputs, %d outputs)", (int)ni, (int)no);
    for (int k = 0; k < nin; ++k) ORT(g_ort->SessionGetInputName(m->sess, (size_t)k, al, &m->in_name[k]));
    for (int k = 0; k < nout; ++k) ORT(g_ort->SessionGetOutputName(m->sess, (size_t)k, al, &m->out_name[k]));
    /* by name when the model has them (the order of the graph is not guaranteed) */
    for (int k = 0; want_in && k < nin; ++k)
        if (strcmp(m->in_name[k], want_in[k])) {
            for (int q = k + 1; q < nin; ++q) if (!strcmp(m->in_name[q], want_in[k])) { char *t = m->in_name[k]; m->in_name[k] = m->in_name[q]; m->in_name[q] = t; }
        }
    for (int k = 0; want_out && k < nout; ++k)
        if (strcmp(m->out_name[k], want_out[k])) {
            for (int q = k + 1; q < nout; ++q) if (!strcmp(m->out_name[q], want_out[k])) { char *t = m->out_name[k]; m->out_name[k] = m->out_name[q]; m->out_name[q] = t; }
        }
}

/* one piece (CHUNK stereo samples, interleaved) -> out[stem][ch][CHUNK] */
static void run_piece(Model *m, const float *chunk, float *out)
{
    const size_t plane = (size_t)NBIN * NFRM;
    OrtValue *iv[2] = { NULL, NULL }, *ov[2] = { NULL, NULL };
    int nin = 1, nout = 1;
    if (m->kind == K_MEL) {
        int64_t shape[4] = { 1, 4, NBIN, NFRM };
        stft(chunk, 2, m->buf, m->buf + plane);
        stft(chunk + 1, 2, m->buf + 2 * plane, m->buf + 3 * plane);
        ORT(g_ort->CreateTensorWithDataAsOrtValue(m->mi, m->buf, plane * 4 * sizeof(float), shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &iv[0]));
    } else if (m->kind == K_BSRSW) {
        int64_t shape[4] = { 1, 2, NBIN, NFRM };
        stft(chunk, 2, m->buf, m->buf2);                       /* real: [L, R] planes, imag: the same */
        stft(chunk + 1, 2, m->buf + plane, m->buf2 + plane);
        ORT(g_ort->CreateTensorWithDataAsOrtValue(m->mi, m->buf, plane * 2 * sizeof(float), shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &iv[0]));
        ORT(g_ort->CreateTensorWithDataAsOrtValue(m->mi, m->buf2, plane * 2 * sizeof(float), shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &iv[1]));
        nin = 2; nout = 2;
    } else {
        int64_t shape[3] = { 1, 2, CHUNK };
        for (long k = 0; k < CHUNK; ++k) { m->buf[k] = chunk[k * 2]; m->buf[CHUNK + k] = chunk[k * 2 + 1]; }
        ORT(g_ort->CreateTensorWithDataAsOrtValue(m->mi, m->buf, (size_t)CHUNK * 2 * sizeof(float), shape, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &iv[0]));
    }
    ORT(g_ort->Run(m->sess, NULL, (const char *const *)m->in_name, (const OrtValue *const *)iv, (size_t)nin,
                   (const char *const *)m->out_name, (size_t)nout, ov));
    float *o = NULL, *o2 = NULL;
    ORT(g_ort->GetTensorMutableData(ov[0], (void **)&o));
    if (nout > 1) ORT(g_ort->GetTensorMutableData(ov[1], (void **)&o2));
    {   /* the size it gave back */
        OrtTensorTypeAndShapeInfo *ti;
        size_t cnt = 0;
        ORT(g_ort->GetTensorTypeAndShape(ov[0], &ti));
        ORT(g_ort->GetTensorShapeElementCount(ti, &cnt));
        g_ort->ReleaseTensorTypeAndShapeInfo(ti);
        size_t need = m->kind == K_MEL ? plane * 4 : m->kind == K_BSRSW ? plane * 12 : (size_t)CHUNK * 12;
        if (cnt != need) fail("the model gave %zu values instead of %zu", cnt, need);
    }
    if (m->kind == K_MEL) {
        istft(o, o + plane, out, 1);
        istft(o + 2 * plane, o + 3 * plane, out + CHUNK, 1);
    } else if (m->kind == K_BSRSW) {
        for (int st = 0; st < 6; ++st)
            for (int c = 0; c < 2; ++c) {
                size_t off = ((size_t)st * 2 + (size_t)c) * plane;
                istft(o + off, o2 + off, out + ((size_t)st * 2 + (size_t)c) * (size_t)CHUNK, 1);
            }
    } else
        memcpy(out, o, (size_t)CHUNK * 12 * sizeof(float));
    for (int k = 0; k < 2; ++k) { if (ov[k]) g_ort->ReleaseValue(ov[k]); if (iv[k]) g_ort->ReleaseValue(iv[k]); }
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    {   /* the arguments as UTF-8 (the ANSI argv loses non-Latin letters in names) */
        int wc = 0;
        wchar_t **wv = CommandLineToArgvW(GetCommandLineW(), &wc);
        if (wv && wc > 0) {
            char **nv = calloc((size_t)wc + 1, sizeof(char *));
            for (int i = 0; nv && i < wc; ++i) {
                int n = WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, NULL, 0, NULL, NULL);
                nv[i] = malloc((size_t)n);
                if (nv[i]) WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, nv[i], n, NULL, NULL);
            }
            if (nv) { argv = nv; argc = wc; }
        }
    }
#endif
    if (argc == 3 && !strcmp(argv[1], "--sha256")) return sha256_file(argv[2]);
    const char *ort = NULL, *model = NULL, *in = NULL, *vo = NULL, *io = NULL, *kind_s = "mel", *outdir = NULL, *want_s = NULL;
    int threads = 0;
    double overlap = -1;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--ort")) ort = argv[i + 1];
        else if (!strcmp(argv[i], "--model")) model = argv[i + 1];
        else if (!strcmp(argv[i], "--in")) in = argv[i + 1];
        else if (!strcmp(argv[i], "--vocals")) vo = argv[i + 1];
        else if (!strcmp(argv[i], "--inst")) io = argv[i + 1];
        else if (!strcmp(argv[i], "--kind")) kind_s = argv[i + 1];
        else if (!strcmp(argv[i], "--outdir")) outdir = argv[i + 1];
        else if (!strcmp(argv[i], "--want")) want_s = argv[i + 1];
        else if (!strcmp(argv[i], "--threads")) threads = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--overlap")) overlap = atof(argv[i + 1]);
    }
    Model M;
    memset(&M, 0, sizeof M);
    if (!strcmp(kind_s, "mel")) { M.kind = K_MEL; M.nstem = 1; g_hop = 441; g_chunk = 352800; if (overlap < 0) overlap = 2; }
    else if (!strcmp(kind_s, "bsrsw")) { M.kind = K_BSRSW; M.nstem = 6; g_hop = 512; g_chunk = 176400; if (overlap < 0) overlap = 4.0 / 3.0; }   /* (as its reference: 1/4 overlap) */
    else if (!strcmp(kind_s, "demucs")) { M.kind = K_DEMUCS; M.nstem = 6; g_hop = 512; g_chunk = 343980; if (overlap < 0) overlap = 4.0 / 3.0; }
    else fail("unknown --kind %s (mel, bsrsw, demucs)", kind_s);
    g_nfrm = 1 + g_chunk / g_hop;
    if (!ort || !model || !in) fail("usage: qlim-separate --ort LIB --model M.onnx --in MIX.wav ...");
    if (M.kind == K_MEL && (!vo || !io)) fail("mel: --vocals V.wav --inst I.wav are needed");
    if (M.kind != K_MEL && !outdir) fail("%s: --outdir DIR is needed", kind_s);
    if (overlap < 1) overlap = 1;
    if (overlap > 8) overlap = 8;
    const char *const *names = M.kind == K_BSRSW ? BSRSW_NAMES : DEMUCS_NAMES;
    bool want[6] = { true, true, true, true, true, true };
    if (want_s && M.kind != K_MEL) {
        for (int k = 0; k < 6; ++k) {
            const char *p = strstr(want_s, names[k]);
            size_t l = strlen(names[k]);
            want[k] = p && (p == want_s || p[-1] == ',') && (p[l] == 0 || p[l] == ',');
        }
    }
    fft_init();

    /* the mix (stereo float, 44.1 kHz) */
    SF_INFO si;
    memset(&si, 0, sizeof si);
    SNDFILE *sf = sfopen(in, SFM_READ, &si);
    if (!sf) fail("cannot read %s", in);
    if (si.samplerate != SR) fail("the input must be %d Hz (it is %d)", SR, si.samplerate);
    long len = (long)si.frames;
    if (len <= 0) fail("empty input");
    float *mix = malloc((size_t)len * 2 * sizeof(float));
    if (!mix) fail("out of memory");
    {   /* read in blocks (a mono file is not read twice as big) */
        float blk[4096 * 8];
        long got = 0;
        int nc = si.channels > 8 ? 8 : si.channels;
        if (si.channels > 8) fail("too many channels (%d)", si.channels);
        while (got < len) {
            sf_count_t r = sf_readf_float(sf, blk, 4096);
            if (r <= 0) break;
            for (sf_count_t i = 0; i < r; ++i) {
                float l = blk[i * nc], rr = nc > 1 ? blk[i * nc + 1] : l;
                mix[(got + i) * 2] = l; mix[(got + i) * 2 + 1] = rr;
            }
            got += (long)r;
        }
        len = got;
    }
    sf_close(sf);
    if (len <= 0) fail("empty input");

    /* the reference demix: reflect-pad by `border`, pieces every `step`, linear fades of `fade` at the piece edges */
    long step = (long)((double)CHUNK / overlap);
    if (step < 1) step = 1;
    if (step > CHUNK) step = CHUNK;
    const long border = CHUNK - step, fade = CHUNK / 10 < border ? CHUNK / 10 : border;
    bool padded = len > 2 * border && border > 0;
    long plen = padded ? len + 2 * border : len;
    if (getenv("QLIM_SEP_ONECHUNK")) { padded = false; plen = len; step = CHUNK; }   /* (tests: one piece, as the reference) */
    #define PM(i, c) mix[(padded ? reflect((i) - border, len) : (i)) * 2 + (c)]

    /* the model */
    load_ort(ort);
    OrtEnv *env;
    ORT(g_ort->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "qlim", &env));
    OrtSessionOptions *so;
    ORT(g_ort->CreateSessionOptions(&so));
    if (threads > 0) ORT(g_ort->SetIntraOpNumThreads(so, threads));
    {   /* memory: no arena (it keeps the biggest pieces for ever), optimization level from the environment for tests */
        const char *ol = getenv("QLIM_SEP_OPT");
        int lv = ol ? atoi(ol) : 0;     /* measured (mel): no graph rewriting = ~2.6 GB instead of ~3.2 GB, ~8 % slower */
        ORT(g_ort->SetSessionGraphOptimizationLevel(so, lv == 0 ? ORT_DISABLE_ALL : lv == 1 ? ORT_ENABLE_BASIC : lv == 2 ? ORT_ENABLE_EXTENDED : ORT_ENABLE_ALL));
        if (!getenv("QLIM_SEP_ARENA")) ORT(g_ort->DisableCpuMemArena(so));
    }
    printf("PROGRESS 0 1\n");
    fflush(stdout);
#ifdef _WIN32
    wchar_t wm[2048];
    MultiByteToWideChar(CP_UTF8, 0, model, -1, wm, 2048);
    ORT(g_ort->CreateSession(env, wm, so, &M.sess));
#else
    ORT(g_ort->CreateSession(env, model, so, &M.sess));
#endif
    OrtAllocator *alloc;
    ORT(g_ort->GetAllocatorWithDefaultOptions(&alloc));
    static const char *const BS_IN[2] = { "spec_real", "spec_imag" }, *const BS_OUT[2] = { "out_spec_real", "out_spec_imag" };
    if (M.kind == K_BSRSW) find_names(&M, alloc, 2, 2, BS_IN, BS_OUT);
    else find_names(&M, alloc, 1, 1, NULL, NULL);
    ORT(g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &M.mi));
    printf("LOADED\n");                                       /* the model is in memory: from here it separates */
    fflush(stdout);

    const size_t plane = (size_t)NBIN * NFRM;
    const int S = M.nstem;
    M.buf = malloc((M.kind == K_DEMUCS ? (size_t)CHUNK * 2 : plane * 4) * sizeof(float));
    M.buf2 = M.kind == K_BSRSW ? malloc(plane * 2 * sizeof(float)) : NULL;
    float *chunk = malloc((size_t)CHUNK * 2 * sizeof(float));
    float *pout = malloc((size_t)CHUNK * 2 * (size_t)S * sizeof(float));
    /* the sliding sum: positions [flushed, i + CHUNK) live in a ring of 2 pieces */
    const long W = 2 * CHUNK;
    float *ring = calloc((size_t)W * 2 * (size_t)S, sizeof(float));
    float *rcnt = calloc((size_t)W, sizeof(float));
    if (!M.buf || (M.kind == K_BSRSW && !M.buf2) || !chunk || !pout || !ring || !rcnt) fail("out of memory");

    /* the outputs, written while it runs */
    SF_INFO so2;
    memset(&so2, 0, sizeof so2);
    so2.samplerate = SR; so2.channels = 2; so2.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
    if (len > 200000000L) so2.format = SF_FORMAT_RF64 | SF_FORMAT_FLOAT;
    int nfile = M.kind == K_MEL ? 2 : 6;
    SNDFILE *wf[6] = { NULL };
    char fpath[6][2048];
    for (int k = 0; k < nfile; ++k) {
        if (M.kind == K_MEL) snprintf(fpath[k], sizeof fpath[k], "%s", k == 0 ? vo : io);
        else if (!want[k]) continue;
        else snprintf(fpath[k], sizeof fpath[k], "%s/%s.wav", outdir, names[k]);
        wf[k] = sfopen(fpath[k], SFM_WRITE, &so2);
        if (!wf[k]) fail("cannot write %s", fpath[k]);
    }
    #define OBLK 4096
    float *ob = malloc((size_t)OBLK * 2 * (size_t)nfile * sizeof(float));
    if (!ob) fail("out of memory");
    long ob_n = 0, flushed = 0;
    #define OB(k, j, c) ob[((size_t)(k) * OBLK + (size_t)(j)) * 2 + (c)]
    #define WRITE_OB() do { for (int k_ = 0; k_ < nfile; ++k_) if (wf[k_] && ob_n > 0 && \
            sf_writef_float(wf[k_], &OB(k_, 0, 0), ob_n) != ob_n) fail("writing %s failed (disk full?)", fpath[k_]); ob_n = 0; } while (0)
    /* positions [flushed, upto) are final: out of the ring into the files */
    #define FLUSH(upto) do { \
        for (; flushed < (upto); ++flushed) { \
            long r_ = flushed % W, o_ = padded ? flushed - border : flushed; \
            float cn_ = rcnt[r_]; \
            if (o_ >= 0 && o_ < len) { \
                for (int c_ = 0; c_ < 2; ++c_) { \
                    if (M.kind == K_MEL) { \
                        float e_ = cn_ > 1e-9f ? ring[(size_t)r_ * 2 + c_] / cn_ : 0.0f; \
                        OB(0, ob_n, c_) = e_; OB(1, ob_n, c_) = mix[o_ * 2 + c_] - e_; \
                    } else for (int k_ = 0; k_ < S; ++k_) \
                        OB(k_, ob_n, c_) = cn_ > 1e-9f ? ring[((size_t)k_ * W + (size_t)r_) * 2 + c_] / cn_ : 0.0f; \
                } \
                if (++ob_n == OBLK) WRITE_OB(); \
            } \
            for (int k_ = 0; k_ < S; ++k_) ring[((size_t)k_ * W + (size_t)r_) * 2] = ring[((size_t)k_ * W + (size_t)r_) * 2 + 1] = 0.0f; \
            rcnt[r_] = 0.0f; \
        } } while (0)

    long nchunks = (plen + step - 1) / step, done = 0;
    if (getenv("QLIM_SEP_ONECHUNK")) nchunks = 1;
    for (long i = 0; i < plen && done < nchunks; i += step) {
        FLUSH(i);
        long n = plen - i < CHUNK ? plen - i : CHUNK;
        /* the piece; a short last piece: reflected (if more than half, as the reference) or zeros */
        for (long k = 0; k < CHUNK; ++k) {
            long src;
            if (k < n) src = i + k;
            else if (M.kind != K_DEMUCS && n > CHUNK / 2 + 1) src = i + reflect(k, n);
            else { chunk[k * 2] = chunk[k * 2 + 1] = 0.0f; continue; }
            chunk[k * 2] = PM(src, 0); chunk[k * 2 + 1] = PM(src, 1);
        }
        run_piece(&M, chunk, pout);
        /* the window of this piece: fades at both ends, except at the very start / end */
        for (long k = 0; k < n; ++k) {
            float w = 1.0f;
            if (k < fade && i > 0) w = (float)k / (float)fade;
            if (k >= CHUNK - fade && i + CHUNK < plen) w = (float)(CHUNK - 1 - k) / (float)fade;
            long r = (i + k) % W;
            for (int st = 0; st < S; ++st)
                for (int c = 0; c < 2; ++c)
                    ring[((size_t)st * W + (size_t)r) * 2 + c] += pout[((size_t)st * 2 + (size_t)c) * (size_t)CHUNK + (size_t)k] * w;
            rcnt[r] += w;
        }
        ++done;
        printf("PROGRESS %ld %ld\n", done, nchunks);
        fflush(stdout);
    }
    FLUSH(plen);
    WRITE_OB();
    for (int k = 0; k < nfile; ++k) if (wf[k]) sf_close(wf[k]);
    printf("DONE\n");
    fflush(stdout);
    return 0;
}
