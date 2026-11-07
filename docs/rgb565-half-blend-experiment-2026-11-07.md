# Uniform half-weight RGB565 rows

Date: 2026-11-07. Reference: esp32emu `f07689e`, esp32mac2 `6e8ddc7`.
This follows the [Mac II presentation investigation](../../esp32mac2/docs/presentation-followup-2026-11-07.md).
Tab5 ESP32-P4 at 360 MHz, ESP-IDF 5.5.6 / GCC 14.2.0, runtime at -O2,
32 MB / 200 MHz PSRAM, instructions and rodata in PSRAM.

Decision: retain the isolated half-row helper for its kernel saving and
modest presentation benefit, with an observed CPU-throughput tradeoff.
Across two full-firmware RGB runs, the signature proxy rises 3.63% and
emulated CPU-frame throughput falls 0.31%. This is not a general FPS,
panel refresh or application-speed claim.

## Candidate

The vertical blend weight is constant across an output row. For weight
16 out of 32, each RGB565 channel is the floored average of its two
inputs. Compute it directly as:

```c
(p & q) + (((p ^ q) & 0xF7DE) >> 1)
```

The mask removes the low bit of each channel before the shift, preventing
bits from crossing channel boundaries. An isolated, non-inlined
`blend_half_row` handles this case once per row. Keep the original general
blend loop for other weights. Horizontal scaling, tap generation, zero
vertical-weight bypass, line hashes and unchanged-row skipping are intact.
The helper requires only the existing uint16_t alignment, including odd
widths; it adds no table, allocation or four-byte alignment requirement.
This is portable C, not an ISA-specific assembly path.

On P4, the helper is 54 bytes and `present_task` grows 18 bytes: 72 added
text bytes outside IRAM. On S3 the helper uses the existing `PRESENT_HOT`
placement in internal instruction RAM; compilation and hardware performance
are separate checks.

## Correctness

A native fixture extracts the retained helper and related scaler code from
the actual runtime source. ASan plus UBSan at -O2 passes:

- 18,504,256 color/weight comparisons against an independent per-channel
  oracle, including every RGB565 first color against eight representative
  second colors at all 33 weights; every pair of isolated channel values;
  and one million seeded mixed-color/weight cases.
- 12,194 horizontal row/tap/output-canary cases over varied source and
  destination sizes, with smooth and nearest-neighbor taps.
- 7,260 complete vertical row/output-canary cases over all 33 weights,
  empty/even/odd widths and output buffers offset by two bytes.

These checks establish pixel equivalence within that coverage. They do not
establish OS/application performance or physical input/audio behavior.

## Isolated P4 measurements

Both source versions run in the same minimal firmware. Source rows, tap
arrays and output buffers are internal RAM, as in the real presenter.
Code is in PSRAM, as in the P4 emulator build. Each timed pass processes
500 rows of 960 pixels; a volatile checksum consumes output. Seven ABBA
repetitions compare reference/candidate/candidate/reference. Interrupts
remain enabled; guest CPU, LCD, audio and network tasks are absent.

Three early alternatives are rejected: a one-multiply interpolation is
neutral horizontally but around 4% slower on unequal vertical rows and
13–14% slower on zero/equal-color controls. A per-pixel half-weight branch
regresses common unequal cases. Selecting two inline loops once per row
saves about 52% at half weight but also regresses other weights by 8–15%.
Separating only the averaging helper avoids that general-loop regression.
The original arithmetic remains preferable for the general case on this
P4/compiler configuration; no S3 timing conclusion follows.

Retained isolated-loop version, median cycle-cost changes:

| Case | Candidate versus reference |
| --- | ---: |
| Vertical, half weight, mixed RGB565 | −51.901% |
| Vertical, half weight, 16-color palette | −51.299% |
| Vertical, half weight, equal colors | −14.168% |
| Vertical, weight 5, mixed colors | +0.007% |
| Vertical, weight 26, mixed colors | −0.016% |
| Vertical, zero weight control | +0.014% |
| Horizontal controls | within 0.07% |

Tiny control differences are not claimed as gains or regressions. The
half-weight numbers measure only the row kernel, not whole-emulator FPS.

## Full firmware and build validation

Use the prior diskless Mac II 640×480 packed-RGB guest fill, 4 MB,
32-bit addressing, interrupts masked. Flash each app, finish the same ROM
boot wait, inject identical guest code and registers, and read frame and
screen-signature counters at three host-monotonic timestamps about 30
seconds apart. No periodic PC sampling or connected debugger is present
in those windows. Both apps use the isolated disk-selection namespace;
original guest disk images are never mounted.

Run reference then candidate; repeat fresh boots in reverse order:

| Run/build | Emulated FPS, two windows | Signature increments/s |
| --- | --- | --- |
| First/reference | 51.453, 51.515 | 10.517, 10.469 |
| First/candidate | 51.228, 51.262 | 10.878, 10.818 |
| Reverse/candidate | 51.397, 51.402 | 10.719, 10.706 |
| Reverse/reference | 51.460, 51.495 | 10.352, 10.272 |

Pooled over approximately 120 seconds per build, emulated CPU-frame
throughput is 51.481/51.322 FPS (−0.308%) and signature increments are
10.403/10.780 per second (+3.629%). These are observations from two runs,
not broad statistical confidence. The signature counts dirty-screen
observations, not distinct pixels, completed panel frames or smoothness;
the prior presentation investigation explains that distinction.

An indexed guest control keeps the same dimensions but uses the direct
VRAM window and grayscale palette. Pooled throughput is 60.145/60.149 FPS;
the signature proxy is 10.318/10.552 per second (+2.27%). No indexed CPU
speed improvement is claimed.

Three separate 32,000-sample core-0 captures at 25 microseconds resolve
against the exact flashed ELF. Reference presenter-body residency is
18.2–18.3%. Candidate presenter body is 15.9–16.1%, plus 1.5–1.6% in the
new helper. These exclusive sample shares are consistent with less blend
work, not measurements of per-call speed. Renderer and idle shares vary;
no overall idle-time improvement is claimed.

For full-pipeline pixel checking, a deterministic guest fills VRAM once,
then stays in a self-loop. Entire 1280×720 LCD-buffer captures are
byte-identical between reference and candidate for both packed RGB and
indexed video. This checks real tap generation, scaling, blending, output
placement and clamped edges on P4. It is not a tearing or panel-timing test.

The P4 test and normal production builds pass. The production ELF has the
normal disk-selection namespace. The board is restored to the candidate
test app's ordinary II ROM disk-wait screen, without synthetic guest code
running or guest disks mounted. Production source has no temporary test
namespace or instrumentation change.

All 24 required builds pass (10 S3, 14 P4), including the normal Mac2
production build:

| Project | S3 | P4 |
| --- | --- | --- |
| esp32ste | pass | pass |
| esp32mac | pass | pass |
| esp32cpc | pass | pass |
| esp32zx | pass | pass |
| esp32cbm | pass | pass |
| esp32xl | pass | pass |
| esp32msx | pass | pass |
| esp32gs | pass | pass |
| esp32coco | pass | pass |
| esp32pc | pass | pass |
| esp32mac2 | unsupported | pass |
| esp32mac3 | unsupported | pass |
| esp32arc | unsupported | pass |
| esp32ami | unsupported | pass |

The S3 STE build places the 56-byte helper in IRAM. Sibling repository
statuses match their pre-build states; no tracked source/configuration
changes are introduced by the matrix. S3 compilation is not S3 hardware
performance validation; no S3 timing result is claimed.
OS/application, physical-input and audio-by-ear coverage remains open.

## Next target

Measure a 3:2 horizontal-scaling specialization. The current tap generator
repeats weights 26/16/5 in the interior for that ratio, with clamped first
and last pixels. Selecting a specialization at geometry changes could
remove per-pixel tap reads and variable-weight dispatch. It must preserve
integer rounding, source cropping, edge clamps, arbitrary widths and the
general fallback. No such change is included in this trial.

Scratch fixtures, full logs, firmware and disassembly stay outside the
repositories under `/tmp/rgb565-blend-2026-11-07`.

The subsequent [exact 3:2 horizontal trial](rgb565-3to2-experiment-2026-11-07.md)
tests that next target but rejects its integration after repeated CPU
throughput losses in both 3:2 and non-3:2 controls. The runtime stays at
this committed half-weight implementation.
