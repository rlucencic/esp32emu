# Direct dispatch for exact 3:2 RGB565 scaling

Date: 2026-11-07. Reference: esp32emu `18e0e3d` (runtime `784bba1`),
esp32mac2 `843acdd`. Follows the
[rejected indirect-dispatch trial](rgb565-3to2-experiment-2026-11-07.md).
Tab5 ESP32-P4 at 360 MHz, ESP-IDF 5.5.6 / GCC 14.2.0, runtime at -O2,
32 MB / 200 MHz PSRAM, instructions and rodata in PSRAM.

Decision: retain direct conditional dispatch for its higher dirty-screen
observation rate in the eligible 3:2 mode, with an observed CPU-throughput
tradeoff. Two RGB runs show +11.13% signature increments and −1.13%
emulated CPU-frame throughput. The non-3:2 control stays within 0.05%.
These are not panel-FPS or general application-speed gains.

## Candidate

Keep the previous exact 3:2 row kernel and replace its function-pointer
selection with a boolean chosen when geometry changes. The presenter makes
a direct call for eligible rows and otherwise executes the original
`scale_row` at the original source location. The P4 compiler keeps that
general loop inline: the final ELF has no standalone `scale_row` symbol.
The specialization remains isolated and non-inlined. Horizontal taps,
cropping, clamped edges, vertical blending and unchanged-row skipping
retain their previous behavior. Other ratios and nearest-neighbor scaling
use the general loop. No new table or allocation is introduced.

P4 text grows 344 bytes: the specialized helper is 268 bytes and
`present_task` grows from 1574 to 1650 bytes. The helper is outside IRAM.
This differs from the previous indirect implementation's 406 added bytes.
The S3 STE ELF keeps the general loop inline and places the 213-byte
specialization in IRAM; this is a build result, not S3 timing validation.
Code layout also changes, so this comparison cannot attribute every
throughput difference solely to dispatch latency.

## Correctness and builds

A native fixture extracts the actual candidate selector, helper and
presenter dispatch block. ASan plus UBSan at -O2 passes 245,760 3:2
row/canary cases and 48,776 dispatch/fallback cases against the committed
reference. Coverage includes every even source width 2..1536, sixteen
crop offsets, four output alignment offsets, seeded mixed RGB, flat,
palette, ramp and alternating black/white patterns. Fallback coverage
varies source/destination dimensions, crop offsets and smoothing mode.
This verifies pixels within that coverage, not application performance.

Candidate test and normal production P4 builds pass. The test app uses an
isolated disk-selection namespace; the production source/build has the
normal namespace. Original guest disks are never mounted during testing.
All 24 required builds pass (10 S3, 14 P4), including the normal Mac2
production build. The thirteen other emulator repository statuses match
their pre-build states. The shared source matches the tested snapshot
throughout the matrix.

## Hardware comparison

Test the non-3:2 fallback first, because that exposed the rejected
integration's cost. Use the diskless II, 4 MB, 12-inch 512×384 mode scaled
to 960×720 (15:8 horizontally). Inject the same packed-RGB guest fill,
32-bit addressing and masked guest interrupts. Each fresh app boot gets
the same wait, code and initial registers. Capture three frame/signature
counter readings about 30 seconds apart using host-monotonic timestamps;
no PC profiling or debugger connection is active during measurement.

Non-3:2 control, one fresh boot per build:

| Build | Emulated FPS, two windows | Signature increments/s |
| --- | --- | --- |
| Reference | 52.230, 52.099 | 13.216, 13.216 |
| Direct candidate | 52.137, 52.142 | 13.209, 13.210 |

Pooled CPU-frame throughput is 52.164/52.139 FPS (−0.048%), and signature
increments are 13.216/13.210/s (−0.048%). This run does not reproduce the
previous indirect version's 1.27% fallback CPU-throughput loss. Do not
interpret the tiny difference as a gain or a demonstrated regression;
one run per build does not establish broad statistical confidence.
The complete 1280×720 fallback LCD captures are byte-identical.

The complete 1280×720 RGB and indexed LCD-buffer captures are also
byte-identical between builds. These use a deterministic guest fill that
then stays in a self-loop, checking real scaling, blending, edges and
output placement. This does not test tearing or physical panel timing.

The indexed control uses the same 640×480 dimensions, direct VRAM writes
and a grayscale palette. Reference CPU throughput is 60.152/60.141 FPS
across the two windows; candidate is 60.156/60.154 FPS. Signature increments
are 10.486/10.511 per second versus 11.385/11.352. No indexed CPU-speed
improvement is claimed.

For eligible 3:2 scaling, use the II's 13-inch 640×480 mode, with the
same packed-RGB guest fill, initial registers and measurement procedure.
Run reference/candidate, then repeat fresh boots in reverse order:

| Run/build | Emulated FPS, two windows | Signature increments/s |
| --- | --- | --- |
| First/reference | 51.378, 51.332 | 10.808, 10.719 |
| First/candidate | 50.965, 50.915 | 11.817, 11.872 |
| Reverse/candidate | 50.659, 50.563 | 11.783, 11.750 |
| Reverse/reference | 51.373, 51.336 | 10.414, 10.554 |

Pooled over approximately 120 seconds per build, CPU-frame throughput is
51.355/50.775 FPS (−1.129%), and signature increments are 10.624/11.806
per second (+11.126%). Both orders show the same direction, with visible
run-to-run variation; this small sample is not broad statistical confidence.
The indexed control pools to 60.147/60.155 FPS and 10.499/11.369 signature
increments/s (+8.286%). Do not claim an indexed CPU-throughput gain.

Unlike the rejected indirect implementation, this integration does not
show a material fallback CPU cost in its measured control. Retain it for
the eligible graphics path while documenting the RGB CPU tradeoff. These
experiments do not isolate the source of every throughput difference or
establish how representative software behaves.

The board is restored to the uninstrumented candidate test app, ordinary
II 4 MB / 13-inch ROM disk-wait screen, without synthetic guest code or
mounted guest disks. Production source contains the normal disk namespace;
there are no temporary machine-local probes or configuration changes.
The screen signature counts dirty-screen observations,
not completed LCD frames or smoothness. CPU-frame throughput is distinct
from application speed and panel FPS.

## Next target

Measure LC paired indexed reads and RGB555 conversion next;
real-software tests on copied disks and S3 hardware timing remain open.

Scratch fixtures, full logs and firmware stay outside the repositories
under `/tmp/rgb565-direct-2026-11-07`.
