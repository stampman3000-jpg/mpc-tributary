/* mpc-overprobe: a no-audio probe. Can a plugin on an Akai MPC claim the Overbridge audio
 * interface of an Elektron Digitone II over USB while its MIDI interface keeps working?
 *
 * Lists USB devices from sysfs, opens the usbfs node, claims interface 1 only, releases it,
 * and logs every step. It never detaches a kernel driver, never resets the device, and never
 * sets a configuration or alt setting. Results go to the plugin readouts and to a log file.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
    if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &ifn) == 0) {
        claimed = 1;
        snprintf(claim, sizeof claim, "if1 claim: OK");
        plog(&L, "claim if1: OK");
        if (ioctl(fd, USBDEVFS_RELEASEINTERFACE, &ifn) == 0) {
            released = 1;
            snprintf(rel, sizeof rel, "if1 release: OK");
            plog(&L, "release if1: OK");
        } else {
            int e = errno;
            snprintf(rel, sizeof rel, "if1 release: %s", errname(e));
            plog(&L, "release if1: FAILED %s (%d) %s", errname(e), e, strerror(e));
        }
    } else {
        claim_err = errno;
        snprintf(claim, sizeof claim, "if1 claim: %s", errname(claim_err));
        snprintf(rel, sizeof rel, "not claimed");
        plog(&L, "claim if1: FAILED %s (%d) %s", errname(claim_err), claim_err, strerror(claim_err));
    }
    close(fd);
    fd = -1;

    if (claimed && released) snprintf(verdict, sizeof verdict, "CLAIM OK, RELEASED");
    else if (claimed) snprintf(verdict, sizeof verdict, "CLAIMED, NOT RELEASED");
    else if (claim_err == EBUSY) snprintf(verdict, sizeof verdict, "BUSY: IF1 HELD");
    else snprintf(verdict, sizeof verdict, "CLAIM FAILED");

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
