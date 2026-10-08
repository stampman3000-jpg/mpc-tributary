/* mpc-overprobe: a no-audio probe. Can a plugin on an Akai MPC claim the Overbridge audio
 * interface of an Elektron Digitone II over USB while its MIDI interface keeps working?
 *
 * Lists USB devices from sysfs, opens the usbfs node, claims interface 1 only, switches that
 * interface to alt setting 3, claims interface 2 and does the same, sends a few silent
 * blocks out of endpoint 0x03 while reading endpoint 0x83, then puts both interfaces
 * back to alt 0 and releases them. It never detaches a kernel driver, never resets the
 * device, and never sets a configuration. Results go to the plugin readouts and a log file.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <linux/usbdevice_fs.h>

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
#define OP_LOG_PATHS "/media/EOS_DIGITAL/mpc-overprobe.log", "/sdcard/mpc-overprobe.log"
#endif

#define ELEKTRON_VID 0x1935
#define IF_AUDIO_IN 1
#define IF_AUDIO_OUT 2
#define IF_MIDI 5
#define LOG_CAP (96 * 1024)
#define TXT 24
#define PATH_CAP 1024

typedef struct {
    pthread_mutex_t lock;
    int runs;
    char v[TXT], dev[TXT], claim[TXT], rel[TXT], mid[TXT], logst[TXT];
} probe_t;

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

static int d2_rank(int vid, int pid) {
    if (vid != ELEKTRON_VID) return 0;
    if (pid == 0x0B34) return 3;
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

static void fill_out_block(unsigned char *dst, uint16_t counter) {
    memset(dst, 0, OP_OUT_BYTES);
    dst[0] = (unsigned char)(OP_OUT_HDR >> 8);
    dst[1] = (unsigned char)(OP_OUT_HDR & 0xff);
    dst[2] = (unsigned char)(counter >> 8);
    dst[3] = (unsigned char)(counter & 0xff);
}

static void cancel_urb(int fd, struct usbdevfs_urb *urb) {
    ioctl(fd, USBDEVFS_DISCARDURB, urb);
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    poll(&pfd, 1, 200);
    struct usbdevfs_urb *done = NULL;
    ioctl(fd, USBDEVFS_REAPURBNDELAY, &done);
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

static void report_in_packets(plog_t *L, const unsigned char *pkts, const int *lens, int got,
                              int out_sent, char *verdict, char *claim, char *rel) {
    struct burst_view v;
    view_burst(pkts, lens, got, &v);
    if (got > 0 && lens[0] >= 16) {
        const unsigned char *p = pkts;
        plog(L, "first 16 bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
             p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
        char counters[128] = "";
        size_t used = 0;
        for (int i = 0; i < got && lens[i] == OP_PKT_BYTES; i++) {
            uint16_t c = load_be16(pkts + (size_t)i * OP_PKT_BYTES + 2);
            int n = snprintf(counters + used, sizeof counters - used, "%s%u", i ? "," : "", c);
            if (n < 0 || (size_t)n >= sizeof counters - used) break;
            used += (size_t)n;
        }
        plog(L, "header 0x%04x, counters %s", v.header, counters);
    }
    plog(L, "in %d, out %d, wrong length %d, header same %d, counter +7 %d, main peak %d",
         v.n, out_sent, v.bad_len, v.headers_same, v.counter_step, v.peak);
    if (got == 0) snprintf(verdict, TXT, "%s", out_sent ? "SENT, NO PACKETS" : "NO PACKETS");
    else if (v.bad_len) snprintf(verdict, TXT, "SHORT PACKETS");
    else if (got < 4) snprintf(verdict, TXT, "TOO FEW PACKETS");
    else if (!v.counter_step || !v.headers_same) snprintf(verdict, TXT, "BAD COUNTER");
    else if (v.header != OP_HDR_MARK) snprintf(verdict, TXT, "HDR NOT 0700");
    else snprintf(verdict, TXT, "AUDIO PACKETS OK");
    snprintf(claim, TXT, "out %d in %d", out_sent, got);
    snprintf(rel, TXT, "main peak %d", v.peak);
}

/* Listen on 0x83 while sending silent blocks on 0x03. out_bytes is one full OUT packet. */
static void exchange_burst(int fd, plog_t *L, int out_bytes, char *verdict, char *claim, char *rel) {
    unsigned char *pkts = calloc(OP_BURST, OP_PKT_BYTES);
    unsigned char *out_buf = calloc(1, (size_t)out_bytes);
    int lens[OP_BURST];
    struct usbdevfs_urb in_urb, out_urb;
    int got = 0, out_sent = 0, in_flight = 0, out_flight = 0, misses = 0;
    uint16_t counter = 0;
    if (!pkts || !out_buf) {
        snprintf(verdict, TXT, "NO PACKETS");
        snprintf(claim, TXT, "out of memory");
        snprintf(rel, TXT, "-");
        free(pkts);
        free(out_buf);
        return;
    }
    int blocks = out_bytes / OP_OUT_BYTES;
    plog(L, "-- sending %d-byte silent blocks on 0x03 (%d per packet) and reading 0x83", OP_OUT_BYTES, blocks);
    for (int i = 0; i < blocks; i++) {
        fill_out_block(out_buf + (size_t)i * OP_OUT_BYTES, counter);
        counter = (uint16_t)(counter + 7);
    }
    if (submit_urb(fd, &out_urb, 0x03, out_buf, out_bytes) == 0) out_flight = 1;
    else plog(L, "submit out: %s (%d)", errname(errno), errno);
    if (submit_urb(fd, &in_urb, 0x83, pkts, OP_PKT_BYTES) == 0) in_flight = 1;
    else plog(L, "submit in: %s (%d)", errname(errno), errno);

    for (int spin = 0; spin < 48 && got < OP_BURST && (in_flight || out_flight); spin++) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, got ? 40 : 200);
        if (pr <= 0) {
            if (++misses >= 2) break;
            continue;
        }
        misses = 0;
        for (;;) {
            struct usbdevfs_urb *done = NULL;
            if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &done) < 0) break;
            if (done == &in_urb) {
                in_flight = 0;
                if (in_urb.status == 0) {
                    lens[got] = in_urb.actual_length;
                    plog(L, "in %d: %d bytes", got, in_urb.actual_length);
                    got++;
                } else {
                    plog(L, "in status %d", in_urb.status);
                }
                if (got < OP_BURST && submit_urb(fd, &in_urb, 0x83, pkts + (size_t)got * OP_PKT_BYTES, OP_PKT_BYTES) == 0)
                    in_flight = 1;
            } else if (done == &out_urb) {
                out_flight = 0;
                if (out_urb.status == 0) out_sent++;
                else plog(L, "out status %d", out_urb.status);
                if (out_sent < 32 && out_urb.status == 0) {
                    for (int i = 0; i < blocks; i++) {
                        fill_out_block(out_buf + (size_t)i * OP_OUT_BYTES, counter);
                        counter = (uint16_t)(counter + 7);
                    }
                    if (submit_urb(fd, &out_urb, 0x03, out_buf, out_bytes) == 0) out_flight = 1;
                }
            }
        }
    }
    if (in_flight) cancel_urb(fd, &in_urb);
    if (out_flight) cancel_urb(fd, &out_urb);
    plog(L, "exchange finished: out accepted %d, in received %d", out_sent, got);
    report_in_packets(L, pkts, lens, got, out_sent, verdict, claim, rel);
    free(pkts);
    free(out_buf);
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

static void run_probe(probe_t *P) {
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

    plog(&L, "=== mpc-overprobe run %d  %s ===", run, ts);
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
    snprintf(P->v, TXT, "press RUN PROBE");
    snprintf(P->dev, TXT, "-");
    snprintf(P->claim, TXT, "-");
    snprintf(P->rel, TXT, "-");
    snprintf(P->mid, TXT, "-");
    snprintf(P->logst, TXT, "-");
    return P;
}

static void destroy(void *inst) {
    probe_t *P = inst;
    if (!P) return;
    pthread_mutex_destroy(&P->lock);
    free(P);
}

static void midi(void *inst, const uint8_t *msg, int len) { (void)inst; (void)msg; (void)len; }

static void set_param(void *inst, const char *key, const char *val) {
    probe_t *P = inst;
    if (!P || !key || !val) return;
    if (!strcmp(key, "run") && atof(val) > 0.5) run_probe(P);
}

static int get_param(void *inst, const char *key, char *buf, int buf_len) {
    probe_t *P = inst;
    const char *src = NULL;
    if (!P || !key || !buf || buf_len < 2) return 0;
    if (!strcmp(key, "p_v")) src = P->v;
    else if (!strcmp(key, "p_dev")) src = P->dev;
    else if (!strcmp(key, "p_claim")) src = P->claim;
    else if (!strcmp(key, "p_rel")) src = P->rel;
    else if (!strcmp(key, "p_mid")) src = P->mid;
    else if (!strcmp(key, "p_log")) src = P->logst;
    if (!src) return 0;
    pthread_mutex_lock(&P->lock);
    int n = snprintf(buf, buf_len, "%s", src);
    pthread_mutex_unlock(&P->lock);
    return n;
}

static void render(void *inst, int16_t *out_lr, int frames) {
    (void)inst;
    memset(out_lr, 0, sizeof(int16_t) * 2 * (size_t)frames);
}

static const mpc_engine_t API = { create, destroy, midi, set_param, get_param, render };
const mpc_engine_t *mpc_engine(void) { return &API; }
