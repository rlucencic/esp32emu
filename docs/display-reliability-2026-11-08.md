# Tab5 rotation stalls and presenter snapshots — 2026-11-08

MSX sprite measurements exposed two display correctness problems. Treat
complete guest pictures as insufficient evidence that presentation works:
decoded whole-LCD pixels and core-0 profiles are required before using timings.
The sprite trial is documented in
[esp32msx](../../esp32msx/docs/sprite-bounds-experiment-2026-11-08.md).

## PPA transaction stall

On the attached Tab5 (P4 rev 1.0), guest frames and redraw signatures keep
advancing while LCD output freezes. JTAG inspection
finds the `present` task on FreeRTOS's indefinitely suspended list, waiting
on a semaphore while holding one mutex. Saved stack addresses resolve to
`xQueueSemaphoreTake`, `ppa_do_operation` at ESP-IDF `ppa_core.c:532` and
`ppa_do_scale_rotate_mirror` at `ppa_srm.c:335`. The runtime's display path
holds its LCD mutex across that blocking rotation. `s_present_on` is true,
with neither a pause request nor a paused machine. This establishes a PPA
completion wait in the observed stall; it does not identify every possible
reason an interrupt or completion can fail.

The flashed image's embedded ELF SHA matches the exact inspected ELF.
OpenOCD's automatic RTOS enumeration fails on this build, so task lists and
the named task's saved stack are inspected manually. No target registers
are modified. Cores are resumed after each inspection; those intervals
are excluded from timing.

Espressif confirms a rotated-SRM blocking problem and fixes the rotated
geometry calculation in
[upstream commit 469aa16](https://github.com/espressif/esp-idf/commit/469aa16c3502c6860dbc70511dcb2f922bc6c57d),
linked from [issue 19096](https://github.com/espressif/esp-idf/issues/19096).
The installed clean ESP-IDF 5.5.6 tree still uses the unrotated dimensions
for that workaround. This matches the observed call path; no SDK upgrade
or global SDK patch is made in this trial.

`lcd_tab5.c` instead expands each nonempty flush interval outward to
multiples of eight landscape rows, after clamping. For this fixed
1280-wide RGB565 source, rotation makes the strip height the output width;
its leftover width is now at least eight pixels. The other dimension,
1280, is divisible by either supported 16/32-pixel macroblock size. The
smallest leftover batch is therefore 8×16×16 = 2048 bits, above the
1536-bit DMA FIFO threshold used by the SDK workaround. Destination column
offsets are aligned too. This geometry argument applies to the current
Tab5 dimensions and unit scale, not arbitrary PPA configurations.

Both source-row and destination-column offsets use the expanded start,
so no pixels move. Every requested row is included; at most fourteen
neighbor rows are additionally rotated. 720 is divisible by eight, so
rounding never extends beyond the picture. No new allocation or API.
The S3 backend is unchanged.

An ASan/UBSan harness executes the actual extracted `lcd_flush` with mocked
driver/lock calls: 525,689 interval, coverage and tail assertions, including
all endpoints -2 through 722, integer extremes and a disabled-PPA control.
It checks lock balance, empty intervals, bounded coverage, alignment and
both macroblock sizes. Run `python3 tools/test_tab5_flush.py` to repeat it.
Hardware results are recorded below.

## Hash of the presented snapshot

Initial preview interpretations of missing static rows are not confirmed
by decoded PNG pixels: the captures contain the full sky and scene. Do not
use those preview interpretations or the setup redraw as proof of a second
field failure. Independently, the actual native presenter harness identifies
a cache consistency defect: hashes are saved from the change-detection read,
but scaling uses a later source read.

If detection sees A, scaling reads B, and the guest then settles on A,
the old code records A while displaying B. Subsequent A hashes match and
can suppress repair indefinitely. Hashes now describe the snapshot actually
read into the local scaling buffer. The detection pass does not commit
hashes; each source row fetched for scaling commits its hash immediately
before scaling. Cached scaled-row reuse keeps that same hash. A later
change to the guest source will therefore be detected on a following pass.

The native regression harness executes the actual presenter, its tap
creation, scalers and hash, with mocked scheduling, source callbacks and
LCD submission. It forces the A/B/A interleaving across 36 combinations
of nearest/smooth, cropped/full source, horizontal scaling (including
3:2), vertical scaling and cached-row reuse. All 36 old-code cases retain
the wrong snapshot; all 36 corrected cases converge to a stable-reference
picture under ASan/UBSan. This tests the presenter rather than only a model
of its order. It does not provide atomic whole-guest-frame presentation or
prove this race caused every captured board artifact.

The correction hashes changed rows again when fetched for scaling. This
additional work is a correctness cost and belongs in new baseline and
candidate measurements. No S3 runtime or performance equivalence is claimed.
Run `python3 tools/test_presenter_snapshots.py` for the regression harness.

## Validation and remaining limits

The normal MSX S3 and P4 builds and all 22 sibling builds pass with both
fixes (ten dual-board projects plus four P4-only projects, 24 builds).
Sibling repository status is unchanged by the matrix. The two native
regression scripts pass again after restoring production source.

In the linked P4 app, `present_task` grows from 1650 to 1768 bytes and
`lcd_flush` from 184 to 190 bytes. Their compiler stack frames remain
240 and 16 bytes respectively. There is no new heap or persistent state.
Changed-row hashing and additional rotated neighbor rows incur work; this
trial does not isolate their performance cost. Sprite baseline/candidate
comparisons use the same corrected runtime in both apps.

On the attached Tab5, a dedicated test app pauses the emulator while DSI
scanout continues, fills the 1280×720 source with deterministic RGB565
pixels `(x * 37) ^ (y * 113)`, and performs 5760 blocking flushes: each
starting row 0–719 with requested heights 1–8, clipped at 720. All eight
height sweeps complete, taking 6.450829 seconds in total. A final check of
every rotated panel-buffer pixel against the expected source pattern
reports zero mismatches across 921,600 pixels. This tests completion and
rotated destination contents, beyond checking the landscape capture.
The test hooks exist only in the temporary stress firmware; production
source is restored byte for byte before normal builds.

Fresh Lilly baseline/candidate runs both use the fixes, stay near 60 guest
FPS, show full decoded LCD scenes, and contain presenter samples in all
six 32,000-sample core-0 profiles (3.1–3.3%). Fresh SCREEN 6/8 candidate LCD
captures match all decoded RGB pixels of the earlier reference goldens.
This is bounded validation on one P4 board, not proof against every PPA
stall or a long-duration reliability claim. The hash race has a native
reproducer; it is not attributed to a particular field screenshot.

After testing, the owned temporary cartridge is ejected and deleted.
The normal `msxdisks` app is flashed and restored to MSX2 / 60 Hz / 512 KB.
The final guest capture and decoded 1280×720 LCD picture show Disk BASIC
at its `Ok` prompt, with guest frames continuing at 60 FPS. No test hooks,
profiler counters or isolated disk namespace remain in production source.
