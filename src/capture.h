/* Bar-locked loop capture. The audio thread only copies samples.
 * Copyright (C) 2026 Stampman3000
 * This program is free software under the GNU General Public License, version 3.
 */
#ifndef TRIBUTARY_CAPTURE_H
#define TRIBUTARY_CAPTURE_H
#include <stddef.h>
#include <stdint.h>

#define CAP_SR 44100
/* Longest take: 16 bars of 4/4 at 60 BPM. 64 seconds, about 11 MB of samples. */
#define CAP_MAX_FRAMES (16 * 4 * CAP_SR)

/* Monitoring sits this many Digitone frames (48 kHz) behind the newest sample.
 * probe.c's HEAR_LAG must stay the same number. At 44.1 kHz that wait is
 * CAP_LATENCY_SAMPLES of the file, about 3.7 ms. */
#define CAP_HEAR_LAG 176
#define CAP_LATENCY_SAMPLES ((CAP_HEAR_LAG * CAP_SR + 24000) / 48000)

enum {
    CAP_IDLE = 0,
    CAP_ARMED,
    CAP_RECORDING,
    CAP_SAVED,
    CAP_ERROR,
    CAP_CANCELLED
};

enum {
    CAP_ERR_NONE = 0,
    CAP_ERR_TEMPO,
    CAP_ERR_STOP,
    CAP_ERR_MOVE,
    CAP_ERR_METER,
    CAP_ERR_LONG,
    CAP_ERR_FOLDER,
    CAP_ERR_DISK
};

/* One host block. ppq is the playhead at the first sample.
 * bar_start < 0 means "work it out from ppq and the meter".
 * cycle is the host loop. cycle == 0 means the host did not say. */
typedef struct {
    double ppq;
    double bpm;
    int numer;
    int denom;
    double bar_start;
    int playing;
    int cycle;
    double cycle_start;
    double cycle_end;
    int frames;
} cap_time_t;

typedef struct cap cap_t;

/* Exact length of `bars` at this tempo and meter, in 44.1 kHz samples. */
long long cap_length_frames(int bars, int numer, int denom, double bpm);

cap_t *cap_create(const char *data_dir);
void cap_destroy(cap_t *c);

void cap_set_tag(cap_t *c, const char *device_name);
void cap_set_override(cap_t *c, const char *folder);
void cap_set_latency(cap_t *c, int samples);
void cap_set_drive(cap_t *c, const char *data_dir);

int cap_set_len_index(cap_t *c, int index);
int cap_set_source(cap_t *c, int index);
int cap_set_dest(cap_t *c, int index);
int cap_set_auto(cap_t *c, int on);
int cap_arm(cap_t *c);
void cap_cancel(cap_t *c);

/* Audio thread. No allocation, no file, no lock. */
void cap_transport(cap_t *c, const cap_time_t *t);
void cap_audio(cap_t *c, const int16_t *lr, int frames);
/* The output samples of this block that belong in the take. Clears them
 * so a later render in the same host block can take the rest. */
void cap_take_span(cap_t *c, int frames, int *skip, int *keep);
/* Append already-chosen samples. Used for the Digitone's own 48 kHz frames. */
void cap_write(cap_t *c, const int16_t *lr, int frames);
void cap_set_rate(cap_t *c, int rate);

int cap_keep_count(const cap_t *c);
int cap_source(const cap_t *c);
int cap_len_index(const cap_t *c);
int cap_dest(const cap_t *c);
int cap_auto(const cap_t *c);
int cap_arm_on(const cap_t *c);
int cap_code(const cap_t *c);
int cap_status(const cap_t *c, char *buf, int n);
int cap_path(const cap_t *c, char *buf, int n);
int cap_wait(cap_t *c, int ms);
const char *cap_last_file(const cap_t *c);

/* Full host clock, matching the fields the MPC reports on each block
 * (playhead, tempo, meter, bar start, playing, loop points).
 * song_pos on the engine fills in 4/4 and no loop, which is all the
 * current open-mpc wrapper forwards. */
void tributary_host_time(void *inst, double ppq, double bpm, int playing, int frames,
                         int numer, int denom, double bar_start,
                         int cycle, double cycle_start, double cycle_end);

#endif
