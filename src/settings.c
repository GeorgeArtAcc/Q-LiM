/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#define _POSIX_C_SOURCE 200809L   /* fileno, fsync (needed on musl/Alpine) */
#include "settings.h"
#include "safefile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

// ============ Helpers ============

static int ensure_dir_recursive(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);

    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            struct stat st;
            if (stat(tmp, &st) != 0) {
                if (mkdir(tmp, 0755) != 0) return 1;
            } else if (!S_ISDIR(st.st_mode)) {
                return 1;
            }
            *p = '/';
        }
    }

    struct stat st;
    if (stat(tmp, &st) != 0) {
        if (mkdir(tmp, 0755) != 0) return 1;
    }
    return 0;
}

void settings_get_config_path(char *out_path, size_t size)
{
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(out_path, size, "%s/.config/qlim/audio.conf", home);
}

// ============ Load ============

int settings_load_audio(AudioConfig *config)
{
    if (!config) return 1;

    // Defaults
    config->host_api_index = -1;
    config->device_index = -1;
    config->sample_rate = 48000;
    config->buffer_size = 256;
    config->output_channels = 2;
    config->convert_files = 0;
    config->disk_mode = 0;
    config->backend = AUDIO_BACKEND_PORTAUDIO;
    config->pw_target[0] = 0;
    config->pw_graph = 1;
    config->api_name[0] = 0;
    config->dev_name[0] = 0;

    char path[1024];
    settings_get_config_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        // No file → defaults
        return 0;
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        int val;
        if (sscanf(line, "host_api_index=%d", &val) == 1) {
            config->host_api_index = val;
        } else if (sscanf(line, "device_index=%d", &val) == 1) {
            config->device_index = val;
        } else if (sscanf(line, "sample_rate=%d", &val) == 1) {
            if (val > 0) config->sample_rate = val;
        } else if (sscanf(line, "buffer_size=%d", &val) == 1) {
            if (val > 0) config->buffer_size = val;
        } else if (sscanf(line, "output_channels=%d", &val) == 1) {
            if (val >= 2) config->output_channels = val;
        } else if (strncmp(line, "disk_buffer=", 12) == 0) {
            const char *v = line + 12;
            if (strncmp(v, "small", 5) == 0) config->disk_mode = 1;
            else if (strncmp(v, "medium", 6) == 0) config->disk_mode = 2;
            else if (strncmp(v, "large", 5) == 0) config->disk_mode = 3;
            else config->disk_mode = 0;
        } else if (sscanf(line, "convert_files=%d", &val) == 1) {
            config->convert_files = val ? 1 : 0;
        } else if (strncmp(line, "backend=", 8) == 0) {
            config->backend = strncmp(line + 8, "pipewire", 8) == 0 ? AUDIO_BACKEND_PIPEWIRE : AUDIO_BACKEND_PORTAUDIO;
        } else if (sscanf(line, "pipewire_graph=%d", &val) == 1) {
            config->pw_graph = val ? 1 : 0;
        } else if (strncmp(line, "host_api_name=", 14) == 0) {
            snprintf(config->api_name, sizeof config->api_name, "%.63s", line + 14);
            config->api_name[strcspn(config->api_name, "\r\n")] = 0;
        } else if (strncmp(line, "device_name=", 12) == 0) {
            snprintf(config->dev_name, sizeof config->dev_name, "%.255s", line + 12);
            config->dev_name[strcspn(config->dev_name, "\r\n")] = 0;
        } else if (strncmp(line, "pipewire_output=", 16) == 0) {
            snprintf(config->pw_target, sizeof config->pw_target, "%.255s", line + 16);
            config->pw_target[strcspn(config->pw_target, "\r\n")] = 0;
        }
    }

    fclose(f);
    printf("settings_load_audio: OK '%s'\n", path);
    return 0;
}

// ============ Save ============

int settings_save_audio(const AudioConfig *config)
{
    if (!config) return 1;

    char path[1024];
    settings_get_config_path(path, sizeof(path));

    // Create ~/.config/qlim/ if it does not exist
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    ensure_dir_recursive(dir);

    char tmp[1100];
    FILE *f = safe_open(path, tmp, sizeof tmp);
    if (!f) {
        fprintf(stderr, "settings_save_audio: cannot open '%s'\n", path);
        return 1;
    }

    fprintf(f, "# Q-LiM audio settings\n");
    fprintf(f, "# Automatically generated file. Edit it through Q-LiM.\n\n");
    fprintf(f, "backend=%s\n", config->backend == AUDIO_BACKEND_PIPEWIRE ? "pipewire" : "portaudio");
    fprintf(f, "pipewire_output=%s\n", config->pw_target);
    fprintf(f, "pipewire_graph=%d\n", config->pw_graph ? 1 : 0);
    fprintf(f, "host_api_index=%d\n", config->host_api_index);
    fprintf(f, "device_index=%d\n", config->device_index);
    fprintf(f, "host_api_name=%s\n", config->api_name);
    fprintf(f, "device_name=%s\n", config->dev_name);
    fprintf(f, "sample_rate=%d\n", config->sample_rate);
    fprintf(f, "buffer_size=%d\n", config->buffer_size);
    fprintf(f, "output_channels=%d\n", config->output_channels);
    fprintf(f, "convert_files=%d\n", config->convert_files ? 1 : 0);
    {
        static const char *const dn[] = { "auto", "small", "medium", "large" };
        int dm = config->disk_mode;
        fprintf(f, "disk_buffer=%s\n", dn[(dm >= 0 && dm <= 3) ? dm : 0]);
    }

    if (safe_commit(f, tmp, path, 0) != 0) return 1;

    printf("settings_save_audio: OK '%s'\n", path);
    return 0;
}
