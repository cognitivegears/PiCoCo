# PiCoCo Manager Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A CoCo program (`PICOCO.BIN` on `PICOCO.DSK`) that browses, mounts, creates and boots disk images and changes PiCoCo settings, talking to the firmware over DriveWire 4 virtual serial channels.

**Architecture:** The firmware gains a DW4 virtual-serial layer (`src/dw/dw_vser.c`) inside the existing DriveWire server. Each command line it receives runs through the existing console dispatcher, via a deny-by-default allowlist, with output captured into a buffer and framed DW4-style (`OK command successful` / `FAIL nnn msg`, then hangup). The CoCo program is plain C (CMOC). It has its own Becker client, a pure-C parser that is tested on the host, and a 32x16 text UI.

**Tech Stack:** C11 firmware (pico-sdk / host build with ASan+UBSan, ctest); CMOC + LWTOOLS + ToolShed `decb` for the 6809 side; XRoar for emulation; Python 3 for `dwtest.py`.

**Spec:** `docs/superpowers/specs/2026-09-23-coco-manager-design.md`

## Global Constraints

- core1 code, `bus_table` Becker entries and the single-writer Becker rule are untouched: everything new runs on core0 (CLAUDE.md "Firmware").
- Core0 stack is 4 KB: buffers over ~256 bytes are `static` (existing pattern, e.g. `cmd_save`).
- Virtual-serial channels 1-13 only; one session at a time.
- Reply framing is byte-exact: `OK command successful\n\r` and `FAIL %03d <msg>\n\r` (LF then CR, as DW4 Java 4.3.3p).
- Remote allowlist exactly: `status`, `version`, `help`, `time`, `save`, `fs ls`, `fs new`, `dw mount`, `dw eject`, `dw hdbdos`, `dw disk`, `rom boot`. Everything else is `FAIL 255 console only`.
- FAIL codes: message starting `usage` → 010, `bad drive` → 101, else 255; message cut to 80 chars.
- Reply body buffer 4096 bytes; overflow ends with a `...` line.
- `fs new` image: 161,280 bytes, all `$FF` except bytes 68-255 of the sector at offset `0x13300` (track 17 sector 2), which are `$00`.
- Firmware version becomes `1.2` (`firmware/CMakeLists.txt`).
- CoCo program: 32K, CoCo 1/2/3, 32x16 uppercase VDG text at `$0400`, loads at `$3800`, and code+data+BSS must end below `$7800` (the BASIC stack and string space sit just under `$7FFF`).
- CoCo C code is C89-style (declarations at the top of blocks) so CMOC and the host compiler both accept `parse.c`.
- Never hand-edit generated files (CLAUDE.md). Don't commit unless the user asked; this plan's commit steps assume the user approved committing on the feature branch.

## Review Focus

1. **A CoCo reset or BREAK mid-session.** The Pico reboots on reset, and a new SS.Open must always start a clean session, so no stale queue ever leaks into the next command. Pinned in Task 3 (`reopen_after_hangup_is_clean`).
2. **A file name with spaces, or one of 32+ characters.** Spaces must mount via `dw disk insert`; long names must fail with a readable message, not a crash. Pinned in Task 6 (`disk_insert_name_with_spaces`, `disk_insert_long_name_fails`).
3. **`fs ls` output larger than the reply buffer** (a 14.5 MB Plus-W volume full of images). The body must truncate with `...`, the CoCo must not overrun its buffer, and the parser must stop cleanly. Pinned in Task 5 (`remote_output_truncates`) and Task 10 (`ls_truncated_tail`).
4. **A second DW client opening another channel while the manager holds channel 1.** That client gets a hangup and channel 1's reply is undisturbed. Pinned in Task 3 (`second_channel_rejected`).
5. **`fs new` on a name that exists, or a full volume.** Never truncate an existing image; remove the partial file. Pinned in Task 7 (`fs_new_refuses_existing`).

---

## File map

Firmware (`firmware/`):
- Create `src/dw/dw_vser.h`, `src/dw/dw_vser.c`: DW4 virtual-serial state, poll encoding, command-mode line handling, reply queue. No console dependency: takes an `exec` callback.
- Modify `src/dw/dw.h`, `src/dw/dw_server.c`: embed `dw_vser`, route SER* opcodes to it, `dw_set_exec()`.
- Modify `src/console/console.c`, `src/console/console.h`: `console_exec_remote()` (allowlist + capture), `dw disk ...`, `fs new`, `rom boot`, `rom now/next` in `status`, `clock` in `time`.
- Modify `src/dev/rom.c`, `src/dev/rom.h`: `rom_check_file()`.
- Modify `src/plat.h`, `host/plat_host.c`, `src/plat_pico.c`, `src/main.c`: `plat_rtc_get/set` (Task 8).
- Modify `CMakeLists.txt`: add `src/dw/dw_vser.c` to `CORE_SRC`, version 1.2.
- Create `tests/test_dw_vser.c`. Modify `tests/test_console.c`.
- Modify `tools/dwtest.py`: a vserial session check.

CoCo (`coco/`, new):
- `Makefile`, `PICOCO.BAS`, `README.md`
- `parse.h`, `parse.c`: pure C (host + CMOC), `test_parse.c` (host only)
- `dw.h`, `dw.c`: Becker I/O, `picoco_cmd()`, `dw_read_sector()`
- `ui.h`, `ui.c`: screen, keys, main and settings screens
- `boot.h`, `boot.c`: boot flows; `hook.asm`: BASIC command handoff (from the Task 2 spike)
- `main.c`: entry

---

### Task 1: CoCo toolchain and a hello-world disk in XRoar

**Files:**
- Create: `coco/Makefile`, `coco/PICOCO.BAS`, `coco/main.c` (temporary hello), `coco/README.md`, `coco/.gitignore`

**Interfaces:**
- Produces: `make -C coco` → `coco/PICOCO.BIN`, `coco/PICOCO.DSK`; `make -C coco test` (host tests, used from Task 10).

This task needs the user for two things: network installs (Homebrew, CMOC and ToolShed sources) and CoCo BASIC ROMs for XRoar, which cannot be redistributed. Ask the user for the ROMs up front.

- [ ] **Step 1: Install LWTOOLS and XRoar**

Run: `brew install lwtools xroar`
Expected: `lwasm --version` prints 4.2x; `xroar -help` prints usage.

- [ ] **Step 2: Build and install CMOC**

Download the latest CMOC tarball from `http://sarrazip.com/dev/cmoc.html` (needs network allowlisting for that host), then:
```bash
tar xzf cmoc-*.tar.gz && cd cmoc-*/ && ./configure --prefix=/opt/homebrew && make && make install
```
Expected: `cmoc --version` prints a version. Keep the tarball's `doc/cmoc-manual.markdown`: later tasks check `inkey()`, `--org` and `asm {}` syntax against it.

- [ ] **Step 3: Build and install ToolShed**

```bash
git clone https://github.com/nitros9project/toolshed.git /tmp/toolshed
make -C /tmp/toolshed/build/unix && sudo make -C /tmp/toolshed/build/unix install   # or copy build/unix/decb/decb into /opt/homebrew/bin
```
Expected: `decb` prints its usage.

- [ ] **Step 4: Get the ROMs for XRoar**

Ask the user to put `bas13.rom`, `extbas11.rom` (CoCo 2) and `coco3.rom` (CoCo 3) into `~/.xroar/roms/`. Copy `firmware/roms/hdbdw3bck.rom` and `hdbdw3bc3.rom` there too.

- [ ] **Step 5: Write the build files**

`coco/Makefile`:
```make
# PiCoCo manager: CMOC + LWTOOLS build of PICOCO.BIN, ToolShed decb for the .DSK.
CMOC ?= cmoc
DECB ?= decb
ORG  ?= 0x3800
SRCS  = main.c

all: PICOCO.DSK

PICOCO.BIN: $(SRCS) $(wildcard *.h)
	$(CMOC) --coco --org=$(ORG) -o $@ $(SRCS)

PICOCO.DSK: PICOCO.BIN PICOCO.BAS
	rm -f $@
	$(DECB) dskini $@
	$(DECB) copy -2 -b PICOCO.BIN $@,PICOCO.BIN
	$(DECB) copy -0 -a -t PICOCO.BAS $@,PICOCO.BAS

test:
	@echo "no host tests yet"

clean:
	rm -f PICOCO.BIN PICOCO.DSK *.o *.map *.lst *.s test_parse

.PHONY: all test clean
```

`coco/PICOCO.BAS` (ASCII BASIC; line 10 skips `WIDTH` on a CoCo 1/2, where the reset vector at 65534 is not $8C):
```basic
10 IF PEEK(65534)=140 THEN WIDTH32
20 LOADM"PICOCO":EXEC
```

`coco/main.c` (temporary):
```c
#include <cmoc.h>

int main(void)
{
    printf("PICOCO HELLO\n");
    return 0;
}
```

`coco/.gitignore`:
```
PICOCO.BIN
PICOCO.DSK
*.o
*.map
*.lst
*.s
test_parse
```

- [ ] **Step 6: Build**

Run: `make -C coco`
Expected: `coco/PICOCO.DSK` exists, and `decb dir coco/PICOCO.DSK` lists `PICOCO BIN` and `PICOCO BAS`.

- [ ] **Step 7: Run it in XRoar against picoco-host**

```bash
cmake -B build-host -G Ninja -DPICOCO_HOST=ON firmware && ninja -C build-host
mkdir -p "$TMPDIR/pcdisk" && cp coco/PICOCO.DSK "$TMPDIR/pcdisk/"
build-host/picoco-host --dir "$TMPDIR/pcdisk" --mount 0=PICOCO.DSK &
xroar -machine coco2bus -cart hdbdos -becker -rompath ~/.xroar/roms
```
(Local port binding may need `sandbox.network.allowLocalBinding: true`, or run outside the sandbox.) Check XRoar's `-help` for the exact CoCo 2 machine name on this version, and for the becker host/port options if the defaults are not 127.0.0.1:65504. At the `OK` prompt type `RUN"PICOCO"`.
Expected: `PICOCO HELLO` then `OK`. Repeat with `-machine coco3`: same result.

- [ ] **Step 8: Write `coco/README.md`**

```markdown
# PiCoCo manager (CoCo side)

Build: `make` (needs CMOC, LWTOOLS, ToolShed `decb`). Host tests: `make test`.

Emulate: run `picoco-host --dir <dir> --mount 0=PICOCO.DSK` from the
firmware host build, then `xroar -machine coco2bus -cart hdbdos -becker`
and `RUN"PICOCO"`.

On a PiCoCo: copy `PICOCO.DSK` to the flash (`fs export`), `dw mount 3
PICOCO.DSK`, `save`, then on the CoCo `DRIVE 3:RUN"PICOCO"`.
Needs firmware 1.2 or later and `becker native`.
```

- [ ] **Step 9: Commit**

```bash
git add coco/Makefile coco/PICOCO.BAS coco/main.c coco/README.md coco/.gitignore
git commit -m "coco: toolchain skeleton for the PiCoCo manager"
```

---

### Task 2: Spike: hand a command line to BASIC from machine code (throwaway)

Question: can the program return to BASIC and have BASIC run `RUN"X"` or `LOADM"X":EXEC` on its own, on Color BASIC 1.3 + Extended + HDB-DOS (CoCo 2) and on CoCo 3 BASIC + HDB-DOS?

Candidate: temporarily hook the CONSOLE IN RAM vector RVEC4 (`$016A`, 3 bytes). Color BASIC `JSR`s it at the top of CONSOLE IN. The hook drops that return address (`LEAS 2,S`) and `RTS`es with the next queued character in A, which returns the character straight to CONSOLE IN's caller. After it hands back the final CR it restores the original 3 bytes.

- [ ] **Step 1: Write `coco/hook.asm`**

```asm
* hook.asm: feed a command line to BASIC through RVEC4 (CONSOLE IN).
* void hook_install(void) arms the hook with the NUL-terminated text in
* hook_text (the caller copies text in and appends CR 13 before the NUL).
        SECTION code
        EXPORT  _hook_install
        EXPORT  _hook_text
RVEC4   EQU     $016A
_hook_text RMB  64
hook_ptr   RMB  2
hook_save  RMB  3

_hook_install
        ldx     #_hook_text
        stx     hook_ptr
        ldd     RVEC4
        std     hook_save
        lda     RVEC4+2
        sta     hook_save+2
        lda     #$7E            JMP extended
        sta     RVEC4
        ldx     #hook
        stx     RVEC4+1
        rts

hook    pshs    b,x
        ldx     hook_ptr
        lda     ,x+
        stx     hook_ptr
        cmpa    #13
        bne     hookout
        ldx     hook_save       last char: put RVEC4 back
        stx     RVEC4
        ldb     hook_save+2
        stb     RVEC4+2
hookout puls    b,x
        leas    2,s             drop CONSOLE IN's JSR RVEC4 return...
        rts                     ...so A goes straight to CONSOLE IN's caller
        ENDSECTION
```

- [ ] **Step 2: Temporary `main.c` that queues `PRINT"HANDOFF OK"` and returns**

```c
#include <cmoc.h>

extern char hook_text[];
void hook_install(void);

int main(void)
{
    strcpy(hook_text, "PRINT\"HANDOFF OK\"\r");
    hook_install();
    return 0;
}
```
Assemble and link: `lwasm --format=obj -o hook.o hook.asm`, then add `hook.o` to the `cmoc` command (check the CMOC manual's "linking with assembly" section for the exact form).

- [ ] **Step 3: Run in XRoar, CoCo 2 then CoCo 3 (setup from Task 1 Step 7)**

Expected on success: after `RUN"PICOCO"` the screen shows `OK`, then `PRINT"HANDOFF OK"` echoed, `HANDOFF OK`, `OK`, and the keyboard works normally afterwards.

- [ ] **Step 4: If it fails, iterate within the time box (about 1 hour)**

- Disassemble CONSOLE IN in XRoar's monitor (`-gdb`, or the ROM listing *Color Basic Unravelled*) to check what is on the stack when RVEC4 is called, and adjust the `LEAS`.
- If the hook can't be made reliable on both machines, choose the fallback: exit to BASIC and print the command at the cursor (Task 14 Step 3b).

- [ ] **Step 5: Record the result**

Append a short "Spike result (date)" paragraph to spec §5.5 saying which method works on which machines. Keep `hook.asm` only if it works. Restore `main.c` to the Task 1 hello. Do not commit the throwaway `main.c`.

---

### Task 3: `dw_vser`: DW4 virtual serial and command mode

**Files:**
- Create: `firmware/src/dw/dw_vser.h`, `firmware/src/dw/dw_vser.c`, `firmware/tests/test_dw_vser.c`
- Modify: `firmware/CMakeLists.txt:9-11` (add `src/dw/dw_vser.c` to `CORE_SRC`)

**Interfaces:**
- Produces:
  ```c
  typedef int (*vser_exec_fn)(void *ctx, const char *line, char *out, size_t cap, size_t *outn);
  void   vser_init(dw_vser *v, vser_exec_fn exec, void *ctx);
  void   vser_reset(dw_vser *v);
  void   vser_open(dw_vser *v, uint8_t ch);
  void   vser_close(dw_vser *v, uint8_t ch);
  void   vser_write(dw_vser *v, uint8_t ch, const uint8_t *b, size_t n);
  void   vser_serread(dw_vser *v, uint8_t out[2]);
  size_t vser_serreadm(dw_vser *v, uint8_t ch, size_t n, uint8_t *out);
  ```
  The `exec` contract: return 0 means OK, with `out[0..*outn)` as the body. Non-zero is a DW4 code, with `out[0..*outn)` as a one-line message without a newline.

- [ ] **Step 1: Write the header**

`firmware/src/dw/dw_vser.h`:
```c
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* DW4 virtual serial, command mode only (spec 2026-09-23 §4.1-4.2). One
 * session at a time on channels 1..13; wire behaviour follows DW4 Java
 * 4.3.3p DWVSerialPorts.serRead / DWVSerialPort. */
#define VSER_CH_MIN   1
#define VSER_CH_MAX   13
#define VSER_LINE_MAX 128
#define VSER_HDR_MAX  96
#define VSER_BODY_MAX 4096

/* Runs one command line. Returns 0 for OK with out[0..*outn) as the reply
 * body, or a DW4 result code with out[0..*outn) as a one-line message. */
typedef int (*vser_exec_fn)(void *ctx, const char *line, char *out, size_t cap, size_t *outn);

typedef struct {
    vser_exec_fn exec; void *exec_ctx;
    uint8_t ch, opens, reject_ch;
    bool replied, closing, overflow;
    char line[VSER_LINE_MAX]; uint16_t linelen;
    /* Reply queue: the body is written at q+VSER_HDR_MAX and the status
     * line is placed just before it, so neither is copied. */
    uint8_t q[VSER_HDR_MAX + VSER_BODY_MAX]; uint16_t qhead, qlen;
} dw_vser;

void   vser_init(dw_vser *v, vser_exec_fn exec, void *ctx);
void   vser_reset(dw_vser *v);                  /* DWINIT, RESET */
void   vser_open(dw_vser *v, uint8_t ch);       /* SERINIT, SS.Open */
void   vser_close(dw_vser *v, uint8_t ch);      /* SERTERM, SS.Close */
void   vser_write(dw_vser *v, uint8_t ch, const uint8_t *b, size_t n);
void   vser_serread(dw_vser *v, uint8_t out[2]);
size_t vser_serreadm(dw_vser *v, uint8_t ch, size_t n, uint8_t *out); /* n bytes, or 0 */
```

- [ ] **Step 2: Write the failing tests**

`firmware/tests/test_dw_vser.c`:
```c
#include "test.h"
#include "dw_vser.h"
#include <stdlib.h>

static dw_vser v;
static char last_line[VSER_LINE_MAX];
static int exec_calls;
static int fake_rc;
static const char *fake_out;

static int fake_exec(void *ctx, const char *line, char *out, size_t cap, size_t *outn) {
    (void)ctx;
    exec_calls++;
    snprintf(last_line, sizeof(last_line), "%s", line);
    size_t n = strlen(fake_out);
    if (n > cap) n = cap;
    memcpy(out, fake_out, n);
    *outn = n;
    return fake_rc;
}

static void setup(void) {
    vser_init(&v, fake_exec, NULL);
    exec_calls = 0; fake_rc = 0; fake_out = "hello\n"; last_line[0] = '\0';
}

static void wr(uint8_t ch, const char *s) { vser_write(&v, ch, (const uint8_t *)s, strlen(s)); }

/* Drains channel ch the way a client does; returns bytes read, sets *hung. */
static size_t drain(uint8_t ch, uint8_t *buf, size_t cap, int *hung) {
    size_t n = 0; *hung = 0;
    for (int guard = 0; guard < 10000; guard++) {
        uint8_t r[2];
        vser_serread(&v, r);
        if (r[0] == 0x10 && r[1] == ch) { *hung = 1; return n; }
        if (r[0] == 0 && r[1] == 0) return n;
        if (r[0] == ch + 1) { if (n < cap) buf[n++] = r[1]; continue; }
        if (r[0] == ch + 17) {
            uint8_t tmp[256];
            size_t got = vser_serreadm(&v, ch, r[1], tmp);
            if (got != r[1]) return n;
            for (size_t i = 0; i < got && n < cap; i++) buf[n++] = tmp[i];
            continue;
        }
        return n;
    }
    return n;
}

TEST(idle_serread_is_zero) {
    setup();
    uint8_t r[2] = {9, 9};
    vser_serread(&v, r);
    ASSERT_EQ(r[0], 0); ASSERT_EQ(r[1], 0);
}

TEST(write_without_open_is_dropped) {
    setup();
    wr(1, "version\r");
    ASSERT_EQ(exec_calls, 0);
}

TEST(ok_reply_framing_and_hangup) {
    setup();
    vser_open(&v, 1);
    wr(1, "version\r");
    ASSERT_EQ(exec_calls, 1);
    ASSERT(strcmp(last_line, "version") == 0);
    uint8_t buf[512]; int hung;
    size_t n = drain(1, buf, sizeof(buf), &hung);
    const char *want = "OK command successful\n\rhello\n";
    ASSERT_EQ(n, strlen(want));
    ASSERT_MEMEQ(buf, want, n);
    ASSERT(hung);
    ASSERT_EQ(v.ch, 0);
}

TEST(fail_reply_framing) {
    setup();
    fake_rc = 10; fake_out = "usage: dw hdbdos on|off";
    vser_open(&v, 1);
    wr(1, "dw hdbdos\r");
    uint8_t buf[512]; int hung;
    size_t n = drain(1, buf, sizeof(buf), &hung);
    const char *want = "FAIL 010 usage: dw hdbdos on|off\n\r";
    ASSERT_EQ(n, strlen(want));
    ASSERT_MEMEQ(buf, want, n);
    ASSERT(hung);
}

TEST(serread_small_is_single_byte_large_is_bulk) {
    setup();
    fake_out = "";   /* body empty: queue is just the 23-byte OK line */
    vser_open(&v, 3);
    wr(3, "x\r");
    uint8_t r[2];
    vser_serread(&v, r);
    ASSERT_EQ(r[0], 3 + 17); ASSERT_EQ(r[1], 23);
    uint8_t tmp[256];
    ASSERT_EQ(vser_serreadm(&v, 3, 21, tmp), 21);
    vser_serread(&v, r);            /* 2 left: single-byte mode */
    ASSERT_EQ(r[0], 3 + 1); ASSERT_EQ(r[1], '\n');
    vser_serread(&v, r);
    ASSERT_EQ(r[0], 3 + 1); ASSERT_EQ(r[1], '\r');
    vser_serread(&v, r);
    ASSERT_EQ(r[0], 0x10); ASSERT_EQ(r[1], 3);
}

TEST(bulk_count_caps_at_255) {
    setup();
    static char big[1000];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    fake_out = big;
    vser_open(&v, 1);
    wr(1, "x\r");
    uint8_t r[2];
    vser_serread(&v, r);
    ASSERT_EQ(r[0], 18); ASSERT_EQ(r[1], 255);
}

TEST(serreadm_short_or_wrong_channel_sends_nothing) {
    setup();
    vser_open(&v, 1);
    wr(1, "x\r");
    uint8_t tmp[256];
    ASSERT_EQ(vser_serreadm(&v, 2, 5, tmp), 0);
    ASSERT_EQ(vser_serreadm(&v, 1, 200, tmp), 0);   /* only 29 queued */
    ASSERT_EQ(v.qlen, 29);
}

TEST(line_editing) {
    setup();
    vser_open(&v, 1);
    wr(1, "  verx");
    uint8_t bs = 0x08, nul = 0, lf = '\n';
    vser_write(&v, 1, &bs, 1);
    vser_write(&v, 1, &nul, 1);
    wr(1, "sion  ");
    vser_write(&v, 1, &lf, 1);
    ASSERT_EQ(exec_calls, 0);
    wr(1, "\r");
    ASSERT_EQ(exec_calls, 1);
    ASSERT(strcmp(last_line, "version") == 0);
}

TEST(blank_line_ignored) {
    setup();
    vser_open(&v, 1);
    wr(1, "   \r");
    ASSERT_EQ(exec_calls, 0);
    wr(1, "status\r");
    ASSERT_EQ(exec_calls, 1);
}

TEST(overlong_line_fails_010) {
    setup();
    vser_open(&v, 1);
    char l[300];
    memset(l, 'a', 299); l[299] = '\0';
    wr(1, l);
    wr(1, "\r");
    ASSERT_EQ(exec_calls, 0);
    uint8_t buf[256]; int hung;
    size_t n = drain(1, buf, sizeof(buf), &hung);
    buf[n] = 0;
    ASSERT(strncmp((char *)buf, "FAIL 010 line too long\n\r", n) == 0);
    ASSERT(hung);
}

TEST(writes_after_command_dropped) {
    setup();
    vser_open(&v, 1);
    wr(1, "a\rb\r");
    ASSERT_EQ(exec_calls, 1);
}

TEST(second_channel_rejected) {
    setup();
    vser_open(&v, 1);
    vser_open(&v, 2);
    uint8_t r[2];
    vser_serread(&v, r);
    ASSERT_EQ(r[0], 0x10); ASSERT_EQ(r[1], 2);
    wr(1, "version\r");                 /* channel 1 undisturbed */
    uint8_t buf[512]; int hung;
    size_t n = drain(1, buf, sizeof(buf), &hung);
    ASSERT(n > 0 && hung);
}

TEST(bad_channels_ignored) {
    setup();
    vser_open(&v, 0);
    vser_open(&v, 14);
    vser_open(&v, 15);
    ASSERT_EQ(v.ch, 0);
}

TEST(open_count_and_close) {
    setup();
    vser_open(&v, 1);
    vser_open(&v, 1);
    vser_close(&v, 1);
    ASSERT_EQ(v.ch, 1);
    vser_close(&v, 1);
    ASSERT_EQ(v.ch, 0);
}

TEST(reset_drops_session) {
    setup();
    vser_open(&v, 1);
    wr(1, "x\r");
    vser_reset(&v);
    uint8_t r[2];
    vser_serread(&v, r);
    ASSERT_EQ(r[0], 0); ASSERT_EQ(r[1], 0);
}

TEST(reopen_after_hangup_is_clean) {
    setup();
    vser_open(&v, 1);
    wr(1, "x");                  /* partial line, then client vanishes (BREAK) */
    vser_close(&v, 1);
    vser_open(&v, 1);
    wr(1, "status\r");
    ASSERT(strcmp(last_line, "status") == 0);
}

TEST(no_exec_handler_fails_255) {
    vser_init(&v, NULL, NULL);
    vser_open(&v, 1);
    wr(1, "x\r");
    uint8_t buf[256]; int hung;
    size_t n = drain(1, buf, sizeof(buf), &hung);
    buf[n] = 0;
    ASSERT(strncmp((char *)buf, "FAIL 255 no command handler\n\r", n) == 0);
}

TEST(fail_message_cut_to_80) {
    setup();
    static char longmsg[200];
    memset(longmsg, 'm', 199); longmsg[199] = '\0';
    fake_rc = 255; fake_out = longmsg;
    vser_open(&v, 1);
    wr(1, "x\r");
    ASSERT_EQ(v.qlen, 9 + 80 + 2);
}

int main(void) {
    RUN(idle_serread_is_zero);
    RUN(write_without_open_is_dropped);
    RUN(ok_reply_framing_and_hangup);
    RUN(fail_reply_framing);
    RUN(serread_small_is_single_byte_large_is_bulk);
    RUN(bulk_count_caps_at_255);
    RUN(serreadm_short_or_wrong_channel_sends_nothing);
    RUN(line_editing);
    RUN(blank_line_ignored);
    RUN(overlong_line_fails_010);
    RUN(writes_after_command_dropped);
    RUN(second_channel_rejected);
    RUN(bad_channels_ignored);
    RUN(open_count_and_close);
    RUN(reset_drops_session);
    RUN(reopen_after_hangup_is_clean);
    RUN(no_exec_handler_fails_255);
    RUN(fail_message_cut_to_80);
    TEST_MAIN_END
}
```

- [ ] **Step 3: Add the source to CMake and confirm the tests fail to link**

In `firmware/CMakeLists.txt` `CORE_SRC`, change `src/dw/dw_util.c src/dw/dw_disk.c src/dw/dw_server.c` to `src/dw/dw_util.c src/dw/dw_disk.c src/dw/dw_server.c src/dw/dw_vser.c`. Create an empty `firmware/src/dw/dw_vser.c` containing only `#include "dw_vser.h"`.
Run: `cmake -B build-host -G Ninja -DPICOCO_HOST=ON firmware && ninja -C build-host`
Expected: link errors for `vser_init` and the other `vser_*` symbols.

- [ ] **Step 4: Implement**

`firmware/src/dw/dw_vser.c`:
```c
#include "dw_vser.h"
#include <stdio.h>
#include <string.h>

static const char OK_HDR[] = "OK command successful\n\r";

static void session_clear(dw_vser *v) {
    v->ch = 0; v->opens = 0;
    v->replied = v->closing = v->overflow = false;
    v->linelen = 0; v->qhead = v->qlen = 0;
}

void vser_init(dw_vser *v, vser_exec_fn exec, void *ctx) {
    memset(v, 0, sizeof(*v));
    v->exec = exec;
    v->exec_ctx = ctx;
}

void vser_reset(dw_vser *v) {
    session_clear(v);
    v->reject_ch = 0;
}

void vser_open(dw_vser *v, uint8_t ch) {
    if (ch < VSER_CH_MIN || ch > VSER_CH_MAX) return;
    if (v->ch == 0) {
        session_clear(v);
        v->ch = ch;
        v->opens = 1;
    } else if (v->ch == ch) {
        v->opens++;
    } else {
        /* ponytail: one session at a time; a second channel gets an
         * immediate hangup. Per-port state when tcp/WiFi channels need it. */
        v->reject_ch = ch;
    }
}

void vser_close(dw_vser *v, uint8_t ch) {
    if (v->ch == 0 || ch != v->ch) return;
    if (v->opens > 0) v->opens--;
    if (v->opens == 0) session_clear(v);
}

static void queue_fail(dw_vser *v, int code, const char *msg) {
    char *h = (char *)v->q;   /* msg lives at q+VSER_HDR_MAX: no overlap */
    int n = snprintf(h, VSER_HDR_MAX - 2, "FAIL %03d %.80s", code, msg);
    if (n < 0) n = 0;
    if (n > VSER_HDR_MAX - 3) n = VSER_HDR_MAX - 3;
    h[n++] = '\n';
    h[n++] = '\r';
    v->qhead = 0;
    v->qlen = (uint16_t)n;
}

static void run_line(dw_vser *v) {
    char *l = v->line;
    l[v->linelen] = '\0';
    bool overflow = v->overflow;
    v->linelen = 0;
    v->overflow = false;
    while (*l == ' ') l++;
    size_t n = strlen(l);
    while (n && l[n - 1] == ' ') l[--n] = '\0';
    if (!overflow && n == 0) return;
    v->replied = true;
    if (overflow) { queue_fail(v, 10, "line too long"); return; }
    if (!v->exec) { queue_fail(v, 255, "no command handler"); return; }
    char *body = (char *)v->q + VSER_HDR_MAX;
    size_t bodyn = 0;
    int rc = v->exec(v->exec_ctx, l, body, VSER_BODY_MAX, &bodyn);
    if (bodyn > VSER_BODY_MAX) bodyn = VSER_BODY_MAX;
    if (rc != 0) {
        if (bodyn >= VSER_BODY_MAX) bodyn = VSER_BODY_MAX - 1;
        body[bodyn] = '\0';
        queue_fail(v, rc, body);
        return;
    }
    size_t hl = sizeof(OK_HDR) - 1;
    memcpy(v->q + VSER_HDR_MAX - hl, OK_HDR, hl);
    v->qhead = (uint16_t)(VSER_HDR_MAX - hl);
    v->qlen = (uint16_t)(hl + bodyn);
}

void vser_write(dw_vser *v, uint8_t ch, const uint8_t *b, size_t n) {
    if (v->ch == 0 || ch != v->ch) return;
    for (size_t i = 0; i < n && !v->replied; i++) {
        uint8_t c = b[i];
        if (c == '\r') run_line(v);
        else if (c == '\n' || c == 0) continue;
        else if (c == 0x08) { if (v->linelen) v->linelen--; }
        else if (v->linelen < VSER_LINE_MAX - 1) v->line[v->linelen++] = (char)c;
        else v->overflow = true;
    }
}

static void pop(dw_vser *v, size_t n) {
    v->qhead = (uint16_t)(v->qhead + n);
    v->qlen = (uint16_t)(v->qlen - n);
    if (v->qlen == 0 && v->replied) v->closing = true;
}

void vser_serread(dw_vser *v, uint8_t out[2]) {
    out[0] = out[1] = 0;
    if (v->reject_ch) {
        out[0] = 0x10; out[1] = v->reject_ch;
        v->reject_ch = 0;
        return;
    }
    if (v->ch == 0) return;
    if (v->closing) {
        out[0] = 0x10; out[1] = v->ch;
        session_clear(v);
        return;
    }
    if (v->qlen == 0) return;
    if (v->qlen < 3) {   /* DW4 VSerial_MultiReadLimit */
        out[0] = (uint8_t)(v->ch + 1);
        out[1] = v->q[v->qhead];
        pop(v, 1);
        return;
    }
    out[0] = (uint8_t)(v->ch + 17);
    out[1] = (uint8_t)(v->qlen > 255 ? 255 : v->qlen);
}

size_t vser_serreadm(dw_vser *v, uint8_t ch, size_t n, uint8_t *out) {
    if (v->ch == 0 || ch != v->ch || n == 0 || n > v->qlen) return 0;
    memcpy(out, v->q + v->qhead, n);
    pop(v, n);
    return n;
}
```

- [ ] **Step 5: Run the tests**

Run: `ninja -C build-host && ctest --test-dir build-host --output-on-failure -R test_dw_vser`
Expected: `18 tests, 0 failed`.

- [ ] **Step 6: Commit**

```bash
git add firmware/src/dw/dw_vser.h firmware/src/dw/dw_vser.c firmware/tests/test_dw_vser.c firmware/CMakeLists.txt
git commit -m "firmware: DW4 virtual serial command-mode layer (dw_vser)"
```

---

### Task 4: Route the SER* opcodes in `dw_server` to `dw_vser`

**Files:**
- Modify: `firmware/src/dw/dw.h` (include `dw_vser.h`, add a `dw_vser vser;` field to `dw_server`, add `dw_set_exec`)
- Modify: `firmware/src/dw/dw_server.c` (the `dispatch()` switch, `dw_init`, new `dw_set_exec`)
- Test: `firmware/tests/test_dw_vser.c` (append wire-level tests)

**Interfaces:**
- Consumes: Task 3's `vser_*`.
- Produces: `void dw_set_exec(dw_server *s, vser_exec_fn fn, void *ctx);`

- [ ] **Step 1: Append failing wire tests to `test_dw_vser.c`**

Add `#include "dw.h"` at the top, then these tests before `main`, and add their `RUN(...)` lines to `main`:
```c
static dw_server srv;
static uint8_t wout[8192];
static size_t woutn;
static void wsend(void *ctx, const uint8_t *b, size_t n) { (void)ctx; memcpy(wout + woutn, b, n); woutn += n; }
#define WFEED(...) do { uint8_t _b[] = { __VA_ARGS__ }; dw_feed(&srv, _b, sizeof(_b), 0); } while (0)

static void wsetup(void) {
    dw_init(&srv, NULL, wsend, NULL);
    dw_set_exec(&srv, fake_exec, NULL);
    woutn = 0; exec_calls = 0; fake_rc = 0; fake_out = "hi\n";
}

TEST(wire_setstat_open_serwritem_serread_serreadm) {
    wsetup();
    WFEED(0xC4, 1, 0x29);                        /* SS.Open ch 1 */
    WFEED(0x64, 1, 4, 'v', 'e', 'r', '\r');      /* SERWRITEM */
    ASSERT_EQ(exec_calls, 1);
    ASSERT_EQ(woutn, 0);
    WFEED(0x43);                                 /* SERREAD: 26 queued -> bulk */
    ASSERT_EQ(woutn, 2);
    ASSERT_EQ(wout[0], 18); ASSERT_EQ(wout[1], 26);
    woutn = 0;
    WFEED(0x63, 1, 26);                          /* SERREADM */
    ASSERT_EQ(woutn, 26);
    ASSERT_MEMEQ(wout, "OK command successful\n\rhi\n", 26);
    woutn = 0;
    WFEED(0x43);
    ASSERT_EQ(wout[0], 0x10); ASSERT_EQ(wout[1], 1);
}

TEST(wire_serinit_is_open_and_serwrite_fastwrite) {
    wsetup();
    WFEED(0x45, 2);                              /* SERINIT ch 2 */
    WFEED(0xC3, 2, 'x');                         /* SERWRITE */
    WFEED(0x82, '\r');                           /* FASTWRITE ch 2 */
    ASSERT_EQ(exec_calls, 1);
    ASSERT(strcmp(last_line, "x") == 0);
    WFEED(0xC5, 2);                              /* SERTERM */
    ASSERT_EQ(srv.vser.ch, 0);
}

TEST(wire_setstat_close) {
    wsetup();
    WFEED(0xC4, 1, 0x29);
    WFEED(0xC4, 1, 0x2A);
    ASSERT_EQ(srv.vser.ch, 0);
}

TEST(wire_dwinit_and_reset_clear_sessions) {
    wsetup();
    WFEED(0xC4, 1, 0x29);
    WFEED(0x5A, 0x80);
    ASSERT_EQ(srv.vser.ch, 0);
    WFEED(0xC4, 1, 0x29);
    WFEED(0xFE);
    ASSERT_EQ(srv.vser.ch, 0);
}

TEST(wire_serwritem_count_zero_is_256_bytes) {
    wsetup();
    WFEED(0xC4, 1, 0x29);
    uint8_t pkt[3 + 256];
    pkt[0] = 0x64; pkt[1] = 1; pkt[2] = 0;
    memset(pkt + 3, 'a', 256);
    dw_feed(&srv, pkt, sizeof(pkt), 0);
    WFEED(0xC3, 1, '\r');
    ASSERT_EQ(exec_calls, 0);    /* overflowed line: FAIL 010, exec never runs */
    WFEED(0x43);
    ASSERT_EQ(wout[0], 18);
    ASSERT_EQ(wout[1], 24);      /* "FAIL 010 line too long\n\r" */
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `ninja -C build-host`
Expected: compile error, `dw_set_exec` undeclared and no `vser` member.

- [ ] **Step 3: Implement**

In `firmware/src/dw/dw.h`, add `#include "dw_vser.h"` after `#include "dw_util.h"`. Add `dw_vser vser;` as the last field of `struct dw_server`, after `dw_stats stats;`. Add the declaration at the end of the prototypes:
```c
void dw_set_exec(dw_server *s, vser_exec_fn fn, void *ctx);  /* command handler for vserial lines */
```

In `firmware/src/dw/dw_server.c` `dispatch()`:
1. Remove `case DW_OP_SERINIT:`, `case DW_OP_SERTERM:` and `case DW_OP_SERWRITE:` from the consume-only group, and add:
```c
        case DW_OP_SERINIT:
            vser_open(&s->vser, s->buf[0]);
            break;
        case DW_OP_SERTERM:
            vser_close(&s->vser, s->buf[0]);
            break;
        case DW_OP_SERWRITE:
            vser_write(&s->vser, s->buf[0], &s->buf[1], 1);
            break;
```
2. In the RESET1/2/3 case, add `vser_reset(&s->vser);` before `break;`. In the DWINIT case, add `vser_reset(&s->vser);` before `tx(s, &r, 1);`.
3. Replace the SERREAD, SERREADM, SERSETSTAT and SERWRITEM cases with:
```c
        case DW_OP_SERREAD: {
            uint8_t r[2];
            vser_serread(&s->vser, r);
            tx(s, r, sizeof(r));
            break;
        }
        case DW_OP_SERREADM: {
            /* buf = [chan, count]; count 0 means 256. Nothing is sent when
             * the channel can't supply that many bytes (DW4 behaviour). */
            uint8_t r[256];
            size_t n = vser_serreadm(&s->vser, s->buf[0], s->buf[1] ? s->buf[1] : 256, r);
            if (n) tx(s, r, n);
            break;
        }
        case DW_OP_SERSETSTAT:
            /* buf = [chan, code]; COMST (0x28) carries 26 more status bytes. */
            if (s->have == 2 && s->buf[1] == 0x28) {
                s->need = 2 + 26;
                return;
            }
            if (s->have == 2 && s->buf[1] == 0x29) vser_open(&s->vser, s->buf[0]);   /* SS.Open */
            if (s->have == 2 && s->buf[1] == 0x2A) vser_close(&s->vser, s->buf[0]);  /* SS.Close */
            break;
        case DW_OP_SERWRITEM:
            /* buf = [chan, count]; count byte 0 means 256 (a full block).
             * 2+256 always fits buf[264]. */
            if (s->have == 2) {
                uint16_t cnt = s->buf[1] ? s->buf[1] : 256;
                s->need = (uint16_t)(2 + cnt);
                return;
            }
            vser_write(&s->vser, s->buf[0], &s->buf[2], (size_t)(s->have - 2));
            break;
```
4. In `default:`, before `break;`:
```c
            if (s->op >= DW_OP_FASTWRITE_BASE && s->op <= DW_OP_FASTWRITE_BASE + 0x0F)
                vser_write(&s->vser, (uint8_t)(s->op - DW_OP_FASTWRITE_BASE), &s->buf[0], 1);
```
5. In `dw_init`, after `s->time_base_ms = 0;`, add `vser_init(&s->vser, NULL, NULL);`, and at the end of the file:
```c
void dw_set_exec(dw_server *s, vser_exec_fn fn, void *ctx) {
    s->vser.exec = fn;
    s->vser.exec_ctx = ctx;
}
```

- [ ] **Step 4: Run the full suite (the existing SER* tests must still pass)**

Run: `ninja -C build-host && ctest --test-dir build-host --output-on-failure`
Expected: all suites pass. `test_dw_server`'s `serread_no_data`, `sersetstat_*` and `serwritem_*` keep passing because no session is open or no line is sent.

- [ ] **Step 5: Commit**

```bash
git add firmware/src/dw/dw.h firmware/src/dw/dw_server.c firmware/tests/test_dw_vser.c
git commit -m "firmware: route DW4 SER* opcodes to the vserial layer"
```

---

### Task 5: `console_exec_remote`: allowlist, output capture, wiring

**Files:**
- Modify: `firmware/src/console/console.c`, `firmware/src/console/console.h`
- Test: `firmware/tests/test_console.c`

**Interfaces:**
- Consumes: `dw_set_exec` (Task 4), the `vser_exec_fn` signature (Task 3).
- Produces: `int console_exec_remote(void *ctx, const char *line, char *out, size_t cap, size_t *outn);`, and a static `g_raw` line pointer plus `raw_tail(int skip)` for Task 6.

- [ ] **Step 1: Write the failing tests** (append to `tests/test_console.c`, and add `RUN(...)` lines)

```c
static char rbuf[4096];
static size_t rn;
static int remote(const char *line) {
    memset(rbuf, 0, sizeof(rbuf));
    rn = 0;
    return console_exec_remote(NULL, line, rbuf, sizeof(rbuf) - 1, &rn);
}

TEST(remote_allowed_commands_run) {
    setup();
    ASSERT_EQ(remote("version"), 0);
    ASSERT(strstr(rbuf, "version"));
    ASSERT(!strstr(rbuf, "ok\n"));          /* no console "ok" terminator */
    ASSERT_EQ(remote("fs ls"), 0);
    ASSERT(strstr(rbuf, "raw.dsk"));
    ASSERT_EQ(remote("dw mount 0 raw.dsk"), 0);
    ASSERT(dw.drives[0].mounted);
    ASSERT_EQ(outn, 0);                     /* nothing leaked to the USB console */
}

TEST(remote_refuses_console_only) {
    setup();
    const char *deny[] = { "smoke", "halt on", "bus drive off", "becker off", "fs rm raw.dsk",
                           "fs format", "fs export", "rom load x.rom", "rom pattern", "trace dump",
                           "crash", "log dw debug", "stats reset", "dw capture off", "dw selftest",
                           "reboot", "bootsel", "frob" };
    for (size_t i = 0; i < sizeof(deny) / sizeof(deny[0]); i++) {
        ASSERT_EQ(remote(deny[i]), 255);
        ASSERT(strcmp(rbuf, "console only") == 0);
    }
    ASSERT_EQ(mode_get(), MODE_DIAG);
}

TEST(remote_error_codes) {
    setup();
    ASSERT_EQ(remote("dw hdbdos"), 10);
    ASSERT(strncmp(rbuf, "usage", 5) == 0);
    ASSERT(!strchr(rbuf, '\n'));
    ASSERT_EQ(remote("dw eject 9"), 101);
    ASSERT(strcmp(rbuf, "bad drive") == 0);
    ASSERT_EQ(remote("dw mount 0 nope.dsk"), 255);
    ASSERT(strcmp(rbuf, "mount failed") == 0);
}

TEST(remote_output_truncates) {
    setup();
    for (int i = 0; i < 200; i++) {
        char n[64];
        snprintf(n, sizeof(n), "file_with_a_long_name_%03d.dsk", i);
        mk(n, 1);
    }
    char small[256];
    size_t sn = 0;
    ASSERT_EQ(console_exec_remote(NULL, "fs ls", small, sizeof(small), &sn), 0);
    ASSERT(sn <= sizeof(small));
    ASSERT_MEMEQ(small + sn - 4, "...\n", 4);
}

TEST(remote_via_vserial_end_to_end) {
    setup();
    uint8_t pkt[] = { 0xC4, 1, 0x29, 0x64, 1, 8, 'v', 'e', 'r', 's', 'i', 'o', 'n', '\r' };
    dw_feed(&dw, pkt, sizeof(pkt), 0);
    ASSERT_EQ(dw.vser.qlen > 23, 1);
    ASSERT_MEMEQ(dw.vser.q + dw.vser.qhead, "OK command successful\n\rversion ", 31);
}
```

- [ ] **Step 2: Run to see them fail**

Run: `ninja -C build-host`
Expected: `console_exec_remote` undeclared.

- [ ] **Step 3: Implement in `console.c`**

1. After `static uint32_t cap_off;` add:
```c
/* The line being executed, untokenised: "dw disk insert" re-joins its tail
 * so image names may contain spaces (DW4 behaviour). */
static const char *g_raw = "";

static const char *raw_tail(int skip) {
    static char tail[128];
    const char *p = g_raw;
    for (int i = 0; i < skip; i++) {
        while (*p == ' ') p++;
        while (*p && *p != ' ') p++;
    }
    while (*p == ' ') p++;
    snprintf(tail, sizeof(tail), "%s", p);
    size_t n = strlen(tail);
    while (n && (tail[n - 1] == ' ' || tail[n - 1] == '\r' || tail[n - 1] == '\n')) tail[--n] = '\0';
    return tail;
}
```
2. Replace the body of `console_exec` with the shared tokenizer:
```c
static int tokenize(char *copy, char **argv) {
    char *save = NULL;
    int argc = 0;
    char *tok = strtok_r(copy, " ", &save);
    while (tok && argc < 6) {
        argv[argc++] = tok;
        tok = strtok_r(NULL, " ", &save);
    }
    return argc;
}

int console_exec(const char *line) {
    char copy[136];
    snprintf(copy, sizeof(copy), "%s", line);
    char *argv[6];
    int argc = tokenize(copy, argv);
    if (argc == 0) return cerr("empty");
    g_raw = line;
    int rc = dispatch(argc, argv);
    if (rc == 0) outf("ok\n");
    return rc;
}
```
3. After `console_exec`, add:
```c
/* DriveWire virtual-serial front end (spec 2026-09-23 §4.3). Deny by
 * default: a new console command is USB-only until it is listed here. */
static const char *const remote_allow[] = {
    "status", "version", "help", "time", "save",
    "fs ls", "fs new", "dw mount", "dw eject", "dw hdbdos", "dw disk", "rom boot",
};

static bool remote_allowed(int argc, char **argv) {
    for (size_t i = 0; i < sizeof(remote_allow) / sizeof(remote_allow[0]); i++) {
        const char *e = remote_allow[i];
        const char *sp = strchr(e, ' ');
        size_t vl = sp ? (size_t)(sp - e) : strlen(e);
        if (strlen(argv[0]) != vl || strncasecmp(argv[0], e, vl) != 0) continue;
        if (!sp) return true;
        if (argc >= 2 && strcasecmp(argv[1], sp + 1) == 0) return true;
    }
    return false;
}

static char *rcap;
static size_t rcap_cap, rcap_len;
static bool rcap_trunc;

static void remote_out(void *ctx, const char *s) {
    (void)ctx;
    size_t l = strlen(s);
    if (rcap_len + l > rcap_cap) {
        l = rcap_cap - rcap_len;
        rcap_trunc = true;
    }
    memcpy(rcap + rcap_len, s, l);
    rcap_len += l;
}

int console_exec_remote(void *ctx, const char *line, char *out, size_t cap, size_t *outn) {
    (void)ctx;
    char copy[136];
    snprintf(copy, sizeof(copy), "%s", line);
    char *argv[6];
    int argc = tokenize(copy, argv);
    if (argc == 0 || !remote_allowed(argc, argv)) {
        *outn = (size_t)snprintf(out, cap, "console only");
        return 255;
    }
    console_out_fn saved = g_out;
    void *saved_ctx = g_out_ctx;
    rcap = out;
    rcap_cap = cap > 5 ? cap - 5 : 0;   /* room for "...\n" and a NUL */
    rcap_len = 0;
    rcap_trunc = false;
    g_out = remote_out;
    g_out_ctx = NULL;
    g_raw = line;
    int rc = dispatch(argc, argv);
    g_out = saved;
    g_out_ctx = saved_ctx;
    out[rcap_len] = '\0';
    if (rc != 0) {
        /* cerr() printed "err <msg>\n"; hand back just <msg>. */
        const char *msg = "failed";
        for (char *p = out; p && *p; ) {
            if (strncmp(p, "err ", 4) == 0) msg = p + 4;
            p = strchr(p, '\n');
            if (p) p++;
        }
        char m[96];
        size_t ml = strcspn(msg, "\n");
        if (ml > 80) ml = 80;
        memcpy(m, msg, ml);
        m[ml] = '\0';
        int code = strncmp(m, "usage", 5) == 0 ? 10 : strcmp(m, "bad drive") == 0 ? 101 : 255;
        *outn = (size_t)snprintf(out, cap, "%s", m);
        return code;
    }
    if (rcap_trunc) {
        memcpy(out + rcap_len, "...\n", 4);
        rcap_len += 4;
    }
    *outn = rcap_len;
    return 0;
}
```
`tokenize` must be defined before `console_exec_remote`, and `dispatch` before both. `dispatch` is already above `console_exec`, so no forward declarations are needed. Include `<strings.h>` for `strncasecmp` if it isn't already included.

4. In `console_init`, after `mode_bind(dw);` add `dw_set_exec(dw, console_exec_remote, NULL);`.

5. In `console.h` add:
```c
/* DriveWire vserial command handler (vser_exec_fn): allowlisted commands only,
 * output captured into out. 0 ok, else DW4 result code with out = message. */
int  console_exec_remote(void *ctx, const char *line, char *out, size_t cap, size_t *outn);
```

- [ ] **Step 4: Run the tests**

Run: `ninja -C build-host && ctest --test-dir build-host --output-on-failure`
Expected: every suite passes, including the 5 new `test_console` tests.

- [ ] **Step 5: Commit**

```bash
git add firmware/src/console/console.c firmware/src/console/console.h firmware/tests/test_console.c
git commit -m "firmware: remote console front end with deny-by-default allowlist"
```

---

### Task 6: `dw disk show|insert|eject` (DW4-compatible)

**Files:**
- Modify: `firmware/src/console/console.c` (`cmd_dw`, new `cmd_dw_disk`)
- Test: `firmware/tests/test_console.c`

**Interfaces:**
- Consumes: `raw_tail()` and `g_raw` (Task 5).
- Produces: console output formats that the CoCo parser (Task 10) reads:
  - `X%-3d%c%s\r\n` per mounted drive (flag `*` for read-only, else a space)
  - `Disk inserted in drive %d.`
  - `Disk ejected from drive %d.\r\n`

- [ ] **Step 1: Write the failing tests**

```c
TEST(disk_show_format) {
    setup();
    ASSERT_EQ(console_exec("dw mount 0 raw.dsk"), 0);
    ASSERT_EQ(console_exec("dw mount 2 raw.dsk ro"), 0);
    outn = 0; out[0] = 0;
    ASSERT_EQ(console_exec("dw disk show"), 0);
    ASSERT(strstr(out, "\r\nCurrent DriveWire disks:\r\n\r\nX0   raw.dsk\r\nX2  *raw.dsk\r\n"));
    outn = 0; out[0] = 0;
    ASSERT_EQ(console_exec("dw disk show 2"), 0);
    ASSERT(strstr(out, "Details for disk in drive #2:\r\n\r\nraw.dsk\r\n"));
    ASSERT_EQ(console_exec("dw disk show 1"), -1);
}

TEST(disk_insert_and_eject) {
    setup();
    ASSERT_EQ(remote("dw disk insert 1 raw.dsk"), 0);
    ASSERT(strcmp(rbuf, "Disk inserted in drive 1.") == 0);
    ASSERT(dw.drives[1].mounted);
    ASSERT_EQ(remote("dw disk eject 1"), 0);
    ASSERT(strcmp(rbuf, "Disk ejected from drive 1.\r\n") == 0);
    ASSERT(!dw.drives[1].mounted);
    ASSERT_EQ(remote("dw disk eject 1"), 255);
    ASSERT(strcmp(rbuf, "drive not loaded") == 0);
    ASSERT_EQ(remote("dw disk insert x raw.dsk"), 101);
    ASSERT_EQ(remote("dw disk insert 4 raw.dsk"), 101);
    ASSERT_EQ(remote("dw disk"), 10);
}

TEST(disk_insert_name_with_spaces) {
    setup();
    mk("my game disk.dsk", 630);
    ASSERT_EQ(remote("dw disk insert 0 my game disk.dsk  "), 0);
    ASSERT(strcmp(dw.drives[0].name, "my game disk.dsk") == 0);
}

TEST(disk_insert_long_name_fails) {
    setup();
    mk("a_really_long_disk_image_name_over_32.dsk", 630);
    ASSERT_EQ(remote("dw disk insert 0 a_really_long_disk_image_name_over_32.dsk"), 255);
    ASSERT(strcmp(rbuf, "mount failed") == 0);
}

TEST(disk_insert_replaces_mounted) {
    setup();
    mk("b.dsk", 630);
    ASSERT_EQ(console_exec("dw mount 0 raw.dsk"), 0);
    ASSERT_EQ(remote("dw disk insert 0 b.dsk"), 0);
    ASSERT(strcmp(dw.drives[0].name, "b.dsk") == 0);
}
```
Add the five `RUN(...)` lines.

- [ ] **Step 2: Run to see them fail**

Run: `ninja -C build-host && ctest --test-dir build-host -R test_console --output-on-failure`
Expected: the new tests FAIL (`dw disk` gets a usage error).

- [ ] **Step 3: Implement**

Add above `cmd_dw`:
```c
/* -1 unless s is a single digit naming a drive. */
static int parse_drive(const char *s) {
    if (!isdigit((unsigned char)s[0]) || s[1] != '\0') return -1;
    int n = s[0] - '0';
    return n < DW_MAX_DRIVES ? n : -1;
}

/* DW4-compatible "dw disk" (spec 2026-09-23 §4.4). Output formats follow DW4
 * DWCmdDiskShow/Insert/Eject so NitrOS-9's dw utility reads them unchanged. */
static int cmd_dw_disk(int argc, char **argv) {
    const char *usage = "usage: dw disk show [n]|insert <n> <file>|eject <n>";
    if (argc < 3) return cerr(usage);
    if (strcasecmp(argv[2], "show") == 0) {
        if (argc >= 4) {
            int n = parse_drive(argv[3]);
            if (n < 0) return cerr("bad drive");
            if (!g_dw->drives[n].mounted) return cerr("drive not loaded");
            outf("Details for disk in drive #%d:\r\n\r\n%s\r\n", n, g_dw->drives[n].name);
            return 0;
        }
        outf("\r\nCurrent DriveWire disks:\r\n\r\n");
        for (int i = 0; i < DW_MAX_DRIVES; i++) {
            dw_disk *d = &g_dw->drives[i];
            if (d->mounted) outf("X%-3d%c%s\r\n", i, d->read_only ? '*' : ' ', d->name);
        }
        return 0;
    }
    if (strcasecmp(argv[2], "insert") == 0) {
        if (argc < 5) return cerr(usage);
        int n = parse_drive(argv[3]);
        if (n < 0) return cerr("bad drive");
        if (dw_mount(g_dw, n, raw_tail(4), false) != 0) return cerr("mount failed");
        outf("Disk inserted in drive %d.", n);
        return 0;
    }
    if (strcasecmp(argv[2], "eject") == 0) {
        if (argc < 4) return cerr(usage);
        int n = parse_drive(argv[3]);
        if (n < 0) return cerr("bad drive");
        if (!g_dw->drives[n].mounted) return cerr("drive not loaded");
        dw_eject(g_dw, n);
        outf("Disk ejected from drive %d.\r\n", n);
        return 0;
    }
    return cerr(usage);
}
```
In `cmd_dw`, after the `selftest` line add `if (strcasecmp(argv[1], "disk") == 0) return cmd_dw_disk(argc, argv);`. Change both of its usage strings to `"usage: dw mount|eject|disk|hdbdos|stats|capture|selftest ..."`. Add `#include <ctype.h>` if it isn't already included.

- [ ] **Step 4: Run the tests**

Run: `ninja -C build-host && ctest --test-dir build-host --output-on-failure`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add firmware/src/console/console.c firmware/tests/test_console.c
git commit -m "firmware: DW4-compatible dw disk show/insert/eject"
```

---

### Task 7: `fs new`, `rom boot`, `rom now/next` in status, version 1.2

**Files:**
- Modify: `firmware/src/console/console.c`, `firmware/src/dev/rom.c`, `firmware/src/dev/rom.h`, `firmware/CMakeLists.txt:3`
- Test: `firmware/tests/test_console.c`

**Interfaces:**
- Produces:
  - `int rom_check_file(dw_store *st, const char *name);`: 0 ok, -1 not found, -2 bad size.
  - `status` lines `rom now <cmd|none>` and `rom next <cmd|none>`, where `<cmd>` is `load NAME` or `pattern`.

- [ ] **Step 1: Write the failing tests**

```c
static uint32_t file_size(const char *name) {
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", g_dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return (uint32_t)n;
}

TEST(fs_new_makes_formatted_rsdos_image) {
    setup();
    ASSERT_EQ(remote("fs new blank.dsk"), 0);
    ASSERT_EQ(file_size("blank.dsk"), 161280);
    char p[512];
    snprintf(p, sizeof(p), "%s/blank.dsk", g_dir);
    FILE *f = fopen(p, "rb");
    static uint8_t img[161280];
    ASSERT_EQ(fread(img, 1, sizeof(img), f), sizeof(img));
    fclose(f);
    for (uint32_t i = 0; i < sizeof(img); i++) {
        uint8_t want = (i >= 0x13300 + 68 && i < 0x13400) ? 0x00 : 0xFF;
        if (img[i] != want) { printf("  byte %u = %02x\n", i, img[i]); ASSERT(0); }
    }
    ASSERT_EQ(remote("dw disk insert 0 blank.dsk"), 0);
}

TEST(fs_new_refuses_existing) {
    setup();
    ASSERT_EQ(remote("fs new raw.dsk"), 255);
    ASSERT(strcmp(rbuf, "file exists") == 0);
    ASSERT_EQ(file_size("raw.dsk"), 630 * 256);
    ASSERT_EQ(remote("fs new"), 10);
    ASSERT_EQ(remote("fs new a/b.dsk"), 255);
}

TEST(rom_boot_records_without_loading) {
    setup();
    static uint8_t rom[8192];
    memset(rom, 0xAB, sizeof(rom));
    char p[512];
    snprintf(p, sizeof(p), "%s/next.rom", g_dir);
    FILE *f = fopen(p, "wb");
    fwrite(rom, 1, sizeof(rom), f);
    fclose(f);
    console_exec("rom off");
    ASSERT_EQ(remote("rom boot next.rom"), 0);
    ASSERT_EQ(bus_table[0x10], 0xFF);                 /* nothing swapped live */
    ASSERT_EQ(remote("status"), 0);
    ASSERT(strstr(rbuf, "rom now none\n"));
    ASSERT(strstr(rbuf, "rom next load next.rom\n"));
    ASSERT_EQ(remote("rom boot raw.dsk"), 255);       /* wrong size */
    ASSERT_EQ(remote("rom boot nope.rom"), 255);
    ASSERT_EQ(console_exec("save"), 0);
    char cfg[1024];
    int n = plat_cfg_read(cfg, sizeof(cfg) - 1);
    ASSERT(n > 0);
    cfg[n] = 0;
    ASSERT(strstr(cfg, "rom load next.rom\n"));
}

TEST(version_is_1_2) {
    setup();
    ASSERT_EQ(remote("version"), 0);
    ASSERT(strcmp(rbuf, "version 1.2\n") == 0);
}
```
Add the `RUN(...)` lines.

- [ ] **Step 2: Run to see them fail**

Run: `ninja -C build-host && ctest --test-dir build-host -R test_console --output-on-failure`
Expected: the four new tests FAIL.

- [ ] **Step 3: Implement**

`firmware/src/dev/rom.h`, add:
```c
int  rom_check_file(dw_store *st, const char *name);  /* 0 ok, -1 not found, -2 size not 8192/16384 */
```
`firmware/src/dev/rom.c`, add:
```c
int rom_check_file(dw_store *st, const char *name) {
    dw_file f;
    if (st->ops->open(st->ctx, name, false, &f) < 0) return -1;
    uint32_t size;
    int r = st->ops->size(&f, &size);
    st->ops->close(&f);
    if (r < 0) return -1;
    return (size == 8192 || size == 16384) ? 0 : -2;
}
```

`console.c`:
1. Next to `static char rom_cmd[64];` add `static char rom_now[64];   /* what is in bus_table now */`. In `console_init` add `rom_now[0] = '\0';`.
2. In `cmd_rom`, set `rom_now` wherever `rom_cmd` is set for `pattern`, `off` and `load` (same string). Then add before the final usage error:
```c
    if (strcasecmp(argv[1], "boot") == 0) {
        /* Next boot only: a live swap would change the DOS under a running CoCo. */
        if (argc < 3) return cerr("usage: rom boot <file>");
        int r = rom_check_file(g_store, argv[2]);
        if (r == -2) return cerr("rom must be 8192 or 16384 bytes");
        if (r != 0) return cerr("rom not found");
        snprintf(rom_cmd, sizeof(rom_cmd), "load %s", argv[2]);
        return 0;
    }
```
   Change both usage strings in `cmd_rom` to `"usage: rom pattern|off|load <file>|boot <file>"`.
3. In `cmd_status`, after the `dw hdbdos` line:
```c
    outf("rom now %s\n", rom_now[0] ? rom_now : "none");
    outf("rom next %s\n", rom_cmd[0] ? rom_cmd : "none");
```
4. `fs new`: above `cmd_fs` add:
```c
#define RSDOS_35T_BYTES (35u * 18u * 256u)         /* 161280 */
#define RSDOS_FAT_OFF   ((17u * 18u + 1u) * 256u)  /* track 17 sector 2 = 0x13300 */

/* A blank 35-track RS-DOS disk as DSKINI leaves it: all $FF, except FAT
 * bytes 68..255 are $00 (68 free granules, the rest unused). */
static int cmd_fs_new(const char *name) {
    if (strchr(name, '/')) return cerr("bad name");
    dw_file f;
    if (g_store->ops->open(g_store->ctx, name, false, &f) >= 0) {
        g_store->ops->close(&f);
        return cerr("file exists");
    }
    if (!g_store->ops->create || g_store->ops->create(g_store->ctx, name, &f) != 0)
        return cerr("create failed");
    static uint8_t blk[4096]; /* static: Pico core0 stack is 4 KB */
    int rc = 0;
    for (uint32_t off = 0; off < RSDOS_35T_BYTES && rc == 0; off += sizeof(blk)) {
        uint32_t n = RSDOS_35T_BYTES - off < sizeof(blk) ? RSDOS_35T_BYTES - off : sizeof(blk);
        memset(blk, 0xFF, n);
        if (off <= RSDOS_FAT_OFF && RSDOS_FAT_OFF < off + n)
            memset(blk + (RSDOS_FAT_OFF - off) + 68, 0x00, 256 - 68);
        if (g_store->ops->write(&f, off, blk, n) != (int)n) rc = -1;
    }
    if (rc == 0 && g_store->ops->sync && g_store->ops->sync(&f) != 0) rc = -1;
    g_store->ops->close(&f);
    if (rc != 0) {
        plat_fs_remove(name);
        return cerr("write failed");
    }
    return 0;
}
```
   In `cmd_fs`, after the `ls` line add:
```c
    if (strcasecmp(argv[1], "new") == 0) {
        if (argc < 3) return cerr("usage: fs new <file>");
        return cmd_fs_new(argv[2]);
    }
```
   Change both usage strings in `cmd_fs` to `"usage: fs ls|new|rm|format|export|import"`.
5. `firmware/CMakeLists.txt:3`: `set(PICOCO_VERSION "1.2")`.

- [ ] **Step 4: Check the blank image against ToolShed**

Run:
```bash
decb dskini "$TMPDIR/ref.dsk" && build-host/test_console >/dev/null
python3 - <<'EOF'
import os
ref = open(os.environ['TMPDIR'] + '/ref.dsk', 'rb').read()
want = bytearray(b'\xff' * 161280); want[0x13300 + 68:0x13400] = bytes(188)
print('match' if ref == bytes(want) else [i for i in range(len(ref)) if ref[i] != want[i]][:10])
EOF
```
Expected: `match`. If ToolShed differs, e.g. in the directory sectors, make `cmd_fs_new` and the test follow ToolShed, which mirrors real `DSKINI`.

- [ ] **Step 5: Run all tests**

Run: `ninja -C build-host && ctest --test-dir build-host --output-on-failure`
Expected: all pass.

- [ ] **Step 6: Commit**

```bash
git add firmware/src/console/console.c firmware/src/dev/rom.c firmware/src/dev/rom.h firmware/CMakeLists.txt firmware/tests/test_console.c
git commit -m "firmware: fs new, rom boot, rom now/next in status; version 1.2"
```

---

### Task 8: Clock persistence (bench spike + `plat_rtc`)

**Files:**
- Modify: `firmware/src/plat.h`, `firmware/host/plat_host.c`, `firmware/src/plat_pico.c`, `firmware/src/main.c`, `firmware/src/console/console.c` (`cmd_time`), `firmware/CMakeLists.txt` (link `pico_aon_timer`)
- Test: `firmware/tests/test_console.c`

**Interfaces:**
- Produces:
  - `bool plat_rtc_get(int64_t *unix_secs);`: true if a reset-surviving clock is running.
  - `void plat_rtc_set(int64_t unix_secs);`
  - `time` prints `time N` and then `clock kept|lost`.

- [ ] **Step 1: Failing host test**

```c
TEST(time_reports_clock_lost_on_host) {
    setup();
    ASSERT_EQ(remote("time"), 0);
    ASSERT(strstr(rbuf, "clock lost\n"));
}
```

- [ ] **Step 2: Implement the API with the host stub**

`plat.h`:
```c
bool plat_rtc_get(int64_t *unix_secs);   /* true if a clock that survives a CoCo reset is running; host: false */
void plat_rtc_set(int64_t unix_secs);    /* host: no-op */
```
`host/plat_host.c`:
```c
bool plat_rtc_get(int64_t *unix_secs) { (void)unix_secs; return false; }
void plat_rtc_set(int64_t unix_secs) { (void)unix_secs; }
```
`console.c` `cmd_time`:
```c
static int cmd_time(int argc, char **argv) {
    if (argc >= 2 && strcasecmp(argv[1], "set") == 0) {
        if (argc < 3) return cerr("usage: time set <unix>");
        int64_t t = atoll(argv[2]);
        dw_time_set(g_dw, t, plat_now_ms());
        plat_rtc_set(t);
        return 0;
    }
    int64_t dummy;
    outf("time %lld\n", (long long)dw_time_get(g_dw, plat_now_ms()));
    outf("clock %s\n", plat_rtc_get(&dummy) ? "kept" : "lost");
    return 0;
}
```
`src/plat_pico.c`, first as stubs identical to the host ones. `src/main.c`, after `console_run_config` has run:
```c
    int64_t rtc;
    if (plat_rtc_get(&rtc)) dw_time_set(&g_dw, rtc, plat_now_ms());
```
Run: `ninja -C build-host && ctest --test-dir build-host --output-on-failure`. Expected: all pass.

- [ ] **Step 3: Bench spike: does the AON timer survive a CoCo reset? (needs the user at the bench)**

Temporarily replace the Pico stubs with the AON implementation:
```c
#include "pico/aon_timer.h"
bool plat_rtc_get(int64_t *unix_secs) {
    struct timespec ts;
    if (!aon_timer_is_running() || !aon_timer_get_time(&ts)) return false;
    *unix_secs = ts.tv_sec;
    return true;
}
void plat_rtc_set(int64_t unix_secs) {
    struct timespec ts = { .tv_sec = (time_t)unix_secs, .tv_nsec = 0 };
    if (aon_timer_is_running()) aon_timer_set_time(&ts);
    else aon_timer_start(&ts);
}
```
Add `pico_aon_timer` to the `target_link_libraries(picoco ...)` list. Build and flash per `firmware/README.md`. Then on the console:
1. `time set 1790000000`, then `time` → expect about 1790000000 and `clock kept`.
2. Press the CoCo's reset button. The Pico reboots.
3. `time` → **pass** if the value is about 1790000000 plus the elapsed seconds and shows `clock kept`; **fail** if it shows the 2026-01-01 default or `clock lost`.
4. Power-cycle: expected `clock lost`. This is only a note, not a pass condition.

- [ ] **Step 4: Keep or revert**

- Pass: keep the AON implementation.
- Fail: restore the stubs in `plat_pico.c`, remove `pico_aon_timer`, and add a comment `/* ponytail: AON timer does not survive a RUN-pin reset (bench, <date>); clock is lost at every CoCo reset until WiFi/NTP. */`.

Either way, record the result in spec §4.5.

- [ ] **Step 5: Commit**

```bash
git add firmware/src/plat.h firmware/host/plat_host.c firmware/src/plat_pico.c firmware/src/main.c firmware/src/console/console.c firmware/CMakeLists.txt firmware/tests/test_console.c docs/superpowers/specs/2026-09-23-coco-manager-design.md
git commit -m "firmware: clock kept/lost report; AON timer per bench spike"
```

---

### Task 9: `dwtest.py` virtual-serial session against `picoco-host`

**Files:**
- Modify: `firmware/tools/dwtest.py`

- [ ] **Step 1: Add the client method and check**

In `class DW`:
```python
    def vcmd(self, line, ch=1):
        """DW4 virtual-serial command: SS.Open, SERWRITEM, poll until hangup."""
        self.s.sendall(bytes([0xC4, ch, 0x29]))
        data = (line + '\r').encode()
        self.s.sendall(bytes([0x64, ch, len(data)]) + data)
        out = b''
        for _ in range(10000):
            self.s.sendall(b'\x43')
            b1, b2 = self.recv(2)
            if b1 == 0x10 and b2 == ch:
                return out
            if b1 == ch + 1:
                out += bytes([b2])
            elif b1 == ch + 17:
                self.s.sendall(bytes([0x63, ch, b2]))
                out += self.recv(b2)
        raise RuntimeError('no hangup')
```
Module level:
```python
def check_vserial(dw):
    r = dw.vcmd('version')
    if not (r.startswith(b'OK command successful\n\r') and b'version ' in r):
        return False
    r = dw.vcmd('dw disk show')
    if not r.startswith(b'OK command successful\n\r\r\nCurrent DriveWire disks:'):
        return False
    return dw.vcmd('smoke').startswith(b'FAIL 255 console only\n\r')
```
In `main()`, after check 9: `run(10, 'vserial command session', lambda: check_vserial(dw))`.

- [ ] **Step 2: Run it**

```bash
ninja -C build-host
mkdir -p "$TMPDIR/dwt" && python3 -c "open('$TMPDIR/dwt/t.dsk','wb').write(bytes(630*256))"
build-host/picoco-host --dir "$TMPDIR/dwt" --mount 0=t.dsk --hdbdos off &
python3 firmware/tools/dwtest.py --image "$TMPDIR/dwt/t.dsk"
kill %1
```
Expected: checks (1) to (10) print `ok`. Local port binding may need the sandbox off.

- [ ] **Step 3: Commit**

```bash
git add firmware/tools/dwtest.py
git commit -m "dwtest: DW4 virtual-serial command session check"
```

---

### Task 10: CoCo `parse.c` with host tests

**Files:**
- Create: `coco/parse.h`, `coco/parse.c`, `coco/test_parse.c`
- Modify: `coco/Makefile` (the `test` target)

**Interfaces:**
- Produces (used by Tasks 11-14):
  ```c
  typedef struct { char *name; u16 kb; } file_ent;
  typedef struct { char name[13]; u8 type; } rs_ent;
  int  parse_reply(char *buf, char **body);
  int  parse_ls(char *text, file_ent *out, int max, int roms);
  void sort_files(file_ent *f, int n);
  void parse_disks(const char *text, char names[4][32]);
  int  line_value(const char *text, const char *key, char *out, int cap);
  u32  dec_to_u32(const char *s);
  void u32_to_dec(u32 v, char *out);
  u32  civil_to_unix(int y, int mo, int d, int h, int mi);
  void unix_to_civil(u32 t, int *y, int *mo, int *d, int *h, int *mi);
  int  parse_datetime(const char *s, u32 *out);
  int  is_os9_boot(const u8 *sec);
  int  rsdos_dir(const u8 *sec, rs_ent *out, int max, int *end);
  int  ends_with_ci(const char *s, const char *suffix);
  ```

- [ ] **Step 1: Write `coco/parse.h`**

```c
#ifndef PARSE_H
#define PARSE_H
/* Pure-C reply parsing shared by the CoCo program (CMOC) and the host test.
 * C89 style: CMOC wants declarations at the top of a block. */
#ifdef _CMOC_VERSION_
#include <cmoc.h>
typedef unsigned char u8;
typedef unsigned int u16;
typedef unsigned long u32;
#else
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#endif

#define MAX_FILES 128

typedef struct { char *name; u16 kb; } file_ent;  /* name points into the ls text */
typedef struct { char name[13]; u8 type; } rs_ent; /* "NAME.EXT"; type 0 BASIC, 2 ML */

/* Firmware reply "OK ...\n\r<body>" -> 0, body; "FAIL nnn msg\n\r" -> nnn,
 * body = msg (terminated in place); anything else -> -1. */
int  parse_reply(char *buf, char **body);
/* "name size" lines, split in place. roms=1 keeps *.ROM only; roms=0 hides
 * *.ROM and picoco.cfg. Returns entries stored. */
int  parse_ls(char *text, file_ent *out, int max, int roms);
void sort_files(file_ent *f, int n);
/* "dw disk show" -> names[d] per drive, "" when empty. */
void parse_disks(const char *text, char names[4][32]);
/* 1 if a line starts with key; copies the rest of that line into out. */
int  line_value(const char *text, const char *key, char *out, int cap);
u32  dec_to_u32(const char *s);
void u32_to_dec(u32 v, char *out);
u32  civil_to_unix(int y, int mo, int d, int h, int mi);
void unix_to_civil(u32 t, int *y, int *mo, int *d, int *h, int *mi);
int  parse_datetime(const char *s, u32 *out);      /* "YYYY-MM-DD HH:MM": 0 ok, -1 bad */
int  is_os9_boot(const u8 *sec);                   /* track 34 sector 1 starts "OS" */
/* One RS-DOS directory sector (8 entries). Adds BASIC/ML entries to out (at
 * most max); sets *end when the $FF end marker is seen. Returns entries added. */
int  rsdos_dir(const u8 *sec, rs_ent *out, int max, int *end);
int  ends_with_ci(const char *s, const char *suffix);
#endif
```

- [ ] **Step 2: Write the failing host tests, `coco/test_parse.c`**

```c
#include "parse.h"
#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void t_reply(void) {
    char ok[] = "OK command successful\n\rversion 1.2\n";
    char fail[] = "FAIL 101 bad drive\n\r";
    char junk[] = "hello";
    char *b;
    CHECK(parse_reply(ok, &b) == 0 && strcmp(b, "version 1.2\n") == 0);
    CHECK(parse_reply(fail, &b) == 101 && strcmp(b, "bad drive") == 0);
    CHECK(parse_reply(junk, &b) == -1);
}

static void t_ls(void) {
    char text[] = "zaxxon.dsk 161280\nHDBDW3BC3.ROM 8192\npicoco.cfg 120\nmy game.dsk 1024\nbad\n";
    file_ent f[8];
    int n = parse_ls(text, f, 8, 0);
    CHECK(n == 2);
    sort_files(f, n);
    CHECK(strcmp(f[0].name, "my game.dsk") == 0 && f[0].kb == 1);
    CHECK(strcmp(f[1].name, "zaxxon.dsk") == 0 && f[1].kb == 158);
    {
        char t2[] = "zaxxon.dsk 161280\nHDBDW3BC3.ROM 8192\n";
        CHECK(parse_ls(t2, f, 8, 1) == 1 && strcmp(f[0].name, "HDBDW3BC3.ROM") == 0);
    }
}

static void t_ls_truncated_tail(void) {
    char text[] = "a.dsk 100\nb.dsk 2\n...\n";
    file_ent f[8];
    CHECK(parse_ls(text, f, 8, 0) == 2);
    {
        char t3[] = "a.dsk 100\nb.dsk 2\nc.dsk 3\n";
        CHECK(parse_ls(t3, f, 2, 0) == 2);   /* max respected */
    }
}

static void t_disks(void) {
    const char *t = "\r\nCurrent DriveWire disks:\r\n\r\nX0   raw.dsk\r\nX3  *nitros9 boot.dsk\r\n";
    char d[4][32];
    parse_disks(t, d);
    CHECK(strcmp(d[0], "raw.dsk") == 0);
    CHECK(d[1][0] == 0 && d[2][0] == 0);
    CHECK(strcmp(d[3], "nitros9 boot.dsk") == 0);
}

static void t_line_value(void) {
    const char *t = "mode native\nrom now load hdbdw3bc3.rom\nrom next none\ndw hdbdos on\n";
    char v[40];
    CHECK(line_value(t, "rom now ", v, sizeof v) && strcmp(v, "load hdbdw3bc3.rom") == 0);
    CHECK(line_value(t, "dw hdbdos ", v, sizeof v) && strcmp(v, "on") == 0);
    CHECK(!line_value(t, "nope ", v, sizeof v));
}

static void t_time(void) {
    u32 t;
    int y, mo, d, h, mi;
    char s[12];
    CHECK(civil_to_unix(2026, 1, 1, 0, 0) == 1767225600UL);
    CHECK(civil_to_unix(2024, 2, 29, 23, 59) == 1709251140UL);
    unix_to_civil(1709251140UL, &y, &mo, &d, &h, &mi);
    CHECK(y == 2024 && mo == 2 && d == 29 && h == 23 && mi == 59);
    CHECK(parse_datetime("2026-09-23 14:02", &t) == 0 && t == 1790172120UL);
    CHECK(parse_datetime("2026-02-29 00:00", &t) == -1);
    CHECK(parse_datetime("2026-9-23 14:02", &t) == -1);
    CHECK(parse_datetime("2026-09-23 24:00", &t) == -1);
    u32_to_dec(1790172120UL, s);
    CHECK(strcmp(s, "1790172120") == 0);
    CHECK(dec_to_u32("1790172120") == 1790172120UL);
    u32_to_dec(0, s);
    CHECK(strcmp(s, "0") == 0);
}

static void t_disk_detect(void) {
    u8 sec[256];
    rs_ent e[16];
    int end = 0;
    memset(sec, 0, sizeof sec);
    sec[0] = 'O'; sec[1] = 'S';
    CHECK(is_os9_boot(sec));
    memset(sec, 0xFF, sizeof sec);
    memcpy(sec, "HELLO   BAS", 11); sec[11] = 0;
    memcpy(sec + 32, "GAME    BIN", 11); sec[43] = 2;
    memcpy(sec + 64, "DATA    DAT", 11); sec[75] = 1;
    sec[96] = 0;                                   /* killed entry */
    CHECK(rsdos_dir(sec, e, 16, &end) == 2);
    CHECK(end == 1);
    CHECK(strcmp(e[0].name, "HELLO.BAS") == 0 && e[0].type == 0);
    CHECK(strcmp(e[1].name, "GAME.BIN") == 0 && e[1].type == 2);
}

int main(void) {
    t_reply(); t_ls(); t_ls_truncated_tail(); t_disks(); t_line_value(); t_time(); t_disk_detect();
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}
```
Update the `test` target in `coco/Makefile`:
```make
test: test_parse
	./test_parse

test_parse: test_parse.c parse.c parse.h
	cc -std=c99 -Wall -Wextra -fsanitize=address,undefined -o $@ test_parse.c parse.c
```
Run: `make -C coco test`. Expected: a link failure (no `parse.c`).

- [ ] **Step 3: Write `coco/parse.c`**

```c
#include "parse.h"

static const u16 mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

static int leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static char upc(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static int cmp_ci(const char *a, const char *b)
{
    while (*a && upc(*a) == upc(*b)) { a++; b++; }
    return (int)(u8)upc(*a) - (int)(u8)upc(*b);
}

int ends_with_ci(const char *s, const char *suf)
{
    u16 a = (u16)strlen(s), b = (u16)strlen(suf);
    if (b > a) return 0;
    return cmp_ci(s + a - b, suf) == 0;
}

int parse_reply(char *buf, char **body)
{
    char *p;
    int code = 0, i;
    if (buf[0] == 'O' && buf[1] == 'K') {
        p = buf;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        if (*p == '\r') p++;
        *body = p;
        return 0;
    }
    if (strncmp(buf, "FAIL ", 5) != 0) return -1;
    for (i = 5; i < 8; i++) {
        if (buf[i] < '0' || buf[i] > '9') return -1;
        code = code * 10 + (buf[i] - '0');
    }
    p = buf + 8;
    if (*p == ' ') p++;
    *body = p;
    while (*p && *p != '\n' && *p != '\r') p++;
    *p = '\0';
    return code ? code : 255;
}

u32 dec_to_u32(const char *s)
{
    u32 v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10UL + (u32)(*s++ - '0');
    return v;
}

void u32_to_dec(u32 v, char *out)
{
    char tmp[11];
    int n = 0;
    do { tmp[n++] = (char)('0' + (int)(v % 10UL)); v /= 10UL; } while (v);
    while (n) *out++ = tmp[--n];
    *out = '\0';
}

int parse_ls(char *text, file_ent *out, int max, int roms)
{
    int n = 0, keep;
    char *p = text, *line, *end, *sp, *q;
    u32 size;
    while (*p && n < max) {
        line = p;
        while (*p && *p != '\n' && *p != '\r') p++;
        end = p;
        while (*p == '\n' || *p == '\r') { *p = '\0'; p++; }
        sp = 0;
        for (q = line; q < end; q++) if (*q == ' ') sp = q;
        if (!sp || sp == line || sp[1] < '0' || sp[1] > '9') continue;
        *sp = '\0';
        size = dec_to_u32(sp + 1);
        if (roms) keep = ends_with_ci(line, ".ROM");
        else keep = !ends_with_ci(line, ".ROM") && cmp_ci(line, "picoco.cfg") != 0;
        if (!keep) continue;
        out[n].name = line;
        out[n].kb = (u16)((size + 1023UL) / 1024UL);
        n++;
    }
    return n;
}

void sort_files(file_ent *f, int n)
{
    int i, j;
    file_ent t;
    for (i = 1; i < n; i++) {
        t = f[i];
        for (j = i; j > 0 && cmp_ci(f[j - 1].name, t.name) > 0; j--) f[j] = f[j - 1];
        f[j] = t;
    }
}

void parse_disks(const char *text, char names[4][32])
{
    int i, d, k;
    const char *p = text;
    for (i = 0; i < 4; i++) names[i][0] = '\0';
    while (*p) {
        if (p[0] == 'X' && p[1] >= '0' && p[1] <= '3') {
            d = p[1] - '0';
            for (k = 0; k < 5 && *p; k++) p++;   /* "X%-3d" then the '*'/' ' flag */
            i = 0;
            while (*p && *p != '\r' && *p != '\n' && i < 31) names[d][i++] = *p++;
            names[d][i] = '\0';
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
}

int line_value(const char *text, const char *key, char *out, int cap)
{
    u16 kl = (u16)strlen(key);
    const char *p = text;
    int i;
    while (*p) {
        if (strncmp(p, key, kl) == 0) {
            p += kl;
            i = 0;
            while (*p && *p != '\n' && *p != '\r' && i < cap - 1) out[i++] = *p++;
            out[i] = '\0';
            return 1;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return 0;
}

u32 civil_to_unix(int y, int mo, int d, int h, int mi)
{
    u32 days = 0;
    int i;
    for (i = 1970; i < y; i++) days += leap(i) ? 366UL : 365UL;
    for (i = 1; i < mo; i++) days += mdays[i - 1] + (u16)(i == 2 && leap(y));
    days += (u32)(d - 1);
    return days * 86400UL + (u32)h * 3600UL + (u32)mi * 60UL;
}

void unix_to_civil(u32 t, int *y, int *mo, int *d, int *h, int *mi)
{
    u32 days = t / 86400UL, rem = t % 86400UL;
    u16 len;
    int yy = 1970, m = 1;
    *h = (int)(rem / 3600UL);
    *mi = (int)((rem % 3600UL) / 60UL);
    for (;;) { len = leap(yy) ? 366 : 365; if (days < len) break; days -= len; yy++; }
    for (;;) { len = mdays[m - 1] + (u16)(m == 2 && leap(yy)); if (days < len) break; days -= len; m++; }
    *y = yy; *mo = m; *d = (int)days + 1;
}

static int num(const char *s, int n, int *out)
{
    int v = 0, i;
    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return 0;
}

int parse_datetime(const char *s, u32 *out)
{
    int y, mo, d, h, mi;
    if (strlen(s) != 16 || s[4] != '-' || s[7] != '-' || s[10] != ' ' || s[13] != ':') return -1;
    if (num(s, 4, &y) || num(s + 5, 2, &mo) || num(s + 8, 2, &d) || num(s + 11, 2, &h) || num(s + 14, 2, &mi))
        return -1;
    if (y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || h > 23 || mi > 59) return -1;
    if (d > (int)(mdays[mo - 1] + (mo == 2 && leap(y)))) return -1;
    *out = civil_to_unix(y, mo, d, h, mi);
    return 0;
}

int is_os9_boot(const u8 *sec) { return sec[0] == 'O' && sec[1] == 'S'; }

int rsdos_dir(const u8 *sec, rs_ent *out, int max, int *end)
{
    int n = 0, e, i, k;
    const u8 *d;
    for (e = 0; e < 8; e++) {
        d = sec + e * 32;
        if (d[0] == 0xFF) { *end = 1; return n; }
        if (d[0] == 0x00) continue;                 /* killed */
        if (d[11] != 0 && d[11] != 2) continue;     /* BASIC and ML only */
        if (n >= max) return n;
        k = 0;
        for (i = 0; i < 8 && d[i] != ' '; i++) out[n].name[k++] = (char)d[i];
        out[n].name[k++] = '.';
        for (i = 8; i < 11 && d[i] != ' '; i++) out[n].name[k++] = (char)d[i];
        out[n].name[k] = '\0';
        out[n].type = d[11];
        n++;
    }
    return n;
}
```
The `rsdos_dir` test expects `end == 1` because the entries after offset 96 are all `$FF`.

- [ ] **Step 4: Run the host tests**

Run: `make -C coco test`
Expected: `all passed`.

- [ ] **Step 5: Confirm CMOC accepts it**

Run: `cd coco && cmoc --coco -c parse.c`
Expected: no errors. If CMOC lacks `strncmp` or `strlen` in `<cmoc.h>`, add a static helper to `parse.c` and rerun both builds.

- [ ] **Step 6: Commit**

```bash
git add coco/parse.h coco/parse.c coco/test_parse.c coco/Makefile
git commit -m "coco: reply/listing/date/directory parsing with host tests"
```

---

### Task 11: CoCo `dw.c` (Becker client) with a smoke test in XRoar

**Files:**
- Create: `coco/dw.h`, `coco/dw.c`
- Modify: `coco/main.c` (smoke), `coco/Makefile` (`SRCS = main.c dw.c parse.c`)

**Interfaces:**
- Consumes: `parse_reply` (Task 10).
- Produces:
  ```c
  #define PC_BAD     -1
  #define PC_TOOLONG -2
  #define PC_TIMEOUT -3
  int picoco_cmd(const char *line, char *buf, u16 cap, char **body);  /* 0 OK, >0 FAIL code, <0 PC_* */
  int dw_read_sector(u8 drive, u32 lsn, u8 *sec);                     /* 0 ok, DW error code, or PC_TIMEOUT */
  ```

- [ ] **Step 1: Write `coco/dw.h`**

```c
#ifndef DW_H
#define DW_H
#include "parse.h"

#define PC_BAD     -1   /* reply was neither OK nor FAIL */
#define PC_TOOLONG -2
#define PC_TIMEOUT -3

/* Runs one PiCoCo console command over DW4 virtual serial channel 1.
 * Returns 0 (body = output) or the FAIL code (body = message), or PC_*. */
int picoco_cmd(const char *line, char *buf, u16 cap, char **body);
/* OP_READ of one 256-byte sector. 0 ok, DW error code, or PC_TIMEOUT. */
int dw_read_sector(u8 drive, u32 lsn, u8 *sec);
#endif
```

- [ ] **Step 2: Write `coco/dw.c`**

```c
#include "dw.h"

/* Becker port. ponytail: fixed address; probe at runtime once the SDC
 * profile moves Becker to $FF5x (spec §9). */
#define BSTAT (*(volatile u8 *)0xFF41)
#define BDATA (*(volatile u8 *)0xFF42)
#define TICKS (*(volatile u16 *)0x0112)   /* Extended BASIC 60 Hz TIMER */
#define CH 1
#define FIRST_REPLY_TICKS 600             /* 10 s: fs new writes 161 KB of flash */
#define BYTE_TICKS 60

static void put(u8 b) { BDATA = b; }

static int get(u16 ticks)
{
    u16 t0 = TICKS;
    while (!(BSTAT & 0x02))
        if ((u16)(TICKS - t0) > ticks) return -1;
    return BDATA;
}

static void drain(void)
{
    while (BSTAT & 0x02) (void)BDATA;
}

int picoco_cmd(const char *line, char *buf, u16 cap, char **body)
{
    u16 n = (u16)strlen(line), got = 0, i, t0;
    int b1, b2, c;
    if (n > 120) return PC_TOOLONG;
    drain();
    put(0xC4); put(CH); put(0x29);                  /* SERSETSTAT SS.Open */
    put(0x64); put(CH); put((u8)(n + 1));           /* SERWRITEM line + CR */
    for (i = 0; i < n; i++) put((u8)line[i]);
    put(13);
    t0 = TICKS;
    for (;;) {
        put(0x43);                                  /* SERREAD */
        b1 = get(FIRST_REPLY_TICKS);
        if (b1 < 0) return PC_TIMEOUT;
        b2 = get(BYTE_TICKS);
        if (b2 < 0) return PC_TIMEOUT;
        if (b1 == 0x10 && b2 == CH) break;          /* hangup: reply complete */
        if (b1 == CH + 1) {
            if (got < cap - 1) buf[got++] = (char)b2;
        } else if (b1 == CH + 17) {
            put(0x63); put(CH); put((u8)b2);        /* SERREADM */
            for (i = 0; i < (u16)b2; i++) {
                c = get(BYTE_TICKS);
                if (c < 0) return PC_TIMEOUT;
                if (got < cap - 1) buf[got++] = (char)c;
            }
        } else if ((u16)(TICKS - t0) > FIRST_REPLY_TICKS) {
            return PC_TIMEOUT;
        }
    }
    buf[got] = '\0';
    return parse_reply(buf, body);
}

int dw_read_sector(u8 drive, u32 lsn, u8 *sec)
{
    int c, i;
    u16 sum = 0, want;
    drain();
    put(0x52); put(drive);
    put((u8)(lsn >> 16)); put((u8)(lsn >> 8)); put((u8)lsn);
    c = get(300); if (c < 0) return PC_TIMEOUT;
    if (c != 0) return c;
    c = get(BYTE_TICKS); if (c < 0) return PC_TIMEOUT;
    want = (u16)((u16)c << 8);
    c = get(BYTE_TICKS); if (c < 0) return PC_TIMEOUT;
    want |= (u16)c;
    for (i = 0; i < 256; i++) {
        c = get(BYTE_TICKS);
        if (c < 0) return PC_TIMEOUT;
        sec[i] = (u8)c;
        sum += (u8)c;
    }
    return sum == want ? 0 : 0xF3;   /* E_CRC */
}
```

- [ ] **Step 3: Smoke `main.c`**

```c
#include <cmoc.h>
#include "dw.h"

static char buf[1024];
static u8 sec[256];

int main(void)
{
    char *body;
    int rc = picoco_cmd("version", buf, sizeof buf, &body);
    printf("VERSION RC %d: %s\n", rc, rc >= 0 ? body : "");
    rc = picoco_cmd("dw disk show", buf, sizeof buf, &body);
    printf("DISKS RC %d: %s\n", rc, rc >= 0 ? body : "");
    rc = picoco_cmd("smoke", buf, sizeof buf, &body);
    printf("SMOKE RC %d: %s\n", rc, rc > 0 ? body : "");
    rc = dw_read_sector(0, 0, sec);
    printf("READ RC %d\n", rc);
    return 0;
}
```
Set `SRCS = main.c dw.c parse.c` in the Makefile.

- [ ] **Step 4: Run it in XRoar against a Task-7 `picoco-host`** (setup from Task 1 Step 7)

Expected on screen:
```
VERSION RC 0: VERSION 1.2
DISKS RC 0: ... X0   PICOCO.DSK
SMOKE RC 255: CONSOLE ONLY
READ RC 0
```
(Text is uppercase on a CoCo 1/2.) Repeat on the CoCo 3 machine.

- [ ] **Step 5: Commit**

```bash
git add coco/dw.h coco/dw.c coco/main.c coco/Makefile
git commit -m "coco: Becker DriveWire client and vserial command call"
```

---

### Task 12: CoCo main screen (`ui.c`)

**Files:**
- Create: `coco/ui.h`, `coco/ui.c`, `coco/boot.h` (a stub is fine until Task 14)
- Modify: `coco/main.c` (calls `ui_run()`), `coco/Makefile` (`SRCS = main.c ui.c dw.c parse.c boot.c`)

**Interfaces:**
- Consumes: `picoco_cmd`, `parse_ls`, `sort_files`, `parse_disks`, `line_value`.
- Produces (Tasks 13 and 14 rely on these):
  ```c
  void ui_run(void);
  u8   key(void);
  void cls(void);
  void put_at(u8 row, u8 col, const char *s, u8 inv);
  void clear_row(u8 row);
  void msg(const char *s);                               /* inverse on row 14, waits for a key */
  int  ui_cmd(const char *line, char **body);             /* picoco_cmd into the shared reply buffer; shows errors */
  int  input_line(const char *prompt, char *buf, int max);/* row 14; -1 on BREAK */
  int  pick_list(const char *title, file_ent *e, int n);  /* full screen; index or -1 */
  extern u8 ui_dirty;                                     /* changed since last save */
  ```
- `boot.h` declares `int boot_image(const char *name);`: 1 means the program should exit to BASIC (handoff armed); 0 means return to the main screen.

- [ ] **Step 1: Check the CMOC key API**

Run: `grep -n "inkey\|waitkey" <cmoc source>/doc/cmoc-manual.markdown <cmoc source>/src/stdlib/coco.h`
Expected: `inkey()` exists and returns 0 when no key is waiting. If it doesn't, write `key()` below with an `asm { jsr [$A000] ... }` POLCAT call, per the manual's inline-asm section.

- [ ] **Step 2: Write `coco/ui.h`**

```c
#ifndef UI_H
#define UI_H
#include "parse.h"

extern u8 ui_dirty;
void ui_run(void);
u8   key(void);
void cls(void);
void clear_row(u8 row);
void put_at(u8 row, u8 col, const char *s, u8 inv);
void msg(const char *s);
int  ui_cmd(const char *line, char **body);
int  input_line(const char *prompt, char *buf, int max);
int  pick_list(const char *title, file_ent *e, int n);
void settings_run(void);   /* Task 13 */
#endif
```

- [ ] **Step 3: Write `coco/ui.c`**

```c
#include <coco.h>
#include "ui.h"
#include "dw.h"
#include "boot.h"

#define VRAM ((u8 *)0x0400)
#define COLS 32
#define LIST_TOP 4
#define LIST_ROWS 9
#define MSG_ROW 14
#define TICKS (*(volatile u16 *)0x0112)

u8 ui_dirty;
static char reply[768];
static char lsbuf[4096];
static file_ent files[MAX_FILES];
static int nfiles, sel, top;
static char drives[4][32];
static char fw[12];
static char line[160];

/* VDG text: bit 6 set = normal (dark on green), clear = inverse; lowercase
 * shows as inverse uppercase on a CoCo 1/2, so fold to uppercase. */
static u8 vdg(char c, u8 inv)
{
    u8 u = (u8)c;
    if (u >= 'a' && u <= 'z') u = (u8)(u - 32);
    if (u < 0x20 || u > 0x5F) u = '.';
    u &= 0x3F;
    return inv ? u : (u8)(u | 0x40);
}

void clear_row(u8 row) { memset(VRAM + (u16)row * COLS, 0x60, COLS); }
void cls(void) { memset(VRAM, 0x60, 512); }

void put_at(u8 row, u8 col, const char *s, u8 inv)
{
    u8 *p = VRAM + (u16)row * COLS + col;
    while (*s && col < COLS) { *p++ = vdg(*s++, inv); col++; }
}

u8 key(void)
{
    u8 k;
    while ((k = inkey()) == 0) ;
    return k;
}

void msg(const char *s)
{
    clear_row(MSG_ROW);
    put_at(MSG_ROW, 0, s, 1);
    key();
}

int ui_cmd(const char *l, char **body)
{
    int rc = picoco_cmd(l, reply, sizeof reply, body);
    if (rc == PC_TIMEOUT) msg("PICOCO NOT RESPONDING");
    else if (rc == PC_BAD) msg("BAD REPLY (FIRMWARE >= 1.2?)");
    else if (rc == PC_TOOLONG) msg("NAME TOO LONG");
    else if (rc > 0) msg(*body);
    return rc;
}

int input_line(const char *prompt, char *buf, int max)
{
    int n = 0;
    u8 k, col = (u8)strlen(prompt);
    clear_row(MSG_ROW); clear_row(MSG_ROW + 1);
    put_at(MSG_ROW, 0, prompt, 0);
    for (;;) {
        buf[n] = '\0';
        memset(VRAM + MSG_ROW * COLS + col, 0x60, COLS - col);
        put_at(MSG_ROW, col, buf, 0);
        if (col + n < COLS) VRAM[MSG_ROW * COLS + col + n] = 0x20;   /* inverse-space cursor */
        k = key();
        if (k == 13) return n;
        if (k == 3) return -1;
        if (k == 8) { if (n) n--; continue; }
        if (k >= 32 && k < 127 && n < max && col + n < COLS - 1) buf[n++] = (char)k;
    }
}

static void draw_row(u8 row, file_ent *e, u8 inv)
{
    char kb[8];
    clear_row(row);
    if (inv) memset(VRAM + (u16)row * COLS, 0x20, COLS);
    put_at(row, 1, e->name, inv);
    if (e->kb) {
        u32_to_dec(e->kb, kb);
        strcat(kb, "K");
        put_at(row, (u8)(COLS - strlen(kb)), kb, inv);
    }
    VRAM[(u16)row * COLS + 26] = inv ? 0x20 : 0x60;   /* keep a gap before the size */
}

static void draw_list(file_ent *e, int n, int s, int t, u8 row0, u8 rows)
{
    u8 r;
    for (r = 0; r < rows; r++) {
        if (t + r < n) draw_row((u8)(row0 + r), &e[t + r], (u8)(t + r == s));
        else clear_row((u8)(row0 + r));
    }
}

static int clamp_top(int s, int t, int rows)
{
    if (s < t) return s;
    if (s >= t + rows) return s - rows + 1;
    return t;
}

int pick_list(const char *title, file_ent *e, int n)
{
    int s = 0, t = 0;
    u8 k;
    cls();
    put_at(0, 0, title, 0);
    put_at(15, 0, "ENTER:PICK  BREAK:CANCEL", 0);
    for (;;) {
        t = clamp_top(s, t, 12);
        draw_list(e, n, s, t, 2, 12);
        k = key();
        if (k == 3) return -1;
        if (k == 13 && n) return s;
        if (k == 94 && s > 0) s--;
        if (k == 10 && s < n - 1) s++;
    }
}

static void load_drives(void)
{
    char *body;
    if (ui_cmd("dw disk show", &body) == 0) parse_disks(body, drives);
}

static void load_files(void)
{
    char *body;
    nfiles = 0;
    if (picoco_cmd("fs ls", lsbuf, sizeof lsbuf, &body) != 0) { msg("CANNOT LIST FILES"); return; }
    nfiles = parse_ls(body, files, MAX_FILES, 0);
    sort_files(files, nfiles);
    if (sel >= nfiles) sel = nfiles ? nfiles - 1 : 0;
}

static void draw_header(void)
{
    char d[20];
    u8 i;
    clear_row(0);
    put_at(0, 0, "PICOCO MANAGER", 0);
    put_at(0, (u8)(COLS - 3 - strlen(fw)), "FW ", 0);
    put_at(0, (u8)(COLS - strlen(fw)), fw, 0);
    clear_row(1); clear_row(2);
    for (i = 0; i < 4; i++) {
        d[0] = (char)('0' + i); d[1] = ' ';
        strncpy(d + 2, drives[i][0] ? drives[i] : "-", 13);
        d[15] = '\0';
        put_at((u8)(1 + (i & 1)), (u8)((i >> 1) * 16), d, 0);
    }
    memset(VRAM + 3 * COLS, 0x6D, COLS);            /* '-' row */
    memset(VRAM + 13 * COLS, 0x6D, COLS);
}

static void draw_help(void)
{
    clear_row(14); clear_row(15);
    put_at(14, 0, "0-3:MOUNT SHIFT+ E:EJECT B:BOOT", 0);
    put_at(15, 0, "N:NEW S:SET V:SAVE BREAK:EXIT", 0);
}

static void draw_all(void)
{
    cls();
    draw_header();
    top = clamp_top(sel, top, LIST_ROWS);
    if (nfiles) draw_list(files, nfiles, sel, top, LIST_TOP, LIST_ROWS);
    else put_at(LIST_TOP, 1, "NO DISK IMAGES ON FLASH", 0);
    draw_help();
}

static void do_mount(u8 d)
{
    char *body;
    if (!nfiles) return;
    strcpy(line, "dw disk insert ");
    line[15] = (char)('0' + d); line[16] = ' '; line[17] = '\0';
    strcat(line, files[sel].name);
    if (ui_cmd(line, &body) == 0) { ui_dirty = 1; load_drives(); }
}

static void do_eject(void)
{
    char *body;
    u8 k;
    clear_row(MSG_ROW);
    put_at(MSG_ROW, 0, "EJECT WHICH DRIVE (0-3)?", 1);
    k = key();
    if (k < '0' || k > '3') return;
    strcpy(line, "dw disk eject ");
    line[14] = (char)k; line[15] = '\0';
    if (ui_cmd(line, &body) == 0) { ui_dirty = 1; load_drives(); }
}

static void do_new(void)
{
    char name[28], *body;
    int i;
    if (input_line("NEW NAME: ", name, 20) <= 0) return;
    for (i = 0; name[i] && name[i] != '.'; i++) ;
    if (!name[i]) strcat(name, ".DSK");
    strcpy(line, "fs new ");
    strcat(line, name);
    if (ui_cmd(line, &body) != 0) return;
    load_files();
    for (i = 0; i < nfiles; i++) if (ends_with_ci(files[i].name, name) && strlen(files[i].name) == strlen(name)) sel = i;
}

static void do_save(void)
{
    char *body;
    if (ui_cmd("save", &body) == 0) { ui_dirty = 0; msg("SAVED"); }
}

/* Type-ahead: letters typed within a second of each other build a prefix. */
static char jump[5];
static u16 jump_t;

static void do_jump(char c)
{
    int i, l;
    if ((u16)(TICKS - jump_t) > 60) jump[0] = '\0';
    jump_t = TICKS;
    l = (int)strlen(jump);
    if (l < 4) { jump[l] = c; jump[l + 1] = '\0'; }
    l = (int)strlen(jump);
    for (i = 0; i < nfiles; i++) {
        if (strlen(files[i].name) >= (u16)l) {
            char save = files[i].name[l];
            int hit;
            files[i].name[l] = '\0';
            hit = ends_with_ci(files[i].name, jump);   /* whole prefix, case-insensitive */
            files[i].name[l] = save;
            if (hit) { sel = i; return; }
        }
    }
}

static int confirm_exit(void)
{
    u8 k;
    if (!ui_dirty) return 1;
    clear_row(MSG_ROW); clear_row(MSG_ROW + 1);
    put_at(MSG_ROW, 0, "NOT SAVED: V=SAVE+EXIT", 1);
    put_at(MSG_ROW + 1, 0, "BREAK=EXIT  OTHER KEY=STAY", 1);
    k = key();
    if (k == 'V' || k == 'v') { do_save(); return 1; }
    return k == 3;
}

void ui_run(void)
{
    char *body;
    u8 k;
    cls();
    put_at(0, 0, "PICOCO MANAGER", 0);
    put_at(2, 0, "CONNECTING...", 0);
    if (ui_cmd("version", &body) != 0) return;
    line_value(body, "version ", fw, sizeof fw);
    load_drives();
    load_files();
    for (;;) {
        draw_all();
        k = key();
        if (k == 3) { if (confirm_exit()) { cls(); return; } continue; }
        if (k == 94 && sel > 0) sel--;
        else if (k == 10 && sel < nfiles - 1) sel++;
        else if (k == 95) sel = sel > LIST_ROWS ? sel - LIST_ROWS : 0;           /* SHIFT+UP */
        else if (k == 91) sel = sel + LIST_ROWS < nfiles ? sel + LIST_ROWS : (nfiles ? nfiles - 1 : 0); /* SHIFT+DOWN */
        else if (k >= '0' && k <= '3') do_mount((u8)(k - '0'));
        else if (k == 'e') do_eject();
        else if (k == 'n') do_new();
        else if (k == 'v') do_save();
        else if (k == 's') { settings_run(); load_drives(); load_files(); }
        else if (k == 'b' && nfiles) { if (boot_image(files[sel].name)) return; load_drives(); }
        else if (k >= 'A' && k <= 'Z') do_jump((char)k);
    }
}

void settings_run(void) { msg("SETTINGS: TASK 13"); }   /* replaced in Task 13 */
```
The key codes (up 94, down 10, SHIFT+up 95, SHIFT+down 91, BREAK 3, ENTER 13, BS 8, lowercase from SHIFT+letter) are Color BASIC POLCAT values. If the jump prefix match reads awkwardly with `ends_with_ci`, write a small `prefix_ci` helper into `parse.c` instead. It must be tested in `test_parse.c`.

`coco/boot.h` (the stub body is replaced in Task 14):
```c
#ifndef BOOT_H
#define BOOT_H
int boot_image(const char *name);   /* 1 = exit to BASIC (handoff armed), 0 = back to the list */
#endif
```
Temporary `coco/boot.c`:
```c
#include "boot.h"
#include "ui.h"
int boot_image(const char *name) { (void)name; msg("BOOT: TASK 14"); return 0; }
```
`coco/main.c`:
```c
#include <cmoc.h>
#include "ui.h"

int main(void)
{
    ui_run();
    return 0;
}
```

- [ ] **Step 4: Build and check the memory end**

Run: `make -C coco && make -C coco test`
Expected: both succeed. Find the highest address used from the linker map (CMOC writes one next to the .BIN; if not, see the manual's `--map` / lwlink options). It must be below `$7800`. If it isn't, first shrink `lsbuf` to 3072 and `MAX_FILES` (in `parse.h`) to 96.

- [ ] **Step 5: Exercise it in XRoar (CoCo 2 and CoCo 3)**

Put a few images and a ROM in the picoco-host dir. Check:
1. Header shows `FW 1.2` and the drives.
2. The list hides `.ROM` files and `picoco.cfg`, and is sorted.
3. Up/down move the selection and `SHIFT+UP`/`SHIFT+DOWN` page through the list.
4. Typing `Z` jumps to the first Z entry.
5. `1` mounts into drive 1, and the header updates.
6. `SHIFT+E` then `1` ejects.
7. `SHIFT+N` with `TEST` creates `TEST.DSK` (161K) and selects it. Creating it again shows `FILE EXISTS`.
8. `SHIFT+V` shows `SAVED`, and `picoco.cfg` in the host dir now lists the mounts.
9. BREAK with unsaved changes asks for confirmation.
10. BREAK returns to `OK`, and `DIR 1` then lists the image mounted in step 5.

- [ ] **Step 6: Commit**

```bash
git add coco/ui.h coco/ui.c coco/boot.h coco/boot.c coco/main.c coco/Makefile
git commit -m "coco: main screen - browse, mount, eject, new, save"
```

---

### Task 13: Settings screen

**Files:**
- Modify: `coco/ui.c` (replace the `settings_run` stub)

**Interfaces:**
- Consumes: `ui_cmd`, `line_value`, `parse_ls`, `pick_list`, `input_line`, `parse_datetime`, `u32_to_dec`, `dec_to_u32`, `unix_to_civil`.

- [ ] **Step 1: Replace `settings_run`**

```c
static void strip_load(char *s)
{
    if (strncmp(s, "load ", 5) == 0) memmove(s, s + 5, strlen(s + 5) + 1);
}

static void fmt_time(u32 t, char *out)
{
    int y, mo, d, h, mi;
    unix_to_civil(t, &y, &mo, &d, &h, &mi);
    out[0] = (char)('0' + y / 1000); out[1] = (char)('0' + y / 100 % 10);
    out[2] = (char)('0' + y / 10 % 10); out[3] = (char)('0' + y % 10);
    out[4] = '-'; out[5] = (char)('0' + mo / 10); out[6] = (char)('0' + mo % 10);
    out[7] = '-'; out[8] = (char)('0' + d / 10); out[9] = (char)('0' + d % 10);
    out[10] = ' '; out[11] = (char)('0' + h / 10); out[12] = (char)('0' + h % 10);
    out[13] = ':'; out[14] = (char)('0' + mi / 10); out[15] = (char)('0' + mi % 10);
    out[16] = '\0';
}

static void pick_rom(void)
{
    char *body;
    int n, i;
    if (picoco_cmd("fs ls", lsbuf, sizeof lsbuf, &body) != 0) { msg("CANNOT LIST FILES"); return; }
    n = parse_ls(body, files, MAX_FILES, 1);
    if (!n) { msg("NO .ROM FILES ON FLASH"); return; }
    sort_files(files, n);
    i = pick_list("ROM FOR NEXT BOOT", files, n);
    if (i < 0) return;
    strcpy(line, "rom boot ");
    strcat(line, files[i].name);
    if (ui_cmd(line, &body) == 0) { ui_dirty = 1; msg("SAVE, THEN RESET"); }
}

void settings_run(void)
{
    char now[40], next[40], hdb[8], tv[16], clk[8], ts[20], *body;
    u32 t;
    u8 k;
    for (;;) {
        now[0] = next[0] = hdb[0] = tv[0] = clk[0] = '\0';
        if (ui_cmd("status", &body) != 0) return;
        line_value(body, "rom now ", now, sizeof now);
        line_value(body, "rom next ", next, sizeof next);
        line_value(body, "dw hdbdos ", hdb, sizeof hdb);
        if (ui_cmd("time", &body) != 0) return;
        line_value(body, "time ", tv, sizeof tv);
        line_value(body, "clock ", clk, sizeof clk);
        strip_load(now); strip_load(next);
        fmt_time(dec_to_u32(tv), ts);
        cls();
        put_at(0, 0, "SETTINGS", 0);
        put_at(0, (u8)(COLS - 3 - strlen(fw)), "FW ", 0);
        put_at(0, (u8)(COLS - strlen(fw)), fw, 0);
        put_at(2, 0, "ROM NOW  ", 0);  put_at(2, 9, now, 0);
        put_at(3, 0, "ROM NEXT ", 0);  put_at(3, 9, next, 0);
        put_at(5, 0, "R:CHOOSE ROM FOR NEXT BOOT", 0);
        put_at(6, 0, "H:HDB-DOS DRIVE MODE ", 0); put_at(6, 21, hdb, 0);
        put_at(7, 0, "T:CLOCK ", 0);   put_at(7, 8, ts, 0);
        if (strcmp(clk, "kept") != 0) put_at(8, 2, "(LOST AT RESET)", 0);
        put_at(10, 0, "V:SAVE   BREAK:BACK", 0);
        k = key();
        if (k >= 'a' && k <= 'z') k = (u8)(k - 32);
        if (k == 3) return;
        if (k == 'R') pick_rom();
        else if (k == 'H') {
            if (ui_cmd(strcmp(hdb, "on") == 0 ? "dw hdbdos off" : "dw hdbdos on", &body) == 0) ui_dirty = 1;
        } else if (k == 'T') {
            put_at(12, 0, "FORMAT: YYYY-MM-DD HH:MM", 0);
            if (input_line("TIME: ", ts, 16) > 0) {
                if (parse_datetime(ts, &t) != 0) msg("BAD DATE/TIME");
                else {
                    strcpy(line, "time set ");
                    u32_to_dec(t, line + 9);
                    ui_cmd(line, &body);
                }
            }
        } else if (k == 'V') do_save();
    }
}
```
`settings_run` replaces the stub at the end of `ui.c`, after `do_save`, so no forward declaration is needed.

- [ ] **Step 2: Build and exercise in XRoar**

Run: `make -C coco`. Put two 8 KB `.ROM` files in the host dir, then check:
1. SHIFT+S shows `ROM NOW`/`NEXT`, the HDB-DOS mode and the clock with `(LOST AT RESET)`. That note is expected on the host.
2. `R` lists the ROMs; picking one shows `SAVE, THEN RESET`, and `ROM NEXT` updates.
3. `H` toggles ON/OFF.
4. `T` with `2026-09-23 14:02` shows that time. `2026-02-30 00:00` shows `BAD DATE/TIME`.
5. `V` shows `SAVED`, and `picoco.cfg` has `rom load <picked>` and the `dw hdbdos` line.
6. BREAK goes back to the main screen.

- [ ] **Step 3: Commit**

```bash
git add coco/ui.c
git commit -m "coco: settings screen - next-boot ROM, HDB-DOS mode, clock"
```

---

### Task 14: Boot (`boot.c`)

**Files:**
- Modify: `coco/boot.c`, and `coco/Makefile` (add `hook.o` when the Task 2 spike succeeded)
- Keep: `coco/hook.asm` from Task 2 (success path only)

**Interfaces:**
- Consumes: `ui_cmd`, `msg`, `pick_list`, `dw_read_sector`, `is_os9_boot`, `rsdos_dir`; `hook_text`/`hook_install` (spike success path).

- [ ] **Step 1: Write `coco/boot.c`**

```c
#include "boot.h"
#include "ui.h"
#include "dw.h"

#define BOOT_TRACK_LSN 612u   /* track 34 sector 1 */
#define DIR_LSN        308u   /* track 17 sector 3 */

static u8 sec[256];
static rs_ent rs[72];
static file_ent pick[72];
static char cmdline[112];

static void basic_handoff(const char *cmd);

int boot_image(const char *name)
{
    char *body;
    int i, n = 0, end = 0;
    strcpy(cmdline, "dw disk insert 0 ");
    strcat(cmdline, name);
    if (ui_cmd(cmdline, &body) != 0) return 0;
    ui_dirty = 1;
    if (dw_read_sector(0, BOOT_TRACK_LSN, sec) != 0) { msg("CANNOT READ DISK"); return 0; }
    if (is_os9_boot(sec)) {
        /* As Disk BASIC's DOS command: track 34 to $2600, then $2602. The
         * program sits at $3800+, so nothing below is ours. */
        for (i = 0; i < 18; i++) {
            if (dw_read_sector(0, BOOT_TRACK_LSN + (u32)i, (u8 *)(0x2600 + i * 256)) != 0) {
                msg("BOOT FAILED: PRESS RESET");
                for (;;) ;
            }
        }
        asm {
            orcc    #$50
            jmp     $2602
        }
    }
    for (i = 0; i < 9 && !end; i++) {
        if (dw_read_sector(0, DIR_LSN + (u32)i, sec) != 0) { msg("CANNOT READ DIRECTORY"); return 0; }
        n += rsdos_dir(sec, rs + n, 72 - n, &end);
    }
    if (n == 0) { msg("NO BAS/BIN FILES. IN DRIVE 0"); return 0; }
    for (i = 0; i < n; i++) { pick[i].name = rs[i].name; pick[i].kb = 0; }
    i = pick_list("RUN WHICH FILE? (DRIVE 0)", pick, n);
    if (i < 0) return 0;
    if (rs[i].type == 0) { strcpy(cmdline, "RUN\""); strcat(cmdline, rs[i].name); strcat(cmdline, "\""); }
    else { strcpy(cmdline, "LOADM\""); strcat(cmdline, rs[i].name); strcat(cmdline, "\":EXEC"); }
    basic_handoff(cmdline);
    return 1;
}
```

- [ ] **Step 2: Add `basic_handoff`, using the Task 2 result**

3a. The spike succeeded (the hook works on both machines): add `hook.o` to the Makefile link, following the Task 2 recipe, and:
```c
extern char hook_text[];
void hook_install(void);

/* BASIC types the command itself: RVEC4 console-in hook (hook.asm, spike
 * 2026-09-23). Armed now, fed once the program returns to the OK prompt. */
static void basic_handoff(const char *cmd)
{
    strcpy(hook_text, cmd);
    strcat(hook_text, "\r");
    cls();
    hook_install();
}
```

3b. The spike failed: print the command and leave BASIC's cursor below it:
```c
#define CURPOS (*(volatile u16 *)0x0088)   /* Color BASIC cursor address */

/* ponytail: spike found no reliable BASIC handoff; the user types it. */
static void basic_handoff(const char *cmd)
{
    cls();
    put_at(0, 0, "TYPE THIS TO RUN IT:", 0);
    put_at(2, 0, cmd, 0);
    CURPOS = 0x0400 + 4 * 32;
}
```

- [ ] **Step 3: Exercise in XRoar (CoCo 2 and CoCo 3)**

Fixtures:
- An RS-DOS disk with a BASIC program. Build it with `decb dskini t.dsk` then `decb copy -0 -a -t hello.bas t.dsk,HELLO.BAS`.
- An ML program: copy `PICOCO.BIN` into it, which is a harmless self-test.
- A NitrOS-9 Becker boot disk if one is available (`nitros9` releases include `*_becker.dsk`). Otherwise skip the OS-9 check here and do it on the bench.

Check:
1. SHIFT+B on the RS-DOS disk lists `HELLO.BAS` and `PICOCO.BIN`. Picking `HELLO.BAS` runs it (3a) or shows the command to type (3b).
2. SHIFT+B on the OS-9 disk boots NitrOS-9.
3. SHIFT+B on a disk with no BAS/BIN shows the message and returns.

- [ ] **Step 4: Commit**

```bash
git add coco/boot.c coco/Makefile coco/hook.asm
git commit -m "coco: mount-and-boot for OS-9 and RS-DOS disks"
```
(Leave `hook.asm` out of `git add` on the fallback path.)

---

### Task 15: Bench run on the CoCo 3, and docs

**Files:**
- Modify: `firmware/README.md` (console command list: `dw disk`, `fs new`, `rom boot`, remote allowlist, vserial), `docs/firmware-architecture.md` (one paragraph on `dw_vser` and the remote allowlist), `coco/README.md` (bench steps), `docs/ADDITIONAL_ROADMAP.md` (item 3 → done, pointing at the spec)
- Modify: the console `help` string in `firmware/src/console/console.c` if new verbs are needed (none are: the new commands are sub-verbs)

This task needs the user at the bench.

- [ ] **Step 1: Flash firmware 1.2 and copy the disk over**

Build the Pico target per `firmware/README.md` and flash it. `fs export`, copy `coco/PICOCO.DSK` onto the PICOCO volume, eject, `fs import`, `dw mount 3 PICOCO.DSK`, `save`.

- [ ] **Step 2: Run on the CoCo 3**

`DRIVE 3:RUN"PICOCO"`, then repeat the Task 12 Step 5, Task 13 Step 2 and Task 14 Step 3 checks on real hardware. Also check:
- A CoCo reset after `SHIFT+V` keeps the mounts.
- A CoCo reset without saving drops them.
- `SHIFT+N` on a large name set, and `fs new` timing: the reply arrives within the 10 s timeout.
- `status` on the USB console shows no `reply_overflow` and no `becker overrun`.

- [ ] **Step 3: Update the docs listed above.** Keep each change to the facts this plan added.

- [ ] **Step 4: Run the whole verification set**

```bash
ninja -C build-host && ctest --test-dir build-host --output-on-failure
make -C coco test && make -C coco
```
Expected: all green.

- [ ] **Step 5: Commit**

```bash
git add firmware/README.md docs/firmware-architecture.md coco/README.md docs/ADDITIONAL_ROADMAP.md
git commit -m "docs: PiCoCo manager and the DriveWire vserial command channel"
```
