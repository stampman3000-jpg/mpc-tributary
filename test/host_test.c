/* Host test for the probe's decision logic. Builds a fake sysfs and usbfs tree under /tmp/opfix
 * and drives the engine API the way the MPC does. The real USB claim needs the MPC, so the ioctls
 * here fail (a regular file is not a usbfs node); what is checked is which verdict each layout gets.
 * Run with test/run.sh (needs Linux headers, so it runs in the gcc container, not on the Mac).
 */
#define OP_SYSFS_USB "/tmp/opfix/sys/devices"
#define OP_DEV_USB "/tmp/opfix/dev/bus/usb"
#define OP_PROC_USB "/tmp/opfix/proc/bus/usb"
#define OP_LOG_PATHS "/tmp/opfix-nodir/x.log", "/tmp/opfix-log/mpc-overprobe.log"

#include "../src/probe.c"

#include <assert.h>

static int failures = 0;

static void check(int ok, const char *what) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static void sh(const char *cmd) {
    if (system(cmd) != 0) { fprintf(stderr, "setup failed: %s\n", cmd); exit(2); }
}

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fputs(text, f);
    fclose(f);
}

static void reset_fixture(void) {
    sh("rm -rf /tmp/opfix /tmp/opfix-log && mkdir -p /tmp/opfix/sys/devices /tmp/opfix/dev/bus/usb/003 /tmp/opfix/drivers /tmp/opfix-log");
}

static void add_device(const char *name, const char *vid, const char *pid, int bus, int dev) {
    char p[1024], txt[1100];
    snprintf(p, sizeof p, "%s/%s", OP_SYSFS_USB, name);
    snprintf(txt, sizeof txt, "mkdir -p '%s'", p);
    sh(txt);
    snprintf(p, sizeof p, "%s/%s/idVendor", OP_SYSFS_USB, name); write_file(p, vid);
    snprintf(p, sizeof p, "%s/%s/idProduct", OP_SYSFS_USB, name); write_file(p, pid);
    snprintf(p, sizeof p, "%s/%s/busnum", OP_SYSFS_USB, name); snprintf(txt, sizeof txt, "%d\n", bus); write_file(p, txt);
    snprintf(p, sizeof p, "%s/%s/devnum", OP_SYSFS_USB, name); snprintf(txt, sizeof txt, "%d\n", dev); write_file(p, txt);
    snprintf(p, sizeof p, "%s/%s/manufacturer", OP_SYSFS_USB, name); write_file(p, "Elektron\n");
    snprintf(p, sizeof p, "%s/%s/product", OP_SYSFS_USB, name); write_file(p, "Digitone II\n");
    snprintf(p, sizeof p, "%s/%s/speed", OP_SYSFS_USB, name); write_file(p, "480\n");
}

static void add_iface(const char *dev, int ifnum, const char *cls, const char *sub, const char *proto,
                      const char *driver, int with_ep83) {
    char dir[1024], p[1200], cmd[2048];
    snprintf(dir, sizeof dir, "%s/%s:1.%d", OP_SYSFS_USB, dev, ifnum);
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
    sh(cmd);
    snprintf(p, sizeof p, "%s/bInterfaceNumber", dir); { char t[8]; snprintf(t, sizeof t, "%d\n", ifnum); write_file(p, t); }
    snprintf(p, sizeof p, "%s/bInterfaceClass", dir); write_file(p, cls);
    snprintf(p, sizeof p, "%s/bInterfaceSubClass", dir); write_file(p, sub);
    snprintf(p, sizeof p, "%s/bInterfaceProtocol", dir); write_file(p, proto);
    snprintf(p, sizeof p, "%s/bNumEndpoints", dir); write_file(p, with_ep83 ? "2\n" : "0\n");
    if (driver) {
        snprintf(cmd, sizeof cmd, "mkdir -p /tmp/opfix/drivers && ln -sfn /tmp/opfix/drivers/%s '%s/driver'", driver, dir);
        sh(cmd);
    }
    if (with_ep83) {
        snprintf(cmd, sizeof cmd, "mkdir -p '%s/ep_83'", dir);
        sh(cmd);
        snprintf(p, sizeof p, "%s/ep_83/bEndpointAddress", dir); write_file(p, "83\n");
        snprintf(p, sizeof p, "%s/ep_83/type", dir); write_file(p, "Bulk\n");
        snprintf(p, sizeof p, "%s/ep_83/direction", dir); write_file(p, "in\n");
        snprintf(p, sizeof p, "%s/ep_83/wMaxPacketSize", dir); write_file(p, "0200\n");
    }
}

static void make_node(int bus, int dev) {
    char p[256];
    snprintf(p, sizeof p, "mkdir -p /tmp/opfix/dev/bus/usb/%03d && touch /tmp/opfix/dev/bus/usb/%03d/%03d", bus, bus, dev);
    sh(p);
}

static void press(const void *e_inst, const mpc_engine_t *e) {
    e->set_param((void *)e_inst, "run", "1");
}

static void readouts(const void *e_inst, const mpc_engine_t *e, char out[6][64]) {
    const char *keys[6] = { "p_v", "p_dev", "p_claim", "p_rel", "p_mid", "p_log" };
    for (int i = 0; i < 6; i++) {
        out[i][0] = '\0';
        e->get_param((void *)e_inst, keys[i], out[i], 64);
        assert(strlen(out[i]) <= 23);
    }
}

static int log_has(const char *needle) {
    FILE *f = fopen("/tmp/opfix-log/mpc-overprobe.log", "r");
    if (!f) return 0;
    static char buf[LOG_CAP];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
    return strstr(buf, needle) != NULL;
}

int main(void) {
    const mpc_engine_t *e = mpc_engine();
    void *inst = e->create(NULL);
    char r[6][64];

    reset_fixture();
    press(inst, e);
    readouts(inst, e, r);
    check(!strcmp(r[0], "NO DIGITONE II SEEN"), "empty bus: verdict says no Digitone II");
    check(!strcmp(r[1], "nothing found"), "empty bus: device readout");
    check(!strcmp(r[5], "saved: /sdcard") || !strcmp(r[5], "saved: EOS_DIGITAL"), "log falls back to a second path when the first is missing");
    check(log_has("=== mpc-overprobe run 1"), "log file has run 1 header (via fallback path)");

    reset_fixture();
    add_device("3-1", "1935\n", "0b34\n", 3, 5);
    add_iface("3-1", 1, "ff\n", "00\n", "00\n", NULL, 1);
    add_iface("3-1", 4, "ff\n", "00\n", "00\n", NULL, 0);
    add_iface("3-1", 5, "01\n", "03\n", "00\n", "snd-usbmidi-lib", 0);
    press(inst, e);
    readouts(inst, e, r);
    check(!strcmp(r[0], "NO USBFS NODE"), "Digitone II with no usbfs node: verdict");
    check(!strcmp(r[1], "1935:0b34 bus3 dev5"), "Digitone II found: device readout");
    check(!strcmp(r[4], "IF5 MIDI untouched"), "MIDI interface driver unchanged");
    check(log_has("usbfs node /tmp/opfix/dev/bus/usb/003/005"), "log names the usbfs node it looked for");
    check(log_has("interface 3-1:1.1") && log_has("endpoint 0x83"), "log lists interface 1 and its endpoint");

    make_node(3, 5);
    press(inst, e);
    readouts(inst, e, r);
    check(!strcmp(r[0], "CLAIM FAILED"), "node exists but ioctls refuse it: verdict");
    check(!strcmp(r[2], "if1 claim: ENOTTY"), "claim readout shows the errno name");
    check(!strcmp(r[3], "not claimed"), "release readout says not claimed");
    check(log_has("open /tmp/opfix/dev/bus/usb/003/005: ok"), "log records the open");

    reset_fixture();
    add_device("3-1", "1935\n", "0b34\n", 3, 5);
    add_iface("3-1", 4, "ff\n", "00\n", "00\n", NULL, 0);
    add_iface("3-1", 5, "01\n", "03\n", "00\n", "snd-usbmidi-lib", 0);
    press(inst, e);
    readouts(inst, e, r);
    check(!strcmp(r[0], "D2 HAS NO IF1"), "no interface 1: verdict");
    check(!strcmp(r[2], "not tried"), "no interface 1: nothing claimed");

    reset_fixture();
    add_device("3-1", "1935\n", "0b34\n", 3, 5);
    add_iface("3-1", 1, "01\n", "03\n", "00\n", NULL, 0);
    make_node(3, 5);
    press(inst, e);
    readouts(inst, e, r);
    check(!strcmp(r[0], "IF1 IS MIDI, SKIPPED"), "interface 1 is USB MIDI: refused, verdict");
    check(!strcmp(r[2], "not tried"), "interface 1 is USB MIDI: no claim attempted");

    reset_fixture();
    add_device("3-1", "1935\n", "1034\n", 3, 6);
    add_iface("3-1", 1, "ff\n", "00\n", "00\n", NULL, 1);
    add_iface("3-1", 5, "01\n", "03\n", "00\n", "snd-usbmidi-lib", 0);
    mkdir("/tmp/opfix/dev/bus/usb/003/006", 0755);
    press(inst, e);
    readouts(inst, e, r);
    check(!strcmp(r[0], "OPEN FAILED"), "node is a directory, open fails: verdict");
    check(!strcmp(r[1], "1935:1034 bus3 dev6"), "Audio/MIDI pid is also recognised");
    check(!strcmp(r[2], "open: other"), "open failure readout");

    reset_fixture();
    add_device("1-1", "1d6b\n", "0002\n", 1, 1);
    press(inst, e);
    readouts(inst, e, r);
    check(!strcmp(r[0], "NO DIGITONE II SEEN"), "unrelated USB device only: verdict");

    reset_fixture();
    add_device("3-2", "1935\n", "0014\n", 3, 7);
    press(inst, e);
    readouts(inst, e, r);
    check(!strcmp(r[0], "NO DIGITONE II PID"), "other Elektron pid: verdict");

    char zero[64] = "";
    e->set_param(inst, "run", "0");
    e->get_param(inst, "p_v", zero, sizeof zero);
    check(!strcmp(zero, "NO DIGITONE II PID"), "releasing the button does not run the probe");

    unsigned char outb[OP_OUT_BYTES];
    fill_out_block(outb, 14);
    check(outb[0] == 0x07 && outb[1] == 0xff && outb[2] == 0x00 && outb[3] == 14, "silent out block carries header 07ff and the counter");
    check(outb[4] == 0 && outb[32] == 0 && outb[OP_OUT_BYTES - 1] == 0, "silent out block is otherwise zero");

    unsigned char pkts[4][OP_PKT_BYTES];
    int lens[4] = { OP_PKT_BYTES, OP_PKT_BYTES, OP_PKT_BYTES, OP_PKT_BYTES };
    struct burst_view view;
    memset(pkts, 0, sizeof pkts);
    for (int i = 0; i < 4; i++) {
        pkts[i][0] = 0x07;
        pkts[i][2] = (unsigned char)((1000 + i * 7) >> 8);
        pkts[i][3] = (unsigned char)(1000 + i * 7);
        pkts[i][32] = 0;
        pkts[i][33] = 0;
        pkts[i][34] = 0x03;
        pkts[i][35] = 0xe8; /* main sample 1000 */
    }
    view_burst(&pkts[0][0], lens, 4, &view);
    check(view.header == OP_HDR_MARK && view.counter_step && view.headers_same && view.bad_len == 0 && view.peak == 1000,
          "four 1012-byte packets with header 0700 and counter +7");

    pkts[2][3] = 0; /* counter no longer steps by 7 */
    view_burst(&pkts[0][0], lens, 4, &view);
    check(!view.counter_step, "a counter that does not rise by 7 is rejected");

    lens[1] = 100;
    view_burst(&pkts[0][0], lens, 4, &view);
    check(view.bad_len == 1, "a short packet is counted");

    {
        struct listen_stats st;
        unsigned char pkt[OP_PKT_BYTES];
        int prev_set = 0;
        uint16_t prev = 0;
        memset(&st, 0, sizeof st);
        st.header = -1;
        memset(pkt, 0, sizeof pkt);
        pkt[0] = 0x07;
        pkt[3] = 7;
        pkt[32] = 0x00;
        pkt[33] = 0x10;
        pkt[34] = 0x00; /* main L */
        note_packet(&st, pkt, OP_PKT_BYTES, &prev_set, &prev);
        check(st.peak[0] > 0 && st.peak[2] == 0 && st.nonzero > 0, "a level on main L is counted on main only");
        pkt[3] = 14;
        pkt[32] = pkt[33] = pkt[34] = 0;
        pkt[32 + 8] = 0x00;
        pkt[33 + 8] = 0x20;
        pkt[34 + 8] = 0x00; /* track 1 L sits after main L/R */
        note_packet(&st, pkt, OP_PKT_BYTES, &prev_set, &prev);
        check(st.peak[2] > 0 && st.counter_gaps == 0, "track 1 level is counted and the counter still steps by 7");
    }

    char buf[64] = "";
    check(e->get_param(inst, "run", buf, sizeof buf) == 0, "trigger key has no readout text");
    check(e->get_param(inst, "state", buf, sizeof buf) == 0, "no state chunk is saved");

    e->destroy(inst);
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
