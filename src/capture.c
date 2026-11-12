/* Records Tributary's own output for a whole number of bars.
 * The take starts on a bar line from the host transport, then waits out the
 * monitoring spare so the file lines up with sequenced material.
 * Copyright (C) 2026 Stampman3000
 * This program is free software under the GNU General Public License, version 3.
 */
#include "capture.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>

#define CAP_PATH 1024

enum { ST_IDLE = 0, ST_ARMED, ST_DELAY, ST_REC, ST_SAVING };

static const int k_bars[5] = { 1, 2, 4, 8, 16 };

struct cap {
    pthread_t thread;
    int thread_on;
    atomic_int quit;
    atomic_int job;
    atomic_int mode;
    atomic_int status;
    atomic_int err;
    atomic_int bars;
    atomic_int source;
    atomic_int dest;
    atomic_int auto_rearm;
    atomic_int latency;
    atomic_int rearm;
    atomic_uint filled;
    atomic_uint total;
    atomic_uint bar_frames;
    atomic_int bpm_milli;
    atomic_int show_bars;
    int16_t *buf;
    int skip, keep;
    int rec_left;
    int delay_left;
    int last_valid;
    int last_frames;
    int lat_num, lat_den;
    double last_ppq;
    double last_bpm;
    double rec_bpm;
    char drive[CAP_PATH];
    char override[CAP_PATH];
    char tag[8];
    char job_dir[CAP_PATH];
    char job_tag[8];
    char file[CAP_PATH];
    int job_bars;
    int rate;
    int job_rate;
};

static long long length_at(int bars, int numer, int denom, double bpm, int rate) {
    if (bars < 1 || numer < 1 || denom < 1 || rate < 8000 || !(bpm >= 1.0)) return 0;
    double x = (double)bars * (double)numer * 4.0 * (double)rate * 60.0
               / ((double)denom * bpm);
    if (!(x >= 1.0)) return 0;
    if (x > (double)CAP_MAX_FRAMES + 1.0) return (long long)CAP_MAX_FRAMES + 1;
    return llround(x);
}

long long cap_length_frames(int bars, int numer, int denom, double bpm) {
    return length_at(bars, numer, denom, bpm, CAP_SR);
}

static int bars_of(int index) {
    if (index < 0) index = 0;
    if (index > 4) index = 4;
    return k_bars[index];
}

static int index_of_bars(int bars) {
    for (int i = 0; i < 5; i++) if (k_bars[i] == bars) return i;
    return 2;
}

static int recording(const cap_t *c) {
    int m = atomic_load_explicit(&c->mode, memory_order_acquire);
    return m == ST_DELAY || m == ST_REC || m == ST_SAVING;
}

static void set_err(cap_t *c, int err) {
    c->keep = 0;
    c->rec_left = 0;
    c->delay_left = 0;
    c->last_valid = 0;
    atomic_store_explicit(&c->err, err, memory_order_relaxed);
    atomic_store_explicit(&c->mode, ST_IDLE, memory_order_release);
    atomic_store_explicit(&c->status, CAP_ERROR, memory_order_release);
}

static int dir_ok(const char *path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void join_path(char *out, size_t n, const char *root, const char *leaf) {
    if (!root || !root[0]) {
        snprintf(out, n, "%s", leaf);
        return;
    }
    size_t k = strlen(root);
    if (k && root[k - 1] == '/') snprintf(out, n, "%s%s", root, leaf);
    else snprintf(out, n, "%s/%s", root, leaf);
}

/* Another stick under /media, not the drive this plugin loaded from. */
static int find_usb(const cap_t *c, char *out, size_t n) {
    const char *skip = NULL;
    if (c->drive[0] && strncmp(c->drive, "/media/", 7) == 0) skip = c->drive + 7;
    DIR *d = opendir("/media");
    if (!d) return -1;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (skip && !strcmp(e->d_name, skip)) continue;
        char p[CAP_PATH];
        snprintf(p, sizeof p, "/media/%s", e->d_name);
        if (!dir_ok(p)) continue;
        snprintf(out, n, "%s", p);
        found = 1;
        break;
    }
    closedir(d);
    return found ? 0 : -1;
}

/* A memory card can be named anything. Prefer one that already holds plugins. */
static int media_root(char *out, size_t n) {
    DIR *d = opendir("/media");
    if (!d) return -1;
    char any[CAP_PATH];
    any[0] = '\0';
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[CAP_PATH];
        snprintf(p, sizeof p, "/media/%s", e->d_name);
        if (!dir_ok(p)) continue;
        char sub[CAP_PATH];
        join_path(sub, sizeof sub, p, "VST2 Plugins");
        int plugins = dir_ok(sub);
        join_path(sub, sizeof sub, p, "synths");
        plugins = plugins || dir_ok(sub);
        join_path(sub, sizeof sub, p, "Synths");
        plugins = plugins || dir_ok(sub);
        if (plugins) {
            snprintf(out, n, "%s", p);
            closedir(d);
            return 0;
        }
        if (!any[0]) snprintf(any, sizeof any, "%s", p);
    }
    closedir(d);
    if (!any[0]) return -1;
    snprintf(out, n, "%s", any);
    return 0;
}

static int resolve_dir(const cap_t *c, char *out, size_t n) {
    if (c->override[0]) {
        snprintf(out, n, "%s", c->override);
        return 0;
    }
    int dest = atomic_load_explicit(&c->dest, memory_order_relaxed);
    if (dest <= 0 || dest == 2) {
        /* Same drive the plugin loaded from, whatever that card is called.
         * "sd" is that drive too, unless the plugin itself is not on a card. */
        if (c->drive[0] && (dest <= 0 || strncmp(c->drive, "/media/", 7) == 0)) {
            join_path(out, n, c->drive, "Tributary Captures");
            return 0;
        }
        char root[CAP_PATH];
        if (media_root(root, sizeof root) == 0) {
            join_path(out, n, root, "Tributary Captures");
            return 0;
        }
        if (dest <= 0 && dir_ok("/storage")) {
            snprintf(out, n, "/storage/Tributary Captures");
            return 0;
        }
        return -1;
    }
    if (dest == 1) {
        if (!dir_ok("/storage")) return -1;
        snprintf(out, n, "/storage/Tributary Captures");
        return 0;
    }
    char usb[CAP_PATH];
    if (find_usb(c, usb, sizeof usb) != 0) return -1;
    join_path(out, n, usb, "Tributary Captures");
    return 0;
}

static void ensure_folder(cap_t *c) {
    char dir[CAP_PATH];
    if (resolve_dir(c, dir, sizeof dir) != 0) return;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return;
}

void cap_set_drive(cap_t *c, const char *data_dir) {
    if (!c) return;
    c->drive[0] = '\0';
    if (!data_dir || !data_dir[0]) return;
    snprintf(c->drive, sizeof c->drive, "%s", data_dir);
    for (int hop = 0; hop < 6; hop++) {
        size_t n = strlen(c->drive);
        while (n > 1 && c->drive[n - 1] == '/') c->drive[--n] = '\0';
        char *sl = strrchr(c->drive, '/');
        if (!sl || sl == c->drive) break;
        const char *base = sl + 1;
        int strip = !strcmp(base, ".") || !strcmp(base, "VST2 Plugins")
                    || !strcmp(base, "Synths") || !strcmp(base, "synths")
                    || strstr(base, " - VST - ") || strstr(base, " - MPC - ");
        size_t bl = strlen(base);
        if (!strip && bl > 3 && !strcmp(base + bl - 3, ".so")) strip = 1;
        if (!strip) break;
        *sl = '\0';
    }
}

static void apply_tag(cap_t *c, const char *name) {
    const char *t = "EL";
    if (!name || !name[0]) t = "EL";
    else if (!strcmp(name, "Digitone II")) t = "DN2";
    else if (!strcmp(name, "Digitakt II")) t = "DT2";
    else if (!strcmp(name, "Digitakt")) t = "DT";
    else if (!strcmp(name, "Digitone")) t = "DN";
    else if (!strcmp(name, "Digitone Keys")) t = "DNK";
    else if (!strcmp(name, "Analog Four MKII")) t = "AF2";
    else if (!strcmp(name, "Analog Rytm MKII")) t = "AR2";
    else if (!strcmp(name, "Analog Heat")) t = "AH";
    else if (!strcmp(name, "Analog Heat MKII")) t = "AH2";
    else if (!strcmp(name, "Analog Heat +FX")) t = "AHF";
    else if (!strcmp(name, "Syntakt")) t = "ST";
    else if (strlen(name) <= 6 && !strchr(name, ' ')) t = name;
    snprintf(c->tag, sizeof c->tag, "%s", t);
}

void cap_set_rate(cap_t *c, int rate) {
    if (!c || recording(c)) return;
    if (rate < 8000) rate = CAP_SR;
    c->rate = rate;
}

void cap_set_tag(cap_t *c, const char *device_name) {
    if (!c || recording(c)) return;
    apply_tag(c, device_name);
}

void cap_set_override(cap_t *c, const char *folder) {
    if (!c || recording(c)) return;
    c->override[0] = '\0';
    if (folder) snprintf(c->override, sizeof c->override, "%s", folder);
}

void cap_set_latency(cap_t *c, int samples) {
    if (!c || recording(c)) return;
    if (samples < 0) samples = 0;
    if (samples > CAP_SR) samples = CAP_SR;
    atomic_store_explicit(&c->latency, samples, memory_order_relaxed);
}

int cap_set_len_index(cap_t *c, int index) {
    if (!c) return 0;
    if (recording(c)) return index_of_bars(atomic_load_explicit(&c->bars, memory_order_relaxed));
    atomic_store_explicit(&c->bars, bars_of(index), memory_order_relaxed);
    return index_of_bars(atomic_load_explicit(&c->bars, memory_order_relaxed));
}

int cap_set_source(cap_t *c, int index) {
    if (!c) return 0;
    if (index < 0) index = 0;
    if (index > 21) index = 21;
    if (recording(c)) return atomic_load_explicit(&c->source, memory_order_relaxed);
    atomic_store_explicit(&c->source, index, memory_order_relaxed);
    return index;
}

int cap_set_dest(cap_t *c, int index) {
    if (!c) return 0;
    if (index < 0) index = 0;
    if (index > 3) index = 3;
    if (recording(c)) return atomic_load_explicit(&c->dest, memory_order_relaxed);
    atomic_store_explicit(&c->dest, index, memory_order_relaxed);
    return index;
}

int cap_set_auto(cap_t *c, int on) {
    if (!c) return 0;
    atomic_store_explicit(&c->auto_rearm, on ? 1 : 0, memory_order_relaxed);
    return on ? 1 : 0;
}

static void *writer_main(void *arg) {
    cap_t *c = arg;
    for (;;) {
        if (atomic_load_explicit(&c->quit, memory_order_acquire)
            && atomic_load_explicit(&c->job, memory_order_acquire) != 1)
            break;
        if (atomic_load_explicit(&c->job, memory_order_acquire) != 1) {
            struct timespec ts = { 0, 5 * 1000 * 1000 };
            nanosleep(&ts, NULL);
            continue;
        }
        uint32_t frames = atomic_load_explicit(&c->filled, memory_order_acquire);
        int bpm = atomic_load_explicit(&c->bpm_milli, memory_order_acquire);
        int bars = c->job_bars;
        char dir[CAP_PATH];
        char tag[8];
        snprintf(dir, sizeof dir, "%s", c->job_dir);
        snprintf(tag, sizeof tag, "%s", c->job_tag);
        int rc = -1;
        char path[CAP_PATH];
        path[0] = '\0';
        if (frames > 0 && dir[0] && bpm > 0 && bars > 0) {
            if (mkdir(dir, 0755) != 0 && errno != EEXIST) rc = -1;
            else {
                time_t now = time(NULL);
                struct tm tm;
                localtime_r(&now, &tm);
                char stamp[32];
                strftime(stamp, sizeof stamp, "%Y%m%d-%H%M", &tm);
                int bpm_i = (bpm + 500) / 1000;
                int bars_i = bars;
                if (bpm_i < 1) bpm_i = 1;
                if (bpm_i > 999) bpm_i = 999;
                if (bars_i < 1) bars_i = 1;
                if (bars_i > 16) bars_i = 16;
                size_t dl = strlen(dir);
                int named = 0;
                for (int n = 0; n < 1000 && !named; n++) {
                    char leaf[64];
                    int nw = n == 0
                        ? snprintf(leaf, sizeof leaf, "%s_%dbpm_%dbars_%s.wav", tag, bpm_i, bars_i, stamp)
                        : snprintf(leaf, sizeof leaf, "%s_%dbpm_%dbars_%s-%d.wav", tag, bpm_i, bars_i, stamp, n + 1);
                    if (nw < 1 || nw >= (int)sizeof leaf || dl + 1u + (size_t)nw >= sizeof path) {
                        rc = -1;
                        break;
                    }
                    memcpy(path, dir, dl);
                    path[dl] = '/';
                    memcpy(path + dl + 1, leaf, (size_t)nw + 1u);
                    if (access(path, F_OK) != 0) named = 1;
                }
                if (!named) rc = -1;
                FILE *f = !named ? NULL : fopen(path, "wb");
                if (!f) rc = -1;
                else {
                    uint32_t data_bytes = frames * 6u;
                    uint8_t hdr[44];
                    memset(hdr, 0, sizeof hdr);
                    memcpy(hdr, "RIFF", 4);
                    hdr[4] = (uint8_t)(36u + data_bytes);
                    hdr[5] = (uint8_t)((36u + data_bytes) >> 8);
                    hdr[6] = (uint8_t)((36u + data_bytes) >> 16);
                    hdr[7] = (uint8_t)((36u + data_bytes) >> 24);
                    memcpy(hdr + 8, "WAVEfmt ", 8);
                    hdr[16] = 16;
                    hdr[20] = 1;
                    hdr[22] = 2;
                    uint32_t rate = c->job_rate >= 8000 ? (uint32_t)c->job_rate : (uint32_t)CAP_SR;
                    uint32_t byterate = rate * 6u;
                    hdr[24] = (uint8_t)rate; hdr[25] = (uint8_t)(rate >> 8);
                    hdr[26] = (uint8_t)(rate >> 16); hdr[27] = (uint8_t)(rate >> 24);
                    hdr[28] = (uint8_t)byterate; hdr[29] = (uint8_t)(byterate >> 8);
                    hdr[30] = (uint8_t)(byterate >> 16); hdr[31] = (uint8_t)(byterate >> 24);
                    hdr[32] = 6;
                    hdr[34] = 24;
                    memcpy(hdr + 36, "data", 4);
                    hdr[40] = (uint8_t)data_bytes; hdr[41] = (uint8_t)(data_bytes >> 8);
                    hdr[42] = (uint8_t)(data_bytes >> 16); hdr[43] = (uint8_t)(data_bytes >> 24);
                    rc = fwrite(hdr, 1, 44, f) == 44 ? 0 : -1;
                    const int16_t *p = c->buf;
                    uint32_t left = frames;
                    uint8_t chunk[6 * 256];
                    while (rc == 0 && left) {
                        uint32_t n = left > 256 ? 256 : left;
                        for (uint32_t i = 0; i < n; i++) {
                            for (int ch = 0; ch < 2; ch++) {
                                int32_t s = (int32_t)p[i * 2 + ch] << 8;
                                uint8_t *d = chunk + (i * 2 + ch) * 3;
                                d[0] = (uint8_t)s;
                                d[1] = (uint8_t)(s >> 8);
                                d[2] = (uint8_t)(s >> 16);
                            }
                        }
                        if (fwrite(chunk, 6, n, f) != n) rc = -1;
                        p += n * 2;
                        left -= n;
                    }
                    if (fclose(f) != 0) rc = -1;
                    if (rc != 0) unlink(path);
                }
            }
        }
        if (rc == 0) {
            snprintf(c->file, sizeof c->file, "%s", path);
            atomic_store_explicit(&c->err, CAP_ERR_NONE, memory_order_relaxed);
            atomic_store_explicit(&c->status, CAP_SAVED, memory_order_release);
            if (atomic_load_explicit(&c->auto_rearm, memory_order_relaxed))
                atomic_store_explicit(&c->rearm, 1, memory_order_release);
            else
                atomic_store_explicit(&c->rearm, 0, memory_order_release);
        } else {
            atomic_store_explicit(&c->err, CAP_ERR_DISK, memory_order_relaxed);
            atomic_store_explicit(&c->status, CAP_ERROR, memory_order_release);
            atomic_store_explicit(&c->rearm, 0, memory_order_release);
        }
        atomic_store_explicit(&c->mode, ST_IDLE, memory_order_release);
        atomic_store_explicit(&c->job, 0, memory_order_release);
    }
    return NULL;
}

static int ensure_thread(cap_t *c) {
    if (c->thread_on) return 0;
    atomic_store_explicit(&c->quit, 0, memory_order_relaxed);
    if (pthread_create(&c->thread, NULL, writer_main, c) != 0) return -1;
    c->thread_on = 1;
    return 0;
}

int cap_arm(cap_t *c) {
    if (!c) return -1;
    int mode = atomic_load_explicit(&c->mode, memory_order_acquire);
    if (mode == ST_ARMED || mode == ST_DELAY || mode == ST_REC || mode == ST_SAVING) return 0;
    char dir[CAP_PATH];
    if (resolve_dir(c, dir, sizeof dir) != 0) {
        set_err(c, CAP_ERR_FOLDER);
        return -1;
    }
    ensure_folder(c);
    if (!c->buf) {
        c->buf = malloc((size_t)CAP_MAX_FRAMES * 2u * sizeof(int16_t));
        if (!c->buf) {
            set_err(c, CAP_ERR_DISK);
            return -1;
        }
    }
    if (ensure_thread(c) != 0) {
        set_err(c, CAP_ERR_DISK);
        return -1;
    }
    snprintf(c->job_dir, sizeof c->job_dir, "%s", dir);
    snprintf(c->job_tag, sizeof c->job_tag, "%s", c->tag[0] ? c->tag : "EL");
    c->job_bars = atomic_load_explicit(&c->bars, memory_order_relaxed);
    c->file[0] = '\0';
    c->keep = 0;
    c->skip = 0;
    c->rec_left = 0;
    c->delay_left = 0;
    c->last_valid = 0;
    atomic_store_explicit(&c->filled, 0, memory_order_relaxed);
    atomic_store_explicit(&c->rearm, 0, memory_order_relaxed);
    atomic_store_explicit(&c->err, CAP_ERR_NONE, memory_order_relaxed);
    atomic_store_explicit(&c->mode, ST_ARMED, memory_order_release);
    atomic_store_explicit(&c->status, CAP_ARMED, memory_order_release);
    return 1;
}

void cap_cancel(cap_t *c) {
    if (!c) return;
    int mode = atomic_load_explicit(&c->mode, memory_order_acquire);
    if (mode == ST_SAVING || mode == ST_IDLE) return;
    c->keep = 0;
    c->rec_left = 0;
    c->delay_left = 0;
    c->last_valid = 0;
    atomic_store_explicit(&c->rearm, 0, memory_order_relaxed);
    atomic_store_explicit(&c->mode, ST_IDLE, memory_order_release);
    atomic_store_explicit(&c->status, CAP_CANCELLED, memory_order_release);
}

cap_t *cap_create(const char *data_dir) {
    cap_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    atomic_store_explicit(&c->bars, 4, memory_order_relaxed);
    atomic_store_explicit(&c->latency, CAP_LATENCY_SAMPLES, memory_order_relaxed);
    atomic_store_explicit(&c->status, CAP_IDLE, memory_order_relaxed);
    apply_tag(c, "EL");
    c->rate = CAP_SR;
    c->job_rate = CAP_SR;
    cap_set_drive(c, data_dir);
    ensure_folder(c);
    return c;
}

void cap_destroy(cap_t *c) {
    if (!c) return;
    atomic_store_explicit(&c->mode, ST_IDLE, memory_order_release);
    if (c->thread_on) {
        atomic_store_explicit(&c->quit, 1, memory_order_release);
        pthread_join(c->thread, NULL);
        c->thread_on = 0;
    }
    free(c->buf);
    free(c);
}

static int on_bar_line(double ppq, double bar, double tol) {
    if (!(bar > 0)) return 0;
    double m = fmod(ppq, bar);
    if (m < 0) m += bar;
    if (m > bar * 0.5) m = bar - m;
    return m <= tol;
}

static int boundary_sample(const cap_time_t *t) {
    int numer = t->numer >= 1 ? t->numer : 4;
    int denom = t->denom >= 1 ? t->denom : 4;
    double bar = (double)numer * 4.0 / (double)denom;
    if (!(bar > 0) || !(t->bpm > 0)) return -1;
    double next;
    if (t->bar_start >= 0) {
        next = t->bar_start;
        while (next + 1e-9 < t->ppq) next += bar;
    } else {
        double bs = floor(t->ppq / bar) * bar;
        next = (t->ppq <= bs + 1e-12) ? bs : bs + bar;
    }
    double i = (next - t->ppq) * 60.0 * (double)CAP_SR / t->bpm;
    if (i < -0.5 || i >= (double)t->frames) return -1;
    int s = (int)llround(i);
    if (s < 0 || s >= t->frames) return -1;
    return s;
}

static void note_block(cap_t *c, const cap_time_t *t) {
    c->last_ppq = t->ppq;
    c->last_bpm = t->bpm;
    c->last_frames = t->frames;
    c->last_valid = 1;
}

static int is_wrap(const cap_t *c, const cap_time_t *t, double hop) {
    double sample_ppq = t->bpm / (60.0 * (double)CAP_SR);
    double last_end = c->last_ppq + (double)c->last_frames * c->last_bpm / (60.0 * (double)CAP_SR);
    if (!(hop < -sample_ppq)) return 0;
    if (t->cycle && t->cycle_end > t->cycle_start) {
        double tol_e = sample_ppq * (double)(c->last_frames + 2);
        double tol_s = sample_ppq * (double)(t->frames + 2);
        return fabs(t->ppq - t->cycle_start) <= tol_s
               && fabs(last_end - t->cycle_end) <= tol_e;
    }
    int numer = t->numer >= 1 ? t->numer : 4;
    int denom = t->denom >= 1 ? t->denom : 4;
    double bar = (double)numer * 4.0 / (double)denom;
    if (hop > -bar * 0.5) return 0;
    /* The next block can start up to one block past the bar it wrapped to. */
    double tol = sample_ppq * (double)(c->last_frames > t->frames ? c->last_frames : t->frames);
    tol += sample_ppq * 2.0;
    return on_bar_line(t->ppq, bar, tol) && on_bar_line(last_end, bar, tol);
}

static int begin_take(cap_t *c, const cap_time_t *t, int boundary) {
    int numer = t->numer >= 1 ? t->numer : 4;
    int denom = t->denom >= 1 ? t->denom : 4;
    int bars = atomic_load_explicit(&c->bars, memory_order_relaxed);
    int rate = c->rate >= 8000 ? c->rate : CAP_SR;
    long long len = length_at(bars, numer, denom, t->bpm, rate);
    if (len < 1 || len > CAP_MAX_FRAMES) {
        set_err(c, CAP_ERR_LONG);
        return 0;
    }
    int latency = atomic_load_explicit(&c->latency, memory_order_relaxed);
    c->rec_bpm = t->bpm;
    c->lat_num = numer;
    c->lat_den = denom;
    c->job_bars = bars;
    c->job_rate = rate;
    atomic_store_explicit(&c->show_bars, bars, memory_order_relaxed);
    atomic_store_explicit(&c->total, (unsigned)len, memory_order_relaxed);
    atomic_store_explicit(&c->bar_frames,
                          (unsigned)length_at(1, numer, denom, t->bpm, rate),
                          memory_order_relaxed);
    atomic_store_explicit(&c->bpm_milli, (int)llround(t->bpm * 1000.0), memory_order_relaxed);
    atomic_store_explicit(&c->filled, 0, memory_order_relaxed);
    c->rec_left = (int)len;
    int start = boundary + latency;
    if (start < t->frames) {
        c->skip = start;
        c->keep = t->frames - start;
        if (c->keep > c->rec_left) c->keep = c->rec_left;
        c->delay_left = 0;
        atomic_store_explicit(&c->mode, ST_REC, memory_order_release);
    } else {
        c->skip = 0;
        c->keep = 0;
        c->delay_left = start - t->frames;
        atomic_store_explicit(&c->mode, ST_DELAY, memory_order_release);
    }
    atomic_store_explicit(&c->status, CAP_RECORDING, memory_order_release);
    return 1;
}

void cap_transport(cap_t *c, const cap_time_t *t) {
    if (!c || !t) return;
    int mode = atomic_load_explicit(&c->mode, memory_order_acquire);
    c->skip = 0;
    c->keep = 0;
    if (mode == ST_SAVING) return;
    if (mode == ST_IDLE) {
        if (atomic_exchange_explicit(&c->rearm, 0, memory_order_acq_rel)) {
            c->last_valid = 0;
            atomic_store_explicit(&c->mode, ST_ARMED, memory_order_release);
            atomic_store_explicit(&c->status, CAP_ARMED, memory_order_release);
            mode = ST_ARMED;
        } else {
            return;
        }
    }
    if (mode != ST_ARMED && mode != ST_DELAY && mode != ST_REC) return;

    int playing = t->playing && t->bpm > 0.0 && t->ppq >= 0.0 && t->frames > 0;
    if (!playing) {
        if (mode == ST_DELAY || mode == ST_REC) set_err(c, CAP_ERR_STOP);
        else c->last_valid = 0;
        return;
    }

    if (c->last_valid) {
        double expect = c->last_ppq + (double)c->last_frames * c->last_bpm
                        / (60.0 * (double)CAP_SR);
        double hop = t->ppq - expect;
        /* The MPC's playhead wanders by about a thousandth of a beat every
         * block, and a tempo nudge can tug it back by a few hundredths.
         * A step inside one beat is still this playhead. A bigger step is
         * a real jump, unless the sequence just looped back to a bar. */
        if (fabs(hop) > 1.0) {
            if (!is_wrap(c, t, hop)) {
                if (mode == ST_ARMED) c->last_valid = 0;
                else {
                    set_err(c, CAP_ERR_MOVE);
                    return;
                }
            }
        }
    }

    if (mode == ST_DELAY || mode == ST_REC) {
        if (fabs(t->bpm - c->rec_bpm) > 0.01) {
            set_err(c, CAP_ERR_TEMPO);
            return;
        }
        int numer = t->numer >= 1 ? t->numer : 4;
        int denom = t->denom >= 1 ? t->denom : 4;
        if (numer != c->lat_num || denom != c->lat_den) {
            set_err(c, CAP_ERR_METER);
            return;
        }
    }

    if (mode == ST_ARMED) {
        int b = boundary_sample(t);
        if (b >= 0 && !begin_take(c, t, b)) return;
    } else if (mode == ST_DELAY) {
        if (c->delay_left >= t->frames) {
            c->delay_left -= t->frames;
        } else {
            c->skip = c->delay_left;
            c->keep = t->frames - c->delay_left;
            if (c->keep > c->rec_left) c->keep = c->rec_left;
            c->delay_left = 0;
            atomic_store_explicit(&c->mode, ST_REC, memory_order_release);
        }
    } else if (mode == ST_REC) {
        c->skip = 0;
        c->keep = t->frames;
        if (c->keep > c->rec_left) c->keep = c->rec_left;
    }
    note_block(c, t);
}

void cap_audio(cap_t *c, const int16_t *lr, int frames) {
    if (!c || !lr || frames < 1) return;
    if (atomic_load_explicit(&c->mode, memory_order_acquire) != ST_REC) return;
    int skip = c->skip;
    int keep = c->keep;
    c->keep = 0;
    if (keep < 1 || skip >= frames) return;
    if (skip < 0) skip = 0;
    if (skip + keep > frames) keep = frames - skip;
    cap_write(c, lr + (size_t)skip * 2, keep);
}

void cap_take_span(cap_t *c, int frames, int *skip, int *keep) {
    if (skip) *skip = 0;
    if (keep) *keep = 0;
    if (!c || frames < 1) return;
    if (atomic_load_explicit(&c->mode, memory_order_acquire) != ST_REC) return;
    int s = c->skip;
    int k = c->keep;
    if (s < 0) s = 0;
    if (k < 0) k = 0;
    if (s >= frames) {
        c->skip = s - frames;
        return;
    }
    int use = frames - s;
    if (use > k) use = k;
    if (skip) *skip = s;
    if (keep) *keep = use;
    c->skip = 0;
    c->keep = k - use;
}

void cap_write(cap_t *c, const int16_t *lr, int frames) {
    if (!c || !lr || frames < 1) return;
    if (atomic_load_explicit(&c->mode, memory_order_acquire) != ST_REC) return;
    uint32_t filled = atomic_load_explicit(&c->filled, memory_order_relaxed);
    uint32_t total = atomic_load_explicit(&c->total, memory_order_relaxed);
    if (filled >= total) return;
    if (filled + (uint32_t)frames > total) frames = (int)(total - filled);
    if (frames < 1 || !c->buf) return;
    memcpy(c->buf + (size_t)filled * 2u, lr, (size_t)frames * 2u * sizeof(int16_t));
    filled += (uint32_t)frames;
    c->rec_left -= frames;
    atomic_store_explicit(&c->filled, filled, memory_order_release);
    if (c->rec_left < 1 || filled >= total) {
        atomic_store_explicit(&c->mode, ST_SAVING, memory_order_release);
        atomic_store_explicit(&c->job, 1, memory_order_release);
    }
}

int cap_keep_count(const cap_t *c) { return c ? c->keep : 0; }
int cap_source(const cap_t *c) {
    return c ? atomic_load_explicit(&c->source, memory_order_relaxed) : 0;
}
int cap_len_index(const cap_t *c) {
    if (!c) return 2;
    return index_of_bars(atomic_load_explicit(&c->bars, memory_order_relaxed));
}
int cap_dest(const cap_t *c) {
    return c ? atomic_load_explicit(&c->dest, memory_order_relaxed) : 0;
}
int cap_auto(const cap_t *c) {
    return c ? atomic_load_explicit(&c->auto_rearm, memory_order_relaxed) : 0;
}
int cap_arm_on(const cap_t *c) {
    if (!c) return 0;
    int m = atomic_load_explicit(&c->mode, memory_order_acquire);
    return m == ST_ARMED || m == ST_DELAY || m == ST_REC;
}
int cap_code(const cap_t *c) {
    return c ? atomic_load_explicit(&c->status, memory_order_acquire) : CAP_IDLE;
}

int cap_status(const cap_t *c, char *buf, int n) {
    if (!c || !buf || n < 2) return 0;
    int st = atomic_load_explicit(&c->status, memory_order_acquire);
    int err = atomic_load_explicit(&c->err, memory_order_relaxed);
    if (st == CAP_ARMED) return snprintf(buf, n, "Armed");
    if (st == CAP_RECORDING) {
        unsigned one = atomic_load_explicit(&c->bar_frames, memory_order_relaxed);
        unsigned filled = atomic_load_explicit(&c->filled, memory_order_relaxed);
        unsigned bars = (unsigned)atomic_load_explicit(&c->show_bars, memory_order_relaxed);
        if (bars < 1) bars = (unsigned)atomic_load_explicit(&c->bars, memory_order_relaxed);
        unsigned cur = 1;
        if (one > 0) cur = filled / one + 1;
        if (cur < 1) cur = 1;
        if (cur > bars) cur = bars;
        return snprintf(buf, n, "Recording %u/%u", cur, bars);
    }
    if (st == CAP_SAVED) return snprintf(buf, n, "Saved");
    if (st == CAP_CANCELLED) return snprintf(buf, n, "Cancelled");
    if (st == CAP_ERROR) {
        const char *why = "could not save";
        if (err == CAP_ERR_TEMPO) why = "tempo changed";
        else if (err == CAP_ERR_STOP) why = "transport stopped";
        else if (err == CAP_ERR_MOVE) why = "transport moved";
        else if (err == CAP_ERR_METER) why = "meter changed";
        else if (err == CAP_ERR_LONG) why = "too long";
        else if (err == CAP_ERR_FOLDER) why = "no folder";
        return snprintf(buf, n, "Error: %s", why);
    }
    return snprintf(buf, n, "Idle");
}

int cap_path(const cap_t *c, char *buf, int n) {
    if (!c || !buf || n < 2) return 0;
    char dir[CAP_PATH];
    if (resolve_dir(c, dir, sizeof dir) != 0) return snprintf(buf, n, "not plugged in");
    return snprintf(buf, n, "%s", dir);
}

int cap_wait(cap_t *c, int ms) {
    if (!c) return CAP_IDLE;
    int step = 5;
    for (int waited = 0; waited <= ms; waited += step) {
        int st = atomic_load_explicit(&c->status, memory_order_acquire);
        int mode = atomic_load_explicit(&c->mode, memory_order_acquire);
        if (st == CAP_SAVED || st == CAP_ERROR || st == CAP_CANCELLED) {
            if (mode != ST_SAVING) return st;
        }
        struct timespec ts = { 0, step * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return atomic_load_explicit(&c->status, memory_order_acquire);
}

const char *cap_last_file(const cap_t *c) {
    if (!c || !c->file[0]) return "";
    return c->file;
}
