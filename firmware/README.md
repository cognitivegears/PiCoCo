# PiCoCo firmware

Pico 2 <-> Tandy CoCo cartridge firmware. See
`docs/superpowers/specs/2026-09-07-firmware-design.md` for the design.

## Host build (Mac/Linux, for tests and `picoco-host`)

```
brew install cmake ninja
cmake -B build-host -G Ninja -DPICOCO_HOST=ON firmware
ninja -C build-host
ctest --test-dir build-host --output-on-failure
```

## Pico build

Not wired up yet in this task; see Plan B for the RP2350 target.
