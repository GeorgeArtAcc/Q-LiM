/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/*
 * Direct PipeWire output (see audio_pw.h).
 *
 * How it works:
 *   - pw_thread_loop: PipeWire has its own thread for messages; all our calls into it
 *     are made under pw_thread_loop_lock (otherwise PipeWire is not safe from several threads).
 *   - pw_stream (Audio/Playback): PipeWire calls on_process() on its real-time thread every cycle
 *     of the graph ("quantum"). There we fill exactly as many frames as PipeWire asked for (b->requested).
 *   - registry: we track live which outputs (Audio/Sink) exist; if the chosen output disappears -> "lost".
 *   - metadata "default": which output is the system default (for display and channel count).
 *
 * libpipewire is loaded with dlopen -> the program does NOT depend on PipeWire at build time or at startup.
 */
#define _GNU_SOURCE
#include "audio_pw.h"

#include <dlfcn.h>
#include <math.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw-types.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/debug/types.h>
#include <spa/utils/json.h>
#include <spa/utils/result.h>
#include <spa/node/io.h>
#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>

/* ================= dynamically loaded functions ================= */

static struct {
    void *h;
    __typeof__(&pw_init)                   init;
    __typeof__(&pw_get_library_version)    get_library_version;
    __typeof__(&pw_thread_loop_new)        tl_new;
    __typeof__(&pw_thread_loop_destroy)    tl_destroy;
    __typeof__(&pw_thread_loop_start)      tl_start;
    __typeof__(&pw_thread_loop_stop)       tl_stop;
    __typeof__(&pw_thread_loop_lock)       tl_lock;
    __typeof__(&pw_thread_loop_unlock)     tl_unlock;
    __typeof__(&pw_thread_loop_signal)     tl_signal;
    __typeof__(&pw_thread_loop_timed_wait) tl_timed_wait;
    __typeof__(&pw_thread_loop_get_loop)   tl_get_loop;
    __typeof__(&pw_context_new)            context_new;
    __typeof__(&pw_context_destroy)        context_destroy;
    __typeof__(&pw_context_connect)        context_connect;
    __typeof__(&pw_core_disconnect)        core_disconnect;
    __typeof__(&pw_proxy_destroy)          proxy_destroy;
    __typeof__(&pw_properties_new)         properties_new;
    __typeof__(&pw_properties_set)         properties_set;
    __typeof__(&pw_properties_setf)        properties_setf;
    __typeof__(&pw_stream_new)             stream_new;
    __typeof__(&pw_stream_add_listener)    stream_add_listener;
    __typeof__(&pw_stream_connect)         stream_connect;
    __typeof__(&pw_stream_disconnect)      stream_disconnect;
    __typeof__(&pw_stream_destroy)         stream_destroy;
    __typeof__(&pw_stream_dequeue_buffer)  stream_dequeue_buffer;
    __typeof__(&pw_stream_queue_buffer)    stream_queue_buffer;
    __typeof__(&pw_stream_state_as_string) stream_state_as_string;
    __typeof__(&pw_stream_get_node_id)     stream_get_node_id;
} P;

/* for graph mode (step 28) - optional: if any is missing, the graph is simply not used */
static struct {
    bool ok;
    __typeof__(&pw_filter_new)               filter_new;
    __typeof__(&pw_filter_add_listener)      filter_add_listener;
    __typeof__(&pw_filter_connect)           filter_connect;
    __typeof__(&pw_filter_destroy)           filter_destroy;
    __typeof__(&pw_filter_add_port)          filter_add_port;
    __typeof__(&pw_filter_get_dsp_buffer)    filter_get_dsp_buffer;
    __typeof__(&pw_filter_get_node_id)       filter_get_node_id;
    __typeof__(&pw_filter_update_properties) filter_update_properties;
    __typeof__(&pw_filter_state_as_string)   filter_state_as_string;
    __typeof__(&pw_proxy_add_listener)       proxy_add_listener;
} F;

static int  g_load_state = 0;           /* 0 = not tried, 1 = loaded, -1 = no library */
static char g_libver[48];
static bool g_has_requested;            /* pw_buffer.requested exists since 0.3.49 (Ubuntu 22.04 has 0.3.48) */
static bool g_has_target_object;        /* "target.object" exists since 0.3.64; before that - "node.target" */

static int pw_load(void)
{
    if (g_load_state) return g_load_state;
    const char *names[] = { "libpipewire-0.3.so.0", "libpipewire-0.3.so", NULL };
    for (int i = 0; names[i] && !P.h; ++i) P.h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!P.h) { g_load_state = -1; return -1; }
#define LOAD(field, sym) do { *(void **)(&P.field) = dlsym(P.h, sym); if (!P.field) { \
        fprintf(stderr, ">>> PipeWire: missing function %s in libpipewire\n", sym); goto fail; } } while (0)
    LOAD(init, "pw_init");
    LOAD(get_library_version, "pw_get_library_version");
    LOAD(tl_new, "pw_thread_loop_new");
    LOAD(tl_destroy, "pw_thread_loop_destroy");
    LOAD(tl_start, "pw_thread_loop_start");
    LOAD(tl_stop, "pw_thread_loop_stop");
    LOAD(tl_lock, "pw_thread_loop_lock");
    LOAD(tl_unlock, "pw_thread_loop_unlock");
    LOAD(tl_signal, "pw_thread_loop_signal");
    LOAD(tl_timed_wait, "pw_thread_loop_timed_wait");
    LOAD(tl_get_loop, "pw_thread_loop_get_loop");
    LOAD(context_new, "pw_context_new");
    LOAD(context_destroy, "pw_context_destroy");
    LOAD(context_connect, "pw_context_connect");
    LOAD(core_disconnect, "pw_core_disconnect");
    LOAD(proxy_destroy, "pw_proxy_destroy");
    LOAD(properties_new, "pw_properties_new");
    LOAD(properties_set, "pw_properties_set");
    LOAD(properties_setf, "pw_properties_setf");
    LOAD(stream_new, "pw_stream_new");
    LOAD(stream_add_listener, "pw_stream_add_listener");
    LOAD(stream_connect, "pw_stream_connect");
    LOAD(stream_disconnect, "pw_stream_disconnect");
    LOAD(stream_destroy, "pw_stream_destroy");
    LOAD(stream_dequeue_buffer, "pw_stream_dequeue_buffer");
    LOAD(stream_queue_buffer, "pw_stream_queue_buffer");
    LOAD(stream_state_as_string, "pw_stream_state_as_string");
    LOAD(stream_get_node_id, "pw_stream_get_node_id");
#undef LOAD
#define LOADF(field, sym) do { *(void **)(&F.field) = dlsym(P.h, sym); if (!F.field) F.ok = false; } while (0)
    F.ok = true;
    LOADF(filter_new, "pw_filter_new");
    LOADF(filter_add_listener, "pw_filter_add_listener");
    LOADF(filter_connect, "pw_filter_connect");
    LOADF(filter_destroy, "pw_filter_destroy");
    LOADF(filter_add_port, "pw_filter_add_port");
    LOADF(filter_get_dsp_buffer, "pw_filter_get_dsp_buffer");
    LOADF(filter_get_node_id, "pw_filter_get_node_id");
    LOADF(filter_update_properties, "pw_filter_update_properties");
    LOADF(filter_state_as_string, "pw_filter_state_as_string");
    LOADF(proxy_add_listener, "pw_proxy_add_listener");
#undef LOADF
    P.init(NULL, NULL);
    snprintf(g_libver, sizeof g_libver, "%s", P.get_library_version());
    {
        int a = 0, b = 0, c = 0;
        sscanf(g_libver, "%d.%d.%d", &a, &b, &c);
        long v = a * 1000000L + b * 1000L + c;
        g_has_requested = v >= 3049;
        g_has_target_object = v >= 3064;
    }
    g_load_state = 1;
    return 1;
fail:
    dlclose(P.h);
    memset(&P, 0, sizeof P);
    g_load_state = -1;
    return -1;
}

const char *pwb_library_version(void) { return g_load_state == 1 ? g_libver : ""; }

static uint64_t now_ns(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* ================= connection to PipeWire ================= */

typedef struct {
    PwbSink           s;
    struct pw_proxy  *proxy;
    struct spa_hook   hook;
    bool              ch_from_props;     /* the channel count came from the properties (more reliable than EnumFormat) */
    bool              used;
} SinkSlot;

static struct {
    struct pw_thread_loop *loop;
    struct pw_context     *ctx;
    struct pw_core        *core;
    struct spa_hook        core_hook;
    struct pw_registry    *reg;
    struct spa_hook        reg_hook;
    struct pw_proxy       *meta;
    struct spa_hook        meta_hook;
    uint32_t               meta_id;
    int                    pending_seq;
    bool                   synced;
    atomic_bool            core_dead;
    char                   core_err[128];
    SinkSlot               sinks[PWB_MAX_SINKS];
    char                   default_sink[256];
} C;

static bool g_connected = false;

/* --- node (output): properties and formats --- */

static void parse_positions(const char *str, uint32_t *pos, int max, int *count)
{
    /* "[ FL FR ]", "FL,FR", "AUX0 AUX1 ..." */
    int n = 0;
    const char *p = str;
    while (*p && n < max) {
        while (*p == ' ' || *p == ',' || *p == '[' || *p == ']' || *p == '"') p++;
        if (!*p) break;
        char tok[16]; int k = 0;
        while (*p && *p != ' ' && *p != ',' && *p != ']' && *p != '"') { if (k < 15) tok[k++] = *p; p++; }
        tok[k] = 0;
        uint32_t t = spa_debug_type_find_type_short(spa_type_audio_channel, tok);
        pos[n++] = (t == SPA_ID_INVALID) ? SPA_AUDIO_CHANNEL_UNKNOWN : t;
    }
    *count = n;
}

static void node_info(void *data, const struct pw_node_info *info)
{
    SinkSlot *sl = data;
    if (!info || !info->props) return;
    const char *d = spa_dict_lookup(info->props, PW_KEY_NODE_DESCRIPTION);
    if (d && *d) snprintf(sl->s.desc, sizeof sl->s.desc, "%s", d);
    const char *ch = spa_dict_lookup(info->props, "audio.channels");
    const char *ps = spa_dict_lookup(info->props, "audio.position");
    const char *rt = spa_dict_lookup(info->props, "audio.rate");
    if (ch && atoi(ch) > 0) {
        int n = atoi(ch);
        if (n > PWB_MAX_CH) n = PWB_MAX_CH;
        sl->s.channels = n;
        sl->ch_from_props = true;
        int np = 0;
        if (ps) parse_positions(ps, sl->s.pos, PWB_MAX_CH, &np);
        if (np != n) for (int i = 0; i < n; ++i) sl->s.pos[i] = SPA_AUDIO_CHANNEL_AUX0 + (uint32_t)i;
    }
    if (rt && atoi(rt) > 0) sl->s.rate = atoi(rt);
}

/* EnumFormat: the value may be a "choice" -> take the default */
static bool pod_int_value(const struct spa_pod *v, int *out)
{
    uint32_t n_vals = 0, choice = 0;
    const struct spa_pod *val = spa_pod_get_values(v, &n_vals, &choice);
    if (!val || n_vals < 1 || SPA_POD_TYPE(val) != SPA_TYPE_Int) return false;
    *out = ((const struct spa_pod_int *)val)->value;
    return true;
}

static void node_param(void *data, int seq, uint32_t id, uint32_t index, uint32_t next, const struct spa_pod *param)
{
    (void)seq; (void)index; (void)next;
    SinkSlot *sl = data;
    if (id != SPA_PARAM_EnumFormat || !param || !spa_pod_is_object(param)) return;
    const struct spa_pod_prop *prop;
    const struct spa_pod_object *obj = (const struct spa_pod_object *)param;
    int ch = 0, rate = 0;
    uint32_t pos[PWB_MAX_CH]; int npos = 0;
    SPA_POD_OBJECT_FOREACH(obj, prop) {
        if (prop->key == SPA_FORMAT_AUDIO_channels) pod_int_value(&prop->value, &ch);
        else if (prop->key == SPA_FORMAT_AUDIO_rate) pod_int_value(&prop->value, &rate);
        else if (prop->key == SPA_FORMAT_AUDIO_position) {
            npos = (int)spa_pod_copy_array(&prop->value, SPA_TYPE_Id, pos, PWB_MAX_CH);
        }
    }
    if (rate > 0 && sl->s.rate == 0) sl->s.rate = rate;
    if (ch > 0 && !sl->ch_from_props && ch > sl->s.channels) {   /* the largest offered format */
        if (ch > PWB_MAX_CH) ch = PWB_MAX_CH;
        sl->s.channels = ch;
        if (npos == ch) memcpy(sl->s.pos, pos, sizeof(uint32_t) * (size_t)ch);
        else for (int i = 0; i < ch; ++i) sl->s.pos[i] = SPA_AUDIO_CHANNEL_AUX0 + (uint32_t)i;
    }
}

static const struct pw_node_events node_events = {
    PW_VERSION_NODE_EVENTS,
    .info = node_info,
    .param = node_param,
};

/* --- metadata: default output --- */

static int meta_property(void *data, uint32_t subject, const char *key, const char *type, const char *value)
{
    (void)data; (void)type;
    if (subject != PW_ID_CORE) return 0;
    if (key == NULL) { C.default_sink[0] = 0; return 0; }          /* everything deleted */
    if (strcmp(key, "default.audio.sink") != 0) return 0;
    C.default_sink[0] = 0;
    if (!value) return 0;
    struct spa_json it[2];
    char k[64];
    spa_json_init(&it[0], value, strlen(value));
    if (spa_json_enter_object(&it[0], &it[1]) <= 0) return 0;
    while (spa_json_get_string(&it[1], k, sizeof k) > 0) {
        if (strcmp(k, "name") == 0) {
            spa_json_get_string(&it[1], C.default_sink, sizeof C.default_sink);
            break;
        }
        const char *v; if (spa_json_next(&it[1], &v) <= 0) break;   /* skip the value */
    }
    return 0;
}

static const struct pw_metadata_events meta_events = {
    PW_VERSION_METADATA_EVENTS,
    .property = meta_property,
};

/* --- table of the ports and links in the graph (for graph mode) --- */
typedef struct { uint32_t id, node, idx; uint8_t dir; } GPort;     /* dir: 0 = input, 1 = output */
typedef struct { uint32_t id, out_port, in_port; } GLink;
static struct { GPort *p; int np, cap_p; GLink *l; int nl, cap_l; } T;

static GPort *t_port(uint32_t id)
{
    for (int i = 0; i < T.np; ++i) if (T.p[i].id == id) return &T.p[i];
    return NULL;
}
static GPort *t_port_at(uint32_t node, int dir, uint32_t idx)
{
    if (node == SPA_ID_INVALID) return NULL;
    for (int i = 0; i < T.np; ++i)
        if (T.p[i].node == node && T.p[i].dir == dir && T.p[i].idx == idx) return &T.p[i];
    return NULL;
}
static GLink *t_link_pair(uint32_t out, uint32_t in)
{
    for (int i = 0; i < T.nl; ++i) if (T.l[i].out_port == out && T.l[i].in_port == in) return &T.l[i];
    return NULL;
}
static void t_free(void)
{
    free(T.p); free(T.l);
    memset(&T, 0, sizeof T);
}

static void graph_port_event(const GPort *p);
static void graph_link_added(uint32_t out, uint32_t in);
static void graph_link_removed(uint32_t out, uint32_t in);

static void reg_port_link(uint32_t id, const char *type, const struct spa_dict *props)
{
    if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
        const char *nd = spa_dict_lookup(props, PW_KEY_NODE_ID), *pi = spa_dict_lookup(props, PW_KEY_PORT_ID),
                   *dr = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
        if (!nd || !pi || !dr) return;
        if (T.np == T.cap_p) {
            int nc = T.cap_p ? T.cap_p * 2 : 256;
            GPort *np = realloc(T.p, sizeof(GPort) * (size_t)nc);
            if (!np) return;
            T.p = np; T.cap_p = nc;
        }
        GPort *p = &T.p[T.np++];
        p->id = id; p->node = (uint32_t)strtoul(nd, NULL, 10); p->idx = (uint32_t)strtoul(pi, NULL, 10);
        p->dir = strcmp(dr, "out") == 0 ? 1 : 0;
        graph_port_event(p);
    } else if (strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
        const char *o = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_PORT), *i = spa_dict_lookup(props, PW_KEY_LINK_INPUT_PORT);
        if (!o || !i) return;
        if (T.nl == T.cap_l) {
            int nc = T.cap_l ? T.cap_l * 2 : 256;
            GLink *nl = realloc(T.l, sizeof(GLink) * (size_t)nc);
            if (!nl) return;
            T.l = nl; T.cap_l = nc;
        }
        GLink *l = &T.l[T.nl++];
        l->id = id; l->out_port = (uint32_t)strtoul(o, NULL, 10); l->in_port = (uint32_t)strtoul(i, NULL, 10);
        graph_link_added(l->out_port, l->in_port);
    }
}

static void reg_port_link_remove(uint32_t id)
{
    for (int i = 0; i < T.nl; ++i)
        if (T.l[i].id == id) {
            GLink l = T.l[i];
            T.l[i] = T.l[--T.nl];
            graph_link_removed(l.out_port, l.in_port);
            return;
        }
    for (int i = 0; i < T.np; ++i)
        if (T.p[i].id == id) {
            GPort p = T.p[i];
            T.p[i] = T.p[--T.np];
            graph_port_event(&p);
            return;
        }
}

/* --- registry --- */

static void reg_global(void *data, uint32_t id, uint32_t perm, const char *type, uint32_t version,
                       const struct spa_dict *props)
{
    (void)data; (void)perm; (void)version;
    if (!props) return;
    if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        const char *mn = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (mn && strcmp(mn, "default") == 0 && !C.meta) {
            C.meta = pw_registry_bind(C.reg, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0);
            if (C.meta) {
                C.meta_id = id;
                pw_metadata_add_listener((struct pw_metadata *)C.meta, &C.meta_hook, &meta_events, NULL);
            }
        }
        return;
    }
    if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0 || strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
        reg_port_link(id, type, props);
        return;
    }
    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;
    const char *mc = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!mc || (strcmp(mc, "Audio/Sink") != 0 && strcmp(mc, "Audio/Duplex") != 0)) return;
    const char *nm = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (!nm || !*nm) return;
    SinkSlot *sl = NULL;
    for (int i = 0; i < PWB_MAX_SINKS; ++i) if (!C.sinks[i].used) { sl = &C.sinks[i]; break; }
    if (!sl) return;
    memset(sl, 0, sizeof *sl);
    sl->used = true;
    sl->s.id = id;
    snprintf(sl->s.name, sizeof sl->s.name, "%s", nm);
    const char *d = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    snprintf(sl->s.desc, sizeof sl->s.desc, "%s", (d && *d) ? d : nm);
    sl->proxy = pw_registry_bind(C.reg, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
    if (sl->proxy) {
        pw_node_add_listener((struct pw_node *)sl->proxy, &sl->hook, &node_events, sl);
        pw_node_enum_params((struct pw_node *)sl->proxy, 0, SPA_PARAM_EnumFormat, 0, UINT32_MAX, NULL);
    }
}

/* we will tell the stream if exactly its output is removed */
static uint32_t g_target_id = SPA_ID_INVALID;
static atomic_bool g_target_gone;

static void reg_global_remove(void *data, uint32_t id)
{
    (void)data;
    if (id == C.meta_id && C.meta) {
        spa_hook_remove(&C.meta_hook);
        P.proxy_destroy(C.meta);
        C.meta = NULL; C.meta_id = SPA_ID_INVALID;
    }
    for (int i = 0; i < PWB_MAX_SINKS; ++i) {
        SinkSlot *sl = &C.sinks[i];
        if (!sl->used || sl->s.id != id) continue;
        if (sl->proxy) { spa_hook_remove(&sl->hook); P.proxy_destroy(sl->proxy); }
        sl->proxy = NULL;
        sl->used = false;
    }
    if (id == g_target_id) atomic_store(&g_target_gone, true);
    reg_port_link_remove(id);
}

static const struct pw_registry_events reg_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = reg_global,
    .global_remove = reg_global_remove,
};

/* --- core --- */

static void core_done(void *data, uint32_t id, int seq)
{
    (void)data;
    if (id == PW_ID_CORE && seq == C.pending_seq) {
        C.synced = true;
        P.tl_signal(C.loop, false);
    }
}

static void core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
    (void)data; (void)seq;
    if (id != PW_ID_CORE) {
        /* an error for a single object (e.g. an output that is switching profile right now and gives no formats) -
         * harmless: the list simply uses its properties. Just a note, not an "error". */
        printf(">>> PipeWire note: object %u: %s\n", id, message ? message : spa_strerror(res));
        return;
    }
    fprintf(stderr, ">>> PipeWire error: %s (%s)\n", message ? message : "", spa_strerror(res));
    {
        snprintf(C.core_err, sizeof C.core_err, "%s", message ? message : spa_strerror(res));
        if (res == -EPIPE) atomic_store(&C.core_dead, true);     /* the PipeWire server stopped/restarted */
    }
    P.tl_signal(C.loop, false);
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = core_done,
    .error = core_error,
};

/* waits for the server to answer everything sent so far (under lock). false = no answer. */
static bool roundtrip_locked(double timeout_s)
{
    C.synced = false;
    C.pending_seq = pw_core_sync(C.core, PW_ID_CORE, 0);
    uint64_t end = now_ns() + (uint64_t)(timeout_s * 1e9);
    while (!C.synced && !atomic_load(&C.core_dead)) {
        if (now_ns() >= end) return false;
        P.tl_timed_wait(C.loop, 1);
    }
    return C.synced;
}

static void disconnect_all(void);
static void graph_teardown_filters(void);
static void graph_free_tables(void);

static int connect_all(char *err, int err_size)
{
    if (g_connected && atomic_load(&C.core_dead)) disconnect_all();     /* the old connection is dead -> from scratch */
    if (g_connected) return 0;
    if (pw_load() != 1) { snprintf(err, (size_t)err_size, "libpipewire not found"); return -1; }
    memset(&C, 0, sizeof C);
    C.meta_id = SPA_ID_INVALID;
    C.loop = P.tl_new("qlim-pw", NULL);
    if (!C.loop) { snprintf(err, (size_t)err_size, "cannot create PipeWire thread"); return -1; }
    /* Real-time priority for the thread that calls on_process(): up to PipeWire 1.0 only the configuration
     * "client-rt.conf" gives it (it loads module-rt); from 1.2 on the ordinary client.conf does it already. */
    {
        int maj = 0, min = 0;
        sscanf(g_libver, "%d.%d", &maj, &min);
        if (maj == 0 || (maj == 1 && min < 1)) {
            struct pw_properties *cp = P.properties_new(PW_KEY_CONFIG_NAME, "client-rt.conf", NULL);
            C.ctx = P.context_new(P.tl_get_loop(C.loop), cp, 0);       /* cp becomes owned by context */
        }
        if (!C.ctx) C.ctx = P.context_new(P.tl_get_loop(C.loop), NULL, 0);
    }
    if (!C.ctx) { snprintf(err, (size_t)err_size, "cannot create PipeWire context"); P.tl_destroy(C.loop); C.loop = NULL; return -1; }
    if (P.tl_start(C.loop) != 0) {
        snprintf(err, (size_t)err_size, "cannot start PipeWire thread");
        P.context_destroy(C.ctx); P.tl_destroy(C.loop); C.loop = NULL; C.ctx = NULL; return -1;
    }
    P.tl_lock(C.loop);
    C.core = P.context_connect(C.ctx, NULL, 0);
    if (!C.core) {
        int e = errno;
        P.tl_unlock(C.loop);
        snprintf(err, (size_t)err_size, "PipeWire server not running (%s)", strerror(e));
        disconnect_all();
        return -1;
    }
    pw_core_add_listener(C.core, &C.core_hook, &core_events, NULL);
    C.reg = pw_core_get_registry(C.core, PW_VERSION_REGISTRY, 0);
    if (C.reg) pw_registry_add_listener(C.reg, &C.reg_hook, &reg_events, NULL);
    /* 1st roundtrip: all objects; 2nd: the properties/formats of the outputs and the metadata */
    bool ok = roundtrip_locked(3.0) && roundtrip_locked(3.0);
    P.tl_unlock(C.loop);
    g_connected = true;
    if (!ok) {
        snprintf(err, (size_t)err_size, "PipeWire server does not answer");
        disconnect_all();
        return -1;
    }
    return 0;
}

/* ================= stream ================= */

static struct {
    struct pw_stream *stream;
    struct spa_hook   hook;
    PwbRenderFn       fn;
    void             *user;
    int               ch;
    int               rate;
    struct spa_io_position *_Atomic pos;
    /* written only on the RT thread */
    uint64_t          prev_pos, prev_dur;
    uint64_t          prev_cb_ns;
    bool              have_prev;
    char              target_desc[160];
} S;

static atomic_int      g_state;               /* PwbStatus.state */
static atomic_uint_fast64_t g_callbacks, g_xruns;
static atomic_uint     g_quantum, g_graph_rate, g_last_frames;
static char            g_stream_err[128];

static void on_process(void *data)
{
    (void)data;
    struct pw_stream *st = S.stream;
    struct pw_buffer *b = P.stream_dequeue_buffer(st);
    if (!b) { atomic_fetch_add_explicit(&g_xruns, 1, memory_order_relaxed); return; }   /* no free buffer */
    struct spa_buffer *buf = b->buffer;
    struct spa_data *d = &buf->datas[0];
    if (!d->data || !d->chunk) { P.stream_queue_buffer(st, b); return; }

    const uint32_t stride = (uint32_t)sizeof(float) * (uint32_t)S.ch;
    uint32_t n = d->maxsize / stride;
    uint64_t t0 = now_ns();
    struct spa_io_position *pos = atomic_load_explicit(&S.pos, memory_order_acquire);
    if (g_has_requested && b->requested > 0) {
        if (b->requested < n) n = (uint32_t)b->requested;            /* exactly as much as the graph wants */
    } else if (pos && pos->clock.rate.denom == (uint32_t)S.rate && pos->clock.duration > 0 && pos->clock.duration < n) {
        n = (uint32_t)pos->clock.duration;                             /* old PipeWire: one graph cycle */
    }
    if (pos) {
        uint64_t p = pos->clock.position, du = pos->clock.duration;
        /* a gap in the graph clock = the card missed a cycle (xrun) */
        if (S.have_prev && p > S.prev_pos + S.prev_dur && t0 - S.prev_cb_ns < 1000000000ull)
            atomic_fetch_add_explicit(&g_xruns, 1, memory_order_relaxed);
        S.prev_pos = p; S.prev_dur = du; S.have_prev = true;
        atomic_store_explicit(&g_quantum, (unsigned)du, memory_order_relaxed);
        if (pos->clock.rate.denom) atomic_store_explicit(&g_graph_rate, pos->clock.rate.denom, memory_order_relaxed);
    }
    S.prev_cb_ns = t0;

    S.fn((float *)d->data, n, S.ch, S.user);

    d->chunk->offset = 0;
    d->chunk->stride = (int32_t)stride;
    d->chunk->size = n * stride;
    P.stream_queue_buffer(st, b);

    /* are we late: the end of our work is after the end of the cycle */
    if (pos && pos->clock.rate.denom && pos->clock.nsec) {
        uint64_t period = pos->clock.duration * 1000000000ull / pos->clock.rate.denom;
        uint64_t t1 = now_ns();
        if (t1 > pos->clock.nsec + period + period / 4) {
            atomic_fetch_add_explicit(&g_xruns, 1, memory_order_relaxed);
#ifdef PWB_DEBUG
            fprintf(stderr, "PWDBG late: start %+.3f ms after cycle, render %.3f ms, period %.3f ms\n",
                    ((double)t0 - (double)pos->clock.nsec) / 1e6, (double)(t1 - t0) / 1e6, (double)period / 1e6);
#endif
        }
    }
    atomic_store_explicit(&g_last_frames, n, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_callbacks, 1, memory_order_release);
}

static void on_state(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
    (void)data; (void)old;
    int s = 1;
    switch (state) {
    case PW_STREAM_STATE_ERROR:       s = -1; break;
    case PW_STREAM_STATE_UNCONNECTED: s = 0; break;
    case PW_STREAM_STATE_CONNECTING:  s = 1; break;
    case PW_STREAM_STATE_PAUSED:      s = 2; break;
    case PW_STREAM_STATE_STREAMING:   s = 3; break;
    }
    if (state == PW_STREAM_STATE_ERROR)
        snprintf(g_stream_err, sizeof g_stream_err, "%s", error ? error : "stream error");
    printf(">>> PipeWire stream: %s%s%s\n", P.stream_state_as_string(state), error ? " - " : "", error ? error : "");
    atomic_store(&g_state, s);
    P.tl_signal(C.loop, false);
}

static void on_io_changed(void *data, uint32_t id, void *area, uint32_t size)
{
    (void)data;
    if (id == SPA_IO_Position)
        atomic_store_explicit(&S.pos, (area && size >= sizeof(struct spa_io_position)) ? area : NULL,
                              memory_order_release);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state,
    .io_changed = on_io_changed,
    .process = on_process,
};

static SinkSlot *find_sink_locked(const char *name)
{
    if (!name || !*name) return NULL;
    for (int i = 0; i < PWB_MAX_SINKS; ++i)
        if (C.sinks[i].used && strcmp(C.sinks[i].s.name, name) == 0) return &C.sinks[i];
    return NULL;
}

/* ================= 1.4: the input (capture) =================
 * A second stream from the system's default input (2 channels). It runs only while a channel is armed
 * (pwb_input_enable). Its process callback puts the audio into a ring; the output side takes the same number of
 * frames per cycle (pwb_input_take), so the input arrives with one cycle of delay. */
#define IN_RING 32768u                                   /* frames (power of 2) */
static struct { struct pw_stream *stream; struct spa_hook hook; atomic_bool want, open; } I;
static float       g_iring[IN_RING * 2];
static atomic_uint g_iw, g_ir;

static void in_process(void *data)
{
    (void)data;
    struct pw_buffer *b = P.stream_dequeue_buffer(I.stream);
    if (!b) return;
    struct spa_data *d = &b->buffer->datas[0];
    if (d->data && d->chunk && d->chunk->size > 0) {
        const float *src = (const float *)((const uint8_t *)d->data + d->chunk->offset);
        uint32_t n = d->chunk->size / (uint32_t)(sizeof(float) * 2);
        unsigned w = atomic_load_explicit(&g_iw, memory_order_relaxed), r = atomic_load_explicit(&g_ir, memory_order_acquire);
        for (uint32_t i = 0; i < n && w - r < IN_RING; ++i, ++w) {
            g_iring[(w % IN_RING) * 2] = src[i * 2];
            g_iring[(w % IN_RING) * 2 + 1] = src[i * 2 + 1];
        }
        atomic_store_explicit(&g_iw, w, memory_order_release);
    }
    P.stream_queue_buffer(I.stream, b);
}

static const struct pw_stream_events in_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = in_process,
};

/* 1.3.1: the buffer chosen in Edit -> Audio. node.latency alone is only a wish (another program, or the system
 * default, may keep the graph at 1024); node.force-quantum (PipeWire 0.3.65+, ignored by older ones) makes the whole
 * graph run with it while Q-LiM plays - the same thing pw-jack does when a JACK program sets its buffer. */
static int g_req_quantum;

static void in_open_locked(int rate)
{
    if (I.stream || !C.core) return;
    struct pw_properties *props = P.properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Production",
        PW_KEY_APP_NAME, "Q-LiM", PW_KEY_NODE_NAME, "Q-LiM input", PW_KEY_NODE_DESCRIPTION, "Q-LiM input", NULL);
    P.properties_setf(props, PW_KEY_NODE_RATE, "1/%d", rate);
    if (g_req_quantum > 0) P.properties_setf(props, PW_KEY_NODE_LATENCY, "%d/%d", g_req_quantum, rate);
    I.stream = P.stream_new(C.core, "Q-LiM input", props);
    if (!I.stream) return;
    P.stream_add_listener(I.stream, &I.hook, &in_events, NULL);
    uint8_t pod_buf[512];
    struct spa_pod_builder bld = SPA_POD_BUILDER_INIT(pod_buf, sizeof pod_buf);
    struct spa_audio_info_raw info;
    memset(&info, 0, sizeof info);
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = (uint32_t)rate;
    info.channels = 2;
    info.position[0] = SPA_AUDIO_CHANNEL_FL; info.position[1] = SPA_AUDIO_CHANNEL_FR;
    const struct spa_pod *params[1] = { spa_format_audio_raw_build(&bld, SPA_PARAM_EnumFormat, &info) };
    atomic_store(&g_iw, 0u); atomic_store(&g_ir, 0u);
    if (P.stream_connect(I.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                         PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS, params, 1) < 0) {
        spa_hook_remove(&I.hook); P.stream_destroy(I.stream); I.stream = NULL;
        printf(">>> PipeWire input: could not connect\n");
        return;
    }
    atomic_store(&I.open, true);
    printf(">>> PipeWire input opened (the system's default input)\n");
}

static void in_close_locked(void)
{
    if (!I.stream) return;
    atomic_store(&I.open, false);
    spa_hook_remove(&I.hook);
    P.stream_destroy(I.stream);
    I.stream = NULL;
    printf(">>> PipeWire input closed\n");
}

void pwb_input_enable(bool on)
{
    atomic_store(&I.want, on);
    if (!g_connected || !C.loop) return;
    if (on == atomic_load(&I.open)) return;
    P.tl_lock(C.loop);
    if (on) in_open_locked(S.rate > 0 ? S.rate : 48000); else in_close_locked();
    P.tl_unlock(C.loop);
}
int pwb_input_channels(void) { return atomic_load(&I.open) ? 2 : 0; }
unsigned pwb_input_latency_frames(void)
{
    unsigned q = atomic_load(&g_quantum);
    return q ? q * 3 : 0;                            /* out + in + the ring: about three cycles */
}

/* RT (the output side): n frames of the input (stereo) into dst; 0 if there is none */
int pwb_input_take(float *dst, uint32_t n)
{
    if (!atomic_load_explicit(&I.open, memory_order_acquire)) return 0;
    unsigned r = atomic_load_explicit(&g_ir, memory_order_relaxed), w = atomic_load_explicit(&g_iw, memory_order_acquire);
    unsigned have = w - r;
    if (have > n * 4 + 2048) { r = w - n * 2; have = n * 2; }   /* it ran ahead (clock drift / a gap): keep the delay small */
    uint32_t k = have < n ? have : n;
    for (uint32_t i = 0; i < k; ++i, ++r) { dst[i * 2] = g_iring[(r % IN_RING) * 2]; dst[i * 2 + 1] = g_iring[(r % IN_RING) * 2 + 1]; }
    for (uint32_t i = k; i < n; ++i) { dst[i * 2] = 0.0f; dst[i * 2 + 1] = 0.0f; }
    atomic_store_explicit(&g_ir, r, memory_order_release);
    return 2;
}

int pwb_open(const char *target, int rate, int quantum, int channels,
             PwbRenderFn fn, void *user, int *out_channels, char *err, int err_size)
{
    char dummy[8];
    if (!err || err_size <= 0) { err = dummy; err_size = sizeof dummy; }
    err[0] = 0;
    if (S.stream) pwb_close();
    if (!fn) { snprintf(err, (size_t)err_size, "no render function"); return -1; }
    if (rate <= 0) rate = 48000;
    if (quantum <= 0) quantum = 256;
    if (connect_all(err, err_size) != 0) return -1;

    P.tl_lock(C.loop);
    /* to which output: the chosen one (if present) or the system default */
    SinkSlot *sl = find_sink_locked(target);
    bool explicit_target = sl != NULL;
    if (target && *target && !sl)
        printf(">>> PipeWire: saved output '%s' not found -> using the system default\n", target);
    if (!sl) sl = find_sink_locked(C.default_sink);

    uint32_t pos[PWB_MAX_CH];
    int ch = channels;
    if (ch <= 0) ch = (sl && sl->s.channels >= 1) ? sl->s.channels : 2;
    if (ch < 2) ch = 2;                                     /* the mixer always gives at least stereo */
    if (ch > PWB_MAX_CH) ch = PWB_MAX_CH;
    if (sl && sl->s.channels == ch) memcpy(pos, sl->s.pos, sizeof(uint32_t) * (size_t)ch);
    else if (ch == 2) { pos[0] = SPA_AUDIO_CHANNEL_FL; pos[1] = SPA_AUDIO_CHANNEL_FR; }
    else for (int i = 0; i < ch; ++i) pos[i] = SPA_AUDIO_CHANNEL_AUX0 + (uint32_t)i;

    struct pw_properties *props = P.properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Production",
        PW_KEY_APP_NAME, "Q-LiM",
        PW_KEY_NODE_NAME, "Q-LiM",
        PW_KEY_NODE_DESCRIPTION, "Q-LiM",
        PW_KEY_NODE_ALWAYS_PROCESS, "true",
        NULL);
    P.properties_setf(props, PW_KEY_NODE_LATENCY, "%d/%d", quantum, rate);   /* the desired buffer */
    P.properties_setf(props, "node.force-quantum", "%d", quantum);             /* ... and really use it (1.3.1) */
    P.properties_set(props, "node.lock-quantum", "true");
    g_req_quantum = quantum;
    P.properties_setf(props, PW_KEY_NODE_RATE, "1/%d", rate);                /* request that the graph switches to our rate */
    if (explicit_target) {
        P.properties_set(props, PW_KEY_TARGET_OBJECT, sl->s.name);
        if (!g_has_target_object) P.properties_setf(props, "node.target", "%u", sl->s.id);   /* old PipeWire */
        P.properties_set(props, PW_KEY_NODE_DONT_RECONNECT, "true");          /* if the card disappears -> we say so, we do not silently move on */
    }
    if (sl && sl->s.channels == ch) P.properties_set(props, PW_KEY_STREAM_DONT_REMIX, "true"); /* output N -> output N, no mixing */

    memset(&S, 0, sizeof S);
    S.fn = fn; S.user = user; S.ch = ch; S.rate = rate;
    snprintf(S.target_desc, sizeof S.target_desc, "%s", sl ? sl->s.desc : "system default");
    g_target_id = explicit_target ? sl->s.id : SPA_ID_INVALID;
    atomic_store(&g_target_gone, false);
    atomic_store(&g_state, 1);
    atomic_store(&g_callbacks, 0); atomic_store(&g_xruns, 0);
    atomic_store(&g_quantum, 0); atomic_store(&g_graph_rate, 0); atomic_store(&g_last_frames, 0);
    g_stream_err[0] = 0;

    S.stream = P.stream_new(C.core, "Q-LiM output", props);    /* props become owned by the stream */
    if (!S.stream) {
        P.tl_unlock(C.loop);
        snprintf(err, (size_t)err_size, "cannot create PipeWire stream");
        return -1;
    }
    P.stream_add_listener(S.stream, &S.hook, &stream_events, NULL);

    uint8_t pod_buf[1024];
    struct spa_pod_builder bld = SPA_POD_BUILDER_INIT(pod_buf, sizeof pod_buf);
    struct spa_audio_info_raw info;
    memset(&info, 0, sizeof info);
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = (uint32_t)rate;
    info.channels = (uint32_t)ch;
    memcpy(info.position, pos, sizeof(uint32_t) * (size_t)ch);
    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&bld, SPA_PARAM_EnumFormat, &info);

    int r = P.stream_connect(S.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                             PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS,
                             params, 1);
    if (r < 0) {
        snprintf(err, (size_t)err_size, "connect failed: %s", spa_strerror(r));
        P.stream_destroy(S.stream); S.stream = NULL;
        P.tl_unlock(C.loop);
        return -1;
    }
    /* wait for PipeWire to accept the stream (PAUSED) or start calling it (STREAMING) */
    uint64_t end = now_ns() + 4000000000ull;
    while (atomic_load(&g_state) == 1 && !atomic_load(&C.core_dead) && now_ns() < end) P.tl_timed_wait(C.loop, 1);
    int st = atomic_load(&g_state);
    if (st < 2) {
        snprintf(err, (size_t)err_size, "%s", st < 0 ? g_stream_err : atomic_load(&C.core_dead) ? "PipeWire disconnected"
                                                                                                : "PipeWire did not accept the stream");
        spa_hook_remove(&S.hook);
        P.stream_destroy(S.stream); S.stream = NULL;
        P.tl_unlock(C.loop);
        return -1;
    }
    /* up to 1.5 s for it to really start (usually < 50 ms); if not - it is still OK, the session manager may be slow */
    end = now_ns() + 1500000000ull;
    while (atomic_load(&g_state) == 2 && atomic_load(&g_callbacks) == 0 && now_ns() < end) P.tl_timed_wait(C.loop, 1);
    printf(">>> PipeWire node id %u -> %s\n", P.stream_get_node_id(S.stream), S.target_desc);
    if (atomic_load(&I.want)) in_open_locked(rate);     /* 1.4: a channel is armed */
    P.tl_unlock(C.loop);
    if (out_channels) *out_channels = ch;
    return 0;
}

static void disconnect_all(void)
{
    if (!C.loop) { g_connected = false; return; }
    P.tl_stop(C.loop);                    /* the thread stops -> no more events, no lock needed */
    in_close_locked();                    /* 1.4 */
    if (S.stream) { spa_hook_remove(&S.hook); P.stream_destroy(S.stream); S.stream = NULL; }
    graph_teardown_filters();
    for (int i = 0; i < PWB_MAX_SINKS; ++i) {
        SinkSlot *sl = &C.sinks[i];
        if (sl->used && sl->proxy) { spa_hook_remove(&sl->hook); P.proxy_destroy(sl->proxy); }
        sl->proxy = NULL; sl->used = false;
    }
    if (C.meta) { spa_hook_remove(&C.meta_hook); P.proxy_destroy(C.meta); C.meta = NULL; }
    if (C.reg) { spa_hook_remove(&C.reg_hook); P.proxy_destroy((struct pw_proxy *)C.reg); C.reg = NULL; }
    if (C.core) { spa_hook_remove(&C.core_hook); P.core_disconnect(C.core); C.core = NULL; }
    graph_free_tables();                  /* after core_disconnect: it also destroys our links (proxies) */
    t_free();
    if (C.ctx) { P.context_destroy(C.ctx); C.ctx = NULL; }
    P.tl_destroy(C.loop);
    C.loop = NULL;
    g_connected = false;
}

void pwb_close(void)
{
    if (!g_connected) return;
    disconnect_all();                     /* the stream + the connection: the next open starts clean */
    atomic_store(&g_state, 0);
    g_target_id = SPA_ID_INVALID;
}

static bool graph_active(void);
bool pwb_is_open(void) { return g_connected && (S.stream != NULL || graph_active()); }

void pwb_get_status(PwbStatus *st)
{
    memset(st, 0, sizeof *st);
    if (!pwb_is_open()) return;
    st->state = atomic_load(&g_state);
    st->graph_rate = atomic_load(&g_graph_rate);
    st->quantum = atomic_load(&g_quantum);
    st->last_frames = atomic_load(&g_last_frames);
    st->callbacks = atomic_load_explicit(&g_callbacks, memory_order_acquire);
    st->xruns = atomic_load(&g_xruns);
    snprintf(st->node_desc, sizeof st->node_desc, "%s", S.target_desc);
    if (atomic_load(&C.core_dead)) { st->lost = true; snprintf(st->why, sizeof st->why, "PipeWire server stopped"); }
    else if (atomic_load(&g_target_gone)) { st->lost = true; snprintf(st->why, sizeof st->why, "output disappeared"); }
    else if (st->state < 0) { st->lost = true; snprintf(st->why, sizeof st->why, "%s", g_stream_err); }
    else if (st->state == 0) { st->lost = true; snprintf(st->why, sizeof st->why, "stream disconnected"); }
}

int pwb_list_sinks(PwbSink *out, int max)
{
    char err[160];
    bool temp = !g_connected;
    if (connect_all(err, sizeof err) != 0) return 0;
    if (!g_connected) return 0;
    P.tl_lock(C.loop);
    if (!temp) roundtrip_locked(1.0);     /* to catch freshly plugged-in cards */
    int n = 0;
    for (int i = 0; i < PWB_MAX_SINKS && n < max; ++i) {
        if (!C.sinks[i].used) continue;
        out[n] = C.sinks[i].s;
        if (out[n].channels <= 0) {                  /* unknown -> stereo */
            out[n].channels = 2;
            out[n].pos[0] = SPA_AUDIO_CHANNEL_FL; out[n].pos[1] = SPA_AUDIO_CHANNEL_FR;
        }
        out[n].is_default = C.default_sink[0] && strcmp(C.default_sink, out[n].name) == 0;
        n++;
    }
    P.tl_unlock(C.loop);
    if (temp) disconnect_all();
    return n;
}

/* ---- availability (remembered for ~5 s, so we do not connect every frame from the dialog) ---- */
static int    g_avail = -1;
static double g_avail_t;

void pwb_forget_availability(void) { g_avail = -1; }

bool pwb_available(void)
{
    double t = (double)now_ns() * 1e-9;
    if (g_avail >= 0 && t - g_avail_t < 5.0) return g_avail == 1;
    if (g_connected && !atomic_load(&C.core_dead)) { g_avail = 1; g_avail_t = t; return true; }
    char err[160] = "";
    int ok = 0;
    if (pw_load() == 1 && !g_connected) {
        if (connect_all(err, sizeof err) == 0) { ok = 1; disconnect_all(); }
    }
    if (!ok && err[0]) printf(">>> PipeWire native: not available (%s)\n", err);
    g_avail = ok; g_avail_t = t;
    return ok;
}

/* =====================================================================================
 * Graph mode (step 28): the nodes "Q-LiM" (the channel outputs) and "Q-LiM Master" + the links.
 *
 * Threads:
 *   - PipeWire's RT thread calls gch_process (the channels) and gm_process (the master) - in one and the same cycle,
 *     the channels first (the master depends on them through the links);
 *   - the pw_thread_loop thread receives the registry events (ports/links) -> the tables T and the queue G.dl;
 *   - the main thread calls pwb_graph_sync() every frame (under the thread loop's lock).
 * Number of ports: pw_filter does NOT allow safely adding/removing ports while the RT thread is running
 * (in 1.0 the port list is not protected) -> when the NUMBER of channels changes, the channels' node is recreated
 * (a short silence, ~a few ms; when adding/removing a channel the mixer pauses for a moment anyway).
 * Renaming a channel = only a new port name (no interruption).
 * ===================================================================================== */

typedef struct GChF {
    struct pw_filter *f;
    struct spa_hook   hook;
    int               nports;
    void            **ports;
    float           **bufs;              /* RT thread only */
    char            (*pname)[128];      /* the current names (port info - visible right away in pw-dump/Helvum) */
    int               state;              /* the last state of this node (filter_state_code) */
    bool              renamed;            /* the names in the registry (pw-link, qpwgraph, Patchance) are the old ones */
    uint64_t          rename_t;
} GChF;

typedef struct OwnLink {
    struct pw_proxy *proxy;
    struct spa_hook  hook;
    uint32_t         out_port, in_port;
    bool             seen;               /* already visible in the registry */
    uint64_t         t;
} OwnLink;

typedef struct { uint32_t out_port, in_port; uint64_t t; } PairT;
typedef struct { uint32_t out_port, in_port; bool add; uint64_t t; } Delta;
typedef struct { char name[128]; uint32_t in_port; uint64_t t; } Carry;   /* a link to another program - carried over to a new node */

#define RBIT64(b) (1ull << (b))
#define GRAPH_DELTA_NS   300000000ull    /* a change from outside is applied after 300 ms (if the ports still exist) */
#define GRAPH_PDEL_NS   3000000000ull
#define GRAPH_REBUILD_NS 250000000ull

static struct {
    bool          active;
    PwbChannelsFn chfn;
    PwbMasterFn   mfn;
    void         *user;
    int           rate, quantum;
    char          group[48];
    GChF         *ch;                    /* the newest channels node */
    /* Switching without interruption (step 28b): the old node plays until the new one is fully connected; then the switch happens
     * at the boundary between two cycles; the old one is removed a few cycles later. */
    GChF         *old;
    uint32_t      old_node;
    GChF *_Atomic render;                /* which node computes the audio (the other one gives silence) */
    GChF         *latched;               /* RT: the decision for the current cycle */
    uint64_t      latched_pos;
    int           sw_phase;              /* 0 = no switch, 1 = waiting for the new one, 2 = switched, waiting for the old one to be removed */
    uint64_t      sw_t, sw_cb;
    struct pw_filter *mf;
    struct spa_hook   mhook;
    void         *mports[4];             /* In L, In R, Out L, Out R */
    uint32_t      ch_node, m_node, target_node;
    OwnLink     **own; int nown, cap_own;
    PairT        *pdel; int npdel, cap_pdel;
    Delta        *dl; int ndl, cap_dl;
    Carry        *carry; int ncarry, cap_carry;
    /* last synchronized (main thread) */
    PwbRoute     *L; bool *Lk; int Lcap;
    PwbRoute      Lm; bool Lmk;
    bool          resync;
    int           want_nch; uint64_t want_t;
    /* RT */
    uint64_t      prev_pos, prev_dur, prev_cb_ns;
    bool          have_prev;
} G = { .ch_node = SPA_ID_INVALID, .m_node = SPA_ID_INVALID, .target_node = SPA_ID_INVALID };

static bool graph_active(void) { return G.active; }
bool pwb_graph_supported(void) { return pw_load() == 1 && F.ok; }
bool pwb_graph_is_open(void) { return g_connected && G.active; }

static void *grow(void *arr, int *cap, int need, size_t el)
{
    if (need <= *cap) return arr;
    int nc = *cap ? *cap * 2 : 64;
    while (nc < need) nc *= 2;
    void *n = realloc(arr, el * (size_t)nc);
    if (!n) return NULL;
    *cap = nc;
    return n;
}

/* --- RT: the channels --- */
static void gch_process(void *data, struct spa_io_position *pos)
{
    GChF *c = data;
    if (!pos) return;
    uint32_t n = (uint32_t)pos->clock.duration;
    if (n == 0 || n > 32768) return;
    /* who computes the audio: decided once per cycle (by the first of the two nodes in the cycle), so the switch does not happen
     * in the middle of a cycle (then the audio would be computed twice or not at all) */
    if (pos->clock.position != G.latched_pos || !G.latched) {
        G.latched_pos = pos->clock.position;
#ifdef PWB_DEBUG
        GChF *was = G.latched;
#endif
        G.latched = atomic_load_explicit(&G.render, memory_order_acquire);
#ifdef PWB_DEBUG
        if (was != G.latched) fprintf(stderr, "PWDBG %.4f latch -> %p pos %llu\n", now_ns() / 1e9, (void *)G.latched, (unsigned long long)pos->clock.position);
#endif
    }
    if (c != G.latched) {                                    /* not our turn: silence */
        for (int k = 0; k < c->nports; ++k) {
            float *b = F.filter_get_dsp_buffer(c->ports[k], n);
            if (b) memset(b, 0, n * sizeof(float));
        }
        return;
    }
    uint64_t t0 = now_ns();
    uint64_t p = pos->clock.position;
    if (G.have_prev && p > G.prev_pos + G.prev_dur && t0 - G.prev_cb_ns < 1000000000ull) {
        atomic_fetch_add_explicit(&g_xruns, 1, memory_order_relaxed);
#ifdef PWB_DEBUG
        fprintf(stderr, "PWDBG %.4f gap pos %llu prev %llu\n", t0 / 1e9, (unsigned long long)p, (unsigned long long)G.prev_pos);
#endif
    }
#ifdef PWB_DEBUG
    if (G.have_prev && p == G.prev_pos) fprintf(stderr, "PWDBG %.4f SAME pos %llu (rendered twice)\n", t0 / 1e9, (unsigned long long)p);
#endif
    G.prev_pos = p; G.prev_dur = n; G.have_prev = true; G.prev_cb_ns = t0;
    atomic_store_explicit(&g_quantum, n, memory_order_relaxed);
    if (pos->clock.rate.denom) atomic_store_explicit(&g_graph_rate, pos->clock.rate.denom, memory_order_relaxed);
    for (int k = 0; k < c->nports; ++k) c->bufs[k] = F.filter_get_dsp_buffer(c->ports[k], n);
    G.chfn(c->bufs, c->nports, n, G.user);
    atomic_store_explicit(&g_last_frames, n, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_callbacks, 1, memory_order_release);
}

/* --- RT: the master --- */
static void gm_process(void *data, struct spa_io_position *pos)
{
    (void)data;
    if (!pos) return;
    uint32_t n = (uint32_t)pos->clock.duration;
    if (n == 0 || n > 32768) return;
    float *il = F.filter_get_dsp_buffer(G.mports[0], n), *ir = F.filter_get_dsp_buffer(G.mports[1], n);
    float *ol = F.filter_get_dsp_buffer(G.mports[2], n), *orr = F.filter_get_dsp_buffer(G.mports[3], n);
#ifdef PWB_DEBUG
    {
        static uint64_t lastp; static int silent;
        float mx = 0; if (il) for (uint32_t i = 0; i < n; ++i) if (fabsf(il[i]) > mx) mx = fabsf(il[i]);
        if (mx < 1e-4f && !silent) fprintf(stderr, "PWDBG %.4f master in SILENT pos %llu il=%p (last %llu)\n", now_ns() / 1e9, (unsigned long long)pos->clock.position, (void *)il, (unsigned long long)lastp);
        if (mx >= 1e-4f && silent) fprintf(stderr, "PWDBG %.4f master in back pos %llu\n", now_ns() / 1e9, (unsigned long long)pos->clock.position);
        silent = mx < 1e-4f; lastp = pos->clock.position;
    }
#endif
    G.mfn(il, ir, ol, orr, n, G.user);
    /* are we late: the end of all our work (the channels + the master) is after the end of the cycle */
    if (pos->clock.rate.denom && pos->clock.nsec) {
        uint64_t period = pos->clock.duration * 1000000000ull / pos->clock.rate.denom;
        if (now_ns() > pos->clock.nsec + period + period / 4)
            atomic_fetch_add_explicit(&g_xruns, 1, memory_order_relaxed);
    }
}

static int filter_state_code(enum pw_filter_state st)
{
    switch (st) {
    case PW_FILTER_STATE_ERROR:       return -1;
    case PW_FILTER_STATE_UNCONNECTED: return 0;
    case PW_FILTER_STATE_CONNECTING:  return 1;
    case PW_FILTER_STATE_PAUSED:      return 2;
    case PW_FILTER_STATE_STREAMING:   return 3;
    }
    return 1;
}

static void gch_state(void *data, enum pw_filter_state old, enum pw_filter_state st, const char *error)
{
    (void)old;
    GChF *c = data;
    c->state = filter_state_code(st);
    if (c != atomic_load(&G.render) && st != PW_FILTER_STATE_ERROR) return;   /* not the one that is playing */
    if (st == PW_FILTER_STATE_ERROR) snprintf(g_stream_err, sizeof g_stream_err, "%s", error ? error : "filter error");
    printf(">>> PipeWire graph (channels): %s%s%s\n", F.filter_state_as_string(st), error ? " - " : "", error ? error : "");
    atomic_store(&g_state, filter_state_code(st));
    P.tl_signal(C.loop, false);
}

static void gm_state(void *data, enum pw_filter_state old, enum pw_filter_state st, const char *error)
{
    (void)data; (void)old;
    printf(">>> PipeWire graph (master): %s%s%s\n", F.filter_state_as_string(st), error ? " - " : "", error ? error : "");
    if (st == PW_FILTER_STATE_ERROR) {
        snprintf(g_stream_err, sizeof g_stream_err, "%s", error ? error : "master filter error");
        atomic_store(&g_state, -1);
    }
    P.tl_signal(C.loop, false);
}

static const struct pw_filter_events gch_events = {
    PW_VERSION_FILTER_EVENTS,
    .state_changed = gch_state,
    .process = gch_process,
};
static const struct pw_filter_events gm_events = {
    PW_VERSION_FILTER_EVENTS,
    .state_changed = gm_state,
    .process = gm_process,
};

static struct pw_properties *node_props(const char *name, const char *desc)
{
    struct pw_properties *pr = P.properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_ROLE, "Production",
        PW_KEY_APP_NAME, "Q-LiM",
        PW_KEY_APP_ICON_NAME, "qlim",          /* if a "qlim" icon is ever installed - Patchance/qpwgraph use it */
        PW_KEY_NODE_NAME, name,
        PW_KEY_NODE_DESCRIPTION, desc,
        PW_KEY_NODE_NICK, desc,
        PW_KEY_NODE_ALWAYS_PROCESS, "true",
        PW_KEY_NODE_WANT_DRIVER, "true",
        PW_KEY_NODE_AUTOCONNECT, "false",      /* the links are made by the program itself (according to Routing) */
        NULL);
    P.properties_set(pr, PW_KEY_NODE_GROUP, G.group);   /* the two nodes - always in one cycle, with one clock */
    P.properties_setf(pr, PW_KEY_NODE_LATENCY, "%d/%d", G.quantum, G.rate);
    P.properties_setf(pr, "node.force-quantum", "%d", G.quantum);              /* 1.3.1: see g_req_quantum */
    P.properties_set(pr, "node.lock-quantum", "true");
    P.properties_setf(pr, PW_KEY_NODE_RATE, "1/%d", G.rate);
    return pr;
}

static void *add_dsp_port(struct pw_filter *f, enum pw_direction dir, const char *name, const char *chan)
{
    struct pw_properties *pp = P.properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                                                PW_KEY_PORT_NAME, name, NULL);
    if (chan) P.properties_set(pp, PW_KEY_AUDIO_CHANNEL, chan);
    return F.filter_add_port(f, dir, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 8, pp, NULL, 0);
}

/* port names: "<channel> L/R"; empty -> "Ch N"; equal names -> "(N)" after the second one */
static void port_base_name(int i, int nch, const char *const *names, char *out, size_t sz)
{
    const char *nm = (names && i < nch && names[i] && names[i][0]) ? names[i] : NULL;
    if (!nm) { snprintf(out, sz, "Ch %d", i + 1); return; }
    bool dup = false;
    for (int j = 0; j < i; ++j) if (names[j] && strcmp(names[j], nm) == 0) { dup = true; break; }
    if (dup) snprintf(out, sz, "%s (%d)", nm, i + 1);
    else snprintf(out, sz, "%s", nm);
}

static GChF *gch_create(int nch, const char *const *names)
{
    GChF *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    int np = nch * 2;
    c->nports = np;
    c->ports = calloc((size_t)(np ? np : 1), sizeof(void *));
    c->bufs = calloc((size_t)(np ? np : 1), sizeof(float *));
    c->pname = calloc((size_t)(np ? np : 1), sizeof *c->pname);
    if (!c->ports || !c->bufs || !c->pname) goto fail;
    c->f = F.filter_new(C.core, "Q-LiM", node_props("Q-LiM", "Q-LiM"));
    if (!c->f) goto fail;
    G.ch = c;                                     /* for gch_state */
    F.filter_add_listener(c->f, &c->hook, &gch_events, c);
    for (int i = 0; i < nch; ++i) {
        char base[112];
        port_base_name(i, nch, names, base, sizeof base);
        for (int sd = 0; sd < 2; ++sd) {
            snprintf(c->pname[2 * i + sd], sizeof c->pname[0], "%s %s", base, sd ? "R" : "L");
            c->ports[2 * i + sd] = add_dsp_port(c->f, PW_DIRECTION_OUTPUT, c->pname[2 * i + sd], sd ? "FR" : "FL");
            if (!c->ports[2 * i + sd]) { fprintf(stderr, ">>> PipeWire graph: cannot add port\n"); goto fail_f; }
        }
    }
    if (F.filter_connect(c->f, PW_FILTER_FLAG_RT_PROCESS, NULL, 0) < 0) goto fail_f;
    return c;
fail_f:
    spa_hook_remove(&c->hook);
    F.filter_destroy(c->f);
fail:
    if (G.ch == c) G.ch = NULL;
    free(c->ports); free(c->bufs); free(c->pname); free(c);
    return NULL;
}

static void gch_destroy(GChF *c)
{
    if (!c) return;
    if (G.ch == c) G.ch = NULL;
    if (G.old == c) G.old = NULL;
    if (atomic_load(&G.render) == c) atomic_store(&G.render, NULL);
    spa_hook_remove(&c->hook);
    F.filter_destroy(c->f);                       /* waits for the RT thread to let it go */
    free(c->ports); free(c->bufs); free(c->pname); free(c);
}

/* --- our links --- */
static void own_destroy_ev(void *data)
{
    OwnLink *o = data;
    spa_hook_remove(&o->hook);
    o->proxy = NULL;
}
static void own_removed_ev(void *data)
{
    OwnLink *o = data;
    if (o->proxy) P.proxy_destroy(o->proxy);     /* -> own_destroy_ev */
}
static void own_error_ev(void *data, int seq, int res, const char *message)
{
    (void)seq;
    OwnLink *o = data;
    printf(">>> PipeWire graph: link %u -> %u failed: %s\n", o->out_port, o->in_port, message ? message : spa_strerror(res));
}
static const struct pw_proxy_events own_events = {
    PW_VERSION_PROXY_EVENTS,
    .destroy = own_destroy_ev,
    .removed = own_removed_ev,
    .error = own_error_ev,
};

static void pdel_add(uint32_t out, uint32_t in)
{
    PairT *n = grow(G.pdel, &G.cap_pdel, G.npdel + 1, sizeof(PairT));
    if (!n) return;
    G.pdel = n;
    G.pdel[G.npdel++] = (PairT){ out, in, now_ns() };
}

static bool t_pdel_has(uint32_t out, uint32_t in)
{
    for (int i = 0; i < G.npdel; ++i) if (G.pdel[i].out_port == out && G.pdel[i].in_port == in) return true;
    return false;
}

static void own_create(uint32_t out, uint32_t in)
{
    GPort *po = t_port(out), *pi = t_port(in);
    if (!po || !pi) return;
    OwnLink **na = grow(G.own, &G.cap_own, G.nown + 1, sizeof(OwnLink *));
    if (!na) return;
    G.own = na;
    OwnLink *o = calloc(1, sizeof *o);
    if (!o) return;
    char a[16], b[16], an[16], bn[16];
    snprintf(a, sizeof a, "%u", out); snprintf(b, sizeof b, "%u", in);
    snprintf(an, sizeof an, "%u", po->node); snprintf(bn, sizeof bn, "%u", pi->node);
    struct spa_dict_item items[] = {
        SPA_DICT_ITEM_INIT(PW_KEY_LINK_OUTPUT_NODE, an),
        SPA_DICT_ITEM_INIT(PW_KEY_LINK_OUTPUT_PORT, a),
        SPA_DICT_ITEM_INIT(PW_KEY_LINK_INPUT_NODE, bn),
        SPA_DICT_ITEM_INIT(PW_KEY_LINK_INPUT_PORT, b),
        SPA_DICT_ITEM_INIT(PW_KEY_OBJECT_LINGER, "false"),   /* disappears together with the program */
    };
    struct spa_dict d = SPA_DICT_INIT(items, SPA_N_ELEMENTS(items));
    o->proxy = pw_core_create_object(C.core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &d, 0);
    if (!o->proxy) { free(o); return; }
    o->out_port = out; o->in_port = in; o->t = now_ns();
    F.proxy_add_listener(o->proxy, &o->hook, &own_events, o);
    G.own[G.nown++] = o;
}

static bool own_pending(uint32_t out, uint32_t in)
{
    for (int i = 0; i < G.nown; ++i) {
        OwnLink *o = G.own[i];
        if (o->proxy && !o->seen && o->out_port == out && o->in_port == in) return true;
    }
    return false;
}

/* removes all links out -> in (ours or others') */
static void remove_pair(uint32_t out, uint32_t in)
{
    bool any = false;
    for (int i = 0; i < G.nown; ++i) {
        OwnLink *o = G.own[i];
        if (o->proxy && o->out_port == out && o->in_port == in) {
            if (o->seen) any = true;
            P.proxy_destroy(o->proxy);            /* linger=false -> the link disappears */
        }
    }
    for (int i = 0; i < T.nl; ++i)
        if (T.l[i].out_port == out && T.l[i].in_port == in) {
            any = true;
            pw_registry_destroy(C.reg, T.l[i].id);   /* harmless if it is already on its way out */
        }
    if (any) pdel_add(out, in);
}

static void push_bit(uint32_t out, uint32_t in, bool want)
{
    bool have = t_link_pair(out, in) != NULL || own_pending(out, in);
    if (want && !have) own_create(out, in);
    else if (!want && have) remove_pair(out, in);
}

/* --- registry events (thread loop) --- */
static bool our_out_port(const GPort *p)
{
    return p && p->dir == 1 && G.active && (p->node == G.ch_node || p->node == G.m_node);
}

static void graph_port_event(const GPort *p)
{
    if (!G.active) return;
    if (p->node == G.target_node || p->node == G.ch_node || p->node == G.m_node) G.resync = true;
}

static void delta_push(uint32_t out, uint32_t in, bool add)
{
    Delta *n = grow(G.dl, &G.cap_dl, G.ndl + 1, sizeof(Delta));
    if (!n) return;
    G.dl = n;
    G.dl[G.ndl++] = (Delta){ out, in, add, now_ns() };
}

static void graph_link_added(uint32_t out, uint32_t in)
{
    if (!G.active || !our_out_port(t_port(out))) return;
    for (int i = 0; i < G.nown; ++i) {
        OwnLink *o = G.own[i];
        if (o->proxy && !o->seen && o->out_port == out && o->in_port == in) { o->seen = true; return; }
    }
    delta_push(out, in, true);                  /* connected from outside (Patchance, qpwgraph, pw-link ...) */
}

static void graph_link_removed(uint32_t out, uint32_t in)
{
    if (!G.active || !our_out_port(t_port(out))) return;
    for (int i = 0; i < G.npdel; ++i)
        if (G.pdel[i].out_port == out && G.pdel[i].in_port == in) { G.pdel[i] = G.pdel[--G.npdel]; return; }
    delta_push(out, in, false);
}

/* (out, in) -> which route and which bit. item: -1 = the master, >= 0 = a channel. false = not a route of the program. */
static bool map_pair(uint32_t out, uint32_t in, int *item, int *side, int *bit)
{
    GPort *po = t_port(out), *pi = t_port(in);
    if (!po || !pi || po->dir != 1 || pi->dir != 0) return false;
    if (po->node == G.ch_node && G.ch_node != SPA_ID_INVALID) {
        *item = (int)(po->idx / 2); *side = (int)(po->idx % 2);
        if (pi->node == G.m_node && pi->idx < 2) { *bit = (int)pi->idx; return true; }
        if (pi->node == G.target_node && pi->idx < 62) { *bit = 2 + (int)pi->idx; return true; }
        return false;
    }
    if (po->node == G.m_node && G.m_node != SPA_ID_INVALID && po->idx < 2) {
        *item = -1; *side = (int)po->idx;
        if (pi->node == G.target_node && pi->idx < 62) { *bit = (int)pi->idx; return true; }
    }
    return false;
}

static int target_inputs(void)
{
    int n = 0;
    for (int i = 0; i < T.np; ++i)
        if (T.p[i].node == G.target_node && T.p[i].dir == 0 && (int)T.p[i].idx + 1 > n) n = (int)T.p[i].idx + 1;
    return n;
}

static void graph_teardown_filters(void)
{
    atomic_store(&G.render, NULL);
    if (G.ch) gch_destroy(G.ch);
    if (G.old) gch_destroy(G.old);
    G.sw_phase = 0;
    if (G.mf) { spa_hook_remove(&G.mhook); F.filter_destroy(G.mf); G.mf = NULL; }
    G.active = false;
}

static void graph_free_tables(void)
{
    for (int i = 0; i < G.nown; ++i) free(G.own[i]);      /* the proxies were destroyed by core_disconnect */
    free(G.own); free(G.pdel); free(G.dl); free(G.L); free(G.Lk); free(G.carry);
    G.own = NULL; G.pdel = NULL; G.dl = NULL; G.L = NULL; G.Lk = NULL; G.carry = NULL;
    G.ncarry = G.cap_carry = 0;
    G.nown = G.cap_own = G.npdel = G.cap_pdel = G.ndl = G.cap_dl = G.Lcap = 0;
    G.ch_node = G.m_node = G.target_node = SPA_ID_INVALID;
    G.active = false;
}

int pwb_graph_open(const char *target, int rate, int quantum, int nch, const char *const *names,
                   PwbChannelsFn chfn, PwbMasterFn mfn, void *user, int *out_hw_ports, char *err, int err_size)
{
    char dummy[8];
    if (!err || err_size <= 0) { err = dummy; err_size = sizeof dummy; }
    err[0] = 0;
    if (S.stream || G.active) pwb_close();
    if (!chfn || !mfn) { snprintf(err, (size_t)err_size, "no render function"); return -1; }
    if (pw_load() != 1) { snprintf(err, (size_t)err_size, "libpipewire not found"); return -1; }
    if (!F.ok) { snprintf(err, (size_t)err_size, "this libpipewire has no pw_filter"); return -1; }
    if (rate <= 0) rate = 48000;
    if (quantum <= 0) quantum = 256;
    if (nch < 0) nch = 0;
    if (connect_all(err, err_size) != 0) return -1;

    P.tl_lock(C.loop);
    SinkSlot *sl = find_sink_locked(target);
    if (target && *target && !sl)
        printf(">>> PipeWire: saved output '%s' not found -> using the system default\n", target);
    if (!sl) sl = find_sink_locked(C.default_sink);
    if (!sl) for (int i = 0; i < PWB_MAX_SINKS && !sl; ++i) if (C.sinks[i].used) sl = &C.sinks[i];
    if (!sl) {
        P.tl_unlock(C.loop);
        snprintf(err, (size_t)err_size, "no PipeWire output found");
        return -1;
    }

    memset(&S, 0, sizeof S);
    S.rate = rate;
    snprintf(S.target_desc, sizeof S.target_desc, "%s", sl->s.desc);
    G.chfn = chfn; G.mfn = mfn; G.user = user;
    G.rate = rate; G.quantum = quantum; g_req_quantum = quantum;
    snprintf(G.group, sizeof G.group, "qlim-%ld", (long)getpid());
    G.target_node = sl->s.id;
    G.ch_node = G.m_node = SPA_ID_INVALID;
    G.have_prev = false;
    G.Lmk = false;
    G.resync = true;
    G.want_nch = nch;
    g_target_id = sl->s.id;                       /* if the card disappears -> "lost" (and with the default too: then the new one is chosen) */
    atomic_store(&g_target_gone, false);
    atomic_store(&g_state, 1);
    atomic_store(&g_callbacks, 0); atomic_store(&g_xruns, 0);
    atomic_store(&g_quantum, 0); atomic_store(&g_graph_rate, 0); atomic_store(&g_last_frames, 0);
    g_stream_err[0] = 0;

    /* the master */
    G.mf = F.filter_new(C.core, "Q-LiM Master", node_props("Q-LiM-Master", "Q-LiM Master"));
    if (!G.mf) { P.tl_unlock(C.loop); snprintf(err, (size_t)err_size, "cannot create PipeWire filter"); return -1; }
    F.filter_add_listener(G.mf, &G.mhook, &gm_events, NULL);
    G.mports[0] = add_dsp_port(G.mf, PW_DIRECTION_INPUT, "In L", "FL");
    G.mports[1] = add_dsp_port(G.mf, PW_DIRECTION_INPUT, "In R", "FR");
    G.mports[2] = add_dsp_port(G.mf, PW_DIRECTION_OUTPUT, "Out L", "FL");
    G.mports[3] = add_dsp_port(G.mf, PW_DIRECTION_OUTPUT, "Out R", "FR");
    bool ok = G.mports[0] && G.mports[1] && G.mports[2] && G.mports[3] &&
              F.filter_connect(G.mf, PW_FILTER_FLAG_RT_PROCESS, NULL, 0) >= 0;
    if (ok) ok = gch_create(nch, names) != NULL;
    if (ok) { atomic_store(&G.render, G.ch); G.latched = NULL; G.sw_phase = 0; }
    if (!ok) {
        if (G.mf) { spa_hook_remove(&G.mhook); F.filter_destroy(G.mf); G.mf = NULL; }
        P.tl_unlock(C.loop);
        snprintf(err, (size_t)err_size, "cannot create PipeWire filters");
        return -1;
    }
    G.active = true;
    /* wait for the two nodes to appear in the graph */
    uint64_t end = now_ns() + 4000000000ull;
    while (!atomic_load(&C.core_dead) && now_ns() < end) {
        G.ch_node = F.filter_get_node_id(G.ch->f);
        G.m_node = F.filter_get_node_id(G.mf);
        if (G.ch_node != SPA_ID_INVALID && G.m_node != SPA_ID_INVALID && atomic_load(&g_state) >= 2) break;
        if (atomic_load(&g_state) < 0) break;
        P.tl_timed_wait(C.loop, 1);
    }
    int st = atomic_load(&g_state);
    if (G.ch_node == SPA_ID_INVALID || G.m_node == SPA_ID_INVALID || st < 2) {
        snprintf(err, (size_t)err_size, "%s", st < 0 ? g_stream_err : atomic_load(&C.core_dead) ? "PipeWire disconnected"
                                                                                                : "PipeWire did not accept the filters");
        P.tl_unlock(C.loop);
        pwb_close();
        return -1;
    }
    roundtrip_locked(2.0);                         /* the ports of the two nodes are already in the table */
    int hw = target_inputs();
    if (hw <= 0) hw = sl->s.channels > 0 ? sl->s.channels : 2;
    printf(">>> PipeWire graph: nodes 'Q-LiM' (id %u, %d channels) and 'Q-LiM Master' (id %u) -> %s (%d inputs)\n",
           G.ch_node, nch, G.m_node, S.target_desc, hw);
    if (atomic_load(&I.want)) in_open_locked(rate);     /* 1.4: a channel is armed */
    P.tl_unlock(C.loop);
    if (out_hw_ports) *out_hw_ports = hw;
    return 0;
}

int pwb_graph_hw_ports(void)
{
    if (!pwb_graph_is_open()) return 0;
    P.tl_lock(C.loop);
    int n = target_inputs();
    P.tl_unlock(C.loop);
    return n;
}

static void resize_L(int nch)
{
    if (nch <= G.Lcap) return;
    int nc = G.Lcap ? G.Lcap : 16;
    while (nc < nch) nc *= 2;
    PwbRoute *nl = realloc(G.L, sizeof(PwbRoute) * (size_t)nc);
    if (!nl) return;
    G.L = nl;
    bool *nk = realloc(G.Lk, sizeof(bool) * (size_t)nc);
    if (!nk) return;
    G.Lk = nk;
    for (int i = G.Lcap; i < nc; ++i) { G.L[i] = (PwbRoute){ 0, 0 }; G.Lk[i] = false; }
    G.Lcap = nc;
}

/* recreate the channels node (count/names). Under lock. */
static void gch_rebuild(int nch, const char *const *names)
{
    /* the channels' links to OTHER programs (recording, effects ...) are not in Routing -> carry them over by name */
    if (G.ch && G.ch_node != SPA_ID_INVALID) {
        for (int i = 0; i < T.nl; ++i) {
            GPort *po = t_port(T.l[i].out_port);
            int item, side, bit;
            if (!po || po->node != G.ch_node || (int)po->idx >= G.ch->nports) continue;
            if (map_pair(T.l[i].out_port, T.l[i].in_port, &item, &side, &bit)) continue;
            Carry *nc = grow(G.carry, &G.cap_carry, G.ncarry + 1, sizeof(Carry));
            if (!nc) break;
            G.carry = nc;
            Carry *c = &G.carry[G.ncarry++];
            snprintf(c->name, sizeof c->name, "%s", G.ch->pname[po->idx]);
            c->in_port = T.l[i].in_port;
            c->t = now_ns();
        }
    }
    /* the old node keeps playing until the new one is ready (no interruption) */
    GChF *prev = G.ch;
    uint32_t prev_node = G.ch_node;
    G.old = prev;
    G.old_node = prev_node;
    G.ch = NULL;
    G.ch_node = SPA_ID_INVALID;
    for (int i = 0; i < G.Lcap; ++i) G.Lk[i] = false;
    G.resync = true;
#ifdef PWB_DEBUG
    fprintf(stderr, "PWDBG %.4f create new\n", now_ns() / 1e9);
#endif
    if (gch_create(nch, names)) {
        printf(">>> PipeWire graph: new channels node (%d channels) - switching without a gap\n", nch);
        G.sw_phase = 1;
        G.sw_t = now_ns();
    } else {
        fprintf(stderr, ">>> PipeWire graph: cannot rebuild the channels node\n");
        G.ch = prev; G.ch_node = prev_node; G.old = NULL; G.sw_phase = 0;
    }
}

/* removes the old channels node (and our links from it) */
static void old_finish(void)
{
    if (!G.old) { G.sw_phase = 0; return; }
#ifdef PWB_DEBUG
    fprintf(stderr, "PWDBG %.4f old_finish\n", now_ns() / 1e9);
#endif
    if (atomic_load(&G.render) == G.old) atomic_store(&G.render, G.ch);
    for (int i = 0; i < G.nown; ++i) {
        OwnLink *o = G.own[i];
        GPort *po = o->proxy ? t_port(o->out_port) : NULL;
        if (po && po->node == G.old_node) P.proxy_destroy(o->proxy);
    }
    gch_destroy(G.old);
    G.old = NULL;
    G.old_node = SPA_ID_INVALID;
    G.sw_phase = 0;
}

/* the switch: the new one is fully connected -> it plays; after a few cycles the old one is removed */
static void switch_step(int nch, uint64_t now)
{
    if (!G.old || !G.ch) return;
    if (G.sw_phase == 1) {
        bool ready = G.ch_node != SPA_ID_INVALID && G.ch->state == 3 && G.Lmk && G.ncarry == 0 && !G.resync;
        for (int i = 0; i < nch && ready; ++i) if (!G.Lk[i]) ready = false;
        for (int i = 0; i < G.nown && ready; ++i) {
            OwnLink *o = G.own[i];
            GPort *po = (o->proxy && !o->seen) ? t_port(o->out_port) : NULL;
            if (po && po->node == G.ch_node) ready = false;      /* a link that has not appeared yet */
        }
        if (ready || now - G.sw_t > 2000000000ull) {
#ifdef PWB_DEBUG
            fprintf(stderr, "PWDBG %.4f SWITCH (ready %d)\n", now / 1e9, ready);
#endif
            atomic_store(&G.render, G.ch);
            atomic_store(&g_state, G.ch->state);
            G.sw_phase = 2;
            G.sw_t = now;
            G.sw_cb = atomic_load(&g_callbacks);
            if (!ready) printf(">>> PipeWire graph: new channels node not fully connected after 2 s - switching anyway\n");
        }
    } else if (G.sw_phase == 2) {
        if (atomic_load(&g_callbacks) - G.sw_cb >= 3 || now - G.sw_t > 500000000ull) old_finish();
    }
}

int pwb_graph_sync(int nch, const char *const *names, PwbRoute *routes, PwbRoute *master, bool quiet)
{
    if (!g_connected || !G.active || atomic_load(&C.core_dead)) return -1;
    if (nch < 0) nch = 0;
    int changed = 0;
    uint64_t now = now_ns();
    P.tl_lock(C.loop);
    resize_L(nch);
    if (G.Lcap < nch) { P.tl_unlock(C.loop); return 0; }

    /* 1) channel count: the node is recreated (after a short wait - for a series of additions) */
    if (!G.ch || G.ch->nports != nch * 2) {
        if (G.want_nch != nch) { G.want_nch = nch; G.want_t = now; }
        if (now - G.want_t >= GRAPH_REBUILD_NS) {
            if (G.old) old_finish();                         /* a previous switch is not finished yet */
            gch_rebuild(nch, names);
        }
        P.tl_unlock(C.loop);
        return 0;
    }
    /* renamed channel: in the registry the names do not change (pw-link, qpwgraph and Patchance show the old one),
     * so the node is recreated - but only when nothing is playing (otherwise - a short interruption) */
    (void)quiet;     /* the switch is now without interruption -> it may happen while playing too */
    if (G.ch->renamed && !G.old && now - G.ch->rename_t >= 1000000000ull) {
        gch_rebuild(nch, names);
        P.tl_unlock(C.loop);
        return 0;
    }
    if (G.ch_node == SPA_ID_INVALID) {
        G.ch_node = F.filter_get_node_id(G.ch->f);
        if (G.ch_node == SPA_ID_INVALID) { P.tl_unlock(C.loop); return 0; }
        G.resync = true;
    }

    /* 2) port names */
    for (int i = 0; i < nch; ++i) {
        char base[112], nm[128];
        port_base_name(i, nch, names, base, sizeof base);
        for (int sd = 0; sd < 2; ++sd) {
            snprintf(nm, sizeof nm, "%s %s", base, sd ? "R" : "L");
            if (strcmp(nm, G.ch->pname[2 * i + sd]) == 0) continue;
            snprintf(G.ch->pname[2 * i + sd], sizeof G.ch->pname[0], "%s", nm);
            struct spa_dict_item it[] = { SPA_DICT_ITEM_INIT(PW_KEY_PORT_NAME, G.ch->pname[2 * i + sd]) };
            struct spa_dict d = SPA_DICT_INIT(it, 1);
            F.filter_update_properties(G.ch->f, G.ch->ports[2 * i + sd], &d);
            G.ch->renamed = true;
            G.ch->rename_t = now;
        }
    }

    /* 3) garbage: destroyed links, old "removed by us" ones */
    for (int i = 0; i < G.nown; ) {
        if (!G.own[i]->proxy) { free(G.own[i]); G.own[i] = G.own[--G.nown]; } else ++i;
    }
    for (int i = 0; i < G.npdel; ) {
        if (now - G.pdel[i].t > GRAPH_PDEL_NS) G.pdel[i] = G.pdel[--G.npdel]; else ++i;
    }

    /* 3b) protection: Master Out -> Master In is a feedback loop (howling/full volume) - not allowed */
    if (G.m_node != SPA_ID_INVALID)
        for (int i = 0; i < T.nl; ++i) {
            GPort *po = t_port(T.l[i].out_port), *pi = t_port(T.l[i].in_port);
            if (po && pi && po->node == G.m_node && pi->node == G.m_node && po->dir == 1 && pi->dir == 0 &&
                !t_pdel_has(T.l[i].out_port, T.l[i].in_port)) {
                printf(">>> PipeWire graph: removed a feedback loop (Q-LiM Master Out -> Master In)\n");
                pw_registry_destroy(C.reg, T.l[i].id);
                pdel_add(T.l[i].out_port, T.l[i].in_port);
            }
        }

    /* 4) changes from outside -> the routing (only "mature" ones: the ports still exist after 300 ms;
     *    so pulling out a card/restarting does not erase the routing) */
    bool immature = false;
    for (int i = 0; i < G.ndl; ) {
        Delta dd = G.dl[i];
        if (now - dd.t < GRAPH_DELTA_NS) { immature = true; ++i; continue; }
        G.dl[i] = G.dl[--G.ndl];
        int item, side, bit;
        if (!map_pair(dd.out_port, dd.in_port, &item, &side, &bit)) continue;
        /* whether it is still true (connected -> there is still a link; disconnected -> there is none any more) */
        bool exists = t_link_pair(dd.out_port, dd.in_port) != NULL;
        if (exists != dd.add) continue;
        uint64_t m = RBIT64(bit);
        if (item < 0) {
            uint64_t *v = side ? &master->r : &master->l;
            uint64_t *lv = side ? &G.Lm.r : &G.Lm.l;
            uint64_t nv = dd.add ? (*v | m) : (*v & ~m);
            if (nv != *v) { *v = nv; changed = 1; }
            *lv = dd.add ? (*lv | m) : (*lv & ~m);
            printf(">>> PipeWire graph: master %s -> output %d %s from outside\n", side ? "R" : "L", bit + 1,
                   dd.add ? "connected" : "disconnected");
        } else if (item < nch) {
            uint64_t *v = side ? &routes[item].r : &routes[item].l;
            uint64_t *lv = side ? &G.L[item].r : &G.L[item].l;
            uint64_t nv = dd.add ? (*v | m) : (*v & ~m);
            if (nv != *v) { *v = nv; changed = 1; }
            *lv = dd.add ? (*lv | m) : (*lv & ~m);
            if (bit < 2)
                printf(">>> PipeWire graph: channel %d %s -> Master In %s %s from outside\n", item + 1, side ? "R" : "L",
                       bit ? "R" : "L", dd.add ? "connected" : "disconnected");
            else
                printf(">>> PipeWire graph: channel %d %s -> output %d %s from outside\n", item + 1, side ? "R" : "L",
                       bit - 1, dd.add ? "connected" : "disconnected");
        }
    }

    /* 5) the routing -> the links */
    if (G.resync && immature) { P.tl_unlock(C.loop); return changed; }   /* first let the changes from outside mature */
    int nhw = target_inputs();
    if (nhw > 62) nhw = 62;
    GPort *min_l = t_port_at(G.m_node, 0, 0), *min_r = t_port_at(G.m_node, 0, 1);
    bool all = true;
    for (int i = 0; i < nch; ++i) {
        bool ok_i = true;
        for (int sd = 0; sd < 2; ++sd) {
            uint64_t want = sd ? routes[i].r : routes[i].l;
            uint64_t last = sd ? G.L[i].r : G.L[i].l;
            if (G.Lk[i] && want == last && !G.resync) continue;
            GPort *po = t_port_at(G.ch_node, 1, (uint32_t)(2 * i + sd));
            if (!po || !min_l || !min_r) { all = false; ok_i = false; continue; }
            push_bit(po->id, min_l->id, want & RBIT64(0));
            push_bit(po->id, min_r->id, want & RBIT64(1));
            for (int k = 0; k < nhw; ++k) {
                GPort *pi = t_port_at(G.target_node, 0, (uint32_t)k);
                if (pi) push_bit(po->id, pi->id, want & RBIT64(2 + k));
            }
            if (sd) G.L[i].r = want; else G.L[i].l = want;
        }
        if (ok_i) G.Lk[i] = true;
    }
    for (int sd = 0; sd < 2; ++sd) {
        uint64_t want = sd ? master->r : master->l;
        uint64_t last = sd ? G.Lm.r : G.Lm.l;
        if (G.Lmk && want == last && !G.resync) continue;
        GPort *po = t_port_at(G.m_node, 1, (uint32_t)sd);
        if (!po) { all = false; continue; }
        for (int k = 0; k < nhw; ++k) {
            GPort *pi = t_port_at(G.target_node, 0, (uint32_t)k);
            if (pi) push_bit(po->id, pi->id, want & RBIT64(k));
        }
        if (sd) G.Lm.r = want; else G.Lm.l = want;
    }
    if (all) { G.Lmk = true; G.resync = false; }
    /* the carried-over links to other programs */
    for (int i = 0; i < G.ncarry; ) {
        Carry *c = &G.carry[i];
        bool done = now - c->t > GRAPH_PDEL_NS;
        for (int k = 0; k < G.ch->nports && !done; ++k) {
            if (strcmp(G.ch->pname[k], c->name) != 0) continue;
            GPort *po = t_port_at(G.ch_node, 1, (uint32_t)k);
            if (!po) break;                          /* the port is not visible yet - next time again */
            if (t_port(c->in_port) && !t_link_pair(po->id, c->in_port) && !own_pending(po->id, c->in_port))
                own_create(po->id, c->in_port);
            done = true;
        }
        if (done) G.carry[i] = G.carry[--G.ncarry]; else ++i;
    }
    switch_step(nch, now);
    P.tl_unlock(C.loop);
    return changed;
}
