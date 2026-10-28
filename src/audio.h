/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef AUDIO_H
#define AUDIO_H

#include "session.h"
#include <stdbool.h>

// ============ Audio Configuration ============

typedef struct {
    int  host_api_index;    // index into Pa_GetHostApiInfo (-1 = default)
    int  device_index;      // index into Pa_GetDeviceInfo (-1 = default)
    int  sample_rate;       // 44100, 48000, ...
    int  buffer_size;       // 64, 128, 256, 512, ...
    int  output_channels;   // READ ONLY: how many outputs the card has (filled by audio_init; not a setting)
    int  disk_mode;         // disk read buffer: 0 = Auto, 1 = Small, 2 = Medium, 3 = Large
    int  convert_files;     // 0 = files play as they are (default), 1 = they are converted into the cache
    int  backend;           // 0 = PortAudio (ALSA/JACK/OSS), 1 = PipeWire directly (native)
    char pw_target[256];    // PipeWire: node.name of the output ("" = the system default)
    int  pw_graph;          // PipeWire: 1 = the channels and the master are separate nodes in the graph (Patchance/qpwgraph), 0 = one stream
    /* names of the driver and the device (audio.conf): the numbers above shift when a card or a driver (e.g. ASIO)
     * is added, the names do not. "" = not known (then only the numbers are used). */
    char api_name[64];
    char dev_name[256];
} AudioConfig;

#define AUDIO_BACKEND_PORTAUDIO 0
#define AUDIO_BACKEND_PIPEWIRE  1
/* the numbers of the PipeWire outputs in the device list (so they are not confused with PortAudio's) */
#define AUDIO_PW_DEV_BASE 1000000

// ============ Init / Shutdown ============

// Initializes PortAudio with the given configuration.
// If config is NULL — uses the default (auto-detects a Scarlett).
// Returns 0 on success, != 0 on error.
int  audio_init(Session *session, const AudioConfig *config);

// Stops the stream and closes PortAudio.
void audio_shutdown(void);

// ============ 1.4: input (recording) ============
int         audio_input_channels(void);      /* 0 = no input (cannot record audio) */
unsigned    audio_roundtrip_frames(void);    /* the delay out + in (frames): recordings are shifted back by this much */
const char *audio_input_name(void);
void        audio_input_want(bool on);       /* PipeWire: the input is opened only while a channel is armed */

// ============ Lost sound card ============

// Called every frame (it rate-limits itself). If the card has disappeared -> audio_is_lost() becomes true.
void audio_health_poll(void);
bool audio_is_lost(void);
const char *audio_lost_name(void);

// ============ Information about the available drivers/devices ============

// Returns the number of available host APIs (drivers).
int audio_get_host_api_count(void);

// Returns the name of a host API by index.
const char *audio_get_host_api_name(int index);

// Returns the number of output devices for the given host API.
// If host_api_index == -1 → all devices.
int audio_get_device_count(int host_api_index);

// Returns the name of a device for the given host API.
// device_local_index is an index into this API's list (0..count-1).
// out_global_index receives the global Pa index (for Pa_GetDeviceInfo).
const char *audio_get_device_name(int host_api_index,
                                  int device_local_index,
                                  int *out_global_index);

// Returns the REAL number of output channels of a device (virtual ALSA devices = 2; max MAX_HW_OUT).
int audio_get_device_max_channels(int global_device_index);

// Returns the default sample rate of a device.
int audio_get_device_default_samplerate(int global_device_index);

// true = this "driver" in the list is the direct PipeWire one (a virtual entry after PortAudio's ones).
bool audio_host_api_is_pipewire(int index);
#ifdef _WIN32
/* ASIO (Windows): is this device an ASIO driver; open the driver's own settings window.
 * audio_asio_panel must be called while the audio is stopped (ASIO allows only one user of the driver). */
bool audio_device_is_asio(int global_device_index);
int  audio_asio_panel(int global_device_index);
#endif

// Short text about the audio state (for the dialog): with PipeWire - graph rate, buffer, dropouts.
// Returns false if there is nothing to show.
bool audio_engine_status_text(char *out, int size);
/* 1.3.1: the buffer really in use (PipeWire: the graph's quantum) and the output delay in ms; false = unknown */
bool audio_actual_buffer(int *frames, double *out_ms);

// ============ PipeWire graph (step 28) ============

// Called every frame from the main thread: channel names -> ports, Routing <-> the links in the graph.
// Returns true if a route was changed from outside (Patchance/qpwgraph).
bool audio_graph_poll(void);
// true = the audio is currently going through PipeWire graph mode
bool audio_graph_active(void);

// ============ Current configuration ============

// Returns the currently active configuration.
AudioConfig audio_get_current_config(void);

#endif
