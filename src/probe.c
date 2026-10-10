/* Tributary: sum chosen Overbridge outputs onto the MPC track this plugin sits on.
 *
 * Copyright (C) 2026 Stampman3000
 * This program is free software under the GNU General Public License, version 3.
 * See the LICENSE file. Device ids and the channel maps are from Overwitch
 * (https://github.com/dagargo/overwitch), Copyright David García Goñi, also GPL-3.
 * No Overwitch source is copied into this file. See src/devices.h.
 *
 * Digitone II is the machine this has been heard on. Loading does not claim USB
 * and does not set configuration 1. ACTIVE claims interface 1 and interface 2,
 * switches both to alt setting 3, and leaves interface 4 (USB audio control) and
 * interface 5 (MIDI) on their kernel drivers. Dropping the MIDI driver recreates
 * the sound card: the MPC still lists the port, but the sequencer link drops, so
 * play and clock never arrive. Loading the plugin wires that link again. One
 * thread sends silent 4-block bundles out of endpoint 0x03 and reads endpoint
 * 0x83. It decodes only the sources the open copies are actually playing. Finished
 * transfers are collected directly: on this MPC, poll() does not wake when one
 * completes. It never resets the device. Stopping puts both interfaces back to
 * alt 0 and releases them. The card log is written when a session fails to open,
 * and when the one-second check runs. A healthy stream does not keep writing.
 */
#include <stdatomic.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sound/asequencer.h>
#include <time.h>
#include <unistd.h>
#include <linux/usbdevice_fs.h>
#include "devices.h"

/* The transfer structs are the ones in <linux/usbdevice_fs.h>. Their buffer
 * pointers are native pointers, so a 64-bit build is not a 32-bit layout with
 * the sizes written out by hand. */
_Static_assert(sizeof(((struct usbdevfs_urb *)0)->buffer) == sizeof(void *),
               "usbdevfs_urb.buffer must be a native pointer");
_Static_assert(sizeof(((struct usbdevfs_ctrltransfer *)0)->data) == sizeof(void *),
               "usbdevfs_ctrltransfer.data must be a native pointer");
_Static_assert(sizeof(((struct usbdevfs_ioctl *)0)->data) == sizeof(void *),
               "usbdevfs_ioctl.data must be a native pointer");

static int repair_seq_link(void);

#ifndef OP_SYSFS_USB
#define OP_SYSFS_USB "/sys/bus/usb/devices"
#endif
#ifndef OP_DEV_USB
#define OP_DEV_USB "/dev/bus/usb"
#endif
#ifndef OP_PROC_USB
#define OP_PROC_USB "/proc/bus/usb"
#endif
#ifndef OP_LOG_PATHS
#define OP_LOG_PATHS "/media/EOS_DIGITAL/tributary.log", "/sdcard/tributary.log"
#endif

#define ELEKTRON_VID 0x1935
#define IF_AUDIO_IN 1
#define IF_AUDIO_OUT 2
#define IF_CONTROL 4
#define IF_MIDI 5
#define LOG_CAP (96 * 1024)
#define TXT 24
#define PATH_CAP 1024

#define HEAR_PAIRS 21
#define HEAR_CAP 16384
#define HEAR_MASK (HEAR_CAP - 1)
/* Digitone is 48 kHz. The MPC asks for 44.1 kHz. This step walks the Digitone
 * stream at that ratio: one MPC sample takes a bit more than one Digitone sample.
 * Playback sits HEAR_LAG frames behind the newest sample, about 3.7 ms.
 * A USB bundle is 4 blocks, about 0.6 ms, so the cushion can sit closer than
 * it could when a bundle was 3.5 ms. The Digitone clock and the MPC clock
 * are not the same, so the cushion slowly grows. Past HEAR_LAG + HEAR_NUDGE
 * the read walks a hair faster (about one cent) until it is back. A real
 * stall, further behind than HEAR_LAG * 2, still jumps forward. */
#define HEAR_STEP ((48000u * 65536u) / 44100u)
#define HEAR_LAG 176
#define HEAR_NUDGE 40

typedef struct {
    pthread_mutex_t lock;
    int runs;
    atomic_uint mix; /* one bit per switch; set bits are summed onto this track */
    int active;
    uint32_t used; /* bits this copy has asked the reader to decode */
    uint32_t rpos, rfrac;
    int primed;
    int saw; /* this copy has played from the shared read */
    int saw_source; /* an old project set SOURCE; switch defaults must not add Main on top */
    int switches_live; /* host audio has started, so a switch move is the player, not a default */
    char v[TXT], dev[TXT], claim[TXT], rel[TXT], mid[TXT], logst[TXT];
} probe_t;

/* One reader for the whole process. Instances only pick a pair. */
static int16_t g_ring[HEAR_CAP][HEAR_PAIRS * 2];
static atomic_uint g_w;
static atomic_int g_stop;
static atomic_int g_exited;
static atomic_int g_alive;
static pthread_t g_th;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static probe_t *g_owner;
static int g_instances;
static atomic_int g_use[TB_MAX];
static atomic_int g_lamp; /* 0 red, 1 amber, 2 green */
static const tb_dev *g_dev; /* null until a session names the device; null means Digitone II */
static atomic_uint g_rev; /* bumps when the connected machine changes, so switch names redraw */

typedef struct { char *buf; size_t len; } plog_t;

typedef struct {
    int rank;
    int vid, pid, bus, addr;
    char name[256];
} usbdev_t;

static void plog(plog_t *L, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(L->buf + L->len, LOG_CAP - L->len, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    size_t room = LOG_CAP - L->len;
    L->len += ((size_t)n < room) ? (size_t)n : room - 1;
    if (L->len < LOG_CAP - 1) L->buf[L->len++] = '\n';
    L->buf[L->len] = '\0';
}

static int read_attr(const char *dir, const char *attr, char *out, size_t n) {
    char p[PATH_CAP];
    snprintf(p, sizeof p, "%s/%s", dir, attr);
    FILE *f = fopen(p, "r");
    out[0] = '\0';
    if (f) {
        if (!fgets(out, (int)n, f)) out[0] = '\0';
        fclose(f);
    }
    size_t k = strlen(out);
    while (k && (out[k - 1] == '\n' || out[k - 1] == ' ')) out[--k] = '\0';
    if (!out[0]) snprintf(out, n, "-");
    return f ? 0 : -1;
}

static long attr_num(const char *dir, const char *attr, int base) {
    char s[64];
    if (read_attr(dir, attr, s, sizeof s) != 0) return -1;
    return strtol(s, NULL, base);
}

static void driver_of(const char *dir, char *out, size_t n) {
    char p[PATH_CAP], target[PATH_CAP];
    snprintf(p, sizeof p, "%s/driver", dir);
    ssize_t k = readlink(p, target, sizeof target - 1);
    if (k < 0) { snprintf(out, n, "none"); return; }
    target[k] = '\0';
    const char *base = strrchr(target, '/');
    const char *name = base ? base + 1 : target;
    size_t m = strlen(name);
    if (m > n - 1) m = n - 1;
    memcpy(out, name, m);
    out[m] = '\0';
}

static const tb_dev *tb_current(void) {
    const tb_dev *d = g_dev;
    return d ? d : tb_by_pid(0x0B34);
}

static int src_lim(void) {
    const tb_dev *d = tb_current();
    int n = d ? d->nsrc : 1;
    if (n < 1) n = 1;
    if (n > TB_MAX) n = TB_MAX;
    return n;
}

static const char *src_name(int idx) {
    const tb_dev *d = tb_current();
    if (!d || d->nsrc < 1) return "main";
    if (idx < 0) idx = 0;
    if (idx >= d->nsrc) idx = d->nsrc - 1;
    return d->src[idx].name;
}

/* Short label for one switch, from this machine's own output name.
 * Digitone II (and Digitakt II, the same map) stays MAIN, 1–16, DLY, REV, CHO, IN.
 * A switch this machine does not have is a blank. */
static void src_tag(int idx, char *buf, int n) {
    const tb_dev *d = tb_current();
    const char *name;
    int i;
    if (!d || idx < 0 || idx >= d->nsrc || n < 2) {
        if (n > 0) snprintf(buf, (size_t)n, " ");
        return;
    }
    name = d->src[idx].name;
    if (!strcmp(name, "main")) { snprintf(buf, (size_t)n, "MAIN"); return; }
    if (!strncmp(name, "track ", 6)) { snprintf(buf, (size_t)n, "%s", name + 6); return; }
    if (!strncmp(name, "synth track ", 12)) { snprintf(buf, (size_t)n, "S%s", name + 12); return; }
    if (!strcmp(name, "delay")) { snprintf(buf, (size_t)n, "DLY"); return; }
    if (!strcmp(name, "reverb")) { snprintf(buf, (size_t)n, "REV"); return; }
    if (!strcmp(name, "chorus")) { snprintf(buf, (size_t)n, "CHO"); return; }
    if (!strcmp(name, "input")) { snprintf(buf, (size_t)n, "IN"); return; }
    if (!strcmp(name, "fx return")) { snprintf(buf, (size_t)n, "FX"); return; }
    if (!strcmp(name, "analog fx")) { snprintf(buf, (size_t)n, "AFX"); return; }
    if (!strcmp(name, "delay/reverb")) { snprintf(buf, (size_t)n, "D/R"); return; }
    for (i = 0; name[i] && i < n - 1 && i < 6; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        buf[i] = c;
    }
    buf[i] = '\0';
}

/* Digitone II ranks above the other Overbridge machines, which rank above the
 * Audio/MIDI and MIDI-only ids. Those last two are not in the device table:
 * the one-second check still recognises them, and ACTIVE does not claim them. */
static int d2_rank(int vid, int pid) {
    if (vid != ELEKTRON_VID) return 0;
    if (tb_by_pid(pid)) return pid == 0x0B34 ? 3 : 2;
    if (pid == 0x1034) return 2;
    if (pid == 0x0134) return 1;
    return 0;
}

static const char *errname(int e) {
    switch (e) {
    case EPERM: return "EPERM";
    case EACCES: return "EACCES";
    case ENOENT: return "ENOENT";
    case ENXIO: return "ENXIO";
    case ENODEV: return "ENODEV";
    case EBUSY: return "EBUSY";
    case EINVAL: return "EINVAL";
    case ENOTTY: return "ENOTTY";
    case ENODATA: return "ENODATA";
    case EIO: return "EIO";
    case ENOMEM: return "ENOMEM";
    case ETIMEDOUT: return "ETIMEDOUT";
    case EPIPE: return "EPIPE";
    case EAGAIN: return "EAGAIN";
    default: return "other";
    }
}

static void log_endpoints(plog_t *L, const char *ifdir) {
    DIR *d = opendir(ifdir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "ep_", 3)) continue;
        char dir[2 * PATH_CAP], addr[16], type[16], dirn[16], mps[16];
        snprintf(dir, sizeof dir, "%s/%s", ifdir, e->d_name);
        read_attr(dir, "bEndpointAddress", addr, sizeof addr);
        read_attr(dir, "type", type, sizeof type);
        read_attr(dir, "direction", dirn, sizeof dirn);
        read_attr(dir, "wMaxPacketSize", mps, sizeof mps);
        plog(L, "        endpoint 0x%s  %s %s  max %s", addr, dirn, type, mps);
    }
    closedir(d);
}

static int has_endpoint(const char *ifdir, const char *addr_want) {
    DIR *d = opendir(ifdir);
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        if (strncmp(e->d_name, "ep_", 3)) continue;
        char dir[2 * PATH_CAP], addr[16];
        snprintf(dir, sizeof dir, "%s/%s", ifdir, e->d_name);
        read_attr(dir, "bEndpointAddress", addr, sizeof addr);
        if (!strcmp(addr, addr_want)) found = 1;
    }
    closedir(d);
    return found;
}

static void log_if_state(plog_t *L, const char *ifdir, const char *tag) {
    char alt[16], nep[16];
    read_attr(ifdir, "bAlternateSetting", alt, sizeof alt);
    read_attr(ifdir, "bNumEndpoints", nep, sizeof nep);
    plog(L, "-- %s: alt setting %s, endpoint count %s", tag, alt, nep);
    log_endpoints(L, ifdir);
}

/* One Overbridge block from the device: 2 byte header, 2 byte counter, 28 private
 * bytes, then 7 frames. Digitone II frames are 140 bytes, so a block is 1012 bytes.
 * The header on packets coming from the device is 0x0700, and the counter rises by 7. */
#define OP_PKT_BYTES 1012
#define OP_FRAME_BYTES 140
#define OP_FRAMES 7
#define OP_HDR_MARK 0x0700
#define OP_BURST 8
#define OP_Q 8
#define OP_BUNDLE 4
#define OP_LISTEN_MS 1000
#define OP_CHANS 14 /* main L/R plus tracks 1..6, each stereo, 4 bytes */
/* One silent block towards the device: same 32 byte head, then 7 frames of 32 bytes. */
#define OP_OUT_BYTES 256
#define OP_OUT_HDR 0x07ff

struct burst_view {
    int n, bad_len, header, headers_same, counter_step, peak;
};

static uint16_t load_be16(const unsigned char *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static int32_t load_be32(const unsigned char *p) {
    return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                     ((uint32_t)p[2] << 8) | p[3]);
}

static void view_burst(const unsigned char *pkts, const int *lens, int n, struct burst_view *v) __attribute__((unused));
static void view_burst(const unsigned char *pkts, const int *lens, int n, struct burst_view *v) {
    memset(v, 0, sizeof *v);
    v->n = n;
    v->header = -1;
    v->headers_same = 1;
    v->counter_step = n >= 2;
    uint16_t prev = 0;
    for (int i = 0; i < n; i++) {
        const unsigned char *p = pkts + (size_t)i * OP_PKT_BYTES;
        if (lens[i] != OP_PKT_BYTES) {
            v->bad_len++;
            v->counter_step = 0;
            continue;
        }
        uint16_t hdr = load_be16(p);
        uint16_t ctr = load_be16(p + 2);
        if (v->header < 0) v->header = hdr;
        else if (hdr != (uint16_t)v->header) v->headers_same = 0;
        if (i > 0 && (uint16_t)(prev + 7) != ctr) v->counter_step = 0;
        prev = ctr;
        for (int f = 0; f < OP_FRAMES; f++) {
            int32_t s = load_be32(p + 32 + (size_t)f * OP_FRAME_BYTES);
            int a = s < 0 ? (s == INT32_MIN ? INT32_MAX : -s) : s;
            if (a > v->peak) v->peak = a;
        }
    }
}

static void fill_out_n(unsigned char *dst, uint16_t counter, int nbytes) {
    if (nbytes < 4) nbytes = 4;
    memset(dst, 0, (size_t)nbytes);
    dst[0] = (unsigned char)(OP_OUT_HDR >> 8);
    dst[1] = (unsigned char)(OP_OUT_HDR & 0xff);
    dst[2] = (unsigned char)(counter >> 8);
    dst[3] = (unsigned char)(counter & 0xff);
}

static void fill_out_block(unsigned char *dst, uint16_t counter) {
    fill_out_n(dst, counter, OP_OUT_BYTES);
}

/* Drop queued transfers. poll() does not wake on this MPC, so reap directly. */
static void cancel_live(int fd, struct usbdevfs_urb *urbs, int *live, int n) {
    for (int i = 0; i < n; i++) if (live[i]) ioctl(fd, USBDEVFS_DISCARDURB, &urbs[i]);
    for (int spin = 0; spin < 30; spin++) {
        struct usbdevfs_urb *done = NULL;
        int left = 0;
        if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &done) == 0 && done) {
            for (int i = 0; i < n; i++) if (done == &urbs[i]) live[i] = 0;
        }
        for (int i = 0; i < n; i++) left += live[i];
        if (!left) return;
        struct timespec pause = { 0, 1000000 };
        nanosleep(&pause, NULL);
    }
}

static int submit_urb(int fd, struct usbdevfs_urb *urb, unsigned char ep, void *buf, int len) {
    memset(urb, 0, sizeof *urb);
    urb->type = USBDEVFS_URB_TYPE_INTERRUPT;
    urb->endpoint = ep;
    urb->buffer = buf;
    urb->buffer_length = len;
    if (ioctl(fd, USBDEVFS_SUBMITURB, urb) < 0) return errno ? errno : EIO;
    return 0;
}

/* Overbridge 2.1 stores a 24-bit sample in a 4-byte slot. The sample is bytes 1..3,
 * big-endian. A 3-byte slot is the same 24-bit sample with no pad byte. Adapted from
 * Overwitch engine.c, Copyright (C) 2019 Stefan Rehm and Copyright (C) 2021 David
 * García Goñi. The top 16 bits are what the MPC track can hold. */
static int16_t ob_i16(const unsigned char *s) {
    int v = (s[1] << 16) | (s[2] << 8) | s[3];
    if (v & 0x800000) v -= 0x1000000;
    return (int16_t)(v >> 8);
}

static int16_t ob_i16_3(const unsigned char *s) {
    int v = (s[0] << 16) | (s[1] << 8) | s[2];
    if (v & 0x800000) v -= 0x1000000;
    return (int16_t)(v >> 8);
}

/* Digitone II's own map. Tests check delay at byte 116, 3 bytes wide. */
static const char *pair_name(int pair) __attribute__((unused));
static const char *pair_name(int pair) {
    const tb_dev *d = tb_by_pid(0x0B34);
    if (!d || d->nsrc < 1) return "main";
    if (pair < 0) pair = 0;
    if (pair >= d->nsrc) pair = d->nsrc - 1;
    return d->src[pair].name;
}

static void pair_at(int pair, int *off, int *size) __attribute__((unused));
static void pair_at(int pair, int *off, int *size) {
    const tb_dev *d = tb_by_pid(0x0B34);
    if (!d || d->nsrc < 1) { *off = 0; *size = 4; return; }
    if (pair < 0) pair = 0;
    if (pair >= d->nsrc) pair = d->nsrc - 1;
    *off = d->src[pair].off;
    *size = d->src[pair].bytes;
}

static int slot_peak(const unsigned char *s) {
    int hi = (s[0] << 16) | (s[1] << 8) | s[2];
    int lo = (s[1] << 16) | (s[2] << 8) | s[3];
    if (hi & 0x800000) hi -= 0x1000000;
    if (lo & 0x800000) lo -= 0x1000000;
    if (hi < 0) hi = -hi;
    if (lo < 0) lo = -lo;
    return hi > lo ? hi : lo;
}

static const char *chan_name(int ch) {
    static const char *names[OP_CHANS] = {
        "main L", "main R", "t1 L", "t1 R", "t2 L", "t2 R", "t3 L", "t3 R",
        "t4 L", "t4 R", "t5 L", "t5 R", "t6 L", "t6 R"
    };
    return names[ch];
}

struct listen_stats {
    int peak[OP_CHANS];
    int later_peak;
    int nonzero;
    int packets, bad_len, bad_hdr, counter_gaps;
    int header;
    int have_hot;
    unsigned char hot[48];
};

static void note_packet(struct listen_stats *st, const unsigned char *p, int len, int *prev_set, uint16_t *prev) {
    if (len != OP_PKT_BYTES) {
        st->bad_len++;
        return;
    }
    uint16_t hdr = load_be16(p);
    uint16_t ctr = load_be16(p + 2);
    if (st->header < 0) st->header = hdr;
    if (hdr != OP_HDR_MARK) st->bad_hdr++;
    if (*prev_set && (uint16_t)(*prev + 7) != ctr) st->counter_gaps++;
    *prev = ctr;
    *prev_set = 1;
    st->packets++;
    int packet_nz = 0;
    for (int f = 0; f < OP_FRAMES; f++) {
        const unsigned char *fr = p + 32 + (size_t)f * OP_FRAME_BYTES;
        for (int ch = 0; ch < OP_CHANS; ch++) {
            int pk = slot_peak(fr + (size_t)ch * 4);
            if (pk > st->peak[ch]) st->peak[ch] = pk;
        }
        for (int b = OP_CHANS * 4; b + 2 < OP_FRAME_BYTES; b += 3) {
            int v = (fr[b] << 16) | (fr[b + 1] << 8) | fr[b + 2];
            if (v & 0x800000) v -= 0x1000000;
            if (v < 0) v = -v;
            if (v > st->later_peak) st->later_peak = v;
        }
        for (int b = 0; b < OP_FRAME_BYTES; b++) if (fr[b]) packet_nz++;
    }
    st->nonzero += packet_nz;
    if (packet_nz && !st->have_hot) {
        memcpy(st->hot, p + 32, sizeof st->hot);
        st->have_hot = 1;
    }
}

/* Listen for about a second. Each transfer carries 24 blocks, which is how a
 * working Overbridge host paces the stream. out_bytes is one OUT packet. */
static void exchange_burst(int fd, plog_t *L, int out_bytes, char *verdict, char *claim, char *rel) {
    int out_len = OP_BUNDLE * out_bytes;
    int in_len = OP_BUNDLE * OP_PKT_BYTES;
    unsigned char *in_mem = calloc(OP_Q, (size_t)in_len);
    unsigned char *out_mem = calloc(OP_Q, (size_t)out_len);
    struct usbdevfs_urb in_urb[OP_Q], out_urb[OP_Q];
    int in_live[OP_Q], out_live[OP_Q];
    struct listen_stats st;
    int out_sent = 0, prev_set = 0, logged_in = 0, logged_out = 0;
    uint16_t counter = 0, prev = 0;
    struct timespec t0;
    memset(&st, 0, sizeof st);
    st.header = -1;
    memset(in_live, 0, sizeof in_live);
    memset(out_live, 0, sizeof out_live);
    if (!in_mem || !out_mem) {
        snprintf(verdict, TXT, "NO PACKETS");
        snprintf(claim, TXT, "out of memory");
        snprintf(rel, TXT, "-");
        free(in_mem);
        free(out_mem);
        return;
    }
    plog(L, "-- sending %d-block bundles on 0x03 (%d bytes) and reading %d-block bundles on 0x83 (%d bytes) for %d ms",
         OP_BUNDLE, out_len, OP_BUNDLE, in_len, OP_LISTEN_MS);
    plog(L, "collecting transfers directly; poll does not wake on this kernel");
    for (int q = 0; q < OP_Q; q++) {
        unsigned char *buf = out_mem + (size_t)q * out_len;
        for (int i = 0; i < OP_BUNDLE; i++) {
            fill_out_block(buf + (size_t)i * out_bytes, counter);
            counter = (uint16_t)(counter + 7);
        }
        if (submit_urb(fd, &out_urb[q], 0x03, buf, out_len) == 0) out_live[q] = 1;
        else plog(L, "submit out: %s (%d)", errname(errno), errno);
    }
    for (int q = 0; q < OP_Q; q++) {
        if (submit_urb(fd, &in_urb[q], 0x83, in_mem + (size_t)q * in_len, in_len) == 0)
            in_live[q] = 1;
        else
            plog(L, "submit in: %s (%d)", errname(errno), errno);
    }

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        struct usbdevfs_urb *done = NULL;
        struct timespec now, pause;
        int live = 0, ms;
        for (int q = 0; q < OP_Q; q++) live += in_live[q] + out_live[q];
        if (!live) break;
        clock_gettime(CLOCK_MONOTONIC, &now);
        ms = (int)((now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000);
        if (ms >= OP_LISTEN_MS) break;
        if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &done) < 0 || !done) {
            if (errno != EAGAIN) {
                plog(L, "reap: %s (%d)", errname(errno), errno);
                break;
            }
            pause.tv_sec = 0;
            pause.tv_nsec = 50000;
            nanosleep(&pause, NULL);
            continue;
        }
        {
            int status = done->status;
            int alen = done->actual_length;
            unsigned char *buf = done->buffer;
            int qi = -1, qo = -1;
            for (int q = 0; q < OP_Q; q++) {
                if (done == &in_urb[q]) qi = q;
                if (done == &out_urb[q]) qo = q;
            }
            if (qi >= 0) {
                in_live[qi] = 0;
                if (status == 0) {
                    int off = 0;
                    if (!logged_in) {
                        logged_in = 1;
                        plog(L, "first in transfer: %d bytes", alen);
                    }
                    if (alen >= 16 && st.packets == 0) {
                        plog(L, "first 16 bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                             buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
                             buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15]);
                    }
                    while (off + OP_PKT_BYTES <= alen) {
                        note_packet(&st, buf + off, OP_PKT_BYTES, &prev_set, &prev);
                        off += OP_PKT_BYTES;
                    }
                    if (off != alen) st.bad_len++;
                } else {
                    plog(L, "in status %d len %d", status, alen);
                }
                if (submit_urb(fd, &in_urb[qi], 0x83, buf, in_len) == 0) in_live[qi] = 1;
            } else if (qo >= 0) {
                out_live[qo] = 0;
                if (status == 0) {
                    if (!logged_out) {
                        logged_out = 1;
                        plog(L, "first out transfer: %d bytes", alen);
                    }
                    out_sent += alen / OP_OUT_BYTES;
                    for (int i = 0; i < OP_BUNDLE; i++) {
                        fill_out_block(buf + (size_t)i * out_bytes, counter);
                        counter = (uint16_t)(counter + 7);
                    }
                    if (submit_urb(fd, &out_urb[qo], 0x03, buf, out_len) == 0) out_live[qo] = 1;
                } else {
                    plog(L, "out status %d len %d", status, alen);
                }
            }
        }
    }
    {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int ms = (int)((now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000);
        int hot = 0, hot_i = 0;
        cancel_live(fd, in_urb, in_live, OP_Q);
        cancel_live(fd, out_urb, out_live, OP_Q);
        for (int ch = 0; ch < OP_CHANS; ch++) {
            if (st.peak[ch] > hot) {
                hot = st.peak[ch];
                hot_i = ch;
            }
        }
        plog(L, "exchange finished: out accepted %d, in received %d, %d ms", out_sent, st.packets, ms);
        plog(L, "header 0x%04x, wrong length %d, bad header %d, counter gaps %d, nonzero audio bytes %d",
             st.header < 0 ? 0 : st.header, st.bad_len, st.bad_hdr, st.counter_gaps, st.nonzero);
        plog(L, "main L %d  R %d", st.peak[0], st.peak[1]);
        plog(L, "t1 L %d R %d   t2 L %d R %d   t3 L %d R %d",
             st.peak[2], st.peak[3], st.peak[4], st.peak[5], st.peak[6], st.peak[7]);
        plog(L, "t4 L %d R %d   t5 L %d R %d   t6 L %d R %d",
             st.peak[8], st.peak[9], st.peak[10], st.peak[11], st.peak[12], st.peak[13]);
        plog(L, "tracks 7-16 and fx peak %d", st.later_peak);
        if (st.have_hot) {
            const unsigned char *h = st.hot;
            plog(L, "first loud frame: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                 h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8], h[9], h[10], h[11], h[12], h[13], h[14], h[15]);
        }
        if (st.later_peak > hot) hot = st.later_peak;
        snprintf(claim, TXT, "out %d in %d", out_sent, st.packets);
        if (st.packets == 0) snprintf(verdict, TXT, "%s", out_sent ? "SENT, NO PACKETS" : "NO PACKETS");
        else if (st.bad_len > st.packets) snprintf(verdict, TXT, "SHORT PACKETS");
        else if (st.packets < 4) snprintf(verdict, TXT, "TOO FEW PACKETS");
        else if (st.bad_hdr) snprintf(verdict, TXT, "HDR NOT 0700");
        else if (hot > 0) snprintf(verdict, TXT, "AUDIO PACKETS OK");
        else if (st.counter_gaps) snprintf(verdict, TXT, "BAD COUNTER");
        else snprintf(verdict, TXT, "STREAM SILENT");
        if (hot == 0) snprintf(rel, TXT, "all tracks silent");
        else if (st.later_peak > st.peak[hot_i]) snprintf(rel, TXT, "hot later %d", st.later_peak);
        else snprintf(rel, TXT, "hot %s %d", chan_name(hot_i), st.peak[hot_i]);
    }
    free(in_mem);
    free(out_mem);
}

static int endpoint_mps(const char *ifdir, const char *addr_want) {
    DIR *d = opendir(ifdir);
    if (!d) return -1;
    struct dirent *e;
    int mps = -1;
    while (mps < 0 && (e = readdir(d))) {
        if (strncmp(e->d_name, "ep_", 3)) continue;
        char dir[2 * PATH_CAP], addr[16], sz[16];
        snprintf(dir, sizeof dir, "%s/%s", ifdir, e->d_name);
        read_attr(dir, "bEndpointAddress", addr, sizeof addr);
        if (strcmp(addr, addr_want)) continue;
        read_attr(dir, "wMaxPacketSize", sz, sizeof sz);
        mps = (int)strtol(sz, NULL, 16) & 0x7ff;
    }
    closedir(d);
    return mps;
}

static void clear_halt(int fd, unsigned int ep, plog_t *L) {
    unsigned int e = ep;
    if (ioctl(fd, USBDEVFS_CLEAR_HALT, &e) == 0) plog(L, "clear halt 0x%02x: OK", ep);
    else plog(L, "clear halt 0x%02x: %s (%d) %s", ep, errname(errno), errno, strerror(errno));
}

/* Let go of a kernel driver, or give it back. Returns 0 on success, else errno.
 * connect == 0 releases the driver. connect == 1 asks the kernel to bind it again. */
static int kernel_driver(int fd, unsigned int ifnum, int connect, plog_t *L) {
    struct usbdevfs_ioctl cmd;
    const char *what = connect ? "reconnect driver" : "release driver";
    int e;
    memset(&cmd, 0, sizeof cmd);
    cmd.ifno = (int)ifnum;
    cmd.ioctl_code = connect ? USBDEVFS_CONNECT : USBDEVFS_DISCONNECT;
    cmd.data = NULL;
    if (ioctl(fd, USBDEVFS_IOCTL, &cmd) == 0) {
        plog(L, "%s if%u: OK", what, ifnum);
        return 0;
    }
    e = errno ? errno : EIO;
    plog(L, "%s if%u: %s (%d) %s", what, ifnum, errname(e), e, strerror(e));
    return e;
}

/* Not called. Setting configuration 1, or releasing interface 5, recreates the
 * Digitone sound card. The MIDI name stays in the MPC list and its receive
 * counter stays at zero, so play and clock from the Digitone never arrive. */
__attribute__((unused))
static int set_config(int fd, plog_t *L) {
    unsigned int cfg = 1;
    int e;
    if (ioctl(fd, USBDEVFS_SETCONFIGURATION, &cfg) == 0) {
        plog(L, "set configuration 1: OK");
        return 0;
    }
    e = errno ? errno : EIO;
    plog(L, "set configuration 1: %s (%d) %s", errname(e), e, strerror(e));
    return e;
}

static void give_drivers_back(int fd, int *let4, int *let5, plog_t *L) {
    if (*let4 && kernel_driver(fd, IF_CONTROL, 1, L) == 0) *let4 = 0;
    if (*let5 && kernel_driver(fd, IF_MIDI, 1, L) == 0) *let5 = 0;
}

/* Device-wide vendor read. Request 1 is the Overbridge name, request 2 follows it.
 * Returns the byte count, or -1. */
static int vendor_in(int fd, unsigned char req, plog_t *L) {
    unsigned char buf[32];
    struct usbdevfs_ctrltransfer ctl;
    char text[33];
    int i, n;
    memset(&ctl, 0, sizeof ctl);
    memset(buf, 0, sizeof buf);
    ctl.bRequestType = 0xc0; /* IN, vendor, device */
    ctl.bRequest = req;
    ctl.wLength = sizeof buf;
    ctl.timeout = 1000;
    ctl.data = buf;
    n = (int)ioctl(fd, USBDEVFS_CONTROL, &ctl);
    if (n < 0) {
        plog(L, "vendor in req %u: %s (%d) %s", req, errname(errno), errno, strerror(errno));
        return -1;
    }
    for (i = 0; i < n && i < 32; i++) text[i] = (buf[i] >= 32 && buf[i] < 127) ? (char)buf[i] : '.';
    text[i] = '\0';
    plog(L, "vendor in req %u: %d bytes [%02x %02x %02x %02x %02x %02x %02x %02x] \"%s\"",
         req, n, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7], text);
    return n;
}

/* Alt setting for one interface. Returns 0 on success, else errno. */
static int set_alt(int fd, unsigned int ifnum, unsigned int alt, plog_t *L) {
    struct usbdevfs_setinterface si;
    memset(&si, 0, sizeof si);
    si.interface = ifnum;
    si.altsetting = alt;
    if (ioctl(fd, USBDEVFS_SETINTERFACE, &si) == 0) {
        plog(L, "set if%u alt %u: OK", ifnum, alt);
        return 0;
    }
    int e = errno;
    plog(L, "set if%u alt %u: FAILED %s (%d) %s", ifnum, alt, errname(e), e, strerror(e));
    return e ? e : EIO;
}

static void log_interfaces(plog_t *L, const char *devname) {
    DIR *d = opendir(OP_SYSFS_USB);
    if (!d) return;
    size_t pre = strlen(devname);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, devname, pre) || e->d_name[pre] != ':') continue;
        char dir[PATH_CAP], num[16], cls[16], sub[16], pro[16], nep[16], drv[64];
        snprintf(dir, sizeof dir, "%s/%s", OP_SYSFS_USB, e->d_name);
        read_attr(dir, "bInterfaceNumber", num, sizeof num);
        read_attr(dir, "bInterfaceClass", cls, sizeof cls);
        read_attr(dir, "bInterfaceSubClass", sub, sizeof sub);
        read_attr(dir, "bInterfaceProtocol", pro, sizeof pro);
        read_attr(dir, "bNumEndpoints", nep, sizeof nep);
        driver_of(dir, drv, sizeof drv);
        plog(L, "    interface %s  if %s  class %s/%s/%s  endpoints %s  driver %s",
             e->d_name, num, cls, sub, pro, nep, drv);
        log_endpoints(L, dir);
    }
    closedir(d);
}

static void log_usb_tree(plog_t *L, usbdev_t *best, int *elektron_seen) {
    DIR *d = opendir(OP_SYSFS_USB);
    if (!d) {
        plog(L, "sysfs %s: %s", OP_SYSFS_USB, strerror(errno));
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || strchr(e->d_name, ':')) continue;
        char dir[PATH_CAP], man[64], prod[64], cls[16], sub[16], spd[16];
        snprintf(dir, sizeof dir, "%s/%s", OP_SYSFS_USB, e->d_name);
        int vid = (int)attr_num(dir, "idVendor", 16);
        int pid = (int)attr_num(dir, "idProduct", 16);
        int bus = (int)attr_num(dir, "busnum", 10);
        int addr = (int)attr_num(dir, "devnum", 10);
        read_attr(dir, "manufacturer", man, sizeof man);
        read_attr(dir, "product", prod, sizeof prod);
        read_attr(dir, "bDeviceClass", cls, sizeof cls);
        read_attr(dir, "bDeviceSubClass", sub, sizeof sub);
        read_attr(dir, "speed", spd, sizeof spd);
        plog(L, "device %s  %04x:%04x  bus %d addr %d  %s / %s  class %s/%s  %s Mb/s",
             e->d_name, (unsigned)vid, (unsigned)pid, bus, addr, man, prod, cls, sub, spd);
        if (vid == ELEKTRON_VID) (*elektron_seen)++;
        int rank = d2_rank(vid, pid);
        if (rank > best->rank) {
            best->rank = rank;
            best->vid = vid;
            best->pid = pid;
            best->bus = bus;
            best->addr = addr;
            snprintf(best->name, sizeof best->name, "%s", e->d_name);
        }
        log_interfaces(L, e->d_name);
    }
    closedir(d);
}

static int find_iface(const char *devname, int ifnum, char *dir_out, size_t n) {
    DIR *d = opendir(OP_SYSFS_USB);
    if (!d) return 0;
    size_t pre = strlen(devname);
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        if (strncmp(e->d_name, devname, pre) || e->d_name[pre] != ':') continue;
        char dir[PATH_CAP];
        snprintf(dir, sizeof dir, "%s/%s", OP_SYSFS_USB, e->d_name);
        if (attr_num(dir, "bInterfaceNumber", 10) == ifnum) {
            snprintf(dir_out, n, "%s", dir);
            found = 1;
        }
    }
    closedir(d);
    return found;
}

static int usbfs_node(int bus, int addr, char *out, size_t n, plog_t *L) {
    static const char *const roots[] = { OP_DEV_USB, OP_PROC_USB };
    for (size_t i = 0; i < sizeof roots / sizeof roots[0]; i++) {
        snprintf(out, n, "%s/%03d/%03d", roots[i], bus, addr);
        struct stat st;
        if (stat(out, &st) == 0) return 0;
        plog(L, "usbfs node %s: %s", out, strerror(errno));
    }
    return -1;
}

static int save_log(const char *buf, size_t len, int *which) {
    static const char *const paths[] = { OP_LOG_PATHS };
    for (int i = 0; i < (int)(sizeof paths / sizeof paths[0]); i++) {
        int fd = open(paths[i], O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) continue;
        ssize_t w = write(fd, buf, len);
        fsync(fd);
        close(fd);
        if (w == (ssize_t)len) { *which = i; return 1; }
    }
    return 0;
}

/* Decode only sources an open copy is playing. A mono source is written to
 * both sides so it is not stuck on the left. Unused slots are cleared. */
static void push_frames(const unsigned char *pkt) {
    const tb_dev *D = tb_current();
    int frame = D ? D->frame : OP_FRAME_BYTES;
    int nsrc = D ? D->nsrc : 0;
    if (nsrc > TB_MAX) nsrc = TB_MAX;
    for (int f = 0; f < OP_FRAMES; f++) {
        const unsigned char *fr = pkt + 32 + (size_t)f * (size_t)frame;
        uint32_t w = atomic_load_explicit(&g_w, memory_order_relaxed);
        int16_t *dst = g_ring[w & HEAR_MASK];
        memset(dst, 0, sizeof(int16_t) * (size_t)(HEAR_PAIRS * 2));
        for (int p = 0; p < nsrc; p++) {
            if (atomic_load_explicit(&g_use[p], memory_order_relaxed) <= 0) continue;
            const tb_src *s = &D->src[p];
            const unsigned char *b = fr + s->off;
            int16_t L = s->bytes == 4 ? ob_i16(b) : ob_i16_3(b);
            int16_t R = L;
            if (s->stereo)
                R = s->bytes == 4 ? ob_i16(b + 4) : ob_i16_3(b + 3);
            dst[p * 2] = L;
            dst[p * 2 + 1] = R;
        }
        atomic_store_explicit(&g_w, w + 1, memory_order_release);
    }
    atomic_store_explicit(&g_lamp, 2, memory_order_relaxed);
}

static void hear_status(const char *v, const char *dev, const char *claim, const char *mid, const char *logst) {
    pthread_mutex_lock(&g_mu);
    probe_t *P = g_owner;
    if (P) {
        pthread_mutex_lock(&P->lock);
        if (v) snprintf(P->v, TXT, "%s", v);
        if (dev) snprintf(P->dev, TXT, "%s", dev);
        if (claim) snprintf(P->claim, TXT, "%s", claim);
        if (mid) snprintf(P->mid, TXT, "%s", mid);
        if (logst) snprintf(P->logst, TXT, "%s", logst);
        pthread_mutex_unlock(&P->lock);
    }
    pthread_mutex_unlock(&g_mu);
}

/* Keep the same session the one-second probe uses, and leave it running.
 * render() copies one stereo pair out of the ring. This thread is the only
 * place that claims the Digitone. */
static void *hear_thread(void *unused) {
    (void)unused;
    plog_t L = { malloc(LOG_CAP), 0 };
    int failed = 1; /* the card log is only for a session that did not open */
    usbdev_t best = { 0 };
    int elektron = 0, fd = -1, let4 = 0, let5 = 0;
    int claimed = 0, if2_claimed = 0;
    unsigned int ifn = IF_AUDIO_IN, if2 = IF_AUDIO_OUT;
    char ifdir_audio[PATH_CAP] = "", ifdir_out[PATH_CAP] = "", ifdir_midi[PATH_CAP] = "";
    char node[PATH_CAP] = "", drv_before[64] = "-";
    int pkt_in = OP_PKT_BYTES;
    int pkt_out = OP_OUT_BYTES;
    int out_len = OP_BUNDLE * pkt_out;
    int in_len = OP_BUNDLE * pkt_in;
    unsigned char *in_mem = NULL, *out_mem = NULL;
    struct usbdevfs_urb in_urb[OP_Q], out_urb[OP_Q];
    int in_live[OP_Q], out_live[OP_Q];
    uint16_t counter = 0;
    uint32_t frames = 0;
    struct timespec t0, last;
    if (!L.buf) goto finish;
    L.buf[0] = '\0';
    memset(in_live, 0, sizeof in_live);
    memset(out_live, 0, sizeof out_live);
    plog(&L, "=== hear ===");
    log_usb_tree(&L, &best, &elektron);
    if (best.rank <= 0) {
        hear_status(elektron ? "NO DIGITONE II PID" : "NO DIGITONE II SEEN", NULL, NULL, NULL, NULL);
        plog(&L, "no Digitone II on the bus");
        goto save;
    }
    {
        const tb_dev *D = tb_by_pid(best.pid);
        char dev[TXT];
        snprintf(dev, sizeof dev, "1935:%04x bus%d dev%d", (unsigned)best.pid, best.bus, best.addr);
        if (!D) {
            hear_status("NOT OVERBRIDGE", dev, NULL, NULL, NULL);
            plog(&L, "not overbridge");
            goto save;
        }
        g_dev = D;
        atomic_fetch_add_explicit(&g_rev, 1, memory_order_relaxed);
        pkt_in = tb_in_block(D);
        pkt_out = tb_out_block(D);
        out_len = OP_BUNDLE * pkt_out;
        in_len = OP_BUNDLE * pkt_in;
        hear_status("OPENING", dev, NULL, NULL, NULL);
        atomic_store_explicit(&g_lamp, 1, memory_order_relaxed);
        plog(&L, "device %s  in %d  out %d  sources %d%s",
             D->name, pkt_in, pkt_out, D->nsrc, D->tested ? "" : "  untested");
    }
    if (!find_iface(best.name, IF_AUDIO_IN, ifdir_audio, sizeof ifdir_audio)) {
        hear_status("D2 HAS NO IF1", NULL, NULL, NULL, NULL);
        goto save;
    }
    if (attr_num(ifdir_audio, "bInterfaceClass", 16) == 1 &&
        attr_num(ifdir_audio, "bInterfaceSubClass", 16) == 3) {
        hear_status("IF1 IS MIDI, SKIPPED", NULL, NULL, NULL, NULL);
        goto save;
    }
    if (find_iface(best.name, IF_MIDI, ifdir_midi, sizeof ifdir_midi))
        driver_of(ifdir_midi, drv_before, sizeof drv_before);
    if (usbfs_node(best.bus, best.addr, node, sizeof node, &L) != 0) {
        hear_status("NO USBFS NODE", NULL, NULL, NULL, NULL);
        goto save;
    }
    fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        hear_status("OPEN FAILED", NULL, NULL, NULL, NULL);
        plog(&L, "open %s: FAILED %s", node, errname(errno));
        goto save;
    }
    plog(&L, "leaving interface 4 and interface 5 on their kernel drivers");
    if (repair_seq_link() == 0)
        hear_status(NULL, NULL, NULL, "IF5 MIDI linked", NULL);
    if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &ifn) != 0) {
        hear_status(errno == EBUSY ? "BUSY: IF1 HELD" : "CLAIM FAILED", NULL, NULL, NULL, NULL);
        plog(&L, "claim if1: FAILED %s", errname(errno));
        goto save;
    }
    claimed = 1;
    if (set_alt(fd, IF_AUDIO_IN, 3, &L) != 0 || !has_endpoint(ifdir_audio, "83")) {
        hear_status("ALT3 FAILED", NULL, NULL, NULL, NULL);
        goto save;
    }
    if (!find_iface(best.name, IF_AUDIO_OUT, ifdir_out, sizeof ifdir_out) ||
        ioctl(fd, USBDEVFS_CLAIMINTERFACE, &if2) != 0) {
        hear_status("IF2 CLAIM FAILED", NULL, NULL, NULL, NULL);
        goto save;
    }
    if2_claimed = 1;
    if (set_alt(fd, IF_AUDIO_OUT, 3, &L) != 0 || !has_endpoint(ifdir_out, "03")) {
        hear_status("NO EP 0x03", NULL, NULL, NULL, NULL);
        goto save;
    }
    clear_halt(fd, 0x83, &L);
    clear_halt(fd, 0x03, &L);
    give_drivers_back(fd, &let4, &let5, &L);
    vendor_in(fd, 1, &L);
    vendor_in(fd, 2, &L);
    {
        struct timespec pause = { 0, 100000000 };
        nanosleep(&pause, NULL);
    }
    in_mem = calloc(OP_Q, (size_t)in_len);
    out_mem = calloc(OP_Q, (size_t)out_len);
    if (!in_mem || !out_mem) {
        hear_status("NO PACKETS", NULL, NULL, NULL, NULL);
        goto save;
    }
    for (int q = 0; q < OP_Q; q++) {
        unsigned char *buf = out_mem + (size_t)q * out_len;
        for (int i = 0; i < OP_BUNDLE; i++) {
            fill_out_n(buf + (size_t)i * pkt_out, counter, pkt_out);
            counter = (uint16_t)(counter + 7);
        }
        if (submit_urb(fd, &out_urb[q], 0x03, buf, out_len) == 0) out_live[q] = 1;
        if (submit_urb(fd, &in_urb[q], 0x83, in_mem + (size_t)q * in_len, in_len) == 0) in_live[q] = 1;
    }
    hear_status("HEARING", NULL, "waiting", NULL, NULL);
    plog(&L, "hearing: %d-block bundles, %d queued, MIDI left on its driver", OP_BUNDLE, OP_Q);
    failed = 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    last = t0;
    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        struct usbdevfs_urb *done = NULL;
        struct timespec now;
        int live = 0;
        for (int q = 0; q < OP_Q; q++) live += in_live[q] + out_live[q];
        if (!live) { failed = 1; plog(&L, "transfers stopped"); break; }
        if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &done) < 0 || !done) {
            if (errno != EAGAIN) {
                failed = 1;
                plog(&L, "reap: %s (%d)", errname(errno), errno);
                break;
            }
            struct timespec pause = { 0, 50000 };
            nanosleep(&pause, NULL);
        } else {
            int qi = -1, qo = -1;
            unsigned char *buf = done->buffer;
            for (int q = 0; q < OP_Q; q++) {
                if (done == &in_urb[q]) qi = q;
                if (done == &out_urb[q]) qo = q;
            }
            if (qi >= 0) {
                in_live[qi] = 0;
                if (done->status == 0) {
                    int off = 0;
                    while (off + pkt_in <= done->actual_length) {
                        if (load_be16(buf + off) == OP_HDR_MARK) {
                            push_frames(buf + off);
                            frames += OP_FRAMES;
                        }
                        off += pkt_in;
                    }
                }
                if (!atomic_load_explicit(&g_stop, memory_order_relaxed) &&
                    submit_urb(fd, &in_urb[qi], 0x83, buf, in_len) == 0)
                    in_live[qi] = 1;
            } else if (qo >= 0) {
                out_live[qo] = 0;
                if (done->status == 0 && !atomic_load_explicit(&g_stop, memory_order_relaxed)) {
                    for (int i = 0; i < OP_BUNDLE; i++) {
                        fill_out_n(buf + (size_t)i * pkt_out, counter, pkt_out);
                        counter = (uint16_t)(counter + 7);
                    }
                    if (submit_urb(fd, &out_urb[qo], 0x03, buf, out_len) == 0) out_live[qo] = 1;
                }
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        int ms = (int)((now.tv_sec - last.tv_sec) * 1000 + (now.tv_nsec - last.tv_nsec) / 1000000);
        if (ms >= 500) {
            char claim[TXT];
            uint32_t sec = (uint32_t)(now.tv_sec - t0.tv_sec);
            last = now;
            snprintf(claim, sizeof claim, "%us  %u fr", sec, frames);
            hear_status("HEARING", NULL, claim, NULL, NULL);
        }
    }
    cancel_live(fd, in_urb, in_live, OP_Q);
    cancel_live(fd, out_urb, out_live, OP_Q);
    plog(&L, "hear stop, frames %u", frames);

save:
    atomic_store_explicit(&g_lamp, 0, memory_order_relaxed);
    if (if2_claimed) {
        set_alt(fd, IF_AUDIO_OUT, 0, &L);
        if (ioctl(fd, USBDEVFS_RELEASEINTERFACE, &if2) == 0) plog(&L, "release if2: OK");
    }
    if (claimed) {
        set_alt(fd, IF_AUDIO_IN, 0, &L);
        if (ioctl(fd, USBDEVFS_RELEASEINTERFACE, &ifn) == 0) plog(&L, "release if1: OK");
    }
    if (fd >= 0) {
        give_drivers_back(fd, &let4, &let5, &L);
        close(fd);
    }
    if (ifdir_midi[0]) {
        char after[64] = "-";
        const char *midmsg;
        driver_of(ifdir_midi, after, sizeof after);
        if (let5 && !strcmp(after, "none")) {
            struct timespec pause = { 0, 50000000 };
            nanosleep(&pause, NULL);
            driver_of(ifdir_midi, after, sizeof after);
        }
        if (let5)
            midmsg = !strcmp(drv_before, after) ? "IF5 MIDI restored" : "IF5 MIDI CHANGED!";
        else
            midmsg = !strcmp(drv_before, after) ? "IF5 MIDI untouched" : "IF5 MIDI CHANGED!";
        hear_status(NULL, NULL, NULL, midmsg, NULL);
        plog(&L, "-- after: if5 (MIDI) driver %s (was %s)", after, drv_before);
    }
    if (failed && L.buf) {
        int which = 0;
        char logst[TXT] = "NOT SAVED";
        if (save_log(L.buf, L.len, &which))
            snprintf(logst, sizeof logst, "%s", which == 0 ? "saved: EOS_DIGITAL" : "saved: /sdcard");
        hear_status(NULL, NULL, NULL, NULL, logst);
    }
    free(L.buf);
    free(in_mem);
    free(out_mem);
finish:
    atomic_store_explicit(&g_exited, 1, memory_order_release);
    return NULL;
}

static void hear_join_locked(void) {
    /* g_mu is held by the caller. Drop it across the join. */
    pthread_t th = g_th;
    int alive = atomic_load(&g_alive);
    if (!alive) return;
    if (!atomic_load(&g_exited)) atomic_store(&g_stop, 1);
    pthread_mutex_unlock(&g_mu);
    pthread_join(th, NULL);
    pthread_mutex_lock(&g_mu);
    atomic_store(&g_alive, 0);
}

static void mix_label(uint32_t mask, char *buf, int n) {
    int lim = src_lim();
    int count = 0, pos = 0, fit = 1;
    if (n < 2) return;
    buf[0] = '\0';
    for (int i = 0; i < lim; i++) {
        const char *name;
        int k;
        if ((mask & (1u << i)) == 0) continue;
        count++;
        name = src_name(i);
        k = (int)strlen(name);
        if (pos + k + (pos ? 1 : 0) >= n) { fit = 0; break; }
        if (pos) buf[pos++] = '+';
        memcpy(buf + pos, name, (size_t)k);
        pos += k;
        buf[pos] = '\0';
    }
    if (!count) snprintf(buf, (size_t)n, "none");
    else if (!fit) snprintf(buf, (size_t)n, "%d on", count);
}

/* Count only the switches that are on, and only while this copy is active.
 * Turning one off drops that source if no other copy still wants it. */
static void use_apply(probe_t *P) {
    int lim = src_lim();
    uint32_t want = P->active ? atomic_load_explicit(&P->mix, memory_order_relaxed) : 0;
    uint32_t have = P->used;
    if (lim < 32) want &= (1u << lim) - 1u;
    for (int i = 0; i < TB_MAX; i++) {
        uint32_t bit = 1u << i;
        int had = (have & bit) != 0;
        int need = (want & bit) != 0;
        if (had == need) continue;
        if (need) atomic_fetch_add_explicit(&g_use[i], 1, memory_order_relaxed);
        else atomic_fetch_sub_explicit(&g_use[i], 1, memory_order_relaxed);
    }
    P->used = want;
}

/* ACTIVE on starts the shared read, or joins it if another copy already did.
 * A second on does not stop. Only the copy that owns the read stops it. */
static void hear_on(probe_t *P) {
    pthread_mutex_lock(&g_mu);
    P->active = 1;
    use_apply(P);
    if (atomic_load(&g_alive) && !atomic_load(&g_exited)) {
        P->primed = 0;
        P->saw = 1;
        if (!g_owner) g_owner = P;
        pthread_mutex_lock(&P->lock);
        snprintf(P->v, TXT, "HEARING");
        mix_label(atomic_load_explicit(&P->mix, memory_order_relaxed), P->rel, TXT);
        pthread_mutex_unlock(&P->lock);
        pthread_mutex_unlock(&g_mu);
        return;
    }
    if (atomic_load(&g_alive)) hear_join_locked();
    g_owner = P;
    atomic_store(&g_w, 0);
    atomic_store(&g_stop, 0);
    atomic_store(&g_exited, 0);
    atomic_store_explicit(&g_lamp, 1, memory_order_relaxed);
    P->primed = 0;
    P->rpos = 0;
    P->rfrac = 0;
    pthread_mutex_lock(&P->lock);
    snprintf(P->v, TXT, "OPENING");
    mix_label(atomic_load_explicit(&P->mix, memory_order_relaxed), P->rel, TXT);
    pthread_mutex_unlock(&P->lock);
    if (pthread_create(&g_th, NULL, hear_thread, NULL) != 0) {
        g_owner = NULL;
        P->active = 0;
        use_apply(P);
        atomic_store_explicit(&g_lamp, 0, memory_order_relaxed);
        pthread_mutex_lock(&P->lock);
        snprintf(P->v, TXT, "OPEN FAILED");
        pthread_mutex_unlock(&P->lock);
        pthread_mutex_unlock(&g_mu);
        return;
    }
    atomic_store(&g_alive, 1);
    pthread_mutex_unlock(&g_mu);
}

static void hear_off(probe_t *P) {
    pthread_mutex_lock(&g_mu);
    int stop = atomic_load(&g_alive) && g_owner == P;
    P->active = 0;
    use_apply(P);
    if (stop) hear_join_locked();
    pthread_mutex_lock(&P->lock);
    if (stop) snprintf(P->v, TXT, "STOPPED");
    pthread_mutex_unlock(&P->lock);
    pthread_mutex_unlock(&g_mu);
}

static void hear_stop(void) {
    pthread_mutex_lock(&g_mu);
    if (atomic_load(&g_alive)) hear_join_locked();
    pthread_mutex_unlock(&g_mu);
}

static void run_probe(probe_t *P) {
    if (atomic_load(&g_alive) && !atomic_load(&g_exited)) {
        pthread_mutex_lock(&P->lock);
        snprintf(P->v, TXT, "BUSY: HEARING");
        pthread_mutex_unlock(&P->lock);
        return;
    }
    plog_t L = { malloc(LOG_CAP), 0 };
    if (!L.buf) return;
    L.buf[0] = '\0';

    char verdict[TXT] = "-", dev[TXT] = "-", claim[TXT] = "-", rel[TXT] = "-", mid[TXT] = "-", logst[TXT] = "-";

    pthread_mutex_lock(&P->lock);
    int run = ++P->runs;
    pthread_mutex_unlock(&P->lock);

    time_t now = time(NULL);
    struct tm tm;
    char ts[32] = "?";
    if (localtime_r(&now, &tm)) strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    struct utsname un;
    if (uname(&un) != 0) { un.release[0] = '\0'; un.machine[0] = '\0'; }
    char exe[PATH_CAP] = "?";
    ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k >= 0) exe[k] = '\0';

    plog(&L, "=== tributary run %d  %s ===", run, ts);
    plog(&L, "kernel %s %s  uid %d  euid %d", un.release, un.machine, (int)getuid(), (int)geteuid());
    plog(&L, "running inside process %s", exe);

    struct stat st;
    plog(&L, "usbfs root %s: %s", OP_DEV_USB, stat(OP_DEV_USB, &st) == 0 ? "present" : strerror(errno));
    plog(&L, "usbfs root %s: %s", OP_PROC_USB, stat(OP_PROC_USB, &st) == 0 ? "present" : strerror(errno));

    usbdev_t best = { 0 };
    int elektron = 0;
    plog(&L, "-- USB devices seen through sysfs (%s)", OP_SYSFS_USB);
    log_usb_tree(&L, &best, &elektron);
    plog(&L, "-- Elektron devices seen: %d", elektron);

    char ifdir_audio[PATH_CAP] = "", ifdir_midi[PATH_CAP] = "";
    char node[PATH_CAP] = "";
    char drv_audio[64] = "-", drv_midi_before[64] = "-", drv_midi_after[64] = "-";
    int have_midi = 0, claimed = 0, released = 0, fd = -1, claim_err = 0;
    int let4 = 0, let5 = 0, touched5 = 0;
    int chosen = best.rank > 0;

    if (!chosen) {
        if (elektron) {
            snprintf(verdict, sizeof verdict, "NO DIGITONE II PID");
            snprintf(dev, sizeof dev, "Elektron, not D2");
        } else {
            snprintf(verdict, sizeof verdict, "NO DIGITONE II SEEN");
            snprintf(dev, sizeof dev, "nothing found");
        }
        snprintf(claim, sizeof claim, "not tried");
        snprintf(rel, sizeof rel, "not tried");
        plog(&L, "no Digitone II product id (0b34 Overbridge, 1034 Audio/MIDI, 0134 MIDI) on the bus");
        goto done;
    }

    snprintf(dev, sizeof dev, "1935:%04x bus%d dev%d", (unsigned)best.pid, best.bus, best.addr);
    plog(&L, "-- chosen %s  1935:%04x  bus %d addr %d", best.name, (unsigned)best.pid, best.bus, best.addr);
    plog(&L, "   Overwitch (Overbridge 2.1) expects if1 = audio in (endpoint 0x83), if2 = audio out, "
             "if4 = control, if5 = MIDI");

    int have_audio = find_iface(best.name, IF_AUDIO_IN, ifdir_audio, sizeof ifdir_audio);
    have_midi = find_iface(best.name, IF_MIDI, ifdir_midi, sizeof ifdir_midi);
    if (have_audio) driver_of(ifdir_audio, drv_audio, sizeof drv_audio);
    if (have_midi) driver_of(ifdir_midi, drv_midi_before, sizeof drv_midi_before);
    plog(&L, "-- before any usbfs call: if1 driver %s, if5 (MIDI) driver %s", drv_audio, drv_midi_before);

    if (!have_audio) {
        snprintf(verdict, sizeof verdict, "D2 HAS NO IF1");
        snprintf(claim, sizeof claim, "not tried");
        snprintf(rel, sizeof rel, "not tried");
        plog(&L, "no interface 1 on this device; nothing to claim");
        goto done;
    }

    long cls = attr_num(ifdir_audio, "bInterfaceClass", 16);
    long sub = attr_num(ifdir_audio, "bInterfaceSubClass", 16);
    if (cls == 1 && sub == 3) {
        snprintf(verdict, sizeof verdict, "IF1 IS MIDI, SKIPPED");
        snprintf(claim, sizeof claim, "not tried");
        snprintf(rel, sizeof rel, "not tried");
        plog(&L, "if1 is USB MIDI (class 01/03), so it is refused. MIDI is never claimed by this probe.");
        goto done;
    }

    if (usbfs_node(best.bus, best.addr, node, sizeof node, &L) != 0) {
        snprintf(verdict, sizeof verdict, "NO USBFS NODE");
        snprintf(claim, sizeof claim, "not tried");
        snprintf(rel, sizeof rel, "not tried");
        plog(&L, "no usbfs node for this device, so no USB handle is possible from here");
        goto done;
    }
    plog(&L, "usbfs node: %s", node);

    fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        int e = errno;
        snprintf(verdict, sizeof verdict, "OPEN FAILED");
        snprintf(claim, sizeof claim, "open: %s", errname(e));
        snprintf(rel, sizeof rel, "not tried");
        plog(&L, "open %s: FAILED %s (%d) %s", node, errname(e), e, strerror(e));
        goto done;
    }
    plog(&L, "open %s: ok", node);

    struct usbdevfs_getdriver gd;
    memset(&gd, 0, sizeof gd);
    gd.interface = IF_AUDIO_IN;
    if (ioctl(fd, USBDEVFS_GETDRIVER, &gd) == 0)
        plog(&L, "usbfs kernel driver on if1: %s", gd.driver);
    else
        plog(&L, "usbfs kernel driver on if1: none reported (%s, %d)", strerror(errno), errno);

    unsigned int ifn = IF_AUDIO_IN;
    int alt3_err = -1, alt0_err = -1, saw_ep = 0, exchanged = 0;
    int if2_claimed = 0, if2_alt3 = -1, if2_alt0 = -1;
    char audio_verdict[TXT] = "";
    plog(&L, "leaving interface 4 and interface 5 on their kernel drivers");
    if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &ifn) == 0) {
        char ifdir_out[PATH_CAP];
        claimed = 1;
        plog(&L, "claim if1: OK");
        log_if_state(&L, ifdir_audio, "if1 before alt 3");
        alt3_err = set_alt(fd, IF_AUDIO_IN, 3, &L);
        log_if_state(&L, ifdir_audio, "if1 after alt 3");
        if (alt3_err == 0) saw_ep = has_endpoint(ifdir_audio, "83");
        plog(&L, "endpoint 0x83 after alt 3: %s", saw_ep ? "present" : "absent");
        if (alt3_err == 0 && saw_ep && find_iface(best.name, IF_AUDIO_OUT, ifdir_out, sizeof ifdir_out)) {
            long cls2 = attr_num(ifdir_out, "bInterfaceClass", 16);
            long sub2 = attr_num(ifdir_out, "bInterfaceSubClass", 16);
            unsigned int if2 = IF_AUDIO_OUT;
            if (cls2 == 1 && sub2 == 3) {
                snprintf(audio_verdict, sizeof audio_verdict, "IF2 IS MIDI");
                plog(&L, "if2 is USB MIDI, so it is refused");
            } else if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &if2) == 0) {
                if2_claimed = 1;
                plog(&L, "claim if2: OK");
                if2_alt3 = set_alt(fd, IF_AUDIO_OUT, 3, &L);
                log_if_state(&L, ifdir_out, "if2 after alt 3");
                if (if2_alt3 == 0 && has_endpoint(ifdir_out, "03")) {
                    int mps = endpoint_mps(ifdir_out, "03");
                    int out_bytes = OP_OUT_BYTES;
                    plog(&L, "endpoint 0x03 max packet %d", mps);
                    if (mps >= OP_OUT_BYTES && mps % OP_OUT_BYTES == 0 && mps <= 4096) out_bytes = mps;
                    if (mps >= OP_OUT_BYTES) {
                        exchanged = 1;
                        clear_halt(fd, 0x83, &L);
                        clear_halt(fd, 0x03, &L);
                        give_drivers_back(fd, &let4, &let5, &L);
                        vendor_in(fd, 1, &L);
                        vendor_in(fd, 2, &L);
                        {
                            struct timespec pause = { 0, 100000000 };
                            nanosleep(&pause, NULL);
                        }
                        plog(&L, "waited 100 ms after the name requests");
                        exchange_burst(fd, &L, out_bytes, audio_verdict, claim, rel);
                    } else {
                        snprintf(audio_verdict, sizeof audio_verdict, "OUT EP SMALL");
                    }
                } else if (if2_alt3 == 0) {
                    snprintf(audio_verdict, sizeof audio_verdict, "NO EP 0x03");
                }
                if2_alt0 = set_alt(fd, IF_AUDIO_OUT, 0, &L);
                log_if_state(&L, ifdir_out, "if2 after alt 0");
                if (ioctl(fd, USBDEVFS_RELEASEINTERFACE, &if2) == 0) plog(&L, "release if2: OK");
                else plog(&L, "release if2: FAILED %s (%d)", errname(errno), errno);
            } else {
                int e = errno;
                snprintf(audio_verdict, sizeof audio_verdict, "IF2 CLAIM FAILED");
                plog(&L, "claim if2: FAILED %s (%d) %s", errname(e), e, strerror(e));
            }
        } else if (alt3_err == 0 && saw_ep) {
            snprintf(audio_verdict, sizeof audio_verdict, "NO IF2");
            plog(&L, "no interface 2 on this device");
        }
        /* Always return interface 1 to the idle alt setting before releasing. */
        alt0_err = set_alt(fd, IF_AUDIO_IN, 0, &L);
        log_if_state(&L, ifdir_audio, "if1 after alt 0");
        if (ioctl(fd, USBDEVFS_RELEASEINTERFACE, &ifn) == 0) {
            released = 1;
            plog(&L, "release if1: OK");
        } else {
            int e = errno;
            plog(&L, "release if1: FAILED %s (%d) %s", errname(e), e, strerror(e));
        }
        if (!exchanged) {
            snprintf(claim, sizeof claim, "alt3: %s", alt3_err == 0 ? "OK" : errname(alt3_err));
            snprintf(rel, sizeof rel, "alt0: %s", alt0_err == 0 ? "OK" : errname(alt0_err));
        }
    } else {
        claim_err = errno;
        snprintf(claim, sizeof claim, "if1 claim: %s", errname(claim_err));
        snprintf(rel, sizeof rel, "not claimed");
        plog(&L, "claim if1: FAILED %s (%d) %s", errname(claim_err), claim_err, strerror(claim_err));
    }
    give_drivers_back(fd, &let4, &let5, &L);
    close(fd);
    fd = -1;

    if (!claimed && claim_err == EBUSY) snprintf(verdict, sizeof verdict, "BUSY: IF1 HELD");
    else if (!claimed) snprintf(verdict, sizeof verdict, "CLAIM FAILED");
    else if (!released) snprintf(verdict, sizeof verdict, "RELEASE FAILED");
    else if (alt3_err != 0) snprintf(verdict, sizeof verdict, "ALT3 FAILED");
    else if (alt0_err != 0) snprintf(verdict, sizeof verdict, "ALT0 FAILED");
    else if (if2_claimed && if2_alt3 != 0) snprintf(verdict, sizeof verdict, "IF2 ALT3 FAILED");
    else if (if2_claimed && if2_alt0 != 0) snprintf(verdict, sizeof verdict, "IF2 ALT0 FAILED");
    else if (!saw_ep) snprintf(verdict, sizeof verdict, "ALT3 OK, NO EP 0x83");
    else if (audio_verdict[0]) snprintf(verdict, sizeof verdict, "%s", audio_verdict);
    else snprintf(verdict, sizeof verdict, "ALT3 OK, EP 0x83");

done:
    if (!chosen) {
        snprintf(mid, sizeof mid, "-");
    } else if (!have_midi) {
        snprintf(mid, sizeof mid, "IF5 MIDI not found");
    } else if (touched5) {
        driver_of(ifdir_midi, drv_midi_after, sizeof drv_midi_after);
        if (!strcmp(drv_midi_after, "none")) {
            struct timespec pause = { 0, 50000000 };
            nanosleep(&pause, NULL);
            driver_of(ifdir_midi, drv_midi_after, sizeof drv_midi_after);
        }
        if (!strcmp(drv_midi_before, drv_midi_after))
            snprintf(mid, sizeof mid, "IF5 MIDI restored");
        else
            snprintf(mid, sizeof mid, "IF5 MIDI CHANGED!");
        plog(&L, "-- after: if5 (MIDI) driver %s (was %s)", drv_midi_after, drv_midi_before);
    } else {
        driver_of(ifdir_midi, drv_midi_after, sizeof drv_midi_after);
        if (!strcmp(drv_midi_before, drv_midi_after))
            snprintf(mid, sizeof mid, "IF5 MIDI untouched");
        else
            snprintf(mid, sizeof mid, "IF5 MIDI CHANGED!");
        plog(&L, "-- after: if5 (MIDI) driver %s (was %s)", drv_midi_after, drv_midi_before);
    }
    if (fd >= 0) close(fd);

    plog(&L, "RESULT: %s | dev %s | %s | %s | %s", verdict, dev, claim, rel, mid);

    int which = 0;
    if (!save_log(L.buf, L.len, &which)) snprintf(logst, sizeof logst, "NOT SAVED");
    else if (which == 0) snprintf(logst, sizeof logst, "saved: EOS_DIGITAL");
    else snprintf(logst, sizeof logst, "saved: /sdcard");

    pthread_mutex_lock(&P->lock);
    snprintf(P->v, TXT, "%s", verdict);
    snprintf(P->dev, TXT, "%s", dev);
    snprintf(P->claim, TXT, "%s", claim);
    snprintf(P->rel, TXT, "%s", rel);
    snprintf(P->mid, TXT, "%s", mid);
    snprintf(P->logst, TXT, "%s", logst);
    pthread_mutex_unlock(&P->lock);

    free(L.buf);
}

/* Used only when the C library headers omit these. 434 and 438 are the numbers
 * in both the 32-bit ARM and the AArch64 Linux syscall tables. */
#ifndef __NR_pidfd_open
#define __NR_pidfd_open 434
#endif
#ifndef __NR_pidfd_getfd
#define __NR_pidfd_getfd 438
#endif

/* The MPC keeps a private sequencer port for the Digitone. Releasing the USB
 * MIDI driver drops the connection and the MPC does not put it back, so the
 * tempo box stays grey and transport never arrives. Wire it the same way the
 * MPC's own MIDI port is wired: device into the [Out] port, [In] port back. */
static int repair_seq_link(void) {
    FILE *f;
    char line[256];
    int client = -1, kclient = -1, kport = -1, mpc = -1, mpc_out = -1, mpc_in = -1;
    int fd = -1, own = 0, rc = -1;
    f = fopen("/proc/asound/seq/clients", "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        int n, port;
        char name[128];
        if (sscanf(line, "Client %d : \"%127[^\"]\"", &n, name) == 2) {
            client = n;
            if (!strcmp(name, "Elektron Digitone II")) kclient = n;
            if (!strcmp(name, "MPC")) mpc = n;
            continue;
        }
        if (sscanf(line, " Port %d : \"%127[^\"]\"", &port, name) != 2 &&
            sscanf(line, "  Port %d : \"%127[^\"]\"", &port, name) != 2)
            continue;
        if (client == kclient && kport < 0) kport = port;
        if (client == mpc && !strcmp(name, "Elektron Digitone II MIDI 1")) {
            if (strstr(line, "[Out]")) mpc_out = port;
            if (strstr(line, "[In]")) mpc_in = port;
        }
    }
    fclose(f);
    if (kclient < 0 || kport < 0 || mpc < 0 || mpc_out < 0 || mpc_in < 0) return -1;
    {
        DIR *d = opendir("/proc/self/fd");
        struct dirent *de;
        if (d) {
            while ((de = readdir(d))) {
                char path[64], target[64];
                int num, id = -1;
                ssize_t n;
                if (de->d_name[0] == '.') continue;
                num = atoi(de->d_name);
                snprintf(path, sizeof path, "/proc/self/fd/%d", num);
                n = readlink(path, target, sizeof target - 1);
                if (n < 0) continue;
                target[n] = '\0';
                if (strcmp(target, "/dev/snd/seq") != 0) continue;
                if (ioctl(num, SNDRV_SEQ_IOCTL_CLIENT_ID, &id) == 0 && id == mpc) {
                    fd = num;
                    break;
                }
            }
            closedir(d);
        }
    }
    if (fd < 0) {
        DIR *proc = opendir("/proc");
        struct dirent *de;
        if (!proc) return -1;
        while ((de = readdir(proc)) && fd < 0) {
            DIR *fds;
            struct dirent *fe;
            char fdpath[64];
            int pid;
            if (sscanf(de->d_name, "%d", &pid) != 1) continue;
            snprintf(fdpath, sizeof fdpath, "/proc/%d/fd", pid);
            fds = opendir(fdpath);
            if (!fds) continue;
            while ((fe = readdir(fds))) {
                char linkpath[320], target[64];
                int pidfd, targetfd, dupfd, id = -1;
                ssize_t n;
                if (fe->d_name[0] == '.') continue;
                snprintf(linkpath, sizeof linkpath, "/proc/%d/fd/%s", pid, fe->d_name);
                n = readlink(linkpath, target, sizeof target - 1);
                if (n < 0) continue;
                target[n] = '\0';
                if (strcmp(target, "/dev/snd/seq") != 0) continue;
                pidfd = (int)syscall(__NR_pidfd_open, pid, 0);
                if (pidfd < 0) continue;
                targetfd = atoi(fe->d_name);
                dupfd = (int)syscall(__NR_pidfd_getfd, pidfd, targetfd, 0);
                close(pidfd);
                if (dupfd < 0) continue;
                if (ioctl(dupfd, SNDRV_SEQ_IOCTL_CLIENT_ID, &id) == 0 && id == mpc) {
                    fd = dupfd;
                    own = 1;
                    break;
                }
                close(dupfd);
            }
            closedir(fds);
        }
        closedir(proc);
    }
    if (fd < 0) return -1;
    {
        struct snd_seq_port_subscribe sub;
        int i;
        int pairs[2][4] = {
            { kclient, kport, mpc, mpc_out },
            { mpc, mpc_in, kclient, kport }
        };
        rc = 0;
        for (i = 0; i < 2; i++) {
            memset(&sub, 0, sizeof sub);
            sub.sender.client = (unsigned char)pairs[i][0];
            sub.sender.port = (unsigned char)pairs[i][1];
            sub.dest.client = (unsigned char)pairs[i][2];
            sub.dest.port = (unsigned char)pairs[i][3];
            if (ioctl(fd, SNDRV_SEQ_IOCTL_SUBSCRIBE_PORT, &sub) != 0 && errno != EBUSY)
                rc = -1;
        }
    }
    if (own) close(fd);
    return rc;
}

/* ---- engine interface (same six-slot shape as wrapper/engine.h) ---------- */

typedef struct {
    void *(*create)(const char *data_dir);
    void (*destroy)(void *inst);
    void (*midi)(void *inst, const uint8_t *msg, int len);
    void (*set_param)(void *inst, const char *key, const char *val);
    int (*get_param)(void *inst, const char *key, char *buf, int buf_len);
    void (*render)(void *inst, int16_t *out_lr, int frames);
} mpc_engine_t;

static void *create(const char *data_dir) {
    (void)data_dir;
    probe_t *P = calloc(1, sizeof *P);
    if (!P) return NULL;
    pthread_mutex_init(&P->lock, NULL);
    pthread_mutex_lock(&g_mu);
    g_instances++;
    pthread_mutex_unlock(&g_mu);
    atomic_store_explicit(&P->mix, 1u, memory_order_relaxed); /* main, until a switch changes it */
    snprintf(P->v, TXT, "off");
    snprintf(P->dev, TXT, "-");
    snprintf(P->claim, TXT, "-");
    snprintf(P->rel, TXT, "%s", src_name(0));
    snprintf(P->mid, TXT, "-");
    snprintf(P->logst, TXT, "-");
    if (repair_seq_link() == 0)
        snprintf(P->mid, TXT, "IF5 MIDI linked");
    return P;
}

static void destroy(void *inst) {
    probe_t *P = inst;
    int last;
    if (!P) return;
    P->active = 0;
    use_apply(P);
    pthread_mutex_lock(&g_mu);
    if (g_instances > 0) g_instances--;
    last = g_instances <= 0;
    /* Keep the owner pointer until the thread has finished writing its status.
     * Another copy still playing must not be stopped by this one going away. */
    if (!last && g_owner == P) g_owner = NULL;
    pthread_mutex_unlock(&g_mu);
    if (last) {
        hear_stop();
        pthread_mutex_lock(&g_mu);
        if (g_owner == P) g_owner = NULL;
        pthread_mutex_unlock(&g_mu);
    }
    pthread_mutex_destroy(&P->lock);
    free(P);
}

static void midi(void *inst, const uint8_t *msg, int len) { (void)inst; (void)msg; (void)len; }

static int switch_index(const char *key);

static void set_param(void *inst, const char *key, const char *val) {
    probe_t *P = inst;
    if (!P || !key || !val) return;
    if (!strcmp(key, "run") && atof(val) > 0.5) run_probe(P);
    else if (!strcmp(key, "hear") && atof(val) > 0.5) hear_on(P);
    else if (!strcmp(key, "active")) {
        if (atof(val) > 0.5) hear_on(P);
        else hear_off(P);
    } else if (!strcmp(key, "pair") || !strcmp(key, "source")) {
        int n = (int)atof(val);
        int lim = src_lim();
        if (n < 0) n = 0;
        if (n >= lim) n = lim - 1;
        atomic_store_explicit(&P->mix, 1u << n, memory_order_relaxed);
        P->saw_source = 1;
        pthread_mutex_lock(&P->lock);
        snprintf(P->rel, TXT, "%s", src_name(n));
        pthread_mutex_unlock(&P->lock);
        if (P->active) use_apply(P);
    } else {
        int sw = switch_index(key);
        if (sw >= 0) {
            int on = atof(val) > 0.5;
            /* s0 defaults on. The others default off. While a saved SOURCE is being
             * restored, those defaults must not turn Main on or clear the saved output. */
            int is_default = sw == 0 ? on : !on;
            uint32_t m;
            if (sw >= src_lim()) return;
            if (P->saw_source && !P->switches_live && is_default) return;
            m = atomic_load_explicit(&P->mix, memory_order_relaxed);
            if (on) m |= 1u << sw;
            else m &= ~(1u << sw);
            atomic_store_explicit(&P->mix, m, memory_order_relaxed);
            pthread_mutex_lock(&P->lock);
            mix_label(m, P->rel, TXT);
            pthread_mutex_unlock(&P->lock);
            if (P->active) use_apply(P);
        }
    }
}

static int switch_index(const char *key) {
    int n = 0;
    if (!key || key[0] != 's' || key[1] < '0' || key[1] > '9') return -1;
    for (const char *p = key + 1; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        n = n * 10 + (*p - '0');
        if (n >= TB_MAX) return -1;
    }
    return n;
}

static int named_switch(const char *key) {
    char tmp[8];
    const char *us = strrchr(key, '_');
    size_t n;
    if (!us || strcmp(us, "_name") != 0) return -1;
    n = (size_t)(us - key);
    if (n >= sizeof tmp) return -1;
    memcpy(tmp, key, n);
    tmp[n] = '\0';
    return switch_index(tmp);
}

static int get_param(void *inst, const char *key, char *buf, int buf_len) {
    probe_t *P = inst;
    const char *src = NULL;
    int named;
    if (!P || !key || !buf || buf_len < 2) return 0;
    if (!strcmp(key, "display_rev"))
        return snprintf(buf, buf_len, "%u", atomic_load_explicit(&g_rev, memory_order_relaxed));
    named = named_switch(key);
    if (named >= 0) {
        src_tag(named, buf, buf_len);
        return (int)strlen(buf);
    }
    if (!strcmp(key, "p_v")) src = P->v;
    else if (!strcmp(key, "p_dev")) src = P->dev;
    else if (!strcmp(key, "p_claim")) src = P->claim;
    else if (!strcmp(key, "p_rel")) src = P->rel;
    else if (!strcmp(key, "p_mid")) src = P->mid;
    else if (!strcmp(key, "p_log")) src = P->logst;
    else if (!strcmp(key, "pair") || !strcmp(key, "source")) {
        uint32_t m = atomic_load_explicit(&P->mix, memory_order_relaxed);
        int idx = 0, lim = src_lim();
        for (int i = 0; i < lim; i++) if (m & (1u << i)) { idx = i; break; }
        return snprintf(buf, buf_len, "%d", idx);
    } else if (switch_index(key) >= 0) {
        int sw = switch_index(key);
        uint32_t m = atomic_load_explicit(&P->mix, memory_order_relaxed);
        return snprintf(buf, buf_len, "%d", (m & (1u << sw)) ? 1 : 0);
    } else if (!strcmp(key, "active")) {
        return snprintf(buf, buf_len, "%d", P->active ? 1 : 0);
    } else if (!strcmp(key, "lamp")) {
        int lamp = atomic_load_explicit(&g_lamp, memory_order_relaxed);
        if (lamp < 0) lamp = 0;
        if (lamp > 2) lamp = 2;
        return snprintf(buf, buf_len, "%d", lamp);
    } else if (!strcmp(key, "srcname")) {
        uint32_t m = atomic_load_explicit(&P->mix, memory_order_relaxed);
        return mix_label(m, buf, buf_len), (int)strlen(buf);
    }
    if (!src) return 0;
    pthread_mutex_lock(&P->lock);
    int n = snprintf(buf, buf_len, "%s", src);
    pthread_mutex_unlock(&P->lock);
    return n;
}

static void render(void *inst, int16_t *out_lr, int frames) {
    probe_t *P = inst;
    if (!out_lr || frames < 1) return;
    memset(out_lr, 0, sizeof(int16_t) * 2 * (size_t)frames);
    if (!P) return;
    P->switches_live = 1;
    if (!atomic_load_explicit(&g_alive, memory_order_acquire) ||
        atomic_load_explicit(&g_exited, memory_order_acquire)) {
        if (P->saw) {
            P->saw = 0;
            P->primed = 0;
            pthread_mutex_lock(&P->lock);
            if (!strcmp(P->v, "HEARING")) snprintf(P->v, TXT, "STOPPED");
            pthread_mutex_unlock(&P->lock);
        }
        return;
    }
    if (!P->active) return;
    if (!P->saw) {
        P->saw = 1;
        pthread_mutex_lock(&P->lock);
        if (!strcmp(P->v, "off") || !strcmp(P->v, "press HEAR")) snprintf(P->v, TXT, "HEARING");
        pthread_mutex_unlock(&P->lock);
    }
    uint32_t w = atomic_load_explicit(&g_w, memory_order_acquire);
    int lim = src_lim();
    uint32_t mask = atomic_load_explicit(&P->mix, memory_order_relaxed);
    if (lim < 32) mask &= (1u << lim) - 1u;
    if (!P->primed) {
        if (w < HEAR_LAG) return;
        P->primed = 1;
        P->rpos = w - HEAR_LAG;
        P->rfrac = 0;
    }
    if ((uint32_t)(w - P->rpos) > HEAR_LAG * 2) {
        P->rpos = w - HEAR_LAG;
        P->rfrac = 0;
    }
    uint32_t step = HEAR_STEP;
    if ((uint32_t)(w - P->rpos) > (uint32_t)HEAR_LAG + HEAR_NUDGE)
        step += 32u;
    for (int i = 0; i < frames; i++) {
        if ((uint32_t)(w - P->rpos) < 2) {
            w = atomic_load_explicit(&g_w, memory_order_acquire);
            if ((uint32_t)(w - P->rpos) < 2) break;
        }
        int i0 = (int)(P->rpos & HEAR_MASK);
        int i1 = (int)((P->rpos + 1) & HEAR_MASK);
        int frac = (int)(P->rfrac >> 8);
        int sum[2] = { 0, 0 };
        for (int p = 0; p < lim; p++) {
            if ((mask & (1u << p)) == 0) continue;
            for (int ch = 0; ch < 2; ch++) {
                int a = g_ring[i0][p * 2 + ch];
                int b = g_ring[i1][p * 2 + ch];
                sum[ch] += a + ((b - a) * frac) / 256;
            }
        }
        for (int ch = 0; ch < 2; ch++) {
            int v = sum[ch];
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            out_lr[i * 2 + ch] = (int16_t)v;
        }
        uint32_t acc = P->rfrac + step;
        P->rpos += acc >> 16;
        P->rfrac = acc & 0xffff;
    }
}

static const mpc_engine_t API = { create, destroy, midi, set_param, get_param, render };
const mpc_engine_t *mpc_engine(void) { return &API; }
