/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* ONLY for tests: records what comes out of an output's monitor (linked with pw-link). Writes raw float32 to a file. */
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
static struct pw_main_loop *ml; static struct pw_stream *st; static FILE *fo; static int CH;
static void on_proc(void *d) { (void)d; struct pw_buffer *b = pw_stream_dequeue_buffer(st); if (!b) return;
    struct spa_data *sd = &b->buffer->datas[0];
    if (sd->data && sd->chunk) fwrite((char *)sd->data + sd->chunk->offset, 1, sd->chunk->size, fo);
    pw_stream_queue_buffer(st, b); }
static const struct pw_stream_events ev = { PW_VERSION_STREAM_EVENTS, .process = on_proc };
static void quit(int s) { (void)s; pw_main_loop_quit(ml); }
int main(int argc, char **argv) {
    CH = atoi(argv[2]); int rate = atoi(argv[3]);
    pw_init(&argc, &argv); fo = fopen(argv[1], "wb");
    ml = pw_main_loop_new(NULL); signal(SIGINT, quit); signal(SIGTERM, quit);
    st = pw_stream_new_simple(pw_main_loop_get_loop(ml), "qlim-cap",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_NODE_NAME, "qlim-cap",
                          PW_KEY_NODE_AUTOCONNECT, "false", PW_KEY_STREAM_DONT_REMIX, "true", NULL), &ev, NULL);
    uint8_t buf[1024]; struct spa_pod_builder bd = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    struct spa_audio_info_raw ri = { .format = SPA_AUDIO_FORMAT_F32, .rate = (uint32_t)rate, .channels = (uint32_t)CH };
    for (int i = 0; i < CH; ++i) ri.position[i] = CH == 2 ? (i ? SPA_AUDIO_CHANNEL_FR : SPA_AUDIO_CHANNEL_FL) : SPA_AUDIO_CHANNEL_AUX0 + i;
    const struct spa_pod *p[1] = { spa_format_audio_raw_build(&bd, SPA_PARAM_EnumFormat, &ri) };
    pw_stream_connect(st, PW_DIRECTION_INPUT, PW_ID_ANY, PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS, p, 1);
    pw_main_loop_run(ml); pw_stream_destroy(st); fclose(fo); pw_main_loop_destroy(ml); return 0; }
