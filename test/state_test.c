/* Host test for project state: the save/chunk/restore path the VST2 wrapper
 * uses. The wrapper (vst2_wrap.c in mpc-vst-plugins) saves one chunk per copy
 * with get_param(inst, "state", buf, 8192) and restores it with
 * set_param(inst, "state", chunk), both under its own lock. These tests drive
 * that same key the way the wrapper does: save from one copy, restore into a
 * fresh copy, and feed damaged chunks. No USB is claimed; the shared-read
 * flag is forced the way host_test.c does, so no thread is started.
 *
 * Adapted from the state test written for local commit 937a83e (the
 * standalone T1 round). The chunk-format work from that commit is kept as it
 * was, and the additions below pin what the v1.2.5 integration changed: the
 * legacy "v1 <mix> <level>" input line, the mix_owned rule (a valid restore
 * or a real switch press refuses later SOURCE writes), the switches_live
 * re-arm (a loaded copy refuses the switch defaults the host replays after a
 * project load, cold or mid-session), and the ownership rule that an
 * inactive restore holds no decode — P->used and the shared g_use[] stay at
 * zero until the copy is woken, and Main is never forced into a restored
 * mix. Compiles with the same command as host_test.c (needs Linux headers).
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

static int get_state(void *inst, const mpc_engine_t *e, char *buf, int n) {
    buf[0] = '\0';
    return e->get_param(inst, "state", buf, n);
}

static int sw_on(void *inst, const mpc_engine_t *e, int i) {
    char key[16], val[16] = "";
    snprintf(key, sizeof key, "s%d", i);
    e->get_param(inst, key, val, sizeof val);
    return atoi(val);
}

static int num_param(void *inst, const mpc_engine_t *e, const char *key) {
    char val[16] = "";
    e->get_param(inst, key, val, sizeof val);
    return atoi(val);
}

static void set_all_switches(void *inst, const mpc_engine_t *e, int on) {
    char val[4];
    snprintf(val, sizeof val, "%d", on ? 1 : 0);
    for (int i = 0; i < TB_MAX; i++) {
        char key[16];
        snprintf(key, sizeof key, "s%d", i);
        e->set_param(inst, key, val);
    }
}

static int routes_match(void *inst, const mpc_engine_t *e, const int *want) {
    for (int i = 0; i < TB_MAX; i++)
        if (sw_on(inst, e, i) != want[i]) return 0;
    return 1;
}

static int shared_read_held(void) {
    for (int i = 0; i < TB_MAX; i++)
        if (atomic_load(&g_use[i]) != 0) return 1;
    return 0;
}

/* Restore into a fresh copy, the way a project load does, and answer whether
 * the routes came back. Never rereads the saved object. */
static void fresh_restore(const mpc_engine_t *e, const char *chunk, void **out) {
    void *r = e->create(NULL);
    e->set_param(r, "state", chunk);
    *out = r;
}

int main(void) {
    const mpc_engine_t *e = mpc_engine();
    char chunk[8192], again[8192], label[64];

    /* A copy with a level and three routes on saves a state chunk. */
    void *a = e->create(NULL);
    e->set_param(a, "level", "67");
    e->set_param(a, "s5", "1");
    e->set_param(a, "s17", "1");
    int three[] = {1,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0};
    check(routes_match(a, e, three), "setup: main, track 5 and delay are on");
    {
        int n = get_state(a, e, chunk, sizeof chunk);
        check(n > 0, "a copy with routes saves a state chunk");
        check(n > 0 && !strncmp(chunk, "T1 ", 3), "the chunk names version 1 of the state line");
        check(n > 0 && strlen(chunk) == (size_t)n && strlen(chunk) < sizeof chunk,
              "the chunk is one short line of text, far under the wrapper's 8192");
    }

    /* The chunk restores into a fresh copy: level, every route, level label. */
    {
        void *b;
        probe_t *B;
        fresh_restore(e, chunk, &b);
        check(num_param(b, e, "level") == 67, "a fresh copy restores the level");
        check(routes_match(b, e, three), "a fresh copy restores every route selection");
        e->get_param(b, "srcname", label, sizeof label);
        check(strstr(label, "main") && strstr(label, "track 5") && strstr(label, "delay"),
              "the restored copy names the same outputs");
        check(num_param(b, e, "active") == 0, "an inactive copy restores inactive");
        B = (probe_t *)b;
        check(B->used == 0 && !shared_read_held(),
              "an inactive restore takes no decode mask and no shared-read refcounts");
        e->destroy(b);
    }

    /* Waking the restored copy is what hands its routes to the shared read:
     * the decode follows activation, never the save. The read is faked, so
     * no thread is started; the flag drops again before the copy stops. */
    {
        void *w;
        atomic_store(&g_exited, 0);
        atomic_store(&g_alive, 1);
        g_owner = NULL;
        fresh_restore(e, chunk, &w);
        e->set_param(w, "active", "1");
        check(((probe_t *)w)->used == 0x20021u && atomic_load(&g_use[0]) == 1 &&
              atomic_load(&g_use[5]) == 1 && atomic_load(&g_use[17]) == 1,
              "waking a restored copy decodes exactly the restored routes");
        atomic_store(&g_alive, 0);
        e->set_param(w, "active", "0");
        check(((probe_t *)w)->used == 0 && !shared_read_held(),
              "stopping the woken copy gives the decode back");
        e->destroy(w);
    }

    /* An untouched copy saves its creation defaults, so a host that saves at
     * once does not lose the main switch. */
    {
        void *d0 = e->create(NULL);
        int n = get_state(d0, e, chunk, sizeof chunk);
        check(n > 0 && !strcmp(chunk, "T1 100 0 1"), "a fresh copy saves the creation defaults");
        e->destroy(d0);
    }
    e->destroy(a);

    /* Empty route set round-trips. */
    {
        void *c = e->create(NULL), *d;
        int none[TB_MAX] = {0};
        set_all_switches(c, e, 0);
        check(get_state(c, e, chunk, sizeof chunk) > 0, "an empty route set still saves a chunk");
        fresh_restore(e, chunk, &d);
        check(routes_match(d, e, none), "an empty route set restores empty");
        e->get_param(d, "srcname", label, sizeof label);
        check(!strcmp(label, "none"), "an empty route set names none");
        e->destroy(d);
        e->destroy(c);
    }

    /* Fully populated route set round-trips. */
    {
        void *c = e->create(NULL), *d;
        int all[TB_MAX];
        for (int i = 0; i < TB_MAX; i++) all[i] = 1;
        set_all_switches(c, e, 1);
        e->set_param(c, "level", "0");
        check(get_state(c, e, chunk, sizeof chunk) > 0, "a fully populated route set saves a chunk");
        fresh_restore(e, chunk, &d);
        check(routes_match(d, e, all), "a fully populated route set restores every switch");
        check(num_param(d, e, "level") == 0, "level zero round-trips");
        e->destroy(d);
        e->destroy(c);
    }

    /* ACTIVE round-trips. The shared read is faked, so restoring it only
     * marks the copy, exactly as host_test.c does for its two-copy checks. */
    {
        void *c = e->create(NULL), *d;
        atomic_store(&g_exited, 0);
        atomic_store(&g_alive, 1);
        e->set_param(c, "active", "1");
        check(num_param(c, e, "active") == 1, "setup: the saved copy is active");
        check(get_state(c, e, chunk, sizeof chunk) > 0 && !strcmp(chunk, "T1 100 1 1"),
              "the chunk carries active");
        fresh_restore(e, chunk, &d);
        check(num_param(d, e, "active") == 1, "a fresh copy restores ACTIVE on");
        e->set_param(d, "active", "0");
        atomic_store(&g_alive, 0);
        e->set_param(c, "active", "0");
        e->destroy(d);
        e->destroy(c);
    }

    /* A restore into a copy that is already ACTIVE must re-point the shared
     * read at the saved routes, the way the live switch and SOURCE paths do.
     * The shared read is faked, so nothing here starts a thread. */
    {
        void *c = e->create(NULL);
        probe_t *P = (probe_t *)c;
        atomic_store(&g_exited, 0);
        atomic_store(&g_alive, 1);
        g_owner = NULL;
        for (int i = 0; i < TB_MAX; i++) atomic_store(&g_use[i], 0);
        e->set_param(c, "s0", "1");
        e->set_param(c, "s5", "1");
        e->set_param(c, "active", "1");
        check(P->used == 0x21u && atomic_load(&g_use[0]) == 1 && atomic_load(&g_use[5]) == 1,
              "setup: the live copy holds main and track 5 on the shared read");
        e->set_param(c, "state", "T1 67 1 20000"); /* delay only, still ACTIVE */
        check(P->used == 0x20000u,
              "a restore into a live copy re-points the shared read at the saved routes");
        check(atomic_load(&g_use[17]) == 1 && atomic_load(&g_use[0]) == 0 && atomic_load(&g_use[5]) == 0,
              "the restored routes replace the old ones in the shared-read refcounts");
        check(sw_on(c, e, 17) == 1 && sw_on(c, e, 0) == 0 && sw_on(c, e, 5) == 0,
              "the switches themselves follow the restored mask");
        e->set_param(c, "s3", "1");
        check(P->used == 0x20008u && atomic_load(&g_use[3]) == 1,
              "control: a live switch after the restore still moves the refcounts");
        atomic_store(&g_alive, 0);
        e->set_param(c, "active", "0");
        check(P->used == 0u && atomic_load(&g_use[17]) == 0 && atomic_load(&g_use[3]) == 0,
              "ACTIVE off after a restored mask drops every refcount");
        e->destroy(c);
    }

    /* The legacy SOURCE selector is a route too: it comes back by position,
     * not remapped. */
    {
        void *c = e->create(NULL), *d;
        e->set_param(c, "source", "17");
        check(get_state(c, e, chunk, sizeof chunk) > 0, "a SOURCE selection saves a chunk");
        fresh_restore(e, chunk, &d);
        check(num_param(d, e, "source") == 17, "a saved SOURCE comes back as the same output");
        e->get_param(d, "srcname", label, sizeof label);
        check(!strcmp(label, "delay"), "the restored SOURCE names delay");
        e->destroy(d);
        e->destroy(c);
    }

    /* mix_owned: a valid restore owns the row, so a later SOURCE write (the
     * meter-refresh echo) cannot replace the restored switches. A real switch
     * press owns the row the same way. A fresh copy still takes a SOURCE. */
    {
        void *c = e->create(NULL), *d;
        e->set_param(c, "source", "17");
        check(get_state(c, e, chunk, sizeof chunk) > 0, "setup: a SOURCE-only copy saved its chunk");
        e->destroy(c);
        fresh_restore(e, chunk, &d);
        e->set_param(d, "source", "5");
        check(num_param(d, e, "source") == 17 && sw_on(d, e, 17) == 1 && sw_on(d, e, 5) == 0,
              "a SOURCE write after a valid restore cannot replace the routes");
        e->get_param(d, "p_rel", label, sizeof label);
        check(!strcmp(label, "delay"), "the refused SOURCE write leaves the label alone");
        e->destroy(d);
    }
    {
        void *c = e->create(NULL);
        e->set_param(c, "s5", "1");
        e->set_param(c, "source", "3");
        check(sw_on(c, e, 5) == 1 && sw_on(c, e, 3) == 0,
              "a switch press also owns the row against a later SOURCE write");
        e->destroy(c);
    }

    /* Cold load: the host replays the switch defaults after a project load,
     * and a restored copy must refuse them. A load that lands after audio has
     * run re-arms the same guard, so the replay is refused mid-session too. */
    {
        void *c = e->create(NULL), *d;
        e->set_param(c, "level", "40");
        e->set_param(c, "s3", "1");
        e->set_param(c, "s5", "1");
        check(get_state(c, e, chunk, sizeof chunk) > 0, "setup: level 40 with track 3 and track 5 saved");
        e->destroy(c);
        fresh_restore(e, chunk, &d);
        e->set_param(d, "s0", "1"); /* the main default, replayed by the host */
        e->set_param(d, "s3", "0");
        e->set_param(d, "s5", "0");
        check(num_param(d, e, "level") == 40 && sw_on(d, e, 3) == 1 && sw_on(d, e, 5) == 1 &&
              sw_on(d, e, 0) == 1,
              "a cold load refuses the switch defaults the host replays");
        e->destroy(d);
        d = e->create(NULL);
        {
            int16_t tmp[4];
            e->render(d, tmp, 2); /* audio ran before the project loaded */
        }
        e->set_param(d, "state", chunk);
        e->set_param(d, "s0", "1");
        e->set_param(d, "s3", "0");
        e->set_param(d, "s5", "0");
        check(num_param(d, e, "level") == 40 && sw_on(d, e, 3) == 1 && sw_on(d, e, 5) == 1,
              "a load mid-session re-arms the guard against replayed defaults");
        e->destroy(d);
    }

    /* The 1.2.5 line, "v1 <mix> <level>", still loads: level and routes come
     * back, the copy never wakes, and the restore owns the row like a T1. */
    {
        void *d;
        fresh_restore(e, "v1 22 40", &d);
        check(sw_on(d, e, 1) == 1 && sw_on(d, e, 5) == 1 && num_param(d, e, "level") == 40,
              "a 1.2.5 v1 chunk restores its level and routes");
        check(num_param(d, e, "active") == 0, "a v1 chunk never wakes the copy");
        check(((probe_t *)d)->used == 0 && !shared_read_held(),
              "a v1 restore takes no decode ownership either");
        e->set_param(d, "source", "0");
        check(sw_on(d, e, 1) == 1 && sw_on(d, e, 5) == 1,
              "a SOURCE write after a v1 restore keeps the routes too");
        e->destroy(d);
    }

    /* Repeated save/load cycles are stable: the chunk does not drift. */
    {
        void *c = e->create(NULL);
        e->set_param(c, "level", "67");
        e->set_param(c, "s5", "1");
        e->set_param(c, "s17", "1");
        check(get_state(c, e, chunk, sizeof chunk) > 0, "setup: first chunk saved");
        for (int i = 0; i < 5; i++) {
            void *d;
            fresh_restore(e, chunk, &d);
            check(get_state(d, e, again, sizeof again) > 0, "cycle saves a chunk again");
            check(!strcmp(chunk, again), "save/load cycles do not drift");
            check(num_param(d, e, "level") == 67 && sw_on(d, e, 5) && sw_on(d, e, 17) && !sw_on(d, e, 1),
                  "cycle keeps the level and the routes");
            e->destroy(d);
        }
        e->destroy(c);
    }

    /* Damaged chunks: a copy keeps what it had, and nothing crashes. The
     * copy holds values that differ from every field the bad chunks carry
     * (level 12 against their 67, track 3 against their routes, inactive
     * against their active=1), so a parser that half-applied level, ACTIVE,
     * or mix before refusing would be caught here. The shared read is
     * faked, so even a half-applied ACTIVE cannot start a thread. */
    {
        static const char *const bad[] = {
            "",                       /* absent */
            "T2 67 1 5",              /* unsupported version */
            "T1",                     /* truncated: no fields */
            "T1 67",                  /* truncated: no active */
            "T1 67 1",                /* truncated: no mix */
            "T1 abc 1 5",             /* level is not a number */
            "T1 300 1 5",             /* level out of range */
            "T1 -5 1 5",              /* level is negative */
            "T1 67 2 5",              /* active out of range */
            "T1 67 1 xyz",            /* mix is not hex */
            "T1 67 1 100000000",      /* mix overflows the line */
            "T1 67 1 5 junk",         /* trailing junk */
            "T1 67 1 5 ",             /* trailing space */
            " R1 67 1 5",             /* wrong magic */
            "t1 67 1 5",              /* magic is case-bound */
            /* separators: T, the version, then exactly one space per field */
            "T1 67 1a",               /* no space before mix; "a" reads as hex */
            "T 1 67 1 5",             /* a space inside the magic */
            "T1  67 1 5",             /* two spaces before the level */
            "T1 67 1  5",             /* two spaces before the mix */
            /* no leading zeros past a single digit */
            "T01 67 1 5",             /* version 01 */
            "T1 0067 1 5",            /* level 0067 */
            "T1 67 01 5",             /* active 01 */
            "T1 67 1 05",             /* mix 05 */
        };
        void *c = e->create(NULL);
        atomic_store(&g_exited, 0);
        atomic_store(&g_alive, 1);
        g_owner = NULL;
        e->set_param(c, "level", "12");
        e->set_param(c, "s3", "1");
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            e->set_param(c, "state", bad[i]);
            char name[64];
            snprintf(name, sizeof name, "damaged chunk %.24s keeps the copy as it was", bad[i][0] ? bad[i] : "(empty)");
            check(num_param(c, e, "level") == 12 && sw_on(c, e, 3) == 1 && sw_on(c, e, 0) == 1 &&
                  num_param(c, e, "active") == 0, name);
        }
        {
            char junk[9001];
            memset(junk, 'x', sizeof junk - 1);
            junk[sizeof junk - 1] = '\0';
            e->set_param(c, "state", junk);
            check(num_param(c, e, "level") == 12 && sw_on(c, e, 3) == 1,
                  "a chunk longer than the wrapper's 8192 is refused, not crashed on");
        }
        atomic_store(&g_alive, 0);
        e->set_param(c, "active", "0");
        e->destroy(c);
    }

    /* Missing state on a copy that already holds values: defaults are kept,
     * nothing is half-applied. */
    {
        void *c = e->create(NULL);
        e->set_param(c, "level", "40");
        e->set_param(c, "s3", "1");
        e->set_param(c, "state", "");
        check(num_param(c, e, "level") == 40 && sw_on(c, e, 3) == 1 && sw_on(c, e, 0) == 1,
              "absent state keeps the copy at its current values");
        e->destroy(c);
    }

    /* Route bits past the machine's map are dropped, never moved. */
    {
        void *d;
        fresh_restore(e, "T1 67 0 220000", &d); /* bit 17 (delay) + bit 21 (past the map) */
        check(sw_on(d, e, 17) == 1 && sw_on(d, e, 0) == 0,
              "a saved route past the map is dropped and the real routes come back");
        e->get_param(d, "srcname", label, sizeof label);
        check(!strcmp(label, "delay"), "the surviving route is still delay, not remapped");
        e->destroy(d);
        fresh_restore(e, "T1 67 0 200000", &d); /* only a bit past the map */
        check(sw_on(d, e, 0) == 0 && sw_on(d, e, 20) == 0, "a chunk with only past-the-map bits restores no routes");
        e->get_param(d, "srcname", label, sizeof label);
        check(!strcmp(label, "none"), "past-the-map bits leave the sum quiet");
        e->destroy(d);
        fresh_restore(e, "T1 67 0 1fffff", &d); /* all 21 bits of the map */
        {
            int all[TB_MAX];
            for (int i = 0; i < TB_MAX; i++) all[i] = 1;
            check(routes_match(d, e, all), "all 21 map bits restore");
        }
        e->destroy(d);
    }

    /* A buffer too small for the line is refused whole, not truncated. */
    {
        void *c = e->create(NULL);
        char small[4];
        check(get_state(c, e, small, sizeof small) == 0, "a tiny buffer gets no chunk instead of a cut one");
        e->destroy(c);
    }

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
