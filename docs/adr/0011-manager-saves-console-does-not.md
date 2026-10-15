# The Manager persists every change; the console persists on `save`

A change made from the Manager (a mount, the default ROM, a setting, WiFi) is written to `picoco.cfg` at once; there is no save key and no unsaved state. The console keeps the explicit `save`, because bring-up wants to try a mount, a ROM or a mode without committing it, while a CoCo user's next step after a mount is a RESET, which reboots the Pico and replays only what was saved (roadmap item 5.1). Two interfaces, two persistence rules, on purpose: the console is the bench, the Manager is the product.
