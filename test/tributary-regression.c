/* Focused regression for the routing/restore path of the project state line,
 * written against the upstream "v1 <mix> <level>" format. The state-format
 * work (the T1 chunk with level, active and mix) is a separate change and
 * this suite does not depend on it: every chunk here is read and written in
 * the 1.2.5 wire format, so the routing fix is reviewable and testable on
 * its own.
 *
 * What is pinned here:
 *
 *   - a saved "v1" line restores level, routes and labels into a fresh
 *     copy, and the copy stays inactive with nothing held on the shared
 *     read until it is woken;
 *   - waking the copy decodes exactly the saved mix, and stopping gives
 *     every refcount back;
 *   - a restore into a copy that is already ACTIVE re-points the shared
 *     read at the saved routes, the way the live switch path does;
 *   - mix ownership is preserved: a valid restore and a real switch press
 *     own the row, so a later SOURCE write (the meter-refresh echo) cannot
 *     replace the routes; Main is never forced into a restored mix;
 *   - the switch defaults the host replays after a project load cannot
 *     clobber the restore, whether the load is cold or lands after audio
 *     has run. That last case is the routing fix this suite exists for:
 *     the load re-arms the switch-default guard by clearing switches_live.
 *
 * Switch indices from devices.h (tb_src_0): delay=17, reverb=18, chorus=19.
 *
 * The shared read is never started here: g_alive is faked to 1 only inside
 * activation windows, with g_owner clear, and is dropped again before any
 * copy stops or dies - an owner going through hear_off with the flag set
 * would join a thread that was never started. No claim is made here about
 * double-STOP on the device; that stays unverified without hardware.
 *
 * Runs inside test/run.sh (gcc:12; needs Linux headers).
 */
#define OP_SYSFS_USB "/tmp/opfix/sys/devices"
#define OP_DEV_USB "/tmp/opfix/dev/bus/usb"
#define OP_PROC_USB "/tmp/opfix/proc/bus/usb"
#define OP_LOG_PATHS "/tmp/opfix-nodir/x.log", "/tmp/opfix-log/tributary.log"

#include "../src/probe.c"

#include <assert.h>

static int failures = 0;
static void check(int ok, const char *what) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static int sw_on(void *inst, const mpc_engine_t *e, int i) {
    char val[16] = "";
    char key[16];
    snprintf(key, sizeof key, "s%d", i);
    e->get_param(inst, key, val, sizeof val);
    return atoi(val);
}

static int num_param(void *inst, const mpc_engine_t *e, const char *key) {
    char val[16] = "";
    e->get_param(inst, key, val, sizeof val);
    return atoi(val);
}

static int get_state(void *inst, const mpc_engine_t *e, char *buf, int n) {
    buf[0] = '\0';
    return e->get_param(inst, "state", buf, n);
}

static int shared_read_held(void) {
    for (int i = 0; i < TB_MAX; i++)
        if (atomic_load(&g_use[i]) != 0) return 1;
    return 0;
}

/* Fake a running shared read so hear_on takes the join path and starts no
 * thread, and start the ownership accounting from a clean sheet. The caller
 * must drop g_alive back to 0 before any copy stops or is destroyed. */
static void fake_live(void) {
    atomic_store(&g_exited, 0);
    atomic_store(&g_alive, 1);
    g_owner = NULL;
    for (int i = 0; i < TB_MAX; i++) atomic_store(&g_use[i], 0);
}

/* The core failing scenario, restated for the v1 line: restore an inactive
 * copy with delay+reverb+chorus and a level of 42, the way the MPC does on
 * project load. The routes and the label must come back, the copy must stay
 * inactive, and the shared read must hear nothing of it until the copy is
 * woken. */
static void regression_inactive_restore(const mpc_engine_t *e) {
    void *src = e->create(NULL);
    char chunk[8192] = {0};

    e->set_param(src, "level", "42");
    e->set_param(src, "s17", "1"); /* delay */
    e->set_param(src, "s18", "1"); /* reverb */
    e->set_param(src, "s19", "1"); /* chorus */
    check(sw_on(src, e, 17) == 1 && sw_on(src, e, 18) == 1 && sw_on(src, e, 19) == 1,
          "regression setup: delay+reverb+chorus are on");
    check(num_param(src, e, "active") == 0, "regression setup: copy is inactive");
    check(num_param(src, e, "level") == 42, "regression setup: level is 42");

    check(e->get_param(src, "state", chunk, sizeof chunk) > 0, "regression: chunk saved");
    check(!strncmp(chunk, "v1 ", 3), "regression: chunk is a v1 line");
    e->destroy(src);

    /* restore into a FRESH copy */
    {
        void *dst = e->create(NULL);
        probe_t *P;
        char label[64] = {0};

        e->set_param(dst, "state", chunk);

        check(sw_on(dst, e, 17) == 1, "regression: delay restored to ON");
        check(sw_on(dst, e, 18) == 1, "regression: reverb restored to ON");
        check(sw_on(dst, e, 19) == 1, "regression: chorus restored to ON");

        e->get_param(dst, "srcname", label, sizeof label);
        check(strstr(label, "delay") != NULL, "regression: srcname contains 'delay'");
        check(strstr(label, "reverb") != NULL, "regression: srcname contains 'reverb'");
        check(strstr(label, "chorus") != NULL, "regression: srcname contains 'chorus'");

        check(num_param(dst, e, "level") == 42, "regression: level restored to 42");
        check(num_param(dst, e, "active") == 0, "regression: copy stays inactive");

        /* A v1 line names no active flag, so the restore must never wake or
         * stop the copy: an inactive restore asks the shared read for
         * nothing at all - no decode mask, no refcounts, Main included. */
        P = (probe_t *)dst;
        check(P->used == 0 && !shared_read_held(),
              "regression: an inactive restore holds nothing on the shared read");

        /* Waking the copy is what hands the saved routes over, exactly. */
        fake_live();
        e->set_param(dst, "active", "1");
        check(P->used == 0xE0001u &&
              atomic_load(&g_use[0]) == 1 && atomic_load(&g_use[17]) == 1 &&
              atomic_load(&g_use[18]) == 1 && atomic_load(&g_use[19]) == 1,
              "regression: activation decodes exactly the restored mix");
        atomic_store(&g_alive, 0);
        e->set_param(dst, "active", "0");
        check(P->used == 0 && !shared_read_held(),
              "regression: stopping the copy gives the decode back");

        e->destroy(dst);
    }
}

/* All 8 delay/reverb/chorus combinations, restored inactive into a fresh
 * copy from a hand-written v1 line. Every switch comes back as saved, on
 * its own, Main is not forced into the mix, and a re-save of the copy
 * writes the same line back. */
static void regression_combos(const mpc_engine_t *e) {
    struct { const char *name; unsigned dly, rev, cho; } combos[] = {
        {"dly only",    1, 0, 0},
        {"rev only",    0, 1, 0},
        {"cho only",    0, 0, 1},
        {"dly+rev",     1, 1, 0},
        {"dly+cho",     1, 0, 1},
        {"rev+cho",     0, 1, 1},
        {"dly+rev+cho", 1, 1, 1},
        {"none",        0, 0, 0},
    };
    for (int c = 0; c < (int)(sizeof combos / sizeof combos[0]); c++) {
        void *dst = e->create(NULL);
        char chunk[8192], again[8192], label[96];
        unsigned long mix = (combos[c].dly << 17) | (combos[c].rev << 18) | (combos[c].cho << 19);
        probe_t *P;
        int held;

        snprintf(chunk, sizeof chunk, "v1 %lx 88", mix);
        e->set_param(dst, "state", chunk);

        snprintf(label, sizeof label, "combo %s: delay, reverb and chorus restore as saved, not coupled",
                 combos[c].name);
        check(sw_on(dst, e, 17) == (int)combos[c].dly &&
              sw_on(dst, e, 18) == (int)combos[c].rev &&
              sw_on(dst, e, 19) == (int)combos[c].cho, label);

        snprintf(label, sizeof label, "combo %s: Main is not forced into the restored mix",
                 combos[c].name);
        check(sw_on(dst, e, 0) == 0, label);

        P = (probe_t *)dst;
        held = shared_read_held();
        snprintf(label, sizeof label, "combo %s: inactive restore holds nothing on the shared read",
                 combos[c].name);
        check(P->used == 0 && !held, label);

        check(get_state(dst, e, again, sizeof again) > 0 && !strcmp(chunk, again),
              "combo: re-saving the restored copy writes the same v1 line back");

        e->destroy(dst);
    }
}

/* MAIN, a distinctive level, representative tracks and IN in one state, to
 * catch per-copy state swapping: restored cold with no decode, then woken
 * once to show the decode follows activation with the exact saved mix. */
static void regression_full_state(const mpc_engine_t *e) {
    void *src = e->create(NULL);
    char chunk[8192] = {0};
    uint32_t expected = (1u << 0) | (1u << 1) | (1u << 5) | (1u << 17) | (1u << 18) | (1u << 19) | (1u << 20);

    e->set_param(src, "level", "77");
    e->set_param(src, "s0", "1");   /* MAIN */
    e->set_param(src, "s1", "1");   /* track 1 */
    e->set_param(src, "s5", "1");   /* track 5 */
    e->set_param(src, "s17", "1");  /* delay */
    e->set_param(src, "s18", "1");  /* reverb */
    e->set_param(src, "s19", "1");  /* chorus */
    e->set_param(src, "s20", "1");  /* IN */
    check(get_state(src, e, chunk, sizeof chunk) > 0, "full state: chunk saved");
    check(!strncmp(chunk, "v1 ", 3), "full state: chunk is a v1 line");
    e->destroy(src);

    {
        void *dst = e->create(NULL);
        probe_t *P;
        e->set_param(dst, "state", chunk);

        check(sw_on(dst, e, 0) == 1 && sw_on(dst, e, 1) == 1 && sw_on(dst, e, 5) == 1 &&
              sw_on(dst, e, 17) == 1 && sw_on(dst, e, 18) == 1 && sw_on(dst, e, 19) == 1 &&
              sw_on(dst, e, 20) == 1,
              "full state: all 7 switches restored");
        check(num_param(dst, e, "level") == 77, "full state: level is 77");
        check(num_param(dst, e, "active") == 0, "full state: inactive");

        P = (probe_t *)dst;
        check(P->used == 0 && !shared_read_held(),
              "full state: the cold restore holds nothing on the shared read");

        fake_live();
        e->set_param(dst, "active", "1");
        check(P->used == expected, "full state: activation decodes the exact saved mix");
        check(atomic_load(&g_use[0]) == 1 && atomic_load(&g_use[1]) == 1 &&
              atomic_load(&g_use[5]) == 1 && atomic_load(&g_use[17]) == 1 &&
              atomic_load(&g_use[18]) == 1 && atomic_load(&g_use[19]) == 1 &&
              atomic_load(&g_use[20]) == 1,
              "full state: every restored output holds one refcount");
        atomic_store(&g_alive, 0);
        e->set_param(dst, "active", "0");
        check(P->used == 0 && !shared_read_held(),
              "full state: stopping gives every refcount back");
        e->destroy(dst);
    }
}

/* A restore into a copy that is already ACTIVE must re-point the shared
 * read at the saved routes, the way the live switch and SOURCE paths do.
 * The shared read is faked, so nothing here starts a thread. */
static void regression_live_restore(const mpc_engine_t *e) {
    void *c = e->create(NULL);
    probe_t *P = (probe_t *)c;
    fake_live();
    e->set_param(c, "s0", "1");
    e->set_param(c, "s5", "1");
    e->set_param(c, "active", "1");
    check(P->used == 0x21u && atomic_load(&g_use[0]) == 1 && atomic_load(&g_use[5]) == 1,
          "live restore: setup, the playing copy holds main and track 5");
    e->set_param(c, "state", "v1 20000 67"); /* delay only, copy still ACTIVE */
    check(P->used == 0x20000u,
          "live restore: the restore re-points the shared read at the saved routes");
    check(atomic_load(&g_use[17]) == 1 && atomic_load(&g_use[0]) == 0 && atomic_load(&g_use[5]) == 0,
          "live restore: the saved routes replace the old ones in the refcounts");
    check(sw_on(c, e, 17) == 1 && sw_on(c, e, 0) == 0 && sw_on(c, e, 5) == 0,
          "live restore: the switches themselves follow the restored mask");
    e->set_param(c, "s3", "1");
    check(P->used == 0x20008u && atomic_load(&g_use[3]) == 1,
          "live restore: control, a live switch after the restore still moves the refcounts");
    atomic_store(&g_alive, 0);
    e->set_param(c, "active", "0");
    check(P->used == 0u && atomic_load(&g_use[17]) == 0 && atomic_load(&g_use[3]) == 0,
          "live restore: ACTIVE off after the restored mask drops every refcount");
    e->destroy(c);
}

/* mix_owned: a valid restore owns the row, so a later SOURCE write (the
 * meter-refresh echo) cannot replace the restored switches. A real switch
 * press owns the row the same way. A fresh copy still takes a SOURCE, and a
 * saved SOURCE selection comes back as the same output, by position. */
static void regression_mix_owned(const mpc_engine_t *e) {
    void *c = e->create(NULL), *d;
    char chunk[8192], label[64];

    e->set_param(c, "source", "17");
    check(get_state(c, e, chunk, sizeof chunk) > 0, "mix_owned: a SOURCE-only copy saved its chunk");
    e->destroy(c);
    d = e->create(NULL);
    e->set_param(d, "state", chunk);
    check(num_param(d, e, "source") == 17, "mix_owned: the saved SOURCE comes back as the same output");
    e->get_param(d, "srcname", label, sizeof label);
    check(!strcmp(label, "delay"), "mix_owned: the restored SOURCE names delay");
    e->set_param(d, "source", "5");
    check(num_param(d, e, "source") == 17 && sw_on(d, e, 17) == 1 && sw_on(d, e, 5) == 0,
          "mix_owned: a SOURCE write after a valid restore cannot replace the routes");
    e->get_param(d, "p_rel", label, sizeof label);
    check(!strcmp(label, "delay"), "mix_owned: the refused SOURCE write leaves the label alone");
    e->destroy(d);
    c = e->create(NULL);
    e->set_param(c, "source", "5");
    check(num_param(c, e, "source") == 5 && sw_on(c, e, 5) == 1,
          "mix_owned: control, a fresh copy still takes a SOURCE write");
    e->destroy(c);
    c = e->create(NULL);
    e->set_param(c, "s5", "1");
    e->set_param(c, "source", "3");
    check(sw_on(c, e, 5) == 1 && sw_on(c, e, 3) == 0,
          "mix_owned: a switch press also owns the row against a later SOURCE write");
    e->destroy(c);
}

/* Guard for the cold-load default-switch rule. The state load re-arms the
 * guard against replayed switch defaults (it clears switches_live), so the
 * values the host replays after a project load cannot clear the restored
 * DLY/REV/CHO. CONTROL: restore, then default-replay, with no audio before
 * the load. CANDIDATE: render once first, so the load lands mid-session -
 * the restore must arm the guard again all the same. */
static void reg_switches_live_diagnostic(const mpc_engine_t *e) {
    void *src = e->create(NULL);
    char chunk[8192] = {0};

    e->set_param(src, "level", "42");
    e->set_param(src, "s17", "1");
    e->set_param(src, "s18", "1");
    e->set_param(src, "s19", "1");
    check(sw_on(src, e, 17) == 1 && sw_on(src, e, 18) == 1 && sw_on(src, e, 19) == 1,
          "switches_live_diag: source DLY+REV+CHO ON");
    check(e->get_param(src, "state", chunk, sizeof chunk) > 0, "switches_live_diag: chunk saved");
    e->destroy(src);

    /* ---- CONTROL: restore without render ---- */
    {
        void *dst = e->create(NULL);
        e->set_param(dst, "state", chunk);
        /* replay MPC defaults: main on, the rest off */
        e->set_param(dst, "s0", "1");
        e->set_param(dst, "s17", "0");
        e->set_param(dst, "s18", "0");
        e->set_param(dst, "s19", "0");

        check(sw_on(dst, e, 17) == 1, "switches_live_diag CONTROL: delay stays ON after default-replay");
        check(sw_on(dst, e, 18) == 1, "switches_live_diag CONTROL: reverb stays ON after default-replay");
        check(sw_on(dst, e, 19) == 1, "switches_live_diag CONTROL: chorus stays ON after default-replay");
        e->destroy(dst);
    }

    /* ---- CANDIDATE: render before restore, then default-replay ---- */
    {
        void *dst = e->create(NULL);
        int16_t tmp[4];
        e->render(dst, tmp, 2); /* real render API, inactive copy, no thread */
        e->set_param(dst, "state", chunk);
        e->set_param(dst, "s0", "1");
        e->set_param(dst, "s17", "0");
        e->set_param(dst, "s18", "0");
        e->set_param(dst, "s19", "0");

        check(sw_on(dst, e, 17) == 1, "switches_live_diag CANDIDATE: delay stays ON after render->restore->default-replay");
        check(sw_on(dst, e, 18) == 1, "switches_live_diag CANDIDATE: reverb stays ON after render->restore->default-replay");
        check(sw_on(dst, e, 19) == 1, "switches_live_diag CANDIDATE: chorus stays ON after render->restore->default-replay");
        e->destroy(dst);
    }
}

/* A second save after a restore, and the same chunk into fresh copies
 * repeatedly: no drift, and no decode ownership at any point until a copy
 * is woken. */
static void regression_second_save(const mpc_engine_t *e) {
    void *a = e->create(NULL);
    char orig[8192] = {0};

    e->set_param(a, "level", "55");
    e->set_param(a, "s17", "1");
    e->set_param(a, "s18", "1");
    e->set_param(a, "s19", "1");
    check(get_state(a, e, orig, sizeof orig) > 0, "second save: chunk saved");
    e->destroy(a);

    {
        void *b = e->create(NULL);
        char after[8192] = {0};
        e->set_param(b, "state", orig);
        check(get_state(b, e, after, sizeof after) > 0 && !strcmp(orig, after),
              "second save after restore: v1 line identical");
        {
            void *c = e->create(NULL);
            e->set_param(c, "state", after);
            check(sw_on(c, e, 17) == 1 && sw_on(c, e, 18) == 1 && sw_on(c, e, 19) == 1,
                  "third restore: delay/reverb/chorus still on");
            check(num_param(c, e, "level") == 55, "third restore: level still 55");
            check(num_param(c, e, "active") == 0, "third restore: still inactive");
            check(((probe_t *)c)->used == 0 && !shared_read_held(),
                  "third restore: still nothing on the shared read");
            e->destroy(c);
        }
        e->destroy(b);
    }
}

/* A fresh copy saves its creation defaults, so a host that saves at once
 * does not lose the main switch. Pinned in the v1 format here. */
static void regression_fresh_defaults(const mpc_engine_t *e) {
    void *d0 = e->create(NULL);
    char chunk[8192] = {0};
    check(get_state(d0, e, chunk, sizeof chunk) > 0 && !strcmp(chunk, "v1 1 100"),
          "a fresh copy saves the creation defaults as v1 1 100");
    e->destroy(d0);
}

int main(void) {
    const mpc_engine_t *e = mpc_engine();

    /* No thread is ever started in this suite: g_alive is faked only inside
     * the activation windows in fake_live(), and dropped again there before
     * any copy stops or dies. Everything else runs with the read down. */
    regression_fresh_defaults(e);
    regression_inactive_restore(e);
    regression_combos(e);
    regression_full_state(e);
    regression_live_restore(e);
    regression_mix_owned(e);
    reg_switches_live_diagnostic(e);
    regression_second_save(e);

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
