/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
// Q-LiM — MIDI input (RtMidi).
//
// Messages (CC and Note) come from the RtMidi thread, go into a lock-free queue and are
// dispatched by midi_poll() on the main thread. That way all work with the session happens in one place.

#include "midi.h"
#ifdef _WIN32
#define QW_GETENV(n)    qw_getenv(n)
#define QW_FOPEN(p, m)  qw_fopen((p), (m))
#else
#define QW_GETENV(n)    std::getenv(n)
#define QW_FOPEN(p, m)  std::fopen((p), (m))
#endif
#include "safefile.h"

#if defined(__has_include)
#  if __has_include(<rtmidi/RtMidi.h>)
#    include <rtmidi/RtMidi.h>
#  else
#    include <RtMidi.h>
#  endif
#else
#  include <RtMidi.h>
#endif

#include <atomic>
#include <chrono>
extern "C" {
#include "midiq.h"
void GfxWakeFromThread(void);   /* sdlgfx.c (sdlgfx.h pulls in SDL, not needed here) */
void GfxWakeDone(void);
}
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

namespace {

struct Event {
    unsigned char type;      // 1 = CC, 2 = Note On, 3 = Note Off
    unsigned char channel;
    unsigned char a;         // CC number / note
    unsigned char b;         // value / velocity
};

const unsigned RING_SIZE = 2048;          // power of 2
Event                 g_ring[RING_SIZE];
std::atomic<unsigned> g_head(0);          // written by the RtMidi thread
std::atomic<unsigned> g_tail(0);          // read by the main thread

RtMidiIn   *g_in = nullptr;               // only for listing the ports (never opened)
bool        g_learn = false;

// 1.4.6: every open input. The RtMidi threads of the inputs write the queues one at a time (g_push_lock): the
// queues have one writer each. Uses can change while it plays -> atomic.
struct Port {
    RtMidiIn        *in = nullptr;
    std::string      name;                    // the full name when it was opened
    std::string      base;                    // the device (without the numbers the system adds)
    std::atomic<int> uses{ MIDI_USE_PLAY | MIDI_USE_CONTROL };
};
std::vector<Port *> g_open;
// remembered (midi.conf) but not plugged in now: base name + uses
struct Want { std::string base; int uses; };
std::vector<Want> g_missing;
std::atomic_flag  g_push_lock = ATOMIC_FLAG_INIT;
unsigned          g_last_check = 0;           // midi_poll: when the ports were looked at the last time (ms)

MidiCallback      g_cc_cb = nullptr;
void             *g_cc_ud = nullptr;
MidiNoteCallback  g_note_cb = nullptr;
void             *g_note_ud = nullptr;

void push_event(unsigned char type, unsigned char ch, unsigned char a, unsigned char b)
{
    unsigned h = g_head.load(std::memory_order_relaxed);
    unsigned t = g_tail.load(std::memory_order_acquire);
    if (h - t >= RING_SIZE) return;                 // full: drop the newest
    g_ring[h & (RING_SIZE - 1)] = Event{ type, ch, a, b };
    g_head.store(h + 1, std::memory_order_release);
    GfxWakeFromThread();                            // 1.4.2: the main loop handles it now, not at the idle rate
}

void rt_callback(double, std::vector<unsigned char> *msg, void *ud)
{
    if (!msg || msg->empty()) return;
    Port *p = static_cast<Port *>(ud);
    int uses = p ? p->uses.load(std::memory_order_relaxed) : (MIDI_USE_PLAY | MIDI_USE_CONTROL);
    unsigned char s0 = (*msg)[0];
    unsigned char m1 = msg->size() > 1 ? (*msg)[1] : 0, m2 = msg->size() > 2 ? (*msg)[2] : 0;
    while (g_push_lock.test_and_set(std::memory_order_acquire)) { /* another input is writing (a few ns) */ }
    /* 1.2: every channel message also goes to the instruments (the audio thread gives it to the armed channel).
     * 1.4.6: an input only for playing ("Play" without "Control") is never stopped by the pad bindings. */
    if ((uses & MIDI_USE_PLAY) && s0 >= 0x80 && s0 < 0xF0) {
        if (uses & MIDI_USE_CONTROL) midiq_push(MIDIQ_HW, s0, m1, m2);
        else                         midiq_push_free(MIDIQ_HW, s0, m1, m2);
    }
    if ((uses & MIDI_USE_CONTROL) && msg->size() >= 3) {
        unsigned char kind = s0 & 0xF0, ch = s0 & 0x0F, d1 = m1 & 0x7F, d2 = m2 & 0x7F;
        if (kind == 0xB0)      push_event(1, ch, d1, d2);
        else if (kind == 0x90) push_event(d2 > 0 ? 2 : 3, ch, d1, d2);
        else if (kind == 0x80) push_event(3, ch, d1, d2);
    }
    g_push_lock.clear(std::memory_order_release);
}

void drain_ring()
{
    GfxWakeDone();
    for (;;) {
        unsigned t = g_tail.load(std::memory_order_relaxed);
        unsigned h = g_head.load(std::memory_order_acquire);
        if (t == h) break;
        Event e = g_ring[t & (RING_SIZE - 1)];
        g_tail.store(t + 1, std::memory_order_release);
        if (e.type == 1) {
            if (g_cc_cb) g_cc_cb(e.a, e.b, e.channel, g_cc_ud);
        } else if (e.type == 2) {
            if (g_note_cb) g_note_cb(e.a, e.b, true, e.channel, g_note_ud);
        } else if (e.type == 3) {
            if (g_note_cb) g_note_cb(e.a, e.b, false, e.channel, g_note_ud);
        }
    }
}

std::string config_dir()
{
    const char *xdg = QW_GETENV("XDG_CONFIG_HOME");
    const char *home = QW_GETENV("HOME");
    if (xdg && *xdg) return std::string(xdg) + "/qlim";
    return std::string(home ? home : ".") + "/.config/qlim";
}

bool is_digits(const std::string &s, size_t a, size_t b)
{
    if (a >= b) return false;
    for (size_t i = a; i < b; ++i) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

// The device without the numbers the system adds, which change when it is plugged in again or another device is added:
// ALSA "nanoKONTROL Studio:nanoKONTROL Studio _ CTRL 24:0" -> "... CTRL", Windows "nanoKONTROL Studio 1" -> "nanoKONTROL Studio".
std::string base_name(const std::string &full)
{
    std::string s = full;
    size_t sp = s.find_last_of(' ');
    if (sp != std::string::npos) {
        size_t colon = s.find(':', sp);
        if (colon != std::string::npos && is_digits(s, sp + 1, colon) && is_digits(s, colon + 1, s.size())) s.erase(sp);
#ifdef _WIN32
        else if (is_digits(s, sp + 1, s.size())) s.erase(sp);
#endif
    }
    return s;
}

std::vector<std::string> list_ports()
{
    std::vector<std::string> v;
    if (!g_in) return v;
    try {
        unsigned n = g_in->getPortCount();
        for (unsigned i = 0; i < n; ++i) {
            try { v.push_back(g_in->getPortName(i)); } catch (RtMidiError &) { v.push_back("?"); }
        }
    } catch (RtMidiError &) {}
    return v;
}

int find_open(const std::string &full)
{
    for (size_t i = 0; i < g_open.size(); ++i) if (g_open[i]->name == full) return (int)i;
    return -1;
}

void close_one(size_t i)
{
    Port *p = g_open[i];
    try { if (p->in->isPortOpen()) p->in->closePort(); } catch (RtMidiError &) {}
    delete p->in;                                   // its thread has ended: the callback does not run any more
    delete p;
    g_open.erase(g_open.begin() + (long)i);
}

int open_full(int index, const std::string &full, int uses)
{
    if (find_open(full) >= 0) return 0;
    if ((int)g_open.size() >= MIDI_MAX_OPEN) { std::fprintf(stderr, "MIDI: at most %d inputs\n", MIDI_MAX_OPEN); return 1; }
    Port *p = new Port();
    p->name = full;
    p->base = base_name(full);
    p->uses.store(uses ? uses : (MIDI_USE_PLAY | MIDI_USE_CONTROL));
    try {
        p->in = new RtMidiIn(RtMidi::UNSPECIFIED, "Q-LiM");
        p->in->ignoreTypes(true, true, true);       // no sysex, timing clock, active sensing
        p->in->setCallback(&rt_callback, p);
        p->in->openPort((unsigned)index, "Q-LiM input");
    } catch (RtMidiError &e) {
        std::fprintf(stderr, "MIDI open failed (%s): %s\n", full.c_str(), e.getMessage().c_str());
        delete p->in;
        delete p;
        return 1;
    }
    g_open.push_back(p);
    for (size_t k = 0; k < g_missing.size(); ++k)    // it is here now
        if (g_missing[k].base == p->base) { g_missing.erase(g_missing.begin() + (long)k); break; }
    std::printf(">>> MIDI: opened [%d] %s\n", index, full.c_str());
    return 0;
}

// remembered devices -> open the ones that are here; open ones that went away -> remembered (they come back by themselves)
bool reconcile()
{
    bool changed = false;
    std::vector<std::string> ports = list_ports();
    for (size_t i = g_open.size(); i-- > 0;) {
        bool here = false;
        for (auto &n : ports) if (n == g_open[i]->name) { here = true; break; }
        if (!here) {
            std::printf(">>> MIDI: %s went away\n", g_open[i]->name.c_str());
            g_missing.push_back(Want{ g_open[i]->base, g_open[i]->uses.load() });
            close_one(i);
            changed = true;
        }
    }
    for (size_t k = 0; k < g_missing.size();) {
        int found = -1;
        for (size_t i = 0; i < ports.size(); ++i)
            if (base_name(ports[i]) == g_missing[k].base && find_open(ports[i]) < 0) { found = (int)i; break; }
        if (found >= 0 && open_full(found, ports[(size_t)found], g_missing[k].uses) == 0) { changed = true; continue; }   // (erased by open_full)
        ++k;
    }
    return changed;
}

} // namespace

extern "C" {

int midi_init(void)
{
    if (g_in) return 0;
    try {
        g_in = new RtMidiIn(RtMidi::UNSPECIFIED, "Q-LiM ports");
    } catch (RtMidiError &e) {
        std::fprintf(stderr, "MIDI init failed: %s\n", e.getMessage().c_str());
        delete g_in;
        g_in = nullptr;
        return 1;
    }
    return 0;
}

void midi_shutdown(void)
{
    while (!g_open.empty()) close_one(g_open.size() - 1);
    delete g_in;
    g_in = nullptr;
}

int midi_get_port_count(void)
{
    if (!g_in) return 0;
    try { return (int)g_in->getPortCount(); } catch (RtMidiError &) { return 0; }
}

const char *midi_get_port_name(int index)
{
    static std::string name;
    if (!g_in || index < 0) return "?";
    try {
        if (index >= (int)g_in->getPortCount()) return "?";
        name = g_in->getPortName((unsigned)index);
        return name.c_str();
    } catch (RtMidiError &) { return "?"; }
}

int midi_open_port(int index)
{
    if (!g_in || index < 0) return 1;
    std::string full;
    try {
        if (index >= (int)g_in->getPortCount()) return 1;
        full = g_in->getPortName((unsigned)index);
    } catch (RtMidiError &) { return 1; }
    return open_full(index, full, MIDI_USE_PLAY | MIDI_USE_CONTROL);
}

int midi_open_port_by_name(const char *name)
{
    if (!g_in || !name || !*name) return 1;
    std::vector<std::string> ports = list_ports();
    std::string want = base_name(name);
    for (size_t i = 0; i < ports.size(); ++i)                       // the same device
        if (base_name(ports[i]) == want && find_open(ports[i]) < 0) return open_full((int)i, ports[i], 0);
    for (size_t i = 0; i < ports.size(); ++i)                       // a part of the name
        if (ports[i].find(name) != std::string::npos) return open_full((int)i, ports[i], 0);
    return 1;
}

void midi_close_port_index(int index)
{
    int k = find_open(midi_get_port_name(index));
    if (k >= 0) close_one((size_t)k);
}

void midi_close_port(void)
{
    while (!g_open.empty()) close_one(g_open.size() - 1);
}

bool midi_port_is_open(int index) { return index >= 0 && find_open(midi_get_port_name(index)) >= 0; }

int midi_port_uses(int index)
{
    int k = index >= 0 ? find_open(midi_get_port_name(index)) : -1;
    return k >= 0 ? g_open[(size_t)k]->uses.load() : 0;
}

void midi_set_port_uses(int index, int uses)
{
    int k = index >= 0 ? find_open(midi_get_port_name(index)) : -1;
    if (k < 0) return;
    g_open[(size_t)k]->uses.store(uses & (MIDI_USE_PLAY | MIDI_USE_CONTROL));
    midi_save_config();
}

int midi_open_count(void) { return (int)g_open.size(); }

int         midi_missing_count(void) { return (int)g_missing.size(); }
const char *midi_missing_name(int i) { return i >= 0 && i < (int)g_missing.size() ? g_missing[(size_t)i].base.c_str() : "?"; }
void        midi_forget_missing(int i)
{
    if (i < 0 || i >= (int)g_missing.size()) return;
    g_missing.erase(g_missing.begin() + i);
    midi_save_config();
}

void midi_refresh(void)
{
    if (!g_in) midi_init();
    if (reconcile()) midi_save_config();
}

const char *midi_get_current_port_name(void) { return g_open.empty() ? "" : g_open[0]->name.c_str(); }

int midi_get_current_port_index(void)
{
    if (g_open.empty()) return -1;
    std::vector<std::string> ports = list_ports();
    for (size_t i = 0; i < ports.size(); ++i) if (ports[i] == g_open[0]->name) return (int)i;
    return -1;
}

bool midi_is_open(void) { return !g_open.empty(); }

void midi_set_callback(MidiCallback cb, void *user_data) { g_cc_cb = cb; g_cc_ud = user_data; }
void midi_set_note_callback(MidiNoteCallback cb, void *user_data) { g_note_cb = cb; g_note_ud = user_data; }

void midi_set_learn_mode(bool on) { g_learn = on; }
bool midi_get_learn_mode(void) { return g_learn; }

void midi_get_config_path(char *out, size_t size)
{
    if (!out || size == 0) return;
    std::snprintf(out, size, "%s/midi.conf", config_dir().c_str());
}

void midi_save_config(void)
{
    std::string dir = config_dir();
    // ~/.config/qlim (the parent ~/.config usually exists)
    ::mkdir(dir.substr(0, dir.find_last_of('/')).c_str(), 0755);
    ::mkdir(dir.c_str(), 0755);
    char path[1024];
    midi_get_config_path(path, sizeof(path));
    char tmp[1100];
    FILE *f = safe_open(path, tmp, sizeof tmp);
    if (!f) return;
    // 1.4.6: one line per input: in=<uses>:<device>   (uses: 1 = play, 2 = control, 3 = both)
    for (auto *p : g_open) std::fprintf(f, "in=%d:%s\n", p->uses.load(), p->base.c_str());
    for (auto &w : g_missing) std::fprintf(f, "in=%d:%s\n", w.uses, w.base.c_str());
    safe_commit(f, tmp, path, 0);
}

int midi_load_config(void)
{
    char path[1024];
    midi_get_config_path(path, sizeof(path));
    FILE *f = QW_FOPEN(path, "r");
    if (!f) return 1;
    char line[512];
    g_missing.clear();
    while (std::fgets(line, sizeof(line), f)) {
        size_t n = std::strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        int uses = MIDI_USE_PLAY | MIDI_USE_CONTROL;
        const char *name = nullptr;
        if (std::strncmp(line, "port=", 5) == 0) name = line + 5;                 // up to 1.4.5: one input
        else if (std::strncmp(line, "in=", 3) == 0 && line[3] >= '0' && line[3] <= '3' && line[4] == ':') {
            uses = line[3] - '0';
            name = line + 5;
        }
        if (!name || !*name || g_missing.size() >= MIDI_MAX_OPEN) continue;
        g_missing.push_back(Want{ base_name(name), uses });
    }
    std::fclose(f);
    if (g_missing.empty()) return 1;
    reconcile();
    return g_open.empty() ? 1 : 0;
}

void midi_poll(void)
{
    drain_ring();
    /* 1.4.6: every 2 s look whether a remembered device was plugged in or an open one was pulled out */
    unsigned now = (unsigned)std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()).count();
    if (g_in && now - g_last_check >= 2000) {
        g_last_check = now;
        if (!g_missing.empty() || !g_open.empty()) reconcile();
    }
}

} // extern "C"
