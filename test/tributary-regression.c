/* Focused regression for the state-restore path, adapted for the integrated
 * v1.2.5 engine (the T1 chunk on top of upstream's mix ownership).
 *
 * The pre-integration regression asserted use_from_mix semantics: that an
 * INACTIVE restore still takes P->used and shared g_use[] decode ownership,
 * with Main forced into every restored mix. That round was deliberately not
 * carried into the integration. The integrated rule is the opposite, and it
 * is what this suite now pins:
 *
 *   - an inactive restore (a cold project load) brings back level, routes
 *     and labels but takes NO decode ownership: P->used stays 0 and g_use[]
 *     stays 0 until the copy is activated;
 *   - Main is never forced into a restored mix: the saved bits are the
 *     truth, and a re-save adds nothing;
 *   - activation hands the shared read exactly the restored mix, and
 *     stopping the copy gives it all back;
 *   - a saved state round-trips without drift, and the switch defaults the
 *     host replays after a project load cannot clobber the restore, whether
 *     the load is cold or lands after audio has run (the switches_live
 *     re-arm).
 *
 * Switch indices from devices.h (tb_src_0): delay=17, reverb=18, chorus=19.
 *
 * The shared read is never started here: g_alive is faked to 1 only inside
 * activation windows, with g_owner clear, and is dropped again before any
 * copy stops or dies — an owner going through hear_off with the flag set
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

/* The core failing scenario of the old defect, restated for the integrated
 * engine: restore an inactive copy with delay+reverb+chorus and a level of
 * 42, the way the MPC does on project load. The routes and the label must
 * come back, the copy must stay inactive, and the shared read must hear
 * nothing of it until the copy is woken. */
static void regression_inactive_restore(const mpc_engine_t *e) {
    void *src = e->create(NULL);
    char chunk[8192] = {0};
    int n;

    e->set_param(src, "level", "42");
    e->set_param(src, "s17", "1"); /* delay */
    e->set_param(src, "s18", "1"); /* reverb */
    e->set_param(src, "s19", "1"); /* chorus */
    check(sw_on(src, e, 17) == 1 && sw_on(src, e, 18) == 1 && sw_on(src, e, 19) == 1,
          "regression setup: delay+reverb+chorus are on");
    check(num_param(src, e, "active") == 0, "regression setup: copy is inactive");
    check(num_param(src, e, "level") == 42, "regression setup: level is 42");

    n = e->get_param(src, "state", chunk, sizeof chunk);
    check(n > 0, "regression: chunk saved");
    check(!strncmp(chunk, "T1 42 0", 7), "regression: chunk is T1 42 0 <mix>");

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

        /* The integrated rule: an inactive restore asks the shared read for
         * nothing at all — no decode mask, no refcounts, Main included. */
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
 * copy. Every switch comes back as saved, on its own, Main is not forced
 * into the mix, and a re-save of the copy writes the same chunk back. */
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

        snprintf(chunk, sizeof chunk, "T1 88 0 %lx", mix);
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
              "combo: re-saving the restored copy writes the same chunk back");

        e->destroy(dst);
    }
}

/* A second save after a restore: the restored state round-trips without
 * drift. */
static void regression_second_save(const mpc_engine_t *e) {
    void *a = e->create(NULL);
    char orig[8192] = {0};

    e->set_param(a, "level", "55");
    e->set_param(a, "s17", "1");
    e->set_param(a, "s18", "1");
    e->set_param(a, "s19", "1");
    e->get_param(a, "state", orig, sizeof orig);
    e->destroy(a);

    {
        void *b = e->create(NULL);
        char after[8192] = {0};
        e->set_param(b, "state", orig);
        e->get_param(b, "state", after, sizeof after);
        check(!strcmp(orig, after), "second save after restore: chunk identical");

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

/* Restore the same chunk into fresh copies repeatedly: no drift, and no
 * decode ownership at any point until one of the copies is woken. */
static void regression_mutation(const mpc_engine_t *e) {
    void *a = e->create(NULL);
    char chunk[8192] = {0};

    e->set_param(a, "level", "33");
    e->set_param(a, "s17", "1");
    e->set_param(a, "s18", "1");
    e->set_param(a, "s19", "1");
    e->get_param(a, "state", chunk, sizeof chunk);
    e->destroy(a);

    {
        void *b = e->create(NULL);
        e->set_param(b, "state", chunk);
        check(sw_on(b, e, 17) == 1 && sw_on(b, e, 18) == 1 && sw_on(b, e, 19) == 1,
              "mutation: all three on after first restore");
        check(((probe_t *)b)->used == 0 && !shared_read_held(),
              "mutation: first restore holds nothing on the shared read");
        e->destroy(b);
    }
    {
        void *c = e->create(NULL);
        e->set_param(c, "state", chunk);
        check(sw_on(c, e, 17) == 1 && sw_on(c, e, 18) == 1 && sw_on(c, e, 19) == 1,
              "mutation: all three on after second restore");
        check(((probe_t *)c)->used == 0 && !shared_read_held(),
              "mutation: second restore holds nothing on the shared read either");
        e->destroy(c);
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
    e->get_param(src, "state", chunk, sizeof chunk);
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

/* Guard for the cold-load default-switch rule. state_load() re-arms the
 * guard against replayed switch defaults (it clears switches_live), so the
 * values the host replays after a project load cannot clear the restored
 * DLY/REV/CHO. CONTROL: restore, then default-replay, with no audio before
 * the load. CANDIDATE: render once first, so the load lands mid-session —
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
        /* replay MPC defaults: all off */
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
        e->set_param(dst, "s17", "0");
        e->set_param(dst, "s18", "0");
        e->set_param(dst, "s19", "0");

        check(sw_on(dst, e, 17) == 1, "switches_live_diag CANDIDATE: delay stays ON after render->restore->default-replay");
        check(sw_on(dst, e, 18) == 1, "switches_live_diag CANDIDATE: reverb stays ON after render->restore->default-replay");
        check(sw_on(dst, e, 19) == 1, "switches_live_diag CANDIDATE: chorus stays ON after render->restore->default-replay");
        e->destroy(dst);
    }
}

int main(void) {
    const mpc_engine_t *e = mpc_engine();

    /* No thread is ever started in this suite: g_alive is faked only inside
     * the activation windows in fake_live(), and dropped again there before
     * any copy stops or dies. Everything else runs with the read down. */
    regression_inactive_restore(e);
    regression_combos(e);
    regression_second_save(e);
    regression_mutation(e);
    regression_full_state(e);
    reg_switches_live_diagnostic(e);

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
