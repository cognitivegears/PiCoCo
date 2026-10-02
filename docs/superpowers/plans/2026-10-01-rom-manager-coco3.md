# ROM Manager on a CoCo 3 + Double RESET Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The manager launches Program Paks and DOS ROMs on a CoCo 3, and a double RESET brings the manager up from any ROM on any machine.

**Architecture:** A CoCo 3 copies the cart into RAM at boot and runs all-RAM, so leaving the manager needs CoCo 3 paths: a pak is started the way the CoCo 3's own ROM starts one (`$CC` to `$FF90`, ROM mode, `JMP $C000`), and a DOS ROM by re-running the reset routine at `$8C1B`, which copies the new cart into RAM. The firmware picks the action by the capability bits the stub reported; the stub gains three action codes. Double RESET is a marker in RAM that survives the reset.

**Tech Stack:** C11 firmware (host tests, ctest), lwasm, XRoar, `picoco-host --ui`.

**Spec:** `docs/superpowers/specs/2026-10-01-rom-manager-design.md` sections 4.4, 6.5 (double RESET), 7.1, 8 (checks 1, 5, 6). Continues `docs/superpowers/plans/2026-10-01-rom-manager-foundation.md` on the same branch, `rom-manager`.

## Global Constraints

- Never touch `firmware/src/bus/bus_core1.c`; add no read or write hook.
- core1 flash-free check prints `ok` on both Pico targets; the Pico 2 build references no cyw43/lwIP.
- Stub rules: interrupts masked; no BASIC ROM call except `JSR [$A000]`; never `CLR`/`INC`/`DEC`/`TST` on `$FF41`-`$FF43`; entry `$C002`; image starts `DK`; the leave routine runs from RAM and stays position-independent.
- Existing action codes keep their meaning (`$10` jump, `$11` cold, `$13` warm). New codes: `$16` CoCo 3 pak jump, `$17` CoCo 3 cold restart, `$18` CoCo 3 warm restart.
- CoCo 3 facts, read from `~/.xroar/roms/coco3.rom`: reset is `$8C1B` (`ORCC #$50 / LDA #$0A / STA $FF90 / CLR $FFDE / JMP $C000`); the ROM's own cart start is `$8C28` (`CLR $FEED / CLR $FF23 / LDA #$CC / STA $FF90 / CLR $FFDE / RTS`).
- Double-RESET window is `PICOCO_DOUBLE_RESET_MS` = 3000.
- After any change to `coco/stub/manager.asm` run `make -C coco/stub` and commit the regenerated `firmware/src/ui/manager_rom.h`.
- Host suite green and warning-free: `cmake --build build-host && ctest --test-dir build-host --output-on-failure`.

## Review Focus

1. **A pak launched on a CoCo 3 must not land in the RAM copy of the manager** (the fault the last review found): the reply carries `$16`, never `$10`, when the CoCo 3 bit is set. Test in Task 1.
2. **BREAK on a CoCo 3** uses the CoCo 3 warm code, not `JMP [$FFFE]`. Test in Task 1.
3. **A third RESET, or a RESET long after boot, must not bring the manager up**: the marker is cleared on detection and after the window. Test in Task 2.
4. **Double RESET must not change the saved config**: it loads the manager for this boot only. Test in Task 2.
5. **Power-on with random RAM** must not look like a double RESET: the marker is a 32-bit magic value. Covered by design; bench in Task 3.

---

### Task 1: CoCo 3 leave paths in the stub and the firmware

**Files:**
- Modify: `coco/stub/manager.asm`, `firmware/src/ui/manager_rom.h` (regenerated), `firmware/src/ui/ui.h`, `firmware/src/ui/ui.c`, `coco/README.md`
- Test: `firmware/tests/test_ui.c`

**Interfaces:**
- Consumes: `launch()`, `on_key()` and `caps` in `ui.c`; `LEAVER` and `KEYW` in the stub.
- Produces: `UI_ACT_JUMP3 0x16`, `UI_ACT_COLD3 0x17`, `UI_ACT_WARM3 0x18`.

- [ ] **Step 1: Write the failing tests**

In `firmware/tests/test_ui.c`, replace the test `coco3_refuses_every_launch` (and its helper `coco3_refused`, if it has no other user) with these, and update the `RUN` lines to match:

```c
#define CAPS_COCO3 (UI_CAP_64K | UI_CAP_32K | UI_CAP_ECB | UI_CAP_COCO3)

/* Review Focus 1: a CoCo 3 runs the cart from a RAM copy, so JMP $C000 would
 * land in the manager. The pak must be started in ROM mode. */
TEST(coco3_pak_uses_the_rom_mode_jump) {
    setup();
    mkfile("game.rom", "\x7E\xC0\x10", 8192);
    open_with(CAPS_COCO3);
    poll(0, UI_KEY_ENTER, -1, 10);
    int len = reply_ok();
    ASSERT(len >= 2);
    ASSERT_EQ(tx[2 + len - 2], UI_ACT_JUMP3);
    txn = 0;
    uint8_t g = 'G';
    ui_feed(&g, 1, 20);
    ASSERT_EQ(tx[0], UI_GO_OK);
    ASSERT(strcmp(last_line, "rom launch game.rom") == 0);
}

TEST(coco3_dos_rom_uses_the_coco3_cold_restart) {
    setup();
    mkfile("hdb.rom", "DK", 8192);
    open_with(CAPS_COCO3);
    poll(0, UI_KEY_ENTER, -1, 10);
    int len = reply_ok();
    ASSERT(len >= 2);
    ASSERT_EQ(tx[2 + len - 2], UI_ACT_COLD3);
}

/* Review Focus 2 */
TEST(coco3_break_uses_the_coco3_warm_restart) {
    setup();
    open_with(CAPS_COCO3);
    poll(0, UI_KEY_BREAK, -1, 10);
    int len = reply_ok();
    ASSERT(len >= 2);
    ASSERT_EQ(tx[2 + len - 2], UI_ACT_WARM3);
    txn = 0;
    uint8_t g = 'G';
    ui_feed(&g, 1, 20);
    ASSERT_EQ(tx[0], UI_GO_OK);
}

/* The CoCo 1/2 codes are unchanged. */
TEST(coco2_codes_unchanged) {
    setup();
    mkfile("game.rom", "\x7E\xC0\x10", 8192);
    open_with(UI_CAP_32K | UI_CAP_ECB);
    poll(0, UI_KEY_ENTER, -1, 10);
    int len = reply_ok();
    ASSERT_EQ(tx[2 + len - 2], UI_ACT_JUMP);
    poll(0, UI_KEY_BREAK, -1, 20);
    len = reply_ok();
    ASSERT_EQ(tx[2 + len - 2], UI_ACT_WARM);
}
```

Run: `cmake --build build-host && ./build-host/test_ui`
Expected: does not compile (`UI_ACT_JUMP3` undeclared).

- [ ] **Step 2: Firmware side**

`firmware/src/ui/ui.h`, after `UI_ACT_WARM`:

```c
#define UI_ACT_JUMP3 0x16           /* CoCo 3, after a ROM swap: $CC -> $FF90, ROM mode, JMP $C000 */
#define UI_ACT_COLD3 0x17           /* CoCo 3, after a ROM swap: clear $71, JMP $8C1B (recopies the cart to RAM) */
#define UI_ACT_WARM3 0x18           /* CoCo 3: JMP $8C1B with $71 untouched */
```

`firmware/src/ui/ui.c`, in `launch()`: delete the `COCO 3: NOT YET` guard and its comment, and replace the last two lines before the closing brace with:

```c
    /* A CoCo 3 copies the cart to RAM at boot and runs all-RAM: a pak is
     * started in ROM mode like the CoCo 3's own cart start ($8C28), a DOS ROM
     * by re-running reset so the new cart is copied. */
    bool c3 = (caps & UI_CAP_COCO3) != 0;
    pending_act = dos ? (c3 ? UI_ACT_COLD3 : UI_ACT_COLD) : (c3 ? UI_ACT_JUMP3 : UI_ACT_JUMP);
    return pending_act;
```

In `on_key()`, the BREAK line becomes:

```c
    else if (key == UI_KEY_BREAK) { pending[0] = '\0'; act = pending_act = (caps & UI_CAP_COCO3) ? UI_ACT_WARM3 : UI_ACT_WARM; }
```

Run: `cmake --build build-host && ./build-host/test_ui`
Expected: all tests pass (count = previous count + 3).

- [ ] **Step 3: Stub**

In `coco/stub/manager.asm`:

a) Protect the frame pointer across the keyboard call (the CoCo 3's patched keyboard routine is not known to keep Y). Replace

```asm
KEYW    jsr [$A000]
        beq KEYW
```

with

```asm
KEYW    pshs y
        jsr [$A000]
        puls y                  PULS leaves the flags alone: Z is still POLCAT's
        beq KEYW
```

and do the same around the second `jsr [$A000]` (label `POLLB2`):

```asm
POLLB2  pshs y
        jsr [$A000]
        puls y
        beq POLLB2
```

b) In `DETECT`, after the CoCo 3 test sets bit 4, also set the 64K bit, since every CoCo 3 has at least 128K. Replace

```asm
        bne DET2
        orb #$10
```

with

```asm
        bne DET2
        orb #$12                CoCo 3, and 64K with it
```

c) In `LEAVER`, replace everything from `cmpb #$10` to just before `LVFAIL` with:

```asm
        cmpb #$10
        beq LVJMP
        cmpb #$13
        beq LVWARM
        cmpb #$16
        beq LVJMP3
        cmpb #$17
        beq LVCOLD3
        cmpb #$18
        beq LVWARM3
        clr RSTSW               11: cold restart
LVWARM  jmp [$FFFE]
LVJMP   andcc #$AF
        jmp $C000
* CoCo 3: start the pak as the CoCo 3 ROM's own cart start does ($8C28):
* 16K internal + 16K cartridge ROM, ROM mode. Interrupts stay masked.
LVJMP3  lda #$CC
        sta $FF90
        sta $FFDE
        jmp $C000
LVCOLD3 clr RSTSW               reset recopies the cart (now the new ROM) to RAM
LVWARM3 jmp $8C1B
```

Update the comment above `LEAVER` to list the six codes, and the header comment's capability line to say bit 1 is 64K.

Run: `make -C coco/stub && head -c 2 coco/stub/manager.rom && ls -l coco/stub/manager.rom`
Expected: `DK`, 8192 bytes, no assembler error.

Run: `cmake --build build-host && ctest --test-dir build-host --output-on-failure` (the header changed)
Expected: `100% tests passed`.

- [ ] **Step 4: Emulator, CoCo 3 profile**

XRoar cannot swap its cart ROM, so this checks only that the stub starts and survives its restart paths on a CoCo 3; the real launches are bench steps. Follow `coco/README.md` "Manager stub in XRoar" for the harness rules (`-ui null`, one `xrscreen.py` run per XRoar, `pkill -9` between runs, wait before capturing).

```bash
D=$(mktemp -d); cp coco/carttest.rom "$D/CARTTEST.ROM"; cp firmware/roms/hdbdw3bc3.rom "$D/HDB3.ROM"
cp coco/stub/manager.rom ~/.xroar/roms/manager.rom
build-host/picoco-host --dir "$D" --port 65511 --ui &
xroar -machine coco3 -becker -becker-port 65511 -cart-rom manager -rompath ~/.xroar/roms -gdb -ao null -ui null &
sleep 8
python3 coco/tools/xrscreen.py --wait-for 'COCO3' --timeout 40
```

Expected: the manager autostarts (the CoCo 3 found `DK`), row 0 reads `PICOCO  64K ECB COCO3`, the two files are listed.

Then a second run with `-type '\n\r'` (down, ENTER: the `DK` ROM `HDB3.ROM`, action `$17`): the machine restarts and, because XRoar's cart is still the manager, the manager screen comes back. `xrscreen.py --wait-for 'COCO3'` after a 15 s wait exits 0, and the XRoar process is still alive (`pgrep xroar`). A crash or a blank screen is a failure to debug.

If `-type` keys do not reach the stub on the CoCo 3 profile (no BASIC prompt exists when the manager autostarts), say so in the README and leave the restart check to the bench.

- [ ] **Step 5: Document and commit**

Add a short "CoCo 3" paragraph to the README section with the command and what was seen.

```bash
git add coco/stub/manager.asm firmware/src/ui/manager_rom.h firmware/src/ui/ui.h firmware/src/ui/ui.c firmware/tests/test_ui.c coco/README.md
git commit -m "rom manager: CoCo 3 launch paths (ROM-mode pak start, reset at \$8C1B)"
```

---

### Task 2: Double RESET

**Files:**
- Modify: `firmware/src/plat.h`, `firmware/src/plat_pico.c`, `firmware/host/plat_host.c`, `firmware/src/main.c`, `firmware/src/console/console.c`, `firmware/src/console/console.h`, `firmware/README.md`
- Test: `firmware/tests/test_console.c`

**Interfaces:**
- Produces:

```c
/* plat.h */
#define PICOCO_DOUBLE_RESET_MS 3000
bool plat_double_reset(void);                    /* call once, first thing in main: true if this boot is the second reset inside the window */
void plat_double_reset_tick(uint32_t now_ms);    /* main loop: disarms the marker once the window has passed */
/* console.h */
void console_boot_manager(void);                 /* this boot only: load the built-in manager, leave the saved choice alone */
```

- [ ] **Step 1: Write the failing tests**

Add to `firmware/tests/test_console.c` and register both in `main`:

```c
/* Review Focus 4: a double RESET loads the manager for this boot and leaves
 * the saved ROM choice alone. */
TEST(boot_manager_is_one_shot) {
    setup();
    console_exec("rom pattern");
    console_exec("save");
    ASSERT(console_run_config() >= 0);
    console_boot_manager();
    ASSERT_EQ(bus_table[0], 'D');
    ASSERT_EQ(bus_table[1], 'K');
    outn = 0;
    console_exec("status");
    ASSERT(strstr(out, "rom now load manager"));
    ASSERT(strstr(out, "rom next pattern"));
    ASSERT(console_run_config() >= 0);          /* the next boot: the saved choice again */
    ASSERT_EQ(bus_table[0x10], 0x10);
}

/* Review Focus 3: the host platform never reports a double reset. */
TEST(host_never_double_resets) {
    ASSERT(!plat_double_reset());
    plat_double_reset_tick(10000);
    ASSERT(!plat_double_reset());
}
```

Run: `cmake --build build-host`
Expected: compile errors (`console_boot_manager`, `plat_double_reset` undeclared).

- [ ] **Step 2: Platform functions**

`firmware/src/plat.h`, add:

```c
#define PICOCO_DOUBLE_RESET_MS 3000
/* Spec 2026-10-01 §6.5: RESET twice inside the window brings the manager up
 * for that boot. The CoCo's /RESET drives the Pico's RUN pin, so every CoCo
 * reset is a Pico reboot; the marker lives in RAM the reset does not clear. */
bool plat_double_reset(void);                    /* call once, first thing in main */
void plat_double_reset_tick(uint32_t now_ms);    /* main loop: disarm after the window */
```

`firmware/host/plat_host.c`, add:

```c
bool plat_double_reset(void) { return false; }
void plat_double_reset_tick(uint32_t now_ms) { (void)now_ms; }
```

`firmware/src/plat_pico.c`, add (the `__uninitialized_ram` macro is already used by `crash.c`; include whatever header it includes for it):

```c
/* A 32-bit magic value, so RAM noise at power-on cannot look like a hit. */
#define DBL_MAGIC 0xD0B1E5E7u
static uint32_t __uninitialized_ram(dbl_marker);
static bool dbl_armed;

bool plat_double_reset(void) {
    bool hit = dbl_marker == DBL_MAGIC;
    dbl_marker = hit ? 0 : DBL_MAGIC;            /* a third reset is a plain boot again */
    dbl_armed = !hit;
    return hit;
}

void plat_double_reset_tick(uint32_t now_ms) {
    if (dbl_armed && now_ms >= PICOCO_DOUBLE_RESET_MS) { dbl_marker = 0; dbl_armed = false; }
}
```

- [ ] **Step 3: Console helper**

`firmware/src/console/console.h`, add:

```c
void console_boot_manager(void);   /* double RESET: load the built-in manager for this boot; rom_cmd (the saved choice) is untouched */
```

`firmware/src/console/console.c`, add next to `rom_fallback`:

```c
void console_boot_manager(void) {
    if (rom_load_mem(manager_rom, manager_rom_len) == 0) snprintf(rom_now, sizeof(rom_now), "load manager");
}
```

- [ ] **Step 4: Wire it into `main.c`**

In `main()`, make the marker check the first statement after `gpio_setup();`:

```c
    bool dbl_reset = plat_double_reset();
```

After the `if (fs_flash_mount() == 0) { ... } else { ... }` block that runs the config, add:

```c
    if (dbl_reset) {
        console_boot_manager();
        LOG_I(LOG_M_MAIN, "double reset: manager for this boot");
    }
```

In the net boot-hold `while` loop and in the main `for (;;)` loop, add after the `plat_now_ms()` read:

```c
        plat_double_reset_tick(now);
```

(in the boot-hold loop use `plat_double_reset_tick(plat_now_ms());`).

- [ ] **Step 5: Run the tests and builds**

Run: `cmake --build build-host && ctest --test-dir build-host --output-on-failure`
Expected: `100% tests passed`.

Run: `ninja -C build-pico && ninja -C build-pico-plusw`
Expected: both link; `check_core1_flash_free: ok`.

- [ ] **Step 6: Document and commit**

In `firmware/README.md`, next to the manager notes, add: RESET twice within 3 seconds loads the manager for that boot only (the saved ROM is unchanged); on a machine without Extended BASIC then type `EXEC 49154`; the boot log shows `double reset: manager for this boot`.

```bash
git add firmware/src/plat.h firmware/src/plat_pico.c firmware/host/plat_host.c firmware/src/main.c \
        firmware/src/console/console.c firmware/src/console/console.h firmware/tests/test_console.c firmware/README.md
git commit -m "firmware: double RESET loads the manager for one boot"
```

---

### Task 3: Bench — CoCo 3 regression, manager, double RESET

**Files:**
- Modify: `firmware/TEST_PLAN.md` (sections F.5, J), `CLAUDE.md`, the spec (section 8)

Needs the user at the bench; the controller runs it. Board: PCB v2.3.1 #1, Pico 2. CoCo 3: 6309, 2 MB.

- [ ] **Step 1: Flash and set a CoCo 3 default (board on USB, CoCo off or board out)**

```bash
ninja -C build-pico
python3 firmware/tools/pconsole.py /dev/cu.usbmodem3103 bootsel
cp -X build-pico/picoco.uf2 /Volumes/RP2350/
```

Console: `version` reads `1.4`; `rom load hdbdw3bck.rom`, `becker native`, `bus drive on`, `dw mount 0 DINORUN.DSK`, `save`, `reboot`.

- [ ] **Step 2: Add section J.3 to `firmware/TEST_PLAN.md`**

Insert before "### J.2 Results" a section `### J.3 CoCo 3 (6309, 2 MB)` with these steps, and matching rows in the results table:

```markdown
### J.3 CoCo 3 (6309, 2 MB) and double RESET

Read-path regression first (the 2026-10-01 core1 change, TEST_PLAN I.5):

1. `hdbdw3bck.rom` saved: power on, HDB-DOS banner, `DIR`,
   `LOADM"DINORUN":EXEC`. `status`: `late_precompute` near the old 4097
   per boot, `addr_resample 0`, `oe_glitch 0`, `crc_err 0`, `underrun 0`.
2. `rom load hdbdw3bc3.rom`, `save`, power-cycle: same checks at 1.79 MHz,
   four `LOADM`s and a `SAVE`.

Manager:

3. RESET twice within 3 s: the manager autostarts, header
   `PICOCO  64K ECB COCO3`. Boot log: `double reset: manager for this boot`.
4. Arrows move the highlight.
5. ENTER on `HDBDW3BC3.ROM`: the CoCo 3 restarts into HDB-DOS; `DIR` works.
   `status`: `rom now load hdbdw3bc3.rom`, `rom next` unchanged.
6. RESET once: back to the saved default (HDB-DOS), not the manager.
7. RESET twice, ENTER on `CARTTEST.ROM`: the cart test runs and prints
   `PASS` lines with `ERR 0000` (a pak started in ROM mode).
8. RESET twice, BREAK: record what happens (expected: the manager again,
   since a CoCo 3 warm restart re-enters a `DK` cart).
9. RESET once, 10 s later RESET once: HDB-DOS both times (outside the
   window).

On the 16K CoCo 2 (no Extended BASIC):

10. With `carttest.rom` saved as the default, RESET twice, `EXEC 49154`:
    the manager.
```

- [ ] **Step 3: Run J.3 with the user, fill in the results**

Read `status` and `log dump` from the console after each step the user reports. A failure is recorded as seen (screen text, counters, `trace dump`), then debugged with superpowers:systematic-debugging before continuing. If step 3 never triggers, the marker is not surviving the reset: fall back to a POWMAN scratch register (spec section 8 check 5) and say so.

- [ ] **Step 4: Update the F.5 matrix, `CLAUDE.md` and the spec**

F.5: fill the CoCo 3 Pico 2 bare cells from steps 1-2. `CLAUDE.md` Firmware section: one bullet on the CoCo 3 leave paths (`$16`/`$17`/`$18`, why `JMP $C000` is wrong there) and double RESET. Spec section 8: mark checks 1, 5 and 6 settled with what the bench showed.

- [ ] **Step 5: Commit**

```bash
git add firmware/TEST_PLAN.md docs/superpowers/specs/2026-10-01-rom-manager-design.md
git commit -m "docs: CoCo 3 bench (read-path regression, manager, double RESET)"
```
