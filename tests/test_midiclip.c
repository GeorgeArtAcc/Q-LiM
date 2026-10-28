/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.3: MIDI clips - the notes sound exactly on their beats (master tempo), the clip loops, a quantized launch starts on
 * the bar, stop leaves no hanging note, an edit while playing is heard, a clip without loop ends by itself, the notes
 * survive project save + load and undo. Instrument: DISTRHO Kars (CLAP). Argument: the folder with Kars.clap */
#define _POSIX_C_SOURCE 200809L
#include "../src/session.h"
#include "../src/mixer.h"
#include "../src/fxchain.h"
#include "../src/project.h"
#include "../src/undo.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void nap(long us) { struct timespec t = { us / 1000000, (us % 1000000) * 1000 }; nanosleep(&t, NULL); }
#define SR 48000
#define BLK 256
static float *tape; static size_t tape_len, tape_cap;

static void run_to(Session *s, double sec)
{
    size_t end = (size_t)(sec * SR);
    float buf[BLK * 2];
    while (tape_len < end) {
        mixer_render(s, buf, BLK, 2);
        if (tape_len + BLK > tape_cap) { tape_cap = (tape_cap + BLK) * 2; tape = realloc(tape, tape_cap * sizeof(float)); }
        for (int i = 0; i < BLK; ++i) tape[tape_len + i] = buf[i * 2];
        tape_len += BLK;
        session_quant_tick(s); session_midi_tick(s);
    }
}
static double now_s(void) { return tape_len / (double)SR; }
/* the starts of the notes: silence (< thr for 200 samples) then sound */
static int onsets(size_t from, size_t to, size_t *out, int max)
{
    int n = 0; size_t quiet = 1000;
    for (size_t i = from; i < to && i < tape_len; ++i) {
        if (fabsf(tape[i]) > 1e-3f) { if (quiet >= 200 && n < max) out[n++] = i; quiet = 0; }
        else ++quiet;
    }
    return n;
}
static MidiSeq *seq4(float len, const float *starts, int n, int key)
{
    MidiSeq *m = midiseq_new(n, len);
    for (int i = 0; i < n; ++i) { m->note[i].start = starts[i]; m->note[i].len = 0.25f; m->note[i].key = (unsigned char)key; m->note[i].vel = 110; }
    return m;
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: test_midiclip <folder with Kars.clap>\n"); return 2; }
    char kars[1200];
    snprintf(kars, sizeof kars, "%s/Kars.clap|studio.kx.distrho.Kars", argv[1]);
    static Session S; Session *s = &S;
    session_init(s, SR);
    streams_init();
    session_add_channel(s, NULL); session_add_channel(s, NULL);
    for (int i = 0; i < 4; ++i) session_add_scene(s);
    atomic_store(&s->audio_running, false);
    session_fx_add(s, 0, "CLAP", "Kars", kars);
    for (int i = 0; i < 400 && !fxchain_has_instrument(s, 0); ++i) { fxchain_sync(s); nap(5000); }
    CHECK(fxchain_has_instrument(s, 0), "Kars on channel 1");
    /* release short, sustain off (the default) */

    /* 1) a 4-beat clip with notes on every beat, 120 BPM -> starts every 24000 samples, then it loops */
    CHECK(session_new_midi_clip(s, 0, 0, 4.0f, "Beat") == 0 && session_is_midi(s, 0, 0), "new MIDI clip");
    const float st[4] = { 0, 1, 2, 3 };
    session_midi_set(s, 0, 0, seq4(4.0f, st, 4, 60));
    CHECK(session_midi_get(s, 0, 0)->n == 4, "4 notes in it");
    session_toggle_clip(s, 0, 0);
    run_to(s, 4.3);
    size_t on[16];
    int k = onsets(0, tape_len, on, 16);
    bool exact = k >= 9;
    for (int i = 0; i < k && i < 9; ++i) if (labs((long)on[i] - (long)(on[0] + (size_t)i * 24000)) > 2) exact = false;
    CHECK(exact, "notes every beat, exactly (%d starts: %zu %zu %zu %zu ... 9th %zu)", k, on[0], k > 1 ? on[1] : 0, k > 2 ? on[2] : 0,
          k > 3 ? on[3] : 0, k > 8 ? on[8] : 0);
    CHECK(on[0] < 64, "the first note at the start (%zu)", on[0]);
    double ph = session_midi_playhead(s, 0, 0);
    CHECK(ph >= 0.0 && ph < 4.0, "playhead %.2f beats (loops in 0..4)", ph);

    /* 2) the master tempo moves the notes: 150 BPM -> every 19200 samples */
    session_set_tempo(s, 150.0f);
    size_t mark = tape_len;
    run_to(s, now_s() + 2.0);
    k = onsets(mark + 2000, tape_len, on, 16);
    bool t150 = k >= 3;
    for (int i = 1; i < k; ++i) if (labs((long)(on[i] - on[i - 1]) - 19200) > 2) t150 = false;
    CHECK(t150, "at 150 BPM a note every 19200 samples (%d starts, gap %ld)", k, k > 1 ? (long)(on[1] - on[0]) : 0L);
    session_set_tempo(s, 120.0f);

    /* 3) stop: silence, no hanging note */
    session_stop_all(s);
    run_to(s, now_s() + 1.0);
    mark = tape_len;
    run_to(s, now_s() + 0.5);
    float mx = 0; for (size_t i = mark; i < tape_len; ++i) if (fabsf(tape[i]) > mx) mx = fabsf(tape[i]);
    CHECK(mx < 1e-3f, "stopped: silence (%.5f)", mx);

    /* 4) a long note stopped in the middle ends at once */
    const float st1[1] = { 0 };
    MidiSeq *lm = seq4(4.0f, st1, 1, 64); lm->note[0].len = 4.0f;
    session_midi_set(s, 0, 0, lm);
    session_toggle_clip(s, 0, 0);
    run_to(s, now_s() + 0.5);
    session_stop_all(s);
    run_to(s, now_s() + 1.2);
    mark = tape_len; run_to(s, now_s() + 0.3);
    mx = 0; for (size_t i = mark; i < tape_len; ++i) if (fabsf(tape[i]) > mx) mx = fabsf(tape[i]);
    CHECK(mx < 1e-3f, "a long note ends when the clip stops (%.5f)", mx);

    /* 5) quantized launch (1 bar): clip A plays, clip B (channel 2? no - the same channel, scene 2) pressed mid-bar */
    session_midi_set(s, 0, 0, seq4(4.0f, st, 4, 60));
    CHECK(session_new_midi_clip(s, 0, 1, 4.0f, "B") == 0, "clip B");
    const float stb[2] = { 0, 2 };
    session_midi_set(s, 0, 1, seq4(4.0f, stb, 2, 67));
    session_set_quantize(s, 4.0f);
    mark = tape_len;
    session_toggle_clip(s, 0, 0);                /* nothing plays -> at once */
    run_to(s, now_s() + 0.7);                    /* 1.4 beats in */
    session_toggle_clip(s, 0, 1);
    CHECK(session_clip_launch_state(s, 0, 1) == 1, "B waits for the bar");
    run_to(s, now_s() + 1.8);
    k = onsets(mark, tape_len, on, 16);
    /* A: 0, 24000 (beat 1)... at beat 4 (96000) B starts; A's beats 2,3 at 48000, 72000 */
    bool qok = k >= 5 && labs((long)(on[4] - on[0]) - 96000) <= 2;
    CHECK(qok, "B starts exactly on the next bar (%ld samples after A, want 96000)", k >= 5 ? (long)(on[4] - on[0]) : -1L);
    CHECK(session_clip_launch_state(s, 0, 1) == 0 && atomic_load(&s->channels[0].clips[1].playing) &&
          !atomic_load(&s->channels[0].clips[0].playing), "then B plays, A stopped");

    /* 6) an edit while playing is heard in the next loop */
    const float ste[1] = { 1 };
    session_midi_set(s, 0, 1, seq4(4.0f, ste, 1, 72));   /* only one note, on beat 1 */
    for (int i = 0; i < 20; ++i) { run_to(s, now_s() + 0.01); }
    mark = tape_len;
    run_to(s, now_s() + 2.0);                    /* one full loop */
    k = onsets(mark, tape_len, on, 16);
    CHECK(k == 1, "after the edit: one note per loop (%d)", k);

    /* 7) without loop: plays once and stops by itself */
    session_stop_all(s);
    run_to(s, now_s() + 0.5);
    atomic_store(&s->channels[0].clips[0].loop, false);
    session_toggle_clip(s, 0, 0);
    run_to(s, now_s() + 2.3);
    CHECK(!atomic_load(&s->channels[0].clips[0].playing) && atomic_load(&s->channels[0].clips[0].ended), "no loop: ends by itself after 4 beats");
    atomic_store(&s->channels[0].clips[0].loop, true);

    /* 8) many notes (a full block of them) - no overflow */
    MidiSeq *dense = midiseq_new(400, 1.0f);
    for (int i = 0; i < 400; ++i) { dense->note[i].start = (float)(i % 100) * 0.01f; dense->note[i].len = 0.005f; dense->note[i].key = (unsigned char)(30 + i % 60); dense->note[i].vel = 90; }
    session_midi_set(s, 0, 2, NULL);             /* (not a MIDI cell: ignored) */
    CHECK(session_new_midi_clip(s, 0, 2, 1.0f, "Dense") == 0, "dense clip");
    session_midi_set(s, 0, 2, dense);
    session_toggle_clip(s, 0, 2);
    run_to(s, now_s() + 1.0);
    mx = 0; for (size_t i = tape_len - SR; i < tape_len; ++i) if (!isfinite(tape[i]) || fabsf(tape[i]) > mx) mx = fabsf(tape[i]);
    CHECK(isfinite(mx) && mx < 8.0f, "400 notes in one beat: no overflow (peak %.2f)", mx);
    session_stop_all(s);
    run_to(s, now_s() + 1.0);

    /* 8b) a .mid file (format 1, 2 tracks, running status, note-on with velocity 0 = off) -> a MIDI clip */
    {
        static const unsigned char mid[] = {
            'M','T','h','d', 0,0,0,6, 0,1, 0,2, 0x01,0xE0,                     /* format 1, 2 tracks, 480 ticks/beat */
            'M','T','r','k', 0,0,0,11, 0x00,0xFF,0x51,0x03,0x07,0xA1,0x20,       /* tempo meta (ignored) */
            0x00,0xFF,0x2F,0x00,
            'M','T','r','k', 0,0,0,26,
            0x00,0x90,60,100,   0x83,0x60,60,0,      /* C at beat 0, 1 beat long (running status, vel 0) */
            0x00,64,90,         0x81,0x70,0x80,64,0, /* E at beat 1, 0.5 beat */
            0x81,0x70,0x90,67,80, 0x00,0xFF,0x2F,0x00 }; /* G at beat 2: never ends -> 1 beat */
        FILE *mf = fopen("/tmp/qlim_test_audio/t.mid", "wb");
        fwrite(mid, 1, sizeof mid, mf); fclose(mf);
        CHECK(session_set_clip(s, 1, 3, "/tmp/qlim_test_audio/t.mid") == 0 && session_is_midi(s, 1, 3), ".mid file -> MIDI clip");
        const MidiSeq *q = session_midi_get(s, 1, 3);
        CHECK(q && q->n == 3 && q->length == 4.0f, "3 notes, 1 bar (%d, %.1f)", q ? q->n : -1, q ? (double)q->length : 0.0);
        CHECK(q && q->note[0].key == 60 && q->note[0].start == 0.0f && q->note[0].len == 1.0f && q->note[0].vel == 100 &&
              q->note[1].key == 64 && q->note[1].start == 1.0f && q->note[1].len == 0.5f &&
              q->note[2].key == 67 && q->note[2].start == 2.0f && q->note[2].len == 1.0f, "times, lengths, keys right");
        CHECK(strcmp(s->channels[1].clips[3].name, "t") == 0, "named after the file");
        FILE *bf = fopen("/tmp/qlim_test_audio/bad.mid", "wb"); fwrite("MThd\0\0\0\6\0\1\0\5\x01", 1, 13, bf); fclose(bf);
        CHECK(session_set_clip(s, 1, 2, "/tmp/qlim_test_audio/bad.mid") != 0 && !session_is_midi(s, 1, 2), "a broken .mid is refused");
    }

    /* 8b) 1.3.1: play from a beat (double click on the ruler of the editor): from beat 2.5 the next note is beat 3 */
    session_stop_all(s);
    run_to(s, now_s() + 1.5);
    session_midi_set(s, 0, 0, seq4(4.0f, st, 4, 60));
    mark = tape_len;
    session_midi_play_from(s, 0, 0, 2.5);
    run_to(s, now_s() + 1.0);
    k = onsets(mark, tape_len, on, 16);
    CHECK(k >= 1 && labs((long)(on[0] - mark) - 12000) <= 2, "play from beat 2.5: the first note half a beat later (%ld)", k ? (long)(on[0] - mark) : -1L);
    /* while it plays: jump to beat 1.75 -> the note of beat 2 after a quarter beat */
    mark = tape_len;
    session_midi_play_from(s, 0, 0, 1.75);
    run_to(s, now_s() + 0.4);
    k = onsets(mark + 64, tape_len, on, 16);
    CHECK(k >= 1 && labs((long)(on[0] - mark) - 6000) <= 2, "jump while playing to 1.75: next note after 6000 (%ld)", k ? (long)(on[0] - mark) : -1L);
    session_stop_all(s);
    run_to(s, now_s() + 1.5);

    /* 8c) 1.3.1: the metronome - a tick on every beat while the transport runs (empty MIDI clip on channel 2, no
     * instrument), the 1st beat of the bar louder; nothing when it is off */
    CHECK(session_new_midi_clip(s, 1, 0, 4.0f, "Empty") == 0, "empty clip on channel 2");
    atomic_store(&s->metronome, true);
    mark = tape_len;
    session_toggle_clip(s, 1, 0);
    run_to(s, now_s() + 4.3);
    k = onsets(mark, tape_len, on, 16);
    bool mok = k >= 9;
    for (int i = 0; i < k && i < 9; ++i) if (labs((long)(on[i] - mark) - (long)i * 24000) > 2) mok = false;
    CHECK(mok, "metronome: a tick every 24000 samples from the start (%d ticks, 1st %ld, 2nd %ld)", k, k ? (long)(on[0] - mark) : -1L,
          k > 1 ? (long)(on[1] - mark) : -1L);
    float a0 = 0, a1 = 0;
    for (size_t i = on[0]; i < on[0] + 2000; ++i) if (fabsf(tape[i]) > a0) a0 = fabsf(tape[i]);
    for (size_t i = on[1]; i < on[1] + 2000; ++i) if (fabsf(tape[i]) > a1) a1 = fabsf(tape[i]);
    CHECK(a0 > a1 * 1.3f && a1 > 0.1f, "beat 1 louder (%.2f) than beat 2 (%.2f)", a0, a1);
    atomic_store(&s->metronome, false);
    mark = tape_len;
    run_to(s, now_s() + 1.0);
    mx = 0; for (size_t i = mark + 2000; i < tape_len; ++i) if (fabsf(tape[i]) > mx) mx = fabsf(tape[i]);
    CHECK(mx < 1e-3f, "metronome off: silence (%.5f)", mx);
    session_stop_all(s);
    run_to(s, now_s() + 0.5);
    atomic_store(&s->metronome, true);
    mark = tape_len;
    run_to(s, now_s() + 1.0);
    mx = 0; for (size_t i = mark; i < tape_len; ++i) if (fabsf(tape[i]) > mx) mx = fabsf(tape[i]);
    CHECK(mx < 1e-3f, "metronome on but nothing plays: silence (%.5f)", mx);
    /* play from beat 2.5 with nothing else playing: the beat count follows the clip -> ticks on beats 3 and 4 (4 = a new bar) */
    mark = tape_len;
    session_midi_play_from(s, 1, 0, 2.5);
    run_to(s, now_s() + 0.9);
    k = onsets(mark, tape_len, on, 16);
    CHECK(k >= 2 && labs((long)(on[0] - mark) - 12000) <= 2 && labs((long)(on[1] - mark) - 36000) <= 2,
          "play from 2.5: ticks at +12000 and +36000 (%ld, %ld)", k ? (long)(on[0] - mark) : -1L, k > 1 ? (long)(on[1] - mark) : -1L);
    a0 = 0; a1 = 0;
    if (k >= 2) {
        for (size_t i = on[0]; i < on[0] + 2000; ++i) if (fabsf(tape[i]) > a0) a0 = fabsf(tape[i]);
        for (size_t i = on[1]; i < on[1] + 2000; ++i) if (fabsf(tape[i]) > a1) a1 = fabsf(tape[i]);
    }
    CHECK(a1 > a0 * 1.3f, "the tick of beat 4 (= the new bar) is the loud one (%.2f > %.2f)", a1, a0);
    session_stop_all(s);
    run_to(s, now_s() + 0.5);

    /* 9) project save + load, undo */
    const char *pp = "/tmp/qlim_test_audio/midi.limproject";
    if (system("mkdir -p /tmp/qlim_test_audio") != 0) return 2;
    session_midi_set(s, 0, 1, seq4(4.0f, stb, 2, 67));
    CHECK(project_save(s, pp) == 0, "saved");
    undo_reset(s);
    const float stx[3] = { 0, 0.5f, 3.75f };
    session_midi_set(s, 0, 1, seq4(4.0f, stx, 3, 50));
    undo_commit_now(s);
    CHECK(session_midi_get(s, 0, 1)->n == 3, "edited: 3 notes");
    CHECK(undo_undo(s) && session_midi_get(s, 0, 1) && session_midi_get(s, 0, 1)->n == 2 && session_midi_get(s, 0, 1)->note[0].key == 67,
          "undo brings the 2 notes back");
    CHECK(undo_redo(s) && session_midi_get(s, 0, 1)->n == 3 && fabsf(session_midi_get(s, 0, 1)->note[2].start - 3.75f) < 1e-4f, "redo");
    fxchain_shutdown(s);
    session_free(s);
    static Session T; Session *t = &T;
    session_init(t, SR);
    atomic_store(&t->audio_running, false);
    CHECK(project_load(t, pp) == 0, "loaded");
    const MidiSeq *a = session_midi_get(t, 0, 0), *b = session_midi_get(t, 0, 1), *d = session_midi_get(t, 0, 2);
    CHECK(a && a->n == 4 && a->length == 4.0f && strcmp(t->channels[0].clips[0].name, "Beat") == 0, "clip A back (%d notes)", a ? a->n : -1);
    CHECK(b && b->n == 2 && b->note[1].start == 2.0f && b->note[1].key == 67 && b->note[1].vel == 110, "clip B back exactly");
    CHECK(d && d->n == 400 && d->length == 1.0f, "dense clip back (%d notes)", d ? d->n : -1);
    CHECK(atomic_load(&t->channels[0].clips[0].loop), "loop flag back");
    CHECK(atomic_load(&t->metronome), "metronome on - back from the project");
    session_free(t);
    free(tape);
    printf(fails ? "MIDICLIP: %d FAILED\n" : "MIDICLIP: all ok\n", fails);
    return fails != 0;
}
