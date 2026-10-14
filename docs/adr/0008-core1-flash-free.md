# core1 runs the bus loop from SRAM and never touches flash

Everything core1 executes (the event loop, `bus_on_*`, the Becker read hooks, `becker_refresh`) is in the `BUS_HOT` SRAM section and calls no libc, logging or flash-resident code, so core0 can do flash and USB work without stalling the bus. The build sets `PICO_FLASH_ASSUME_CORE1_SAFE` on that promise, and `firmware/tools/check_core1_flash_free.py` fails the build if any reachable core1 symbol, veneer or literal pool lands in flash. Details: `docs/firmware-architecture.md` §2 and §6.
