/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.4.6: several MIDI inputs at once (src/midi.cpp with the fake RtMidi in tests/fake_rtmidi):
 * open / close, Play / Control, midi.conf (also the old one-port file), devices plugged in / pulled out. */
#include "midi.h"
extern "C" {
#include "midiq.h"
void GfxWakeFromThread(void) {}
void GfxWakeDone(void) {}
}
#include <rtmidi/RtMidi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <sys/stat.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static int g_cc = 0, g_notes = 0;
static void on_cc(int, int, int, void *) { ++g_cc; }
static void on_note(int, int, bool, int, void *) { ++g_notes; }

static int pop_n(int *free_n)
{
    unsigned char out[64][4];
    int n = midiq_pop(out, 64), f = 0;
    for (int i = 0; i < n; ++i) if (out[i][3] & 0x80) ++f;
    if (free_n) *free_n = f;
    return n;
}

static int index_of(const char *full)
{
    for (int i = 0; i < midi_get_port_count(); ++i) if (!std::strcmp(midi_get_port_name(i), full)) return i;
    return -1;
}

static std::string read_conf()
{
    char p[1024]; midi_get_config_path(p, sizeof p);
    FILE *f = std::fopen(p, "r");
    std::string s;
    if (!f) return s;
    char b[512];
    while (std::fgets(b, sizeof b, f)) s += b;
    std::fclose(f);
    return s;
}

int main()
{
    char dir[] = "/tmp/qlim_midiports_XXXXXX";
    if (!mkdtemp(dir)) return 1;
    setenv("XDG_CONFIG_HOME", dir, 1);
    std::string qd = std::string(dir) + "/qlim";
    mkdir(qd.c_str(), 0755);

    const char *KEYS = "Keystation 49:Keystation 49 MIDI 1 20:0";
    const char *KORG = "nanoKONTROL Studio:nanoKONTROL Studio _ CTRL 24:0";
    const char *CARD = "UMC404HD:UMC404HD MIDI 1 28:0";
    fake::ports() = { "Midi Through:Midi Through Port-0 14:0", KEYS, KORG, CARD };

    /* the old file (up to 1.4.5): one port, with the ALSA numbers */
    { FILE *f = std::fopen((qd + "/midi.conf").c_str(), "w"); std::fprintf(f, "port=%s\n", KORG); std::fclose(f); }
    CHECK(midi_init() == 0);
    midi_set_callback(on_cc, nullptr);
    midi_set_note_callback(on_note, nullptr);
    CHECK(midi_load_config() == 0);
    CHECK(midi_open_count() == 1);
    CHECK(midi_port_is_open(index_of(KORG)));
    CHECK(midi_port_uses(index_of(KORG)) == (MIDI_USE_PLAY | MIDI_USE_CONTROL));

    /* three at once */
    CHECK(midi_open_port(index_of(KEYS)) == 0);
    CHECK(midi_open_port(index_of(CARD)) == 0);
    CHECK(midi_open_port(index_of(CARD)) == 0);             /* again: still one */
    CHECK(midi_open_count() == 3);
    CHECK(RtMidiIn::open_count() == 3);

    /* every device reaches the program */
    g_cc = g_notes = 0;
    RtMidiIn::send(KORG, { 0xB0, 86, 5 });
    RtMidiIn::send(KEYS, { 0x90, 60, 100 });
    RtMidiIn::send(CARD, { 0x90, 62, 90 });
    midi_poll();
    CHECK(g_cc == 1 && g_notes == 2);
    int fr = -1;
    CHECK(pop_n(&fr) == 3 && fr == 0);                        /* and the instruments */

    /* the keyboard only for playing: not to the program, its notes "free" (never held back by pad bindings) */
    midi_set_port_uses(index_of(KEYS), MIDI_USE_PLAY);
    /* the controller only for control: not to the instruments */
    midi_set_port_uses(index_of(KORG), MIDI_USE_CONTROL);
    g_cc = g_notes = 0;
    RtMidiIn::send(KEYS, { 0x90, 60, 100 });
    RtMidiIn::send(KORG, { 0x90, 41, 127 });
    midi_poll();
    CHECK(g_notes == 1);                                       /* only the controller's */
    CHECK(pop_n(&fr) == 1 && fr == 1);                         /* only the keyboard's, free */

    /* saved: in=<uses>:<device without numbers> */
    std::string c = read_conf();
    CHECK(c.find("in=2:nanoKONTROL Studio:nanoKONTROL Studio _ CTRL\n") != std::string::npos);
    CHECK(c.find("in=1:Keystation 49:Keystation 49 MIDI 1\n") != std::string::npos);
    CHECK(c.find("in=3:UMC404HD:UMC404HD MIDI 1\n") != std::string::npos);

    /* the controller is pulled out ... */
    fake::ports() = { "Midi Through:Midi Through Port-0 14:0", KEYS, CARD };
    midi_refresh();
    CHECK(midi_open_count() == 2);
    CHECK(midi_missing_count() == 1);
    CHECK(std::string(midi_missing_name(0)) == "nanoKONTROL Studio:nanoKONTROL Studio _ CTRL");
    CHECK(read_conf().find("in=2:nanoKONTROL Studio") != std::string::npos);   /* still remembered */
    /* ... and plugged in again, with other ALSA numbers: connects by itself, with its uses */
    const char *KORG2 = "nanoKONTROL Studio:nanoKONTROL Studio _ CTRL 32:0";
    fake::ports() = { "Midi Through:Midi Through Port-0 14:0", KEYS, CARD, KORG2 };
    midi_refresh();
    CHECK(midi_open_count() == 3 && midi_missing_count() == 0);
    CHECK(midi_port_uses(index_of(KORG2)) == MIDI_USE_CONTROL);

    /* the next start: everything comes back */
    midi_shutdown();
    CHECK(RtMidiIn::open_count() == 0);
    CHECK(midi_init() == 0);
    CHECK(midi_load_config() == 0);
    CHECK(midi_open_count() == 3);
    CHECK(midi_port_uses(index_of(KEYS)) == MIDI_USE_PLAY);

    /* closing one keeps the others; closing = it is not remembered */
    midi_close_port_index(index_of(CARD));
    midi_save_config();
    CHECK(midi_open_count() == 2 && !midi_port_is_open(index_of(CARD)));
    CHECK(read_conf().find("UMC404HD") == std::string::npos);

    /* two identical devices: both can be open, and both come back */
    const char *K_A = "nanoKEY2:nanoKEY2 MIDI 1 36:0", *K_B = "nanoKEY2:nanoKEY2 MIDI 1 40:0";
    fake::ports() = { KEYS, KORG2, K_A, K_B };
    CHECK(midi_open_port(index_of(K_A)) == 0 && midi_open_port(index_of(K_B)) == 0);
    CHECK(midi_open_count() == 4);
    midi_save_config();
    midi_shutdown();
    midi_init();
    CHECK(midi_load_config() == 0);
    CHECK(midi_open_count() == 4);

    /* four devices at once, each from its own thread (like RtMidi): nothing lost, nothing mixed up */
    {
        midi_close_port();
        const char *D[4] = { "A:A 1 50:0", "B:B 1 51:0", "C:C 1 52:0", "D:D 1 53:0" };
        fake::ports() = { D[0], D[1], D[2], D[3] };
        for (int i = 0; i < 4; ++i) midi_open_port(i);
        midi_poll(); pop_n(nullptr);
        g_cc = g_notes = 0;
        int got = 0;
        std::thread th[4];
        for (int t = 0; t < 4; ++t)
            th[t] = std::thread([t, &D]() { for (int k = 0; k < 200; ++k) RtMidiIn::send(D[t], { (unsigned char)(0xB0 | t), (unsigned char)t, (unsigned char)(k & 127) }); });
        for (int k = 0; k < 400; ++k) { midi_poll(); got += pop_n(nullptr); }   /* the main thread and the audio thread read meanwhile */
        for (auto &x : th) x.join();
        midi_poll();
        for (int r; (r = pop_n(nullptr)) > 0;) got += r;
        CHECK(g_cc == 800);
        CHECK(got == 800);
    }

    /* the limit */
    midi_close_port();
    std::vector<std::string> many;
    for (int i = 0; i < MIDI_MAX_OPEN + 3; ++i) many.push_back("Dev" + std::to_string(i) + ":Port " + std::to_string(100 + i) + ":0");
    fake::ports() = many;
    int ok = 0;
    for (int i = 0; i < (int)many.size(); ++i) ok += midi_open_port(i) == 0;
    CHECK(ok == MIDI_MAX_OPEN && midi_open_count() == MIDI_MAX_OPEN);
    midi_shutdown();

    /* nothing saved -> 1 */
    std::remove((qd + "/midi.conf").c_str());
    midi_init();
    CHECK(midi_load_config() != 0);
    midi_shutdown();

    std::printf(g_fail ? "test_midiports: %d FAILED\n" : "test_midiports: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
