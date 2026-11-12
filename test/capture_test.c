/* Desktop tests for bar-locked capture. No USB, no MPC.
 * The clock fed in here is the one the official MPC host reports on each block:
 * playhead, tempo, meter, bar start, playing, and loop points.
 */
#include "../src/capture.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

static void check(int ok, const char *what) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static int16_t tone(int k) {
    return (int16_t)((k * 17) % 20001 - 10000);
}

static int g_sine_start;
static int g_sine_period;

static int16_t sine_at(int k) {
    int p = k - g_sine_start;
    double x = sin(2.0 * 3.14159265358979323846 * (double)p / (double)g_sine_period);
    return (int16_t)llround(x * 12000.0);
}

static double step_ppq(double ppq, double bpm, int frames) {
    return ppq + (double)frames * bpm / (60.0 * 44100.0);
}

static void feed(cap_t *c, double ppq, double bpm, int numer, int denom, double bar_start,
                 int playing, int cycle, double c0, double c1, int frames, int k, int16_t (*sig)(int)) {
    int16_t buf[1024];
    if (frames > 512) frames = 512;
    for (int i = 0; i < frames; i++) {
        int16_t s = sig(k + i);
        buf[i * 2] = s;
        buf[i * 2 + 1] = (int16_t)(-s / 2);
    }
    cap_time_t t;
    memset(&t, 0, sizeof t);
    t.ppq = ppq;
    t.bpm = bpm;
    t.numer = numer;
    t.denom = denom;
    t.bar_start = bar_start;
    t.playing = playing;
    t.cycle = cycle;
    t.cycle_start = c0;
    t.cycle_end = c1;
    t.frames = frames;
    cap_transport(c, &t);
    cap_audio(c, buf, frames);
}

static int run(cap_t *c, double ppq, double bpm, int numer, int denom, double bar_start,
               int cycle, double c0, double c1, int frames, int16_t (*sig)(int)) {
    int k = 0;
    int guard = (int)(cap_length_frames(16, numer, denom, bpm) / frames) + 8000;
    if (guard < 100) guard = 100;
    if (guard > 400000) guard = 400000;
    for (int n = 0; n < guard; n++) {
        int st = cap_code(c);
        if (st == CAP_SAVED || st == CAP_ERROR || st == CAP_CANCELLED) break;
        feed(c, ppq, bpm, numer, denom, bar_start, 1, cycle, c0, c1, frames, k, sig);
        k += frames;
        double next = step_ppq(ppq, bpm, frames);
        if (cycle && c1 > c0 && next >= c1) next = c0 + (next - c1);
        else if (!cycle && numer == 4 && denom == 4 && next >= 4.0 && ppq < 4.0 && bpm == 120.0 && c1 == -4.0)
            next -= 4.0;
        ppq = next;
    }
    return cap_wait(c, 4000);
}

static int read_wav(const char *path, int16_t **lr, int *frames) {
    FILE *f = fopen(path, "rb");
    uint8_t hdr[44];
    uint32_t data_bytes;
    if (!f) return -1;
    if (fread(hdr, 1, 44, f) != 44) { fclose(f); return -1; }
    if (memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4) || memcmp(hdr + 36, "data", 4)) {
        fclose(f);
        return -1;
    }
    if (hdr[20] != 1 || hdr[22] != 2 || hdr[34] != 24) { fclose(f); return -1; }
    if (hdr[24] != (44100 & 255) || hdr[25] != ((44100 >> 8) & 255)) { fclose(f); return -1; }
    data_bytes = (uint32_t)hdr[40] | ((uint32_t)hdr[41] << 8) | ((uint32_t)hdr[42] << 16) | ((uint32_t)hdr[43] << 24);
    if (data_bytes % 6u) { fclose(f); return -1; }
    *frames = (int)(data_bytes / 6u);
    *lr = malloc((size_t)(*frames) * 2u * sizeof(int16_t));
    if (!*lr) { fclose(f); return -1; }
    for (int i = 0; i < *frames * 2; i++) {
        uint8_t b[3];
        int32_t s;
        if (fread(b, 1, 3, f) != 3) { free(*lr); fclose(f); return -1; }
        s = (int32_t)b[0] | ((int32_t)b[1] << 8) | ((int32_t)b[2] << 16);
        if (b[2] & 0x80) s |= ~0xFFFFFF;
        (*lr)[i] = (int16_t)(s >> 8);
    }
    fclose(f);
    return 0;
}

static cap_t *fresh(const char *dir, int bars_index, int latency) {
    cap_t *c = cap_create(NULL);
    cap_set_override(c, dir);
    cap_set_tag(c, "DN2");
    cap_set_len_index(c, bars_index);
    cap_set_latency(c, latency);
    return c;
}

static int sample_of(double ppq, double target, double bpm) {
    return (int)llround((target - ppq) * 60.0 * 44100.0 / bpm);
}

int main(void) {
    char dir[] = "/tmp/tributary-capture-XXXXXX";
    char status[128], path[1024];
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }

    check(CAP_LATENCY_SAMPLES == 162, "the monitoring spare is 162 MPC samples");
    check(cap_length_frames(4, 4, 4, 120.0) == 352800, "4 bars at 120 is 352800 samples");
    check(cap_length_frames(1, 4, 4, 120.0) == 88200, "1 bar at 120 is 88200 samples");
    check(cap_length_frames(2, 3, 4, 120.0) == 132300, "2 bars of 3/4 at 120 is 132300 samples");
    check(cap_length_frames(16, 4, 4, 60.0) == CAP_MAX_FRAMES, "16 bars at 60 is the cap");
    check(cap_length_frames(16, 4, 4, 59.0) > CAP_MAX_FRAMES, "16 bars at 59 is over the cap");

    {
        cap_t *c = cap_create("/media/EOS_DIGITAL/VST2 Plugins/johnny - VST - Tributary");
        cap_path(c, path, sizeof path);
        check(!strcmp(path, "/media/EOS_DIGITAL/Tributary Captures"),
              "default folder is Tributary Captures on the plugin drive");
        cap_set_dest(c, 1);
        cap_path(c, path, sizeof path);
        if (access("/storage", F_OK) == 0)
            check(!strcmp(path, "/storage/Tributary Captures"), "internal folder is on /storage");
        else
            check(!strcmp(path, "not plugged in"), "internal folder says so when /storage is absent");
        cap_destroy(c);
    }

    {
        cap_t *c = fresh(dir, 2, 0);
        check(cap_arm(c) == 1, "arm waits for a bar");
        cap_status(c, status, sizeof status);
        check(!strcmp(status, "Armed"), "status says Armed");
        feed(c, 1.0, 120.0, 4, 4, -1, 0, 0, 0, 0, 128, 0, tone);
        cap_status(c, status, sizeof status);
        check(!strcmp(status, "Armed"), "stop while armed stays armed");
        feed(c, 3.0, 100.0, 4, 4, -1, 1, 0, 0, 0, 128, 0, tone);
        cap_status(c, status, sizeof status);
        check(!strcmp(status, "Armed"), "a jump before the bar does not throw the take away");
        cap_cancel(c);
        cap_status(c, status, sizeof status);
        check(!strcmp(status, "Cancelled"), "cancel drops an armed take");
        cap_destroy(c);
    }

    {
        int blocks[] = { 32, 128, 256 };
        for (int bi = 0; bi < 3; bi++) {
            int frames = blocks[bi];
            int start = sample_of(3.5, 4.0, 120.0);
            cap_t *c = fresh(dir, 0, 0);
            char what[80];
            int16_t *lr = NULL;
            int n = 0;
            cap_arm(c);
            int st = run(c, 3.5, 120.0, 4, 4, -1, 0, 0, 0, frames, tone);
            cap_status(c, status, sizeof status);
            snprintf(what, sizeof what, "bar start lines up inside a block of %d", frames);
            check(st == CAP_SAVED, what);
            if (st == CAP_SAVED && read_wav(cap_last_file(c), &lr, &n) == 0) {
                int len = (int)cap_length_frames(1, 4, 4, 120.0);
                snprintf(what, sizeof what, "one bar is %d samples at block %d", len, frames);
                check(n == len && lr[0] == tone(start) && lr[2] == tone(start + 1)
                      && lr[(n - 1) * 2] == tone(start + n - 1), what);
                free(lr);
            } else if (st != CAP_SAVED) {
                printf("    status: %s\n", status);
            }
            cap_destroy(c);
        }
    }

    {
        int start = sample_of(3.5, 4.0, 120.0) + 10;
        cap_t *c = fresh(dir, 0, 10);
        int16_t *lr = NULL;
        int n = 0;
        cap_arm(c);
        int st = run(c, 3.5, 120.0, 4, 4, -1, 0, 0, 0, 128, tone);
        check(st == CAP_SAVED, "a take with a 10 sample wait saves");
        if (st == CAP_SAVED && read_wav(cap_last_file(c), &lr, &n) == 0) {
            check(lr[0] == tone(start) && lr[0] != tone(start - 10),
                  "the file starts after the latency, on the audio that belongs to the bar");
            free(lr);
        }
        cap_destroy(c);
    }

    {
        int start = CAP_LATENCY_SAMPLES;
        cap_t *c = fresh(dir, 2, CAP_LATENCY_SAMPLES);
        int16_t *lr = NULL;
        int n = 0;
        char first[1024];
        struct stat st1, st2;
        cap_arm(c);
        int st = run(c, 0.0, 120.0, 4, 4, -1, 0, 0, 0, 128, tone);
        cap_status(c, status, sizeof status);
        snprintf(first, sizeof first, "%s", cap_last_file(c));
        check(st == CAP_SAVED && strstr(first, "DN2_120bpm_4bars_") && strstr(first, ".wav"),
              "the file is named with the machine, tempo, bars, and time");
        if (st == CAP_SAVED && read_wav(first, &lr, &n) == 0) {
            check(n == 352800 && lr[0] == tone(start) && lr[1] == (int16_t)(-tone(start) / 2),
                  "4 bars is exactly 352800 stereo samples, shifted by the real spare");
            free(lr);
        } else {
            printf("    status: %s\n", status);
        }
        stat(first, &st1);
        cap_arm(c);
        st = run(c, 0.0, 120.0, 4, 4, -1, 0, 0, 0, 128, tone);
        stat(first, &st2);
        check(st == CAP_SAVED && strcmp(first, cap_last_file(c)) != 0 && st1.st_size == st2.st_size,
              "a second take does not overwrite the first file");
        cap_destroy(c);
    }

    {
        g_sine_period = 352800;
        g_sine_start = sample_of(3.9, 4.0, 120.0);
        cap_t *c = fresh(dir, 2, 0);
        int16_t *lr = NULL;
        int n = 0;
        cap_arm(c);
        int st = run(c, 3.9, 120.0, 4, 4, -1, 0, 0, 0, 128, sine_at);
        check(st == CAP_SAVED, "a periodic take saves");
        if (st == CAP_SAVED && read_wav(cap_last_file(c), &lr, &n) == 0 && n == g_sine_period) {
            int seam = (int)lr[0] - (int)lr[(n - 1) * 2];
            int natural = (int)sine_at(g_sine_start + n) - (int)sine_at(g_sine_start + n - 1);
            check(lr[0] == sine_at(g_sine_start) && lr[(n - 1) * 2] == sine_at(g_sine_start + n - 1)
                  && sine_at(g_sine_start + n) == sine_at(g_sine_start) && seam == natural,
                  "played back to back, the join is the next sample of the same wave");
            free(lr);
        } else {
            cap_status(c, status, sizeof status);
            printf("    status: %s frames %d\n", status, n);
        }
        cap_destroy(c);
    }

    {
        cap_t *c = fresh(dir, 1, 0);
        cap_arm(c);
        int st = run(c, 0.0, 120.0, 3, 4, -1, 0, 0, 0, 64, tone);
        int16_t *lr = NULL;
        int n = 0;
        check(st == CAP_SAVED, "a 3/4 take saves when the meter is passed in");
        if (st == CAP_SAVED && read_wav(cap_last_file(c), &lr, &n) == 0) {
            check(n == 132300 && lr[0] == tone(0), "2 bars of 3/4 are 132300 samples");
            free(lr);
        }
        cap_destroy(c);
    }

    {
        cap_t *c = fresh(dir, 4, 0);
        cap_arm(c);
        feed(c, 0.0, 60.0, 4, 4, -1, 1, 0, 0, 0, 128, 0, tone);
        cap_status(c, status, sizeof status);
        check(!strcmp(status, "Recording 1/16"), "16 bars at 60 is accepted");
        cap_cancel(c);
        cap_destroy(c);
        c = fresh(dir, 4, 0);
        cap_arm(c);
        feed(c, 0.0, 59.0, 4, 4, -1, 1, 0, 0, 0, 128, 0, tone);
        cap_status(c, status, sizeof status);
        check(!strcmp(status, "Error: too long") && cap_last_file(c)[0] == '\0',
              "slower than 60 BPM at 16 bars is refused");
        cap_destroy(c);
    }

    {
        cap_t *c = fresh(dir, 0, 0);
        double ppq = 0;
        cap_arm(c);
        for (int i = 0; i < 30; i++) {
            feed(c, ppq, 120.0, 4, 4, -1, 1, 0, 0, 0, 128, i * 128, tone);
            ppq = step_ppq(ppq, 120.0, 128);
        }
        feed(c, ppq, 140.0, 4, 4, -1, 1, 0, 0, 0, 128, 30 * 128, tone);
        cap_status(c, status, sizeof status);
        check(!strcmp(status, "Error: tempo changed") && cap_last_file(c)[0] == '\0',
              "a tempo change throws the take away");
        cap_destroy(c);
    }

    {
        cap_t *c = fresh(dir, 0, 0);
        cap_arm(c);
        feed(c, 0.0, 120.0, 4, 4, -1, 1, 0, 0, 0, 128, 0, tone);
        feed(c, step_ppq(0.0, 120.0, 128), 120.0, 4, 4, -1, 0, 0, 0, 0, 128, 128, tone);
        cap_status(c, status, sizeof status);
        check(!strcmp(status, "Error: transport stopped"), "stop during a take throws it away");
        cap_destroy(c);
    }

    {
        cap_t *c = fresh(dir, 0, 0);
        cap_arm(c);
        feed(c, 0.0, 120.0, 4, 4, -1, 1, 0, 0, 0, 128, 0, tone);
        feed(c, 2.0, 120.0, 4, 4, -1, 1, 0, 0, 0, 128, 128, tone);
        cap_status(c, status, sizeof status);
        check(!strcmp(status, "Error: transport moved"), "a jump in the middle of a bar throws the take away");
        cap_destroy(c);
    }

    {
        cap_t *c = fresh(dir, 1, 0);
        int16_t *lr = NULL;
        int n = 0;
        cap_arm(c);
        int st = run(c, 0.0, 120.0, 4, 4, -1, 1, 0.0, 4.0, 128, tone);
        cap_status(c, status, sizeof status);
        check(st == CAP_SAVED, "a loop that jumps back to the loop start keeps recording");
        if (st == CAP_SAVED && read_wav(cap_last_file(c), &lr, &n) == 0) {
            int bar = 88200;
            check(n == bar * 2 && lr[0] == tone(0) && lr[bar * 2] == tone(bar)
                  && lr[(n - 1) * 2] == tone(n - 1),
                  "across the loop point the samples stay in order");
            free(lr);
        } else {
            printf("    status: %s\n", status);
        }
        cap_destroy(c);
    }

    {
        cap_t *c = fresh(dir, 1, 0);
        int16_t *lr = NULL;
        int n = 0;
        cap_arm(c);
        /* c1 == -4 asks the driver to wrap every bar without telling the plugin. */
        int st = run(c, 0.0, 120.0, 4, 4, -1, 0, 0, -4.0, 128, tone);
        cap_status(c, status, sizeof status);
        check(st == CAP_SAVED, "a bar-to-bar jump with no loop points still keeps the take");
        if (st == CAP_SAVED && read_wav(cap_last_file(c), &lr, &n) == 0) {
            check(n == 176400 && lr[88200 * 2] == tone(88200),
                  "the second bar continues the same waveform");
            free(lr);
        } else {
            printf("    status: %s\n", status);
        }
        cap_destroy(c);
    }

    {
        cap_t *c = fresh(dir, 0, 0);
        cap_arm(c);
        feed(c, 4.0, 120.0, 4, 4, 4.0, 1, 1, 0.0, 8.0, 128, 0, tone);
        feed(c, 0.0, 120.0, 4, 4, 0.0, 1, 1, 0.0, 8.0, 128, 128, tone);
        cap_status(c, status, sizeof status);
        check(!strcmp(status, "Error: transport moved"),
              "a jump to another bar is thrown away when it is not the loop");
        cap_destroy(c);
    }

    {
        cap_t *c = fresh(dir, 0, 0);
        cap_set_auto(c, 1);
        cap_arm(c);
        int st = run(c, 0.0, 120.0, 4, 4, -1, 0, 0, 0, 256, tone);
        check(st == CAP_SAVED, "auto re-arm still saves the take");
        for (int i = 0; i < 5 && cap_code(c) != CAP_ARMED; i++)
            feed(c, 0.5, 120.0, 4, 4, -1, 1, 0, 0, 0, 128, 0, tone);
        cap_status(c, status, sizeof status);
        check(!strcmp(status, "Armed"), "auto re-arm waits for the next bar");
        cap_cancel(c);
        cap_destroy(c);
    }

    {
        cap_t *c = fresh("/tmp/tributary-missing-parent/nope", 0, 0);
        cap_arm(c);
        int st = run(c, 0.0, 120.0, 4, 4, -1, 0, 0, 0, 256, tone);
        cap_status(c, status, sizeof status);
        check(st == CAP_ERROR && !strcmp(status, "Error: could not save"),
              "a missing folder is an error, not a crashed take");
        cap_destroy(c);
    }

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    printf("files in %s\n", dir);
    return failures ? 1 : 0;
}
