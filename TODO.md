# esp32emu roadmap

Proposed work in priority order. See [Using it](README.md#using-it) and the [web API](README.md#web-api) for the current project.

1. **Exercise shared disk operations across machine states.** Cover
   `/api/info`, `/api/status` and `/api/new` before Start, while running and
   while paused. Include an STE with and without an AHDI source, an empty drive
   and an upload during status polling. Verify parseable JSON and that
   blank-disk availability agrees with the setup UI.

2. **Measure SD reliability under MaTouch memory pressure.** Exercise the
   mount-time DMA reservation in `components/emu/sdcard.c` with Wi-Fi, audio
   and an SD-heavy guest active. Record reset reasons, minimum internal DMA
   heap and upload/download hashes across repeated boots. Use a Tab5 run as a
   second board baseline.

3. **Add a shared runtime build matrix.** Document a reproducible job that
   checks out sibling projects at explicit revisions, builds both board targets
   with representative machine configs and runs the ROM-free host tests. Start
   with the shared presenter checks and one machine build per target.
