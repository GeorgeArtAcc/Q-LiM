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

RtMidiIn   *g_in = nullptr;
int         g_port_index = -1;
std::string g_port_name;
bool        g_learn = false;

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
}

void rt_callback(double, std::vector<unsigned char> *msg, void *)
{
    if (!msg || msg->size() < 3) return;
    unsigned char st = (*msg)[0];
    unsigned char kind = st & 0xF0;
    unsigned char ch = st & 0x0F;
    unsigned char d1 = (*msg)[1] & 0x7F;
    unsigned char d2 = (*msg)[2] & 0x7F;
    if (kind == 0xB0)      push_event(1, ch, d1, d2);
    else if (kind == 0x90) push_event(d2 > 0 ? 2 : 3, ch, d1, d2);
    else if (kind == 0x80) push_event(3, ch, d1, d2);
}

void drain_ring()
{
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

} // namespace

extern "C" {

int midi_init(void)
{
    if (g_in) return 0;
    try {
        g_in = new RtMidiIn(RtMidi::UNSPECIFIED, "Q-LiM");
        g_in->ignoreTypes(true, true, true);        // no sysex, timing clock, active sensing
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
    if (!g_in) return;
    try { if (g_in->isPortOpen()) g_in->closePort(); } catch (RtMidiError &) {}
    delete g_in;
    g_in = nullptr;
    g_port_index = -1;
    g_port_name.clear();
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
    if (!g_in) return 1;
    try {
        if (index < 0 || index >= (int)g_in->getPortCount()) return 1;
        if (g_in->isPortOpen()) g_in->closePort();
        g_in->openPort((unsigned)index, "Q-LiM input");
        g_in->setCallback(&rt_callback, nullptr);
        g_port_index = index;
        g_port_name = g_in->getPortName((unsigned)index);
        std::printf(">>> MIDI: opened [%d] %s\n", index, g_port_name.c_str());
        return 0;
    } catch (RtMidiError &e) {
        std::fprintf(stderr, "MIDI open failed: %s\n", e.getMessage().c_str());
        g_port_index = -1;
        g_port_name.clear();
        return 1;
    }
}

int midi_open_port_by_name(const char *name)
{
    if (!g_in || !name || !*name) return 1;
    int n = midi_get_port_count();
    for (int i = 0; i < n; ++i) {
        std::string pn;
        try { pn = g_in->getPortName((unsigned)i); } catch (RtMidiError &) { continue; }
        if (pn.find(name) != std::string::npos) return midi_open_port(i);
    }
    return 1;
}

void midi_close_port(void)
{
    if (!g_in) return;
    try { if (g_in->isPortOpen()) g_in->closePort(); } catch (RtMidiError &) {}
    g_port_index = -1;
    g_port_name.clear();
}

const char *midi_get_current_port_name(void) { return g_port_name.c_str(); }
int midi_get_current_port_index(void) { return g_port_index; }

bool midi_is_open(void)
{
    if (!g_in) return false;
    try { return g_in->isPortOpen(); } catch (RtMidiError &) { return false; }
}

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
    if (!g_port_name.empty()) std::fprintf(f, "port=%s\n", g_port_name.c_str());
    safe_commit(f, tmp, path, 0);
}

int midi_load_config(void)
{
    char path[1024];
    midi_get_config_path(path, sizeof(path));
    FILE *f = QW_FOPEN(path, "r");
    if (!f) return 1;
    char line[512];
    line[0] = '\0';
    if (!std::fgets(line, sizeof(line), f)) { std::fclose(f); return 1; }
    std::fclose(f);
    size_t n = std::strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    const char *name = line;
    if (std::strncmp(line, "port=", 5) == 0) name = line + 5;
    if (!*name) return 1;
    return midi_open_port_by_name(name);
}

void midi_poll(void) { drain_ring(); }

} // extern "C"
