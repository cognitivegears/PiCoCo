# ROM Manager Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A cart-ROM manager that starts on a 16K CoCo 2 with no Extended BASIC, lists the `.ROM` files on the board and launches one, with all screen logic in the Pico firmware.

**Architecture:** A ~0.5 KB 6809 stub (`manager.rom`) polls the firmware over the Becker port with the last key and carries out the action list it gets back (draw text, clear, leave). A new core0 `ui` module owns a 32x16 screen model, diffs it against what the CoCo has, and decides launches. A write to `$FF43` switches the Becker port from DriveWire to a UI session. core1 is not touched.

**Tech Stack:** C11 (firmware, host build with ASan/UBSan, ctest), lwasm (6809), XRoar + `coco/tools/xrscreen.py`, the existing `picoco-host`.

**Spec:** `docs/superpowers/specs/2026-10-01-rom-manager-design.md`. This plan covers spec section 11 steps 1-3 plus the cold/warm restart actions for a CoCo 1/2. Steps 4-8 (Disks, Settings, WiFi, Boot, loader, CoCo 3 restart, double RESET, retiring `PICOCO.BIN`) are a second plan, written after this one's emulator and bench results.

## Global Constraints

- Never touch `firmware/src/bus/bus_core1.c` or add a read hook. The only bus-side change is in `becker_on_write`, which runs on core0.
- core1 stays flash-free: the Pico build's `check_core1_flash_free.py` step must still print `ok`.
- The Pico 2 build must not reference cyw43/lwIP (`net_stub.c` only).
- Stub rules (spec 4.1): interrupts masked, no BASIC ROM call except `POLCAT` (`JSR [$A000]`), never `CLR` a Becker register, entry at `$C002`, image starts with `DK`.
- Screen is 32x16; the firmware sends VDG screen codes, not ASCII.
- Reply to a poll is at most 600 bytes of actions (`UI_REPLY_MAX`).
- Key codes are what `POLCAT` returns: up `$5E`, down `$0A`, ENTER `$0D`, BREAK `$03`.
- CMOC is not used. Do not edit `coco/*.c`.
- Host suite must stay green: `cmake --build build-host && ctest --test-dir build-host --output-on-failure`.
- Work on a branch `rom-manager` off `pcb-v2.3`. The uncommitted core1 read-path fix of 2026-10-01 must be committed on `pcb-v2.3` first (not part of this plan).

## Review Focus

1. **No `.ROM` files on the flash volume** (fresh board, or `fs export` active): the screen says `NO .ROM FILES`, ENTER does nothing, no crash. Test in Task 2.
2. **More ROM files than fit** (over 12 on screen, over 64 in the list): the list scrolls with the selection; past 64 the screen shows `LIST TRUNCATED`. Test in Task 2.
3. **Awkward file names** (a space, 32 characters or longer, lowercase): hidden if unusable by `rom load`, lowercase shown folded to uppercase, never a buffer overrun. Test in Task 2.
4. **Line noise and half a poll** (bad checksum, a poll cut short, stray bytes): no reply, and the parser recovers by itself after 250 ms so the stub's retry works. Test in Task 1.
5. **Launch fails after ENTER** (file removed, wrong size, volume exported): the stub gets `$15`, the session continues and the reason is on the message line. Test in Task 2.

---

## File Structure

| File | Responsibility |
|---|---|
| `firmware/src/ui/ui.h` | Public interface, protocol constants |
| `firmware/src/ui/ui.c` | Poll parser, screen model, diff, reply framing, ROMs screen, launch |
| `firmware/tests/test_ui.c` | Host tests for all of the above |
| `firmware/src/dev/becker.c` / `becker.h` | `$FF43` control write, `becker_set_ctl` |
| `firmware/src/bus/bus.h` | `BUS_IDX_BECKER_CTL` |
| `firmware/src/console/mode.c` | Route the Becker rings to `ui` during a session |
| `firmware/src/console/console.c` / `console.h` | `console_exec_capture`, wiring, `rom load manager`, explicit `rom off`, fallback |
| `firmware/src/ui/manager_rom.h` | Generated: the stub as a C array |
| `coco/stub/manager.asm`, `coco/stub/Makefile` | The stub and its build |
| `firmware/host/picoco_host.c` | `--ui` option for the emulator |
| `firmware/TEST_PLAN.md`, `coco/README.md`, `CLAUDE.md` | Bench section J, docs |

---

### Task 1: `ui` core — poll parser, screen model, diff, reply framing

**Files:**
- Create: `firmware/src/ui/ui.h`, `firmware/src/ui/ui.c`, `firmware/tests/test_ui.c`
- Modify: `firmware/CMakeLists.txt` (`CORE_SRC`, include dirs)

**Interfaces:**
- Consumes: `plat_fs_list` (`src/plat.h`), `dw_store` (`src/dw/dw_store.h`), `rom_check_file` (`src/dev/rom.h`).
- Produces (used by Tasks 2, 3, 5):

```c
typedef void (*ui_send_fn)(void *ctx, const uint8_t *buf, size_t n);
typedef int  (*ui_exec_fn)(const char *line, char *msg, size_t cap);   /* 0 ok, else msg = reason */
void ui_init(ui_send_fn send, void *ctx, ui_exec_fn exec, dw_store *store);
void ui_ctl(uint8_t code);                 /* a write to $FF43: 0xA5 begin, 0x5A end */
bool ui_active(void);
void ui_feed(const uint8_t *buf, size_t n, uint32_t now_ms);
void ui_row_text(int row, char out[UI_COLS + 1]);   /* tests: one model row as ASCII */
```

- [ ] **Step 1: Create the header**

`firmware/src/ui/ui.h`:

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dw_store.h"

/* On-board manager UI (spec 2026-10-01-rom-manager-design.md). core0 only.
 * The CoCo stub polls with the last key; ui answers with an action list. */
#define UI_COLS 32
#define UI_ROWS 16
#define UI_CELLS (UI_COLS * UI_ROWS)
#define UI_REPLY_MAX 600            /* action bytes in one reply; the stub's buffer */

#define UI_CAP_32K   0x01
#define UI_CAP_64K   0x02
#define UI_CAP_ECB   0x04
#define UI_CAP_DOS   0x08
#define UI_CAP_COCO3 0x10

#define UI_ACT_END   0x00
#define UI_ACT_TEXT  0x01           /* offset hi, lo, length, screen codes */
#define UI_ACT_CLEAR 0x02
#define UI_ACT_JUMP  0x10           /* after a ROM swap: JMP $C000 */
#define UI_ACT_COLD  0x11           /* after a ROM swap: cold restart */
#define UI_ACT_WARM  0x13           /* warm restart: back to BASIC */

#define UI_KEY_BREAK 0x03
#define UI_KEY_DOWN  0x0A
#define UI_KEY_ENTER 0x0D
#define UI_KEY_UP    0x5E

#define UI_GO_OK   0x06             /* reply to the stub's 'G' */
#define UI_GO_FAIL 0x15

typedef void (*ui_send_fn)(void *ctx, const uint8_t *buf, size_t n);
typedef int  (*ui_exec_fn)(const char *line, char *msg, size_t cap);   /* 0 ok, else msg = reason */

void ui_init(ui_send_fn send, void *ctx, ui_exec_fn exec, dw_store *store);
void ui_ctl(uint8_t code);          /* a write to $FF43: 0xA5 begin, 0x5A end */
bool ui_active(void);
void ui_feed(const uint8_t *buf, size_t n, uint32_t now_ms);   /* bytes from the CoCo during a session */
void ui_row_text(int row, char out[UI_COLS + 1]);              /* tests: one model row as ASCII */
```

- [ ] **Step 2: Write the failing tests**

`firmware/tests/test_ui.c`:

```c
#include "test.h"
#include "ui.h"
#include "plat.h"
#include "dw_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_dir[300];
static dw_store store;
static uint8_t tx[4096];
static size_t txn;
static char last_line[80];
static int exec_rc;

static void send_cb(void *ctx, const uint8_t *buf, size_t n) {
    (void)ctx;
    memcpy(tx + txn, buf, n);
    txn += n;
}
static int exec_cb(const char *line, char *msg, size_t cap) {
    snprintf(last_line, sizeof last_line, "%s", line);
    if (exec_rc) snprintf(msg, cap, "rom load failed");
    return exec_rc;
}

static void mkfile(const char *name, const char *head, size_t size) {
    char p[600];
    snprintf(p, sizeof p, "%s/%s", g_dir, name);
    FILE *f = fopen(p, "wb");
    uint8_t z[8192];
    memset(z, 0, sizeof z);
    memcpy(z, head, strlen(head));
    for (size_t left = size; left; ) {
        size_t n = left < sizeof z ? left : sizeof z;
        fwrite(z, 1, n, f);
        left -= n;
        memset(z, 0, sizeof z);
    }
    fclose(f);
}

static void wipe(void) {
    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -f '%s'/*", g_dir);
    (void)system(cmd);
}

static void setup(void) {
    wipe();
    txn = 0;
    exec_rc = 0;
    last_line[0] = '\0';
    dw_store_posix_init(&store, g_dir);
    ui_init(send_cb, NULL, exec_cb, &store);
}

/* Send one poll the way the stub does. caps < 0: not a first poll. */
static void poll(uint8_t flags, uint8_t key, int caps, uint32_t now) {
    uint8_t p[5];
    size_t n = 0;
    p[n++] = 'P'; p[n++] = flags; p[n++] = key;
    if (caps >= 0) p[n++] = (uint8_t)caps;
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++) s += p[i];
    p[n++] = s;
    txn = 0;
    ui_feed(p, n, now);
}

/* Check the framing of the reply in tx; returns the action-byte count or -1. */
static int reply_ok(void) {
    if (txn < 4) return -1;
    size_t len = ((size_t)tx[0] << 8) | tx[1];
    if (txn != len + 4) return -1;
    unsigned sum = 0;
    for (size_t i = 0; i < len; i++) sum += tx[2 + i];
    if ((((unsigned)tx[2 + len] << 8) | tx[3 + len]) != (sum & 0xFFFF)) return -1;
    return (int)len;
}

TEST(inactive_until_ctl) {
    setup();
    ASSERT(!ui_active());
    ui_ctl(0xA5);
    ASSERT(ui_active());
    ui_ctl(0x5A);
    ASSERT(!ui_active());
}

TEST(first_poll_clears_and_draws) {
    setup();
    ui_ctl(0xA5);
    poll(2, 0, 0, 0);                       /* 16K, no ECB */
    int len = reply_ok();
    ASSERT(len > 0);
    ASSERT_EQ(tx[2], UI_ACT_CLEAR);
    ASSERT_EQ(tx[2 + len - 1], UI_ACT_END);
    char row[UI_COLS + 1];
    ui_row_text(0, row);
    ASSERT(strstr(row, "PICOCO") != NULL);
    ASSERT(strstr(row, "16K") != NULL);
    ASSERT(strstr(row, "NO ECB") != NULL);
}

TEST(caps_shown) {
    setup();
    ui_ctl(0xA5);
    poll(2, 0, UI_CAP_32K | UI_CAP_ECB, 0);
    char row[UI_COLS + 1];
    ui_row_text(0, row);
    ASSERT(strstr(row, "32K") != NULL);
    ASSERT(strstr(row, "NO ECB") == NULL);
    ASSERT(strstr(row, "ECB") != NULL);
}

TEST(second_poll_sends_only_changes) {
    setup();
    ui_ctl(0xA5);
    poll(2, 0, 0, 0);
    poll(0, 0, -1, 10);                     /* no key: nothing changed */
    ASSERT_EQ(reply_ok(), 1);               /* just UI_ACT_END */
    ASSERT_EQ(tx[2], UI_ACT_END);
}

TEST(resend_repeats_last_reply) {
    setup();
    ui_ctl(0xA5);
    poll(2, 0, 0, 0);
    uint8_t first[sizeof tx];
    size_t n = txn;
    memcpy(first, tx, n);
    poll(3, 0, 0, 10);                      /* first + resend */
    ASSERT_EQ(txn, n);
    ASSERT_MEMEQ(tx, first, n);
}

/* Review Focus 4: bad checksum, half a poll, noise. */
TEST(bad_poll_gets_no_reply) {
    setup();
    ui_ctl(0xA5);
    uint8_t bad[5] = { 'P', 2, 0, 0, 0x99 };
    txn = 0;
    ui_feed(bad, sizeof bad, 0);
    ASSERT_EQ(txn, 0);
    uint8_t noise[3] = { 0x00, 0xFF, 0x41 };
    ui_feed(noise, sizeof noise, 1);
    ASSERT_EQ(txn, 0);
}

TEST(half_poll_recovers_after_quiet) {
    setup();
    ui_ctl(0xA5);
    uint8_t half[2] = { 'P', 2 };
    txn = 0;
    ui_feed(half, sizeof half, 0);
    ASSERT_EQ(txn, 0);
    poll(2, 0, 0, 300);                     /* 300 ms later: parser has reset */
    ASSERT(reply_ok() > 0);
}

TEST(reply_never_exceeds_cap) {
    setup();
    ui_ctl(0xA5);
    poll(2, 0, 0, 0);
    ASSERT(reply_ok() <= UI_REPLY_MAX);
}

int main(void) {
    char tmpl[300];
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = "/tmp";
    snprintf(tmpl, sizeof tmpl, "%s/uiXXXXXX", tmpdir);
    snprintf(g_dir, sizeof g_dir, "%s", mkdtemp(tmpl));
    plat_host_set_dir(g_dir);

    RUN(inactive_until_ctl);
    RUN(first_poll_clears_and_draws);
    RUN(caps_shown);
    RUN(second_poll_sends_only_changes);
    RUN(resend_repeats_last_reply);
    RUN(bad_poll_gets_no_reply);
    RUN(half_poll_recovers_after_quiet);
    RUN(reply_never_exceeds_cap);
    TEST_MAIN_END
}
```

(`mkfile` and `exec_cb` are used from Task 2 on; the compiler may warn that `mkfile` is unused until then. That is expected.)

- [ ] **Step 3: Add the module to the build and run the tests to see them fail**

In `firmware/CMakeLists.txt`, add `src/ui/ui.c` to `CORE_SRC`:

```cmake
set(CORE_SRC
  src/dw/dw_util.c src/dw/dw_disk.c src/dw/dw_server.c src/dw/dw_vser.c
  src/bus/bus.c src/dev/device.c src/dev/rom.c src/dev/becker.c
  src/log.c src/console/console.c src/console/mode.c src/ui/ui.c)
```

and add `src/ui` to the host include path:

```cmake
target_include_directories(picoco_core PUBLIC src src/dw src/bus src/dev src/console src/net src/ui host)
```

Find the Pico target's `target_include_directories(picoco ...)` in the same file and add `src/ui` there too.

Create an empty `firmware/src/ui/ui.c` containing only `#include "ui.h"`.

Run: `cmake -B build-host -G Ninja -DPICOCO_HOST=ON firmware && ninja -C build-host test_ui`
Expected: link errors, `undefined symbol: ui_init` (and the other five functions).

- [ ] **Step 4: Implement `ui.c`**

`firmware/src/ui/ui.c`:

```c
#include "ui.h"
#include "plat.h"
#include "rom.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_ROMS  64
#define NAME_LEN  32            /* longest name kept, with its NUL */
#define LIST_ROWS 12
#define POLL_QUIET_MS 250       /* a poll cut short is dropped after this; the stub retries at ~1.3 s */

static ui_send_fn g_send;
static void *g_ctx;
static ui_exec_fn g_exec;
static dw_store *g_store;
static bool active;

static uint8_t scr[UI_CELLS];   /* what the screen should show, VDG screen codes */
static uint8_t sent[UI_CELLS];  /* what the last reply leaves on the CoCo */
static uint8_t ack[UI_CELLS];   /* what the CoCo is known to show */
static uint8_t reply[UI_REPLY_MAX + 4];
static size_t reply_len;

static uint8_t caps;
static char roms[MAX_ROMS][NAME_LEN];
static int nroms, sel, top;
static bool truncated;
static char msg[UI_COLS + 1];
static char pending[NAME_LEN];
static uint8_t pending_act;

static uint8_t pk[5];
static int pkn;
static uint32_t pk_ms;

/* ASCII to VDG screen code. Normal video is $40-$7F, inverse $00-$3F.
 * ponytail: lowercase folds to uppercase; real lowercase entry is the next
 * plan's line editor. */
static uint8_t code(char c, bool inv) {
    uint8_t u = (uint8_t)c;
    if (u >= 0x60 && u < 0x80) u -= 0x20;
    if (u < 0x20 || u >= 0x80) u = ' ';
    uint8_t s = u < 0x40 ? (uint8_t)(u + 0x40) : u;
    return inv ? (uint8_t)(s & 0x3F) : s;
}

static void put(int row, int col, const char *s, bool inv) {
    for (int i = row * UI_COLS + col; *s && i < (row + 1) * UI_COLS; i++) scr[i] = code(*s++, inv);
}

void ui_row_text(int row, char out[UI_COLS + 1]) {
    for (int c = 0; c < UI_COLS; c++) {
        uint8_t s = scr[row * UI_COLS + c];
        if (s < 0x20) s += 0x40;            /* inverse letters */
        else if (s >= 0x60) s -= 0x40;      /* normal punctuation and digits */
        out[c] = (char)s;                   /* $20-$3F inverse digits, $40-$5F normal letters: as is */
    }
    out[UI_COLS] = '\0';
}

static void rom_cb(const char *name, uint32_t size, void *ctx) {
    (void)size; (void)ctx;
    size_t l = strlen(name);
    /* `rom load` takes one token, so a name with a space cannot be sent. */
    if (l < 5 || l >= NAME_LEN || strcasecmp(name + l - 4, ".rom") != 0 || strchr(name, ' ')) return;
    if (nroms >= MAX_ROMS) { truncated = true; return; }
    memcpy(roms[nroms++], name, l + 1);
}

static int name_cmp(const void *a, const void *b) { return strcasecmp(a, b); }

static void load_list(void) {
    nroms = 0;
    truncated = false;
    plat_fs_list(rom_cb, NULL);
    qsort(roms, (size_t)nroms, NAME_LEN, name_cmp);
    sel = top = 0;
}

static void draw(void) {
    memset(scr, 0x60, sizeof scr);
    char line[UI_COLS + 1];
    snprintf(line, sizeof line, "PICOCO  %s %s%s",
             caps & UI_CAP_64K ? "64K" : caps & UI_CAP_32K ? "32K" : "16K",
             caps & UI_CAP_ECB ? "ECB" : "NO ECB",
             caps & UI_CAP_COCO3 ? " COCO3" : "");
    put(0, 0, line, false);
    put(1, 0, nroms ? "ROMS" : "NO .ROM FILES", false);
    for (int i = 0; i < LIST_ROWS && top + i < nroms; i++) put(2 + i, 1, roms[top + i], top + i == sel);
    put(14, 0, msg[0] ? msg : truncated ? "LIST TRUNCATED" : "", false);
    put(15, 0, "ENTER=RUN  BREAK=BASIC", false);
}

/* Emit the cells that differ from ack as text runs, then act (if any) and
 * the end marker. Runs join across gaps under 4 cells (a run header is 4
 * bytes). Worst case is every cell changed: 3 runs, 524 bytes, inside
 * UI_REPLY_MAX; the bound check is a guard, and a cell it leaves out stays
 * different in `sent` so the next poll sends it. */
static void build_reply(bool full, uint8_t act) {
    uint8_t *p = reply + 2, *end = reply + 2 + UI_REPLY_MAX - 2;
    if (full) { *p++ = UI_ACT_CLEAR; memset(ack, 0x60, sizeof ack); }
    memcpy(sent, ack, sizeof sent);
    for (int i = 0; i < UI_CELLS; ) {
        if (scr[i] == ack[i]) { i++; continue; }
        int last = i;
        for (int j = i + 1; j < UI_CELLS && j - last < 4 && j - i < 255; j++)
            if (scr[j] != ack[j]) last = j;
        int n = last - i + 1;
        if (p + 4 + n > end) break;
        *p++ = UI_ACT_TEXT; *p++ = (uint8_t)(i >> 8); *p++ = (uint8_t)i; *p++ = (uint8_t)n;
        memcpy(p, scr + i, (size_t)n);
        memcpy(sent + i, scr + i, (size_t)n);
        p += n;
        i += n;
    }
    if (act) *p++ = act;
    *p++ = UI_ACT_END;
    size_t len = (size_t)(p - (reply + 2));
    unsigned sum = 0;
    for (size_t k = 0; k < len; k++) sum += reply[2 + k];
    reply[0] = (uint8_t)(len >> 8); reply[1] = (uint8_t)len;
    reply[2 + len] = (uint8_t)(sum >> 8); reply[3 + len] = (uint8_t)sum;
    reply_len = len + 4;
    g_send(g_ctx, reply, reply_len);
}

static uint8_t on_key(uint8_t key);   /* Task 2 */

static void on_poll(uint8_t flags, uint8_t key, uint8_t c) {
    if ((flags & 1) && reply_len) { g_send(g_ctx, reply, reply_len); return; }
    bool first = (flags & 2) != 0;
    uint8_t act = 0;
    if (first) { caps = c; load_list(); }
    else { memcpy(ack, sent, sizeof ack); act = on_key(key); }   /* a new poll acknowledges the last reply */
    draw();
    build_reply(first, act);
}

static void on_go(void);              /* Task 2 */

void ui_feed(const uint8_t *buf, size_t n, uint32_t now_ms) {
    if (pkn && now_ms - pk_ms > POLL_QUIET_MS) pkn = 0;
    pk_ms = now_ms;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = buf[i];
        if (pkn == 0) {
            if (b == 'G') { on_go(); continue; }
            if (b != 'P') continue;                 /* noise: wait for a poll */
        }
        pk[pkn++] = b;
        int want = (pkn >= 2 && (pk[1] & 2)) ? 5 : 4;   /* P flags key [caps] sum8 */
        if (pkn < 2 || pkn < want) continue;
        uint8_t sum = 0;
        for (int k = 0; k < want - 1; k++) sum += pk[k];
        pkn = 0;
        if (sum != pk[want - 1]) continue;          /* no reply: the stub times out and asks again */
        on_poll(pk[1], pk[2], want == 5 ? pk[3] : caps);
    }
}

void ui_ctl(uint8_t codev) {
    if (codev == 0xA5) { active = true; pkn = 0; reply_len = 0; pending_act = 0; }
    else if (codev == 0x5A) active = false;
}

bool ui_active(void) { return active; }

void ui_init(ui_send_fn send, void *ctx, ui_exec_fn exec, dw_store *store) {
    g_send = send; g_ctx = ctx; g_exec = exec; g_store = store;
    active = false;
    pkn = 0; reply_len = 0; pending_act = 0;
    nroms = sel = top = 0; truncated = false;
    msg[0] = '\0'; pending[0] = '\0'; caps = 0;
    memset(scr, 0x60, sizeof scr);
    memset(sent, 0x60, sizeof sent);
    memset(ack, 0x60, sizeof ack);
}

/* Task 2 replaces these two. */
static uint8_t on_key(uint8_t key) { (void)key; return 0; }
static void on_go(void) { }
```

- [ ] **Step 5: Run the tests**

Run: `ninja -C build-host test_ui && ./build-host/test_ui`
Expected: `8 tests, 0 failed`.

Run: `ctest --test-dir build-host --output-on-failure`
Expected: `100% tests passed` (13 suites: the 12 existing plus `test_ui`).

- [ ] **Step 6: Commit**

```bash
git add firmware/src/ui/ui.h firmware/src/ui/ui.c firmware/tests/test_ui.c firmware/CMakeLists.txt
git commit -m "firmware: ui module core (poll parser, screen model, diff)"
```

---

### Task 2: ROMs screen — selection, capability rule, launch

**Files:**
- Modify: `firmware/src/ui/ui.c` (replace the two Task 2 stubs at the end)
- Test: `firmware/tests/test_ui.c`

**Interfaces:**
- Consumes: Task 1's `ui.c` internals (`roms`, `sel`, `top`, `msg`, `pending`, `pending_act`, `g_exec`, `g_store`, `g_send`), `rom_check_file(dw_store *, const char *)` (0 ok, -1 not found, -2 bad size).
- Produces: behaviour only. The stub's `'G'` byte is answered with `UI_GO_OK` or `UI_GO_FAIL`; on OK the session ends (`ui_active()` false).

- [ ] **Step 1: Write the failing tests**

Add to `firmware/tests/test_ui.c`, above `main`:

```c
static void open_with(int capsv) {
    ui_ctl(0xA5);
    poll(2, 0, capsv, 0);
}

static bool row_has(int row, const char *s) {
    char t[UI_COLS + 1];
    ui_row_text(row, t);
    return strstr(t, s) != NULL;
}

/* Review Focus 1 */
TEST(no_roms_message_and_enter_is_harmless) {
    setup();
    open_with(0);
    ASSERT(row_has(1, "NO .ROM FILES"));
    poll(0, UI_KEY_ENTER, -1, 10);
    int len = reply_ok();
    ASSERT(len > 0);
    ASSERT_EQ(tx[2 + len - 1], UI_ACT_END);
    for (int i = 0; i < len; i++) ASSERT(tx[2 + i] != UI_ACT_JUMP || i != len - 2);
    ASSERT(ui_active());
}

TEST(lists_roms_sorted_and_hides_others) {
    setup();
    mkfile("zeta.rom", "\x7E\xC0\x10", 8192);
    mkfile("ALPHA.ROM", "\x7E\xC0\x10", 8192);
    mkfile("disk.dsk", "", 256);
    mkfile("picoco.cfg", "", 16);
    open_with(0);
    ASSERT(row_has(1, "ROMS"));
    ASSERT(row_has(2, "ALPHA.ROM"));
    ASSERT(row_has(3, "ZETA.ROM"));         /* lowercase folded */
    ASSERT(!row_has(4, "DISK"));
}

/* Review Focus 3 */
TEST(awkward_names_hidden) {
    setup();
    mkfile("my game.rom", "\x7E\xC0\x10", 8192);
    mkfile("abcdefghijklmnopqrstuvwxyz012.rom", "\x7E\xC0\x10", 8192);   /* 33 chars */
    mkfile("ok.rom", "\x7E\xC0\x10", 8192);
    open_with(0);
    ASSERT(row_has(2, "OK.ROM"));
    ASSERT(!row_has(3, "ROM"));
}

/* Review Focus 2 */
TEST(scrolls_with_the_selection) {
    setup();
    char name[16];
    for (int i = 0; i < 20; i++) {
        snprintf(name, sizeof name, "r%02d.rom", i);
        mkfile(name, "\x7E\xC0\x10", 8192);
    }
    open_with(0);
    ASSERT(row_has(2, "R00.ROM"));
    ASSERT(row_has(13, "R11.ROM"));
    for (int i = 0; i < 12; i++) poll(0, UI_KEY_DOWN, -1, 10 + (uint32_t)i);
    ASSERT(row_has(13, "R12.ROM"));          /* selection moved past the window: list scrolled */
    ASSERT(row_has(2, "R01.ROM"));
    for (int i = 0; i < 100; i++) poll(0, UI_KEY_DOWN, -1, 100 + (uint32_t)i);
    ASSERT(row_has(13, "R19.ROM"));          /* stops at the last entry */
    for (int i = 0; i < 100; i++) poll(0, UI_KEY_UP, -1, 300 + (uint32_t)i);
    ASSERT(row_has(2, "R00.ROM"));
}

/* Review Focus 2: which 64 of 70 are kept depends on directory order, so
 * only the notice is asserted. */
TEST(more_than_64_shows_truncated) {
    setup();
    char name[16];
    for (int i = 0; i < 70; i++) {
        snprintf(name, sizeof name, "r%02d.rom", i);
        mkfile(name, "\x7E\xC0\x10", 8192);
    }
    open_with(0);
    ASSERT(row_has(14, "LIST TRUNCATED"));
}

TEST(enter_on_pak_asks_for_jump_then_go_loads) {
    setup();
    mkfile("game.rom", "\x7E\xC0\x10", 8192);
    open_with(0);
    poll(0, UI_KEY_ENTER, -1, 10);
    int len = reply_ok();
    ASSERT(len >= 2);
    ASSERT_EQ(tx[2 + len - 2], UI_ACT_JUMP);
    ASSERT_EQ(tx[2 + len - 1], UI_ACT_END);
    txn = 0;
    uint8_t g = 'G';
    ui_feed(&g, 1, 20);
    ASSERT_EQ(txn, 1);
    ASSERT_EQ(tx[0], UI_GO_OK);
    ASSERT(strcmp(last_line, "rom load game.rom") == 0);
    ASSERT(!ui_active());
}

TEST(dos_rom_needs_ecb) {
    setup();
    mkfile("hdb.rom", "DK", 8192);
    open_with(0);                            /* no ECB */
    poll(0, UI_KEY_ENTER, -1, 10);
    ASSERT(row_has(14, "NEEDS EXTENDED BASIC"));
    int len = reply_ok();
    ASSERT_EQ(tx[2 + len - 1], UI_ACT_END);
    ASSERT(len < 2 || tx[2 + len - 2] != UI_ACT_COLD);
}

TEST(dos_rom_with_ecb_cold_restarts) {
    setup();
    mkfile("hdb.rom", "DK", 8192);
    open_with(UI_CAP_32K | UI_CAP_ECB);
    poll(0, UI_KEY_ENTER, -1, 10);
    int len = reply_ok();
    ASSERT_EQ(tx[2 + len - 2], UI_ACT_COLD);
}

TEST(dos_rom_on_coco3_waits_for_next_plan) {
    setup();
    mkfile("hdb.rom", "DK", 8192);
    open_with(UI_CAP_64K | UI_CAP_32K | UI_CAP_ECB | UI_CAP_COCO3);
    poll(0, UI_KEY_ENTER, -1, 10);
    ASSERT(row_has(14, "DOS ROM ON COCO 3: NOT YET"));
}

TEST(bad_size_refused_before_leaving) {
    setup();
    mkfile("odd.rom", "\x7E\xC0\x10", 5000);
    open_with(0);
    poll(0, UI_KEY_ENTER, -1, 10);
    ASSERT(row_has(14, "NOT A ROM SIZE"));
    ASSERT(ui_active());
}

/* Review Focus 5 */
TEST(go_failure_keeps_session_and_shows_reason) {
    setup();
    mkfile("game.rom", "\x7E\xC0\x10", 8192);
    open_with(0);
    poll(0, UI_KEY_ENTER, -1, 10);
    exec_rc = -1;
    txn = 0;
    uint8_t g = 'G';
    ui_feed(&g, 1, 20);
    ASSERT_EQ(tx[0], UI_GO_FAIL);
    ASSERT(ui_active());
    ui_ctl(0xA5);                            /* the stub starts over */
    poll(2, 0, 0, 30);
    ASSERT(row_has(14, "ROM LOAD FAILED"));
}

TEST(go_without_a_pending_launch_fails) {
    setup();
    open_with(0);
    txn = 0;
    uint8_t g = 'G';
    ui_feed(&g, 1, 20);
    ASSERT_EQ(tx[0], UI_GO_FAIL);
}

TEST(break_asks_for_warm_restart) {
    setup();
    open_with(0);
    poll(0, UI_KEY_BREAK, -1, 10);
    int len = reply_ok();
    ASSERT_EQ(tx[2 + len - 2], UI_ACT_WARM);
    txn = 0;
    uint8_t g = 'G';
    ui_feed(&g, 1, 20);
    ASSERT_EQ(tx[0], UI_GO_OK);
    ASSERT(last_line[0] == '\0');            /* nothing to load */
    ASSERT(!ui_active());
}
```

Add to `main`, after the Task 1 `RUN` lines:

```c
    RUN(no_roms_message_and_enter_is_harmless);
    RUN(lists_roms_sorted_and_hides_others);
    RUN(awkward_names_hidden);
    RUN(scrolls_with_the_selection);
    RUN(more_than_64_shows_truncated);
    RUN(enter_on_pak_asks_for_jump_then_go_loads);
    RUN(dos_rom_needs_ecb);
    RUN(dos_rom_with_ecb_cold_restarts);
    RUN(dos_rom_on_coco3_waits_for_next_plan);
    RUN(bad_size_refused_before_leaving);
    RUN(go_failure_keeps_session_and_shows_reason);
    RUN(go_without_a_pending_launch_fails);
    RUN(break_asks_for_warm_restart);
```

- [ ] **Step 2: Run to see them fail**

Run: `ninja -C build-host test_ui && ./build-host/test_ui`
Expected: the Task 1 tests pass; `lists_roms_sorted_and_hides_others` passes (the list is Task 1 code); the launch, capability, scroll and BREAK tests FAIL.

- [ ] **Step 3: Implement**

In `firmware/src/ui/ui.c`, delete the last two lines (`on_key` and `on_go` stubs) and add:

```c
static void set_msg(const char *s) { snprintf(msg, sizeof msg, "%s", s); }

/* First two bytes of a file: "DK" marks a DOS ROM that Extended BASIC must start. */
static int peek2(const char *name, uint8_t two[2]) {
    dw_file f;
    if (g_store->ops->open(g_store->ctx, name, false, &f) < 0) return -1;
    int n = g_store->ops->read(&f, 0, two, 2);
    g_store->ops->close(&f);
    return n == 2 ? 0 : -1;
}

static uint8_t launch(void) {
    if (!nroms) return 0;
    const char *name = roms[sel];
    int rc = rom_check_file(g_store, name);
    uint8_t two[2];
    if (rc == -2) { set_msg("NOT A ROM SIZE"); return 0; }
    if (rc != 0 || peek2(name, two) != 0) { set_msg("CANNOT READ FILE"); return 0; }
    bool dos = two[0] == 'D' && two[1] == 'K';
    if (dos && !(caps & UI_CAP_ECB)) { set_msg("NEEDS EXTENDED BASIC"); return 0; }
    /* ponytail: a CoCo 3 cold restart has to restore ROM mode first; the next plan adds it. */
    if (dos && (caps & UI_CAP_COCO3)) { set_msg("DOS ROM ON COCO 3: NOT YET"); return 0; }
    snprintf(pending, sizeof pending, "%s", name);
    pending_act = dos ? UI_ACT_COLD : UI_ACT_JUMP;
    return pending_act;
}

static uint8_t on_key(uint8_t key) {
    uint8_t act = 0;
    if (key) msg[0] = '\0';
    if (key == UI_KEY_UP && sel > 0) sel--;
    else if (key == UI_KEY_DOWN && sel + 1 < nroms) sel++;
    else if (key == UI_KEY_ENTER) act = launch();
    else if (key == UI_KEY_BREAK) { pending[0] = '\0'; act = pending_act = UI_ACT_WARM; }
    if (sel < top) top = sel;
    if (sel >= top + LIST_ROWS) top = sel - LIST_ROWS + 1;
    return act;
}

/* The stub has left the cart and is running from RAM: do the swap now. */
static void on_go(void) {
    uint8_t r = UI_GO_OK;
    if (!pending_act) r = UI_GO_FAIL;
    else if (pending[0]) {
        char line[16 + NAME_LEN], err[UI_COLS + 1];
        snprintf(line, sizeof line, "rom load %s", pending);
        if (g_exec(line, err, sizeof err) != 0) { r = UI_GO_FAIL; set_msg(err); }
    }
    pending_act = 0;
    if (r == UI_GO_OK) { msg[0] = '\0'; active = false; }
    g_send(g_ctx, &r, 1);
}
```

- [ ] **Step 4: Run the tests**

Run: `ninja -C build-host test_ui && ./build-host/test_ui`
Expected: `21 tests, 0 failed`.

Run: `ctest --test-dir build-host --output-on-failure`
Expected: `100% tests passed`.

- [ ] **Step 5: Commit**

```bash
git add firmware/src/ui/ui.c firmware/tests/test_ui.c
git commit -m "firmware: ui ROMs screen, capability rules, launch handshake"
```

---

### Task 3: UI session on the Becker port

**Files:**
- Modify: `firmware/src/bus/bus.h` (one define), `firmware/src/dev/becker.c`, `firmware/src/dev/becker.h`, `firmware/src/console/mode.c`, `firmware/src/console/console.c`, `firmware/src/console/console.h`
- Test: `firmware/tests/test_ui_session.c` (new)

**Interfaces:**
- Consumes: `ui_init`, `ui_ctl`, `ui_active`, `ui_feed` (Task 1); `mode_dw_send`, `becker_read`, `becker_write`, `becker_tx_free`.
- Produces:

```c
#define BUS_IDX_BECKER_CTL 0x3F43                         /* bus.h */
void becker_set_ctl(void (*fn)(uint8_t code));            /* becker.h: called on core0 for each $FF43 write */
int  console_exec_capture(const char *line, char *msg, size_t cap);   /* console.h: 0 ok; else msg = the "err" text */
```

`console_init` wires the session: after it runs, a `$FF43` write of `0xA5` starts a session and `mode_pump` serves it in every Becker mode.

- [ ] **Step 1: Write the failing test**

`firmware/tests/test_ui_session.c`:

```c
#include "test.h"
#include "ui.h"
#include "mode.h"
#include "becker.h"
#include "bus.h"
#include "device.h"
#include "rom.h"
#include "console.h"
#include "dw.h"
#include "dw_store.h"
#include "log.h"
#include "plat.h"
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static dw_server dw;
static dw_store store;
static char g_dir[300];
static void out_cb(void *ctx, const char *s) { (void)ctx; (void)s; }

static void setup(picoco_mode m) {
    bus_init(); device_reset(); rom_init(); becker_init(); device_init_all();
    log_init();
    mode_reset();
    dw_store_posix_init(&store, g_dir);
    dw_init(&dw, &store, mode_dw_send, NULL);
    console_init(out_cb, NULL, &dw, &store);
    net_forget();
    mode_set(m);
}

static void coco_writes(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) bus_on_write(BUS_IDX_BECKER_DATA, p[i], 0);
}

/* What the firmware queued for the CoCo, read the way the CoCo would. */
static size_t coco_reads(uint8_t *out, size_t cap) {
    size_t n = 0;
    for (int guard = 0; guard < 4000 && n < cap; guard++) {
        mode_pump(&dw, 0);
        bus_on_read_done(BUS_IDX_BECKER_STATUS, 0);
        if (bus_table[BUS_IDX_BECKER_STATUS] != 0x02) { if (guard > 50 && n) break; continue; }
        out[n++] = bus_table[BUS_IDX_BECKER_DATA];
        bus_on_read_done(BUS_IDX_BECKER_DATA, 0);
    }
    return n;
}

static const uint8_t first_poll[5] = { 'P', 2, 0, 0, (uint8_t)('P' + 2) };

TEST(ctl_write_starts_and_ends_a_session) {
    setup(MODE_NATIVE);
    ASSERT(!ui_active());
    bus_on_write(BUS_IDX_BECKER_CTL, 0xA5, 0);
    mode_pump(&dw, 0);
    ASSERT(ui_active());
    bus_on_write(BUS_IDX_BECKER_CTL, 0x5A, 0);
    mode_pump(&dw, 0);
    ASSERT(!ui_active());
}

TEST(poll_is_answered_in_native_mode) {
    setup(MODE_NATIVE);
    bus_on_write(BUS_IDX_BECKER_CTL, 0xA5, 0);
    coco_writes(first_poll, sizeof first_poll);
    uint8_t got[700];
    size_t n = coco_reads(got, sizeof got);
    ASSERT(n > 4);
    size_t len = ((size_t)got[0] << 8) | got[1];
    ASSERT_EQ(n, len + 4);
    ASSERT_EQ(got[2], UI_ACT_CLEAR);
}

/* In bridge mode Becker bytes normally go to CDC0; a session keeps them. */
TEST(poll_is_answered_in_bridge_mode) {
    setup(MODE_BRIDGE);
    bus_on_write(BUS_IDX_BECKER_CTL, 0xA5, 0);
    coco_writes(first_poll, sizeof first_poll);
    uint8_t got[700];
    size_t n = coco_reads(got, sizeof got);
    ASSERT(n > 4);
    ASSERT_EQ(got[2], UI_ACT_CLEAR);
}

/* Bytes written before the session began must not reach the UI parser. */
TEST(session_begin_drops_earlier_bytes) {
    setup(MODE_LOOP);
    const uint8_t stale[3] = { 'P', 'G', 'P' };
    coco_writes(stale, sizeof stale);
    bus_on_write(BUS_IDX_BECKER_CTL, 0xA5, 0);
    coco_writes(first_poll, sizeof first_poll);
    uint8_t got[700];
    size_t n = coco_reads(got, sizeof got);
    size_t len = ((size_t)got[0] << 8) | got[1];
    ASSERT_EQ(n, len + 4);                   /* exactly one reply, nothing looped back */
}

TEST(drivewire_works_again_after_the_session) {
    setup(MODE_NATIVE);
    bus_on_write(BUS_IDX_BECKER_CTL, 0xA5, 0);
    mode_pump(&dw, 0);
    bus_on_write(BUS_IDX_BECKER_CTL, 0x5A, 0);
    const uint8_t dwinit[2] = { 0x5A, 0x00 };       /* OP_DWINIT, driver version */
    coco_writes(dwinit, sizeof dwinit);
    uint8_t got[8];
    ASSERT_EQ(coco_reads(got, sizeof got), 1);      /* one byte: the server's version */
}

TEST(exec_capture_returns_the_error_text) {
    setup(MODE_NATIVE);
    char msg[40];
    ASSERT_EQ(console_exec_capture("rom load nope.rom", msg, sizeof msg), -1);
    ASSERT(strcmp(msg, "rom load failed") == 0);
    ASSERT_EQ(console_exec_capture("rom pattern", msg, sizeof msg), 0);
}

int main(void) {
    char tmpl[300];
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = "/tmp";
    snprintf(tmpl, sizeof tmpl, "%s/uisessXXXXXX", tmpdir);
    snprintf(g_dir, sizeof g_dir, "%s", mkdtemp(tmpl));
    plat_host_set_dir(g_dir);

    RUN(ctl_write_starts_and_ends_a_session);
    RUN(poll_is_answered_in_native_mode);
    RUN(poll_is_answered_in_bridge_mode);
    RUN(session_begin_drops_earlier_bytes);
    RUN(drivewire_works_again_after_the_session);
    RUN(exec_capture_returns_the_error_text);
    TEST_MAIN_END
}
```

- [ ] **Step 2: Run to see it fail**

Run: `cmake -B build-host -G Ninja -DPICOCO_HOST=ON firmware && ninja -C build-host test_ui_session`
Expected: compile errors: `BUS_IDX_BECKER_CTL` undeclared, `console_exec_capture` undeclared.

- [ ] **Step 3: The control index and the Becker hook-up**

`firmware/src/bus/bus.h`, next to `BUS_IDX_BECKER_DATA`:

```c
#define BUS_IDX_BECKER_CTL    0x3F43   /* $FF43, write-only: UI session control (src/ui) */
```

`firmware/src/dev/becker.h`, add:

```c
/* fn runs on core0 (from device_dispatch_writes) for each write to $FF43,
 * in order with the $FF42 bytes around it. A 0xA5 write first drops every
 * byte the CoCo wrote before it. */
void   becker_set_ctl(void (*fn)(uint8_t code));
```

`firmware/src/dev/becker.c`, replace `becker_on_write` and add the setter above it:

```c
static void (*ctl_fn)(uint8_t code);

void becker_set_ctl(void (*fn)(uint8_t code)) { ctl_fn = fn; }

static void becker_on_write(uint16_t idx, uint8_t data) {
    if (idx == BUS_IDX_BECKER_CTL) {
        uint8_t b;
        if (data == 0xA5) while (ring_pop(&from_coco, &b)) { }   /* core0 is this ring's only consumer */
        if (ctl_fn) ctl_fn(data);
        return;
    }
    if (idx != BUS_IDX_BECKER_DATA) return;
    if (ring_push(&from_coco, data)) {
        becker_stats.writes++;
    } else {
        becker_stats.overrun++;
    }
}
```

- [ ] **Step 4: `console_exec_capture`**

`firmware/src/console/console.h`, add:

```c
/* Run any console command with its output captured, for the on-board UI
 * (no allowlist: the caller is firmware). 0 ok; else msg = the text after "err ". */
int  console_exec_capture(const char *line, char *msg, size_t cap);
```

In `firmware/src/console/console.c`, change the signature and the allowlist test of `console_exec_remote` so its body is shared. Rename the existing function to a static helper with one extra parameter:

```c
static int exec_captured(const char *line, char *out, size_t cap, size_t *outn, bool check_allow) {
    if (cap < 5) { *outn = 0; return 255; } /* too small even for "...\n" + NUL */
    char copy[136];
    snprintf(copy, sizeof(copy), "%s", line);
    char *argv[6];
    int argc = tokenize(copy, argv);
    if (argc == 0 || (check_allow && !remote_allowed(argc, argv))) {
        *outn = outn_clamp(snprintf(out, cap, "console only"), cap);
        return 255;
    }
    /* ...the rest of the old console_exec_remote body, unchanged, from
     * "console_out_fn saved = g_out;" to its final return... */
}
```

Keep every line of the old body after the allowlist test exactly as it is. Then add the two public functions below it:

```c
int console_exec_remote(void *ctx, const char *line, char *out, size_t cap, size_t *outn) {
    (void)ctx;
    return exec_captured(line, out, cap, outn, true);
}

int console_exec_capture(const char *line, char *msg, size_t cap) {
    size_t n;
    return exec_captured(line, msg, cap, &n, false) == 0 ? 0 : -1;
}
```

(On failure `exec_captured` already rewrites `out` to just the message after `err `.)

- [ ] **Step 5: Route the rings in `mode_pump` and wire it up**

`firmware/src/console/mode.c`: add `#include "ui.h"`, then factor the reply drain out of the `MODE_NATIVE` case into a helper and use it for the session.

Add above `mode_pump`:

```c
/* Hand queued reply bytes to the Becker port as it frees up. */
static void flush_pending(void) {
    if (pending_len == 0) return;
    size_t free_n = becker_tx_free();
    size_t take = free_n < pending_len ? free_n : pending_len;
    if (take) {
        size_t put = becker_write(pending, take);
        memmove(pending, pending + put, pending_len - put);
        pending_len -= put;
    }
}
```

At the top of `mode_pump`, replace the first line with:

```c
    device_dispatch_writes();
    /* UI session (spec 2026-10-01 §5.1): the manager stub owns the Becker
     * port in every mode until it writes 0x5A to $FF43. */
    static bool was_ui;
    bool ui = ui_active();
    if (ui != was_ui) {
        pending_len = 0;                         /* stale DriveWire or UI reply bytes */
        if (!ui && g_bound_dw) g_bound_dw->state = DW_IDLE;
        was_ui = ui;
    }
    if (ui) {
        if (pending_len == 0) {
            uint8_t buf[64];
            size_t n = becker_read(buf, sizeof(buf));
            if (n) ui_feed(buf, n, now_ms);
        }
        flush_pending();
        return;
    }
```

In the `MODE_NATIVE` case, replace the inline `if (pending_len > 0) { ... }` block with `flush_pending();`.

In `mode_reset`, nothing changes (it already zeroes `pending_len`).

`firmware/src/console/console.c`: add `#include "ui.h"` and, in `console_init` right after `mode_bind(dw);`:

```c
    ui_init(mode_dw_send, NULL, console_exec_capture, store);
    becker_set_ctl(ui_ctl);
```

- [ ] **Step 6: Run the tests**

Run: `cmake --build build-host && ./build-host/test_ui_session`
Expected: `6 tests, 0 failed`.

Run: `ctest --test-dir build-host --output-on-failure`
Expected: `100% tests passed` (14 suites).

- [ ] **Step 7: Build both Pico targets**

Run: `ninja -C build-pico && ninja -C build-pico-plusw`
Expected: both link; each prints `check_core1_flash_free: ok`.

- [ ] **Step 8: Commit**

```bash
git add firmware/src/bus/bus.h firmware/src/dev/becker.c firmware/src/dev/becker.h \
        firmware/src/console/mode.c firmware/src/console/console.c firmware/src/console/console.h \
        firmware/tests/test_ui_session.c
git commit -m "firmware: UI session on the Becker port via \$FF43"
```

---

### Task 4: The stub, the built-in image, and the fallback default

**Files:**
- Create: `coco/stub/manager.asm`, `coco/stub/Makefile`, `firmware/src/ui/manager_rom.h` (generated, checked in)
- Modify: `firmware/src/console/console.c` (`cmd_rom`, `console_run_config`, the `save` writer)
- Test: `firmware/tests/test_console.c`

**Interfaces:**
- Consumes: `rom_load_mem(const uint8_t *, size_t)`, `rom_loaded()`.
- Produces: `rom load manager` loads the built-in stub; `rom off` is remembered and saved as `rom off`; with no `rom` line in the config the firmware loads the manager. Header:

```c
/* firmware/src/ui/manager_rom.h, generated by `make -C coco/stub` */
static const unsigned char manager_rom[8192];
static const unsigned int manager_rom_len = 8192;
```

- [ ] **Step 1: Write the stub**

`coco/stub/manager.asm`:

```asm
* PiCoCo manager stub (spec 2026-10-01-rom-manager-design.md section 4).
* Thin client: the Pico's ui module decides what is on the screen. This ROM
* polls it with the last key, checks the reply's checksum, then carries out
* the action list: draw text, clear, or leave (jump / restart).
*
* Entry is $C002 on every path (the image starts with "DK"):
*   Extended BASIC cold start, or EXEC 49154 from Color BASIC.
* Rules: interrupts masked, no BASIC ROM call except POLCAT [$A000] (it is
* not hooked through RAM), never CLR a Becker register (CLR reads first).
* Y is the frame pointer for the whole session; nothing else may use Y.

BSTAT   equ $FF41
BDATA   equ $FF42
UICTL   equ $FF43               write $A5 begin session, $5A end
SCREEN  equ $0400
RSTSW   equ $71                 BASIC warm-start flag
BUFSZ   equ 600                 largest reply the firmware sends (UI_REPLY_MAX)
RETRIES equ 5

* frame at Y: the reply buffer, then the variables
FLAGS   equ BUFSZ               bit 0 resend, bit 1 first poll (caps follow)
KEY     equ BUFSZ+1
CAPS    equ BUFSZ+2
LEN     equ BUFSZ+3             2 bytes
ACC     equ BUFSZ+5             2 bytes, running sum of the reply
CNT     equ BUFSZ+7             2 bytes
TRIES   equ BUFSZ+9
FRAME   equ BUFSZ+10

        org $C000
        fcc "DK"
START   orcc #$50
        leas -FRAME,s
        tfr s,y
        lda #$A5
        sta UICTL
        jsr DRAIN
        jsr DETECT
        sta CAPS,y
        lda #2
        sta FLAGS,y
        clr KEY,y
MAIN    jsr POLL
KEYW    jsr [$A000]
        beq KEYW
        sta KEY,y
        clr FLAGS,y
        bra MAIN

* A = capability bits: 0 32K, 2 Extended BASIC, 4 CoCo 3
DETECT  clrb
        ldx $8000
        cmpx #$4558
        bne DET1
        orb #$04
DET1    lda $FFFE
        cmpa #$8C
        bne DET2
        orb #$10
DET2    lda $7FFF
        coma
        sta $7FFF
        cmpa $7FFF
        bne DET3
        orb #$01
DET3    coma
        sta $7FFF
        tfr b,a
        rts

* discard whatever the Pico had queued
DRAIN   lda BSTAT
        bita #2
        beq DRAINX
        lda BDATA
        bra DRAIN
DRAINX  rts

* one byte in A, carry set on timeout (~1.3 s at 0.89 MHz)
GETB    pshs x
        ldx #0
GETB1   lda BSTAT
        bita #2
        bne GETB2
        leax -1,x
        bne GETB1
        orcc #1
        puls x,pc
GETB2   lda BDATA
        andcc #$FE
        puls x,pc

* send A to the Pico and add it to the poll checksum in B
PUTB    sta BDATA
        pshs a
        addb ,s+
        rts

* send a poll, receive and check the reply, run it. A bad reply is asked for
* again with the resend flag; after RETRIES show the banner and wait for a
* key, then start a fresh session.
POLL    lda #RETRIES
        sta TRIES,y
POLL1   clrb
        lda #'P
        jsr PUTB
        lda FLAGS,y
        jsr PUTB
        lda KEY,y
        jsr PUTB
        lda FLAGS,y
        bita #2
        beq POLL2
        lda CAPS,y
        jsr PUTB
POLL2   stb BDATA
        jsr GETB
        bcs POLLBAD
        sta LEN,y
        jsr GETB
        bcs POLLBAD
        sta LEN+1,y
        ldd LEN,y
        cmpd #BUFSZ
        bhi POLLBAD
        std CNT,y
        ldx #0
        stx ACC,y
        tfr y,x
POLL3   ldd CNT,y
        beq POLL4
        subd #1
        std CNT,y
        jsr GETB
        bcs POLLBAD
        sta ,x+
        tfr a,b
        clra
        addd ACC,y
        std ACC,y
        bra POLL3
POLL4   jsr GETB
        bcs POLLBAD
        tfr a,b
        jsr GETB
        bcs POLLBAD
        exg a,b
        cmpd ACC,y
        beq RUNLIST
POLLBAD jsr DRAIN
        lda FLAGS,y
        ora #1
        sta FLAGS,y
        dec TRIES,y
        lbne POLL1
        leax NORESP,pcr
        ldu #SCREEN
POLLB1  lda ,x+
        beq POLLB2
        sta ,u+
        bra POLLB1
POLLB2  jsr [$A000]
        beq POLLB2
        lda #$A5
        sta UICTL
        lda #2
        sta FLAGS,y
        clr KEY,y
        lbra POLL

* run the action list in the buffer: X walks it, CNT holds the end address
RUNLIST tfr y,x
        ldd LEN,y
        leau d,x
        stu CNT,y
RUN1    cmpx CNT,y
        bhs RUNX
        lda ,x+
        beq RUNX
        cmpa #$01
        beq ATEXT
        cmpa #$02
        beq ACLR
        cmpa #$10
        bhs ALEAVE
RUNX    rts

* 01 offset(2) length(1) bytes: screen codes straight to video RAM
ATEXT   ldd ,x++
        cmpd #$0200
        bhs RUNX
        addd #SCREEN
        tfr d,u
        ldb ,x+
        beq RUN1
ATEXT1  cmpu #SCREEN+$0200
        bhs RUNX
        lda ,x+
        sta ,u+
        decb
        bne ATEXT1
        bra RUN1

* 02: clear to spaces (screen code $60)
ACLR    ldu #SCREEN
        lda #$60
ACLR1   sta ,u+
        cmpu #SCREEN+$0200
        blo ACLR1
        bra RUN1

* leave: copy LEAVER into the reply buffer and run it there, so nothing
* executes from the cart while the Pico swaps the ROM
ALEAVE  pshs a
        leax LEAVER,pcr
        tfr y,u
        ldb #LEAVEND-LEAVER
ALV1    lda ,x+
        sta ,u+
        decb
        bne ALV1
        puls a
        jmp ,y

* position-independent; A = action code. Sends 'G', waits for $06.
LEAVER  tfr a,b
        lda #'G
        sta BDATA
        ldx #0
LV1     lda BSTAT
        bita #2
        bne LV2
        leax -1,x
        bne LV1
        bra LVFAIL
LV2     lda BDATA
        cmpa #$06
        bne LVFAIL
        lda #$5A
        sta UICTL
        leas FRAME,y
        cmpb #$10
        beq LVJMP
        cmpb #$13
        beq LVWARM
        clr RSTSW
LVWARM  jmp [$FFFE]
LVJMP   andcc #$AF
        jmp $C000
LVFAIL  leas FRAME,y
        jmp START
LEAVEND

* "PICOCO NOT RESPONDING" in screen codes
NORESP  fcb $50,$49,$43,$4F,$43,$4F,$60,$4E,$4F,$54,$60
        fcb $52,$45,$53,$50,$4F,$4E,$44,$49,$4E,$47,0

        rmb $E000-*
        end START
```

Notes for the implementer: `CLR KEY,y` and `CLR RSTSW` are RAM, not a Becker register, so `CLR` is fine there. Every subroutine call is `JSR` (not `BSR`) on purpose: the 16-bit frame offsets make the code too long for 8-bit branches, and lwasm reports that as "Byte overflow".

- [ ] **Step 2: The stub's build**

`coco/stub/Makefile`:

```make
# PiCoCo manager stub: 8 KB cart image, plus the C header the firmware links.
HDR = ../../firmware/src/ui/manager_rom.h

all: manager.rom $(HDR)

manager.rom: manager.asm
	lwasm --raw -o $@ --list=manager.lst $<

$(HDR): manager.rom
	xxd -i -n manager_rom $< | sed 's/^unsigned/static const unsigned/' > $@

clean:
	rm -f manager.rom manager.lst

.PHONY: all clean
```

Run: `make -C coco/stub`
Expected: no errors; `ls -l coco/stub/manager.rom` shows 8192 bytes; `head -c 2 coco/stub/manager.rom` prints `DK`; `firmware/src/ui/manager_rom.h` exists and its last line is `static const unsigned int manager_rom_len = 8192;`.

- [ ] **Step 3: Write the failing console tests**

Add to `firmware/tests/test_console.c`, next to `rom_commands`:

```c
TEST(rom_load_manager_is_built_in) {
    setup();
    ASSERT_EQ(console_exec("rom load manager"), 0);
    ASSERT_EQ(bus_table[0], 'D');
    ASSERT_EQ(bus_table[1], 'K');
    console_exec("status");
    ASSERT(strstr(out, "rom now load manager"));
}

TEST(no_rom_in_config_falls_back_to_manager) {
    setup();
    plat_cfg_write("bus drive on\n", 13);
    ASSERT(console_run_config() >= 0);
    ASSERT_EQ(bus_table[0], 'D');
    ASSERT_EQ(bus_table[1], 'K');
    outn = 0;
    console_exec("status");
    ASSERT(strstr(out, "rom now load manager"));
    ASSERT(strstr(out, "rom next none"));     /* the fallback is not a saved choice */
}

TEST(explicit_rom_off_is_saved_and_kept) {
    setup();
    console_exec("rom off");
    console_exec("save");
    bus_set_read(0, 0x55);
    ASSERT(console_run_config() >= 0);
    ASSERT_EQ(bus_table[0], 0xFF);            /* off stays off: no fallback */
    outn = 0;
    console_exec("status");
    ASSERT(strstr(out, "rom next off"));
}
```

and the `RUN` lines after `RUN(rom_commands);`:

```c
    RUN(rom_load_manager_is_built_in);
    RUN(no_rom_in_config_falls_back_to_manager);
    RUN(explicit_rom_off_is_saved_and_kept);
```

Run: `cmake --build build-host && ./build-host/test_console`
Expected: the three new tests FAIL (`rom load manager` returns -1).

- [ ] **Step 4: Implement in `console.c`**

Add near the other includes:

```c
#include "manager_rom.h"
```

In `cmd_rom`, the `off` branch becomes (it is now a remembered choice):

```c
    if (strcasecmp(argv[1], "off") == 0) {
        rom_off();
        snprintf(rom_cmd, sizeof(rom_cmd), "off");
        rom_now[0] = '\0';
        return 0;
    }
```

In the `load` branch, replace the `int r = rom_load_file(g_store, argv[2]);` line with:

```c
        /* "manager" is the built-in stub (src/ui/manager_rom.h), not a file. */
        int r = strcasecmp(argv[2], "manager") == 0
              ? rom_load_mem(manager_rom, manager_rom_len)
              : rom_load_file(g_store, argv[2]);
```

In the `boot` branch, replace `int r = rom_check_file(g_store, argv[2]);` with:

```c
        int r = strcasecmp(argv[2], "manager") == 0 ? 0 : rom_check_file(g_store, argv[2]);
```

The `save` writer already emits `rom %s` when `rom_cmd` is non-empty, so `rom off` is saved with no further change. The `status` line `rom next %s` prints `off` the same way.

At the end of `console_run_config`, just before it returns the line count on success (and also on the "no config file" path, where it returns `<0`), add the fallback. The simplest place that covers both is a small helper called from both return points:

```c
/* Spec 2026-10-01 §6.5: a board with no ROM choice saved boots the manager.
 * `rom off` is a choice; an absent line is not. */
static void rom_fallback(void) {
    if (rom_cmd[0] || rom_loaded()) return;
    if (rom_load_mem(manager_rom, manager_rom_len) == 0) snprintf(rom_now, sizeof(rom_now), "load manager");
}
```

Call `rom_fallback();` immediately before each `return` in `console_run_config`.

Check the three existing uses of `rom_cmd[0] = '\0'` in the file: the one in `console_init` stays (nothing chosen yet). The one that was in the `off` branch is the line replaced above.

- [ ] **Step 5: Run the tests**

Run: `cmake --build build-host && ctest --test-dir build-host --output-on-failure`
Expected: `100% tests passed`. If `save_and_run_config_round_trip` or `selftest_passes` now sees the manager ROM where it expected none, make that test's config explicit with `console_exec("rom off");` before its `save`, and say so in the commit message.

- [ ] **Step 6: Build both Pico targets**

Run: `ninja -C build-pico && ninja -C build-pico-plusw`
Expected: both link; `check_core1_flash_free: ok` on each.

- [ ] **Step 7: Commit**

```bash
git add coco/stub/manager.asm coco/stub/Makefile firmware/src/ui/manager_rom.h \
        firmware/src/console/console.c firmware/tests/test_console.c
git commit -m "manager stub ROM, built into the firmware; rom load manager; fallback default"
```

---

### Task 5: Emulator check with `picoco-host --ui`

**Files:**
- Modify: `firmware/host/picoco_host.c`
- Modify: `coco/README.md` (a short "Manager stub in XRoar" section)

**Interfaces:**
- Consumes: `ui_init`, `ui_ctl`, `ui_feed`, `console_exec_capture`, `send_sock` (already in `picoco_host.c`).
- Produces: `picoco-host --ui` treats the TCP stream as a UI session (XRoar's Becker cart forwards only `$FF41`/`$FF42`, so the stub's `$FF43` write never arrives).

- [ ] **Step 1: Add the option**

In `firmware/host/picoco_host.c`:

Add `#include "ui.h"` with the other includes.

Add to `longopts`, before the terminator:

```c
        {"ui", no_argument, 0, 'u'},
```

Change the `getopt_long` string to `"d:p:m:H:r:u"`, add `bool ui_mode = false;` beside the other option variables, and a case:

```c
            case 'u': ui_mode = true; break;
```

After the existing `console_init(print_stdout, NULL, &srv, &store);` line add:

```c
    /* XRoar's Becker cart carries only $FF41/$FF42, so the stub's $FF43
     * write never arrives: --ui makes the whole connection a UI session. */
    if (ui_mode) ui_init(send_sock, &dummy_fd, console_exec_capture, &store);
```

The serving loop is in a function that takes the server; pass `ui_mode` and the socket-fd pointer to it the same way `srv` is passed (add a `bool ui_mode` parameter to that function and to its call). Then:

- where a client is accepted, after `srv->send_ctx = &client_fd;` add:

```c
                if (ui_mode) { ui_init(send_sock, &client_fd, console_exec_capture, srv->store); ui_ctl(0xA5); }
```

  (if `dw_server` has no `store` member, add a `dw_store *store` parameter to the loop function next to `ui_mode` and use that)

- where bytes arrive, replace `dw_feed(srv, buf, (size_t)n, plat_now_ms());` with:

```c
                if (ui_mode) {
                    if (!ui_active()) ui_ctl(0xA5);      /* the stub left and came back */
                    ui_feed(buf, (size_t)n, plat_now_ms());
                } else dw_feed(srv, buf, (size_t)n, plat_now_ms());
```

Update the usage string to end with `[--ui]`.

- [ ] **Step 2: Build**

Run: `cmake --build build-host && ctest --test-dir build-host --output-on-failure`
Expected: `100% tests passed`.

- [ ] **Step 3: Emulator, CoCo 2 with no Extended BASIC**

```bash
D=$(mktemp -d)
cp coco/carttest.rom "$D/CARTTEST.ROM"
cp firmware/roms/hdbdw3bck.rom "$D/HDB.ROM"
cp coco/stub/manager.rom ~/.xroar/roms/manager.rom
build-host/picoco-host --dir "$D" --port 65511 --ui &
xroar -machine coco2bus -no-extbas -becker -becker-port 65511 -cart-rom manager \
      -no-cart-autorun -rompath ~/.xroar/roms -gdb -ao null -type 'EXEC 49154\r' &
sleep 4
python3 coco/tools/xrscreen.py --wait-for 'PICOCO  16K NO ECB' --wait-for 'CARTTEST.ROM' --timeout 30
```

Expected: exit 0; the printed screen shows `PICOCO  16K NO ECB` on row 0 (XRoar's `coco2bus` has 64K, so `64K` or `32K` in place of `16K` is also correct here: assert on `NO ECB` and `CARTTEST.ROM` if so and note which was seen), `ROMS` on row 1, `CARTTEST.ROM` highlighted, `HDB.ROM` below it, and `ENTER=RUN  BREAK=BASIC` on the last row.

Kill both processes (`pkill xroar; pkill picoco-host`).

- [ ] **Step 4: Emulator, keys and the launch handshake**

Same commands with `-type 'EXEC 49154\r\n\r'` (down, then ENTER: selects `HDB.ROM`) and:

```bash
python3 coco/tools/xrscreen.py --wait-for 'NEEDS EXTENDED BASIC' --timeout 30
```

Expected: exit 0. This proves a key reaches the firmware, the selection moves, and the capability rule's message is drawn as a diff.

Then with `-type 'EXEC 49154\r\r'` (ENTER on `CARTTEST.ROM`):

Expected: `picoco-host`'s console shows the ROM load being run, and the screen returns to the manager (in the emulator the cart ROM is XRoar's own file, so the jump to `$C000` lands in the manager again; a real launch is a bench step). `xrscreen.py --wait-for 'ROMS'` exits 0.

If XRoar's `-type` does not deliver keys to `POLCAT` once the stub is running (it types by intercepting BASIC's ROM calls), record that in `coco/README.md` and treat the key path as bench-only: the first screen in Step 3 is the emulator gate.

- [ ] **Step 5: Emulator, CoCo 2 with Extended BASIC (autostart)**

```bash
build-host/picoco-host --dir "$D" --port 65511 --ui &
xroar -machine coco2bus -becker -becker-port 65511 -cart-rom manager \
      -rompath ~/.xroar/roms -gdb -ao null &
sleep 5
python3 coco/tools/xrscreen.py --wait-for 'ECB' --timeout 30
```

Expected: the manager appears with no typing (Extended BASIC found `DK` and called `$C002`), row 0 shows `ECB` without `NO`.

- [ ] **Step 6: Document and commit**

Add to `coco/README.md`, after the "Headless XRoar recipe" section, a section "Manager stub in XRoar" holding the three command blocks above and one sentence each on what they prove, plus the `-type` finding from Step 4.

```bash
git add firmware/host/picoco_host.c coco/README.md
git commit -m "picoco-host --ui; manager stub verified in XRoar (no-ECB and ECB CoCo 2)"
```

---

### Task 6: Bench on the 16K CoCo 2, and documentation

**Files:**
- Modify: `firmware/TEST_PLAN.md` (new section J), `CLAUDE.md` (Firmware section), `docs/superpowers/specs/2026-10-01-rom-manager-design.md` (record what the bench settled)

**Interfaces:**
- Consumes: the firmware from Tasks 1-4 flashed on the Pico 2 board; `coco/carttest.rom` on the flash volume.
- Produces: bench results; no code.

This task needs the user at the bench. Flashing and console commands are the session's; typing on the CoCo is the user's.

- [ ] **Step 1: Flash and prepare (board out of the CoCo or CoCo off)**

```bash
ninja -C build-pico
python3 firmware/tools/pconsole.py /dev/cu.usbmodem3103 bootsel
cp -X build-pico/picoco.uf2 /Volumes/RP2350/
```

Then on the console: `fs ls` must list `carttest.rom`; `rom load manager`, `becker native`, `bus drive on`, `save`, `reboot`, `status`.
Expected `status`: `rom now load manager`, `rom next load manager`.

- [ ] **Step 2: Add section J to `firmware/TEST_PLAN.md`**

Insert before "## How to resume with Claude":

```markdown
## J. ROM manager (stub + firmware UI)

Spec `docs/superpowers/specs/2026-10-01-rom-manager-design.md`. The manager
is a stub ROM in the firmware (`rom load manager`); the screens are drawn by
the Pico. Never swap the ROM from the USB console while the stub is running.

### J.1 16K CoCo 2, no Extended BASIC

1. Power on, `EXEC 49154`: screen shows `PICOCO  16K NO ECB`, `ROMS`, the
   `.ROM` files sorted, the first highlighted.
2. Down / up arrows move the highlight; it stops at both ends.
3. ENTER on `HDBDW3BCK.ROM`: `NEEDS EXTENDED BASIC`, still in the manager.
4. ENTER on `CARTTEST.ROM`: the cart test starts by itself and prints
   `PASS` lines (TEST_PLAN I.5). `status`: `rom now load carttest.rom`,
   `rom next load manager`.
5. Power-cycle: `EXEC 49154` is the manager again (launch is one-shot).
6. BREAK in the manager: back to `OK`. `EXEC 49154` enters it again.
7. Pull USB, power-cycle, `EXEC 49154`: works on cart power alone.
8. `status` after all of it: `underrun 0`, `oe_glitch 0`, `addr_resample 0`.
9. With the manager on screen, unplug and replug nothing, just wait 5
   minutes, then press down: the highlight still moves (idle session).

### J.2 Results

| Check | Result | Date |
|---|---|---|
| J.1.1 first screen | | |
| J.1.2 arrows | | |
| J.1.3 DOS ROM refused | | |
| J.1.4 launch carttest | | |
| J.1.5 one-shot launch | | |
| J.1.6 BREAK | | |
| J.1.7 no USB | | |
| J.1.8 counters | | |
| J.1.9 idle session | | |
```

- [ ] **Step 3: Run J.1 with the user and fill in J.2**

After each step the user reports, read `status` from the console and record the result and date in the table. A failure is recorded as a failure with what was seen (screen text, `status`, `trace dump` if the Becker counters moved), then debugged with superpowers:systematic-debugging before continuing.

- [ ] **Step 4: Update `CLAUDE.md` and the spec**

In `CLAUDE.md` "Firmware" section add:

```markdown
- **ROM manager (2026-10-01):** `rom load manager` loads a stub built into
  the firmware (`coco/stub/manager.asm` -> `firmware/src/ui/manager_rom.h`,
  regenerate with `make -C coco/stub`). The screens live in
  `firmware/src/ui/` on core0; the stub only draws and reports keys. A
  write of `$A5` to `$FF43` hands the Becker port to the UI in every mode,
  `$5A` gives it back. No `rom` line in `picoco.cfg` means the manager;
  `rom off` is an explicit, saved choice. Entry is `$C002` (`EXEC 49154`
  without Extended BASIC). TEST_PLAN J.
```

In the spec, change section 5.1's first bullet to say the `$FF43` write is handled by the Becker device's core0 write path (no core1 hook was needed), section 4.1's keyboard bullet to say keys come from `POLCAT`, and add under section 8 which of checks 1-6 this plan settled and which remain for the second plan.

- [ ] **Step 5: Commit**

```bash
git add firmware/TEST_PLAN.md CLAUDE.md docs/superpowers/specs/2026-10-01-rom-manager-design.md
git commit -m "docs: ROM manager bench results (16K CoCo 2), TEST_PLAN J"
```

---

## What the second plan covers

Written after Task 6, with its results in hand: Disks, Settings and WiFi screens with the line editor and cursor action; the BASIC loader and "previous ROM" restore; CoCo 3 cold restart and 32-column forcing; continue-boot and type-a-line actions; the Boot screen and load-and-jump; double RESET; retiring `PICOCO.BIN`.
