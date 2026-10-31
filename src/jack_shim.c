/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.4.0: JACK without depending on it.
 * The Linux packages contain PortAudio's JACK driver, but NOT libjack: these functions stand in for libjack and pass
 * every call on to the real one, which is loaded when PortAudio first asks for it (dlopen "libjack.so.0").
 *  - JACK installed (jackd2, or PipeWire's JACK): the "JACK Audio Connection Kit" driver appears in Edit > Audio;
 *  - not installed: jack_client_open() says "no" and PortAudio simply does not offer JACK - Q-LiM starts as always.
 * Only the functions PortAudio's JACK driver uses (see `nm -u pa_jack.o`). Used by packaging/Makefile.pkg only
 * (a build against the system's PortAudio links the real libjack instead). */
#define _GNU_SOURCE
#include <jack/jack.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <pthread.h>

static void *g_lib;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

#define JACK_FUNCS(X) \
    X(jack_activate) X(jack_client_close) X(jack_client_name_size) X(jack_client_open) X(jack_connect) \
    X(jack_deactivate) X(jack_frame_time) X(jack_get_buffer_size) X(jack_get_client_name) X(jack_get_ports) \
    X(jack_get_sample_rate) X(jack_on_shutdown) X(jack_port_by_name) X(jack_port_connected) X(jack_port_disconnect) \
    X(jack_port_get_buffer) X(jack_port_get_latency) X(jack_port_name) X(jack_port_name_size) X(jack_port_register) \
    X(jack_port_unregister) X(jack_set_error_function) X(jack_set_process_callback) X(jack_set_sample_rate_callback) \
    X(jack_set_xrun_callback)

#define DECL(n) static void *p_##n;
JACK_FUNCS(DECL)

/* all at once, before anything runs (jack_port_get_buffer is called from JACK's own thread) */
static void load(void)
{
    static const char *names[] = { "libjack.so.0", "libjack.so" };
    for (int i = 0; i < 2 && !g_lib; ++i) g_lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!g_lib) {
        printf(">>> JACK: not installed - the JACK driver is not offered (PipeWire / ALSA are)\n");
        return;
    }
#define RESOLVE(n) p_##n = dlsym(g_lib, #n);
    JACK_FUNCS(RESOLVE)
    if (!p_jack_client_open || !p_jack_port_get_buffer) {
        printf(">>> JACK: libjack found, but it is incomplete - JACK is not used\n");
        p_jack_client_open = NULL;
        return;
    }
    Dl_info di;
    printf(">>> JACK: libjack found (%s)\n", dladdr(p_jack_client_open, &di) && di.dli_fname ? di.dli_fname : "libjack.so.0");
}
static int ready(void) { pthread_once(&g_once, load); return p_jack_client_open != NULL; }

/* ---- the functions ---- */
jack_client_t *jack_client_open(const char *client_name, jack_options_t options, jack_status_t *status, ...)
{
    if (!ready()) { if (status) *status = (jack_status_t)(JackFailure | JackServerFailed); return NULL; }
    typedef jack_client_t *(*F)(const char *, jack_options_t, jack_status_t *, ...);
    if (options & JackServerName) {
        va_list ap; va_start(ap, status);
        const char *server = va_arg(ap, const char *);
        va_end(ap);
        return ((F)p_jack_client_open)(client_name, options, status, server);
    }
    return ((F)p_jack_client_open)(client_name, options, status);
}

#define CALL(ret, n, fail, params, args) \
    ret n params { if (!ready() || !p_##n) return fail; return ((ret (*) params)p_##n) args; }
#define CALLV(n, params, args) \
    void n params { if (!ready() || !p_##n) return; ((void (*) params)p_##n) args; }

CALL(int, jack_activate, -1, (jack_client_t *c), (c))
CALL(int, jack_deactivate, -1, (jack_client_t *c), (c))
CALL(int, jack_client_close, -1, (jack_client_t *c), (c))
CALL(int, jack_client_name_size, 64, (void), ())
CALL(int, jack_port_name_size, 320, (void), ())
CALL(int, jack_connect, -1, (jack_client_t *c, const char *a, const char *b), (c, a, b))
CALL(jack_nframes_t, jack_frame_time, 0, (const jack_client_t *c), (c))
CALL(jack_nframes_t, jack_get_buffer_size, 0, (jack_client_t *c), (c))
CALL(jack_nframes_t, jack_get_sample_rate, 0, (jack_client_t *c), (c))
CALL(char *, jack_get_client_name, NULL, (jack_client_t *c), (c))
CALL(const char **, jack_get_ports, NULL, (jack_client_t *c, const char *pn, const char *tn, unsigned long fl), (c, pn, tn, fl))
CALLV(jack_on_shutdown, (jack_client_t *c, JackShutdownCallback cb, void *arg), (c, cb, arg))
CALL(jack_port_t *, jack_port_by_name, NULL, (jack_client_t *c, const char *n), (c, n))
CALL(int, jack_port_connected, 0, (const jack_port_t *p), (p))
CALL(int, jack_port_disconnect, -1, (jack_client_t *c, jack_port_t *p), (c, p))
CALL(void *, jack_port_get_buffer, NULL, (jack_port_t *p, jack_nframes_t n), (p, n))
CALL(jack_nframes_t, jack_port_get_latency, 0, (jack_port_t *p), (p))
CALL(const char *, jack_port_name, "", (const jack_port_t *p), (p))
CALL(jack_port_t *, jack_port_register, NULL,
     (jack_client_t *c, const char *n, const char *t, unsigned long fl, unsigned long bs), (c, n, t, fl, bs))
CALL(int, jack_port_unregister, -1, (jack_client_t *c, jack_port_t *p), (c, p))
CALLV(jack_set_error_function, (void (*f)(const char *)), (f))
CALL(int, jack_set_process_callback, -1, (jack_client_t *c, JackProcessCallback cb, void *arg), (c, cb, arg))
CALL(int, jack_set_sample_rate_callback, -1, (jack_client_t *c, JackSampleRateCallback cb, void *arg), (c, cb, arg))
CALL(int, jack_set_xrun_callback, -1, (jack_client_t *c, JackXRunCallback cb, void *arg), (c, cb, arg))
