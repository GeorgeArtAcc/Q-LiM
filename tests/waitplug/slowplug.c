/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Test CLAP plugin "like a Windows plugin through yabridge": starts slowly (4 s in the module init).
 * Contains the word yabridge, so Q-LiM recognises it and loads it on a background thread. */
const char slowplug_marker[] = "yabridge-like test plugin";
#include <clap/clap.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

atomic_int waitplug_timeouts;
atomic_int waitplug_served;
static atomic_bool pending;
static const clap_host_t *g_host;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static bool p_init(const clap_plugin_t *p) { (void)p; return true; }
static void p_destroy(const clap_plugin_t *p) { (void)p; }
static bool p_activate(const clap_plugin_t *p, double sr, uint32_t a, uint32_t b) { (void)p; (void)sr; (void)a; (void)b; return true; }
static void p_deactivate(const clap_plugin_t *p) { (void)p; }
static bool p_start(const clap_plugin_t *p) { (void)p; return true; }
static void p_stop(const clap_plugin_t *p) { (void)p; }
static void p_reset(const clap_plugin_t *p) { (void)p; }
static clap_process_status p_process(const clap_plugin_t *p, const clap_process_t *pr)
{
    (void)p;
    static int cnt;
    if (0) {                         /* from time to time: waits for the main thread */
        atomic_store(&pending, true);
        g_host->request_callback(g_host);
        double t0 = now();
        while (atomic_load(&pending)) {
            if (now() - t0 > 3.0) { atomic_fetch_add(&waitplug_timeouts, 1); atomic_store(&pending, false); break; }
            struct timespec ts = { 0, 100000 }; nanosleep(&ts, NULL);
        }
    }
    for (uint32_t c = 0; c < 2; ++c) memcpy(pr->audio_outputs[0].data32[c], pr->audio_inputs[0].data32[c], pr->frames_count * 4);
    return CLAP_PROCESS_CONTINUE;
}
static const void *p_ext(const clap_plugin_t *p, const char *id) { (void)p; (void)id; return NULL; }
static void p_main(const clap_plugin_t *p) { (void)p; if (atomic_exchange(&pending, false)) atomic_fetch_add(&waitplug_served, 1); }

static const char *const feats[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL };
static const clap_plugin_descriptor_t desc = { CLAP_VERSION_INIT, "qlim.test.slow", "QTest Slow", "QLiMTest", "", "", "", "1", "", feats };
static clap_plugin_t plug = { &desc, NULL, p_init, p_destroy, p_activate, p_deactivate, p_start, p_stop, p_reset, p_process, p_ext, p_main };

static uint32_t f_count(const clap_plugin_factory_t *f) { (void)f; return 1; }
static const clap_plugin_descriptor_t *f_desc(const clap_plugin_factory_t *f, uint32_t i) { (void)f; (void)i; return &desc; }
static const clap_plugin_t *f_create(const clap_plugin_factory_t *f, const clap_host_t *h, const char *id) { (void)f; (void)id; g_host = h; return &plug; }
static const clap_plugin_factory_t factory = { f_count, f_desc, f_create };
static bool e_init(const char *p) { (void)p; struct timespec ts = { 4, 0 }; nanosleep(&ts, NULL); return true; }
static void e_deinit(void) {}
static const void *e_factory(const char *id) { return strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? NULL : &factory; }
CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, e_init, e_deinit, e_factory };
