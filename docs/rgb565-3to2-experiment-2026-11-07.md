# Exact 3:2 RGB565 horizontal scaling

Date: 2026-11-07. Reference: esp32emu `784bba1`, esp32mac2 `a8490d0`.
Follows the [uniform half-weight row trial](rgb565-half-blend-experiment-2026-11-07.md).
Tab5 ESP32-P4 at 360 MHz, ESP-IDF 5.5.6 / GCC 14.2.0, runtime at -O2,
32 MB / 200 MHz PSRAM, instructions and rodata in PSRAM.

Decision: reject this integration and restore the committed runtime.
The final specialized kernel saves about 46% on mixed-color rows, but
full-firmware CPU-frame throughput falls 1.56% at 3:2 and 1.27% in a
non-3:2 fallback control. The latter cost affects cases that cannot use
the faster kernel. More dirty-screen observations do not establish a
panel-FPS or general application gain. Only documentation is retained.

## Candidate

The existing pixel-centre tap generator repeats weights 26, 16 and 5
for exact 3:2 horizontal enlargement, with clamped first and last pixels.
Select a dedicated row function when geometry changes, only for smooth
scaling at that exact ratio with at least two source pixels. The ratio
requires an even source width. Each source pair produces three output
pixels; adjacent pairs share the intervening pixel reads. Constant weights
use the existing RGB565 interpolation and the exact floored-channel average
for weight 16. The first generated tap supplies the cropped source origin.
Other ratios and nearest-neighbor scaling retain the general row function.

There is no new table, allocation, source padding or four-byte alignment
requirement. Vertical blending, tap generation and unchanged-row skipping
remain intact. This is portable C; no ISA-specific assembly is introduced.
The row function pointer is updated only when geometry changes.

The tested candidate grows P4 text by 406 bytes: the specialized function is 272 bytes, the now
outlined general function 114 bytes, and the presenter grows 20 bytes.
These functions are outside IRAM on P4. The S3 STE build places the
217-byte specialization and 98-byte general function in IRAM. Compilation
does not establish S3 runtime performance.

## Correctness

A native fixture extracts the actual selector and row functions from the
modified runtime. ASan plus UBSan at -O2 passes 245,760 cropped row/canary
cases against the original tap generator and scaler: every even source
width from 2 through 1536, sixteen crop offsets, four uint16_t output
alignment offsets and five color patterns (seeded mixed RGB, flat, palette,
ramp and alternating black/white). Three arithmetic prototypes also pass
those same cases. An additional 48,776 dispatch/fallback cases exercise
varied dimensions, crop offsets and both smooth and nearest-neighbor
scaling against the reference. These are pixel-equivalence checks within
that coverage, not application benchmarks.

## Isolated P4 measurements

All variants run in one minimal firmware, with source rows, output and taps
in internal RAM and code in PSRAM. Each timed pass processes 500 rows;
a volatile checksum consumes output. Seven ABBA repetitions compare
reference/candidate/candidate/reference. Interrupts remain enabled; guest,
LCD, network and audio tasks are absent.

The simple prototype reduces 640→960 row cost by 47.024% for mixed RGB,
36.787% for flat colors, 46.776% for palette colors, and about 47.02% for
black/white and ramp patterns. Mixed-color 320→480, 512→768 and 1024→1536
cases save 46.93–47.07%. Explicitly sharing expanded channels is neutral.
A more complex delta-arithmetic variant saves about 51.6% on mixed RGB but
only 33.3% on flat colors. Carry the simpler version into the real presenter
for its simpler arithmetic and stronger flat-color result.

Repeating the same benchmark with the extracted final runtime function,
including its production argument signature, gives these medians. Both
reference and candidate use a non-inlined public measurement wrapper;
the final helper remains non-inlined as it is in production.

| Case | Reference cycles/row | Candidate cycles/row | Change |
| --- | ---: | ---: | ---: |
| 640→960 mixed RGB | 27982.578 | 15142.025 | −45.888% |
| 640→960 flat | 18354.985 | 11924.686 | −35.033% |
| 640→960 palette | 27577.109 | 15024.669 | −45.518% |
| 640→960 black/white | 27982.986 | 15141.706 | −45.890% |
| 640→960 ramp | 27982.876 | 15141.706 | −45.889% |
| 320→480 mixed RGB | 13990.706 | 7582.891 | −45.801% |
| 512→768 mixed RGB | 22387.155 | 12117.033 | −45.875% |
| 1024→1536 mixed RGB | 44774.361 | 24211.506 | −45.926% |

These numbers measure only the row kernel. The final function's call
structure and source-width calculation differ from the simpler prototype;
the final-source measurements describe the tested candidate.

## Build validation

All 24 required candidate builds pass (10 S3, 14 P4), including Mac2 with
its normal production disk-selection namespace. All thirteen other emulator repository
statuses match their pre-build states. The shared source is identical
throughout the matrix. No S3 hardware timing result is claimed.

Full 1280×720 LCD-buffer captures of deterministic guest fills are
byte-identical between the reference and candidate for both packed RGB
and indexed video. Each guest fills once and then stays in a self-loop;
this checks real tap generation, scaling, vertical blending, output
placement and clamped edges. It does not test tearing or panel timing.

For throughput, use the diskless II 4 MB / 640×480 packed-RGB fill,
32-bit addressing and masked guest interrupts. Each app is flashed and
started after the same ROM-boot wait, then receives identical guest code
and initial registers. Read frame and screen-signature counters at three
host-monotonic timestamps about 30 seconds apart, without PC profiling or
a connected debugger in those windows. Test apps use an isolated disk
selection namespace; original guest disk images are never mounted.

Run reference then candidate, then fresh boots in reverse order:

| Run/build | Emulated FPS, two windows | Signature increments/s |
| --- | --- | --- |
| First/reference | 51.399, 51.385 | 10.852, 10.849 |
| First/candidate | 50.540, 50.559 | 11.745, 11.749 |
| Reverse/candidate | 50.522, 50.474 | 11.749, 11.737 |
| Reverse/reference | 51.253, 51.255 | 10.883, 10.883 |

Pooled over approximately 120 seconds per build, CPU-frame throughput is
51.323/50.524 FPS (−1.557%) and signature increments are 10.867/11.745
per second (+8.080%). The result is consistent across these two runs;
this small sample is not broad statistical confidence. The signature
counts dirty-screen observations, not completed LCD frames or smoothness.
Higher observation frequency can change contention with the guest CPU;
no causal attribution of the CPU difference is established by these runs.

An indexed fill control uses the same dimensions, direct VRAM writes and
a grayscale palette. CPU-frame throughput stays at 60.159 FPS in both
builds; signature increments rise from 10.598 to 11.449/s (+8.037%).

A second control uses the II's 12-inch 512×384 mode, scaled to 960×720
(horizontal ratio 15:8). The specialization is inactive. Repeat both
orders with fresh boots and the same guest fill:

| Run/build | Emulated FPS, two windows | Signature increments/s |
| --- | --- | --- |
| First/reference | 52.099, 51.666 | 13.307, 13.341 |
| First/candidate | 51.090, 51.049 | 13.447, 13.478 |
| Reverse/candidate | 51.686, 51.663 | 13.379, 13.373 |
| Reverse/reference | 52.185, 52.179 | 13.279, 13.311 |

Pooled CPU-frame throughput falls from 52.032 to 51.372 FPS (−1.269%),
while signature increments rise from 13.310 to 13.419/s (+0.822%).
Individual runs vary, but both run orders show lower candidate CPU
throughput. Complete fallback LCD-buffer captures match exactly in both
orders. Correct pixels and a faster specialized kernel do not justify this
general-path cost; reject the integration.

The candidate changes general scaling from an inlined loop to an indirect
row call and changes code layout. These tests do not isolate which change
causes the CPU-throughput difference. Do not attribute all of it to call
latency or claim that every other emulator would have the same cost.

The shared runtime is restored byte-for-byte to `784bba1`. The restored
normal Mac2 production build passes and has the production disk namespace;
source contains no temporary test namespace or instrumentation. The board
is restored to the reference isolated-disk app, ordinary II 4 MB / 13-inch
ROM disk-wait screen, without synthetic guest code or mounted guest disks.
No production scaling change is retained.

## Next target

Try a direct conditional call for the specialized case while preserving
the original inlined general loop, then repeat both ratio controls before
accepting it. If that still costs general-path throughput, leave shared
scaling unchanged and measure LC paired indexed reads and RGB555 conversion.
Real-software tests on copied disks, physical input and audio-by-ear
coverage remain open.

Scratch fixtures, logs and firmware stay outside the repositories under
`/tmp/rgb565-3to2-2026-11-07`.

The subsequent [direct-dispatch trial](rgb565-direct-dispatch-experiment-2026-11-07.md)
keeps the general scaler inline and retains the specialization. Its
non-3:2 control stays within 0.05% of the reference; the eligible RGB path
still has a separately measured CPU-throughput tradeoff.
