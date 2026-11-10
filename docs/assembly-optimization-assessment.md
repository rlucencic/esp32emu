# Assembly optimization assessment

Date: 2026-11-05. Static review of the 14 ESP32 emulator projects and shared
runtime. Priorities below are hypotheses from source inspection, not measured
speedups. Host assembly needs separate ESP32-P4 RISC-V and ESP32-S3 Xtensa
implementations where applicable; retain portable C as reference/fallback.

| Project | Potential | First candidates | Existing constraints / next steps |
| --- | --- | --- | --- |
| esp32ami | High | `amiga/display.c:draw` bitplane expansion, `compose`; `blitter.c:blit_area` | Expand multiple plane words with less intermediate traffic. Specialize copy, masked copy and common minterms in C before assembly. HAM pixels have sequential dependencies. |
| esp32arc | Medium | Shared `arm2/arm2.c` dispatch, shifter/ALU/flags; `archimedes/vidc.c:arc_render_line_rgb565` | Interpreter already has computed-goto dispatch, register-held execution state and direct pages. VIDC 4-bit conversion already uses paired lookup/store. Profile actual Tab5 workloads first. |
| esp32cbm | High shared CPU; medium graphics | Shared 6502 execution for machine and drives; `cbm/vic.c:char_pixels`, sprite composition; `sid.c:sample` | VIC uses 64-bit masks and cached character results. Measure SID separately; fixed-point conversion is a numerical change, not merely assembly. Preserve bus cycles and DMA. |
| esp32coco | Medium | `coco/gime/gime_video.c:graphics_line`, `widen`; `vdg/vdg.c:vdg_emit`; shared 6809 addressing/fetch/flags | Fuse decode and widening to remove temporary-buffer traffic before assembly. |
| esp32cpc | Medium | `cpc/video.c:pixels8`, `render_char`, Plus sprites; shared Z80 | Existing lookup tables and paired stores limit straightforward rewrite gains. Default video draws alternate frames. |
| esp32gs | High investigation priority | Shared 65C816 compact execution; `apple2gs/clemens/clem_audio.c:clem_ensoniq_sync`, oscillator processing; `video.c:shr_line` | Core already documents instruction-count and cache-footprint optimization. Excessive inlining was harmful; preserve compact loop. Profile active-voice audio separately. |
| esp32mac | Medium | `macintosh/video.c:mac_render_line_rgb565` monochrome expansion; Musashi/memory boundary | Try table expansion and packed stores first. CPU-bound applications need shared CPU work; simple audio is low priority. |
| esp32mac2 | High graphics | `mac2/video.c:lc_render_line`; `jmfb.c:jmfb_render_line` indexed and direct color | Specialize 1/2/4/8-bit expansion, RGB555/RGB565 and 24-bit conversion. Best depth depends on active workload. |
| esp32mac3 | High graphics | `mac3/video.c:video_render_line`, shared mac2/mac3 converters; Musashi/memory | Preserve alignment, mapping and VRAM change tracking. Static screens already skip work. |
| esp32msx | High for MSX2 graphics | `msx/vdp.c:draw_bitmap`, `cmd_start` copy/fill commands, sprites | Fuse addressing, palette lookup and packed stores. Preserve overlap direction, logical operations and command timing. MSX1 may benefit more from Z80 work. |
| esp32pc | High, complex | Shared x86 dispatch/ModR/M/RAM paths; `ibmpc/vga.c:render`, planar memory operations | Lazy flags and default chain-4 direct memory already exist. Preserve prefetch, protected-mode faults and device semantics. |
| esp32ste | High | `atari_st/video.c:render_line` 1/2/4-plane converters; `blitter.c:blit` | Current renderer extracts plane bits per pixel. Establish table/bit-transpose C baseline; specialize common blitter operations. |
| esp32xl | Medium | `atari_xl/gtia.c:draw_to`, `draw_gtia_mode` player/missile priority and collisions; shared 6502 | No-object paths already use tables and packed output. Preserve ANTIC DMA and mid-scanline timing. |
| esp32zx | Lower | `zx/ula.c:draw_paper_line`; shared Z80 | Unchanged scanlines already skipped. Packed expansion mainly helps animated scenes. Preserve contention and I/O timing. |

Shared `esp32emu` opportunities: Musashi (STE, Mac family, Amiga), Z80
(ZX, CPC, MSX, C128), 6502 (CBM machine/drives, XL), and presenter
`emu/emu.c:scale_row`. Framebuffer bandwidth may limit presenter gains.
`esp32emu-vscode` is debugger tooling, with no compelling assembly target.

Suggested bounded first experiments: STE planar conversion, Amiga bitplane /
blitter kernels, common mac2/mac3 color conversion. The user selected Acorn
Archimedes for the first actual hardware investigation instead.

## Current checkpoint (2026-11-09)

The [65C816 full-app A/B](../../esp32gs/docs/cpu-fetch-integration-2026-11-09.md)
shows static idle 44.9–45.2% → 50.1–50.2%, animated 25.4–25.9% → 33.5–33.9%,
and live Tour CPU share ~30.5% → 26.9–27.2%. The exact combined candidate is
applied for validation; all 24 production project/board builds and actual host
replays pass. Final reference C and MSX restoration are pending because Tab5
has disappeared from USB, though Wi-Fi still responds. The full backup is
preserved; final drift/restoration and S3 runtime passes are not claimed.

Earlier [fetch trial](../../esp32gs/docs/cpu-fetch-experiment-2026-11-09.md):
P4 mixed-code CPU kernel savings 18.7–20.3%, bus-only regression about 1.8%.

The [DOC helper integration](../../esp32gs/docs/doc-helper-scope-integration-2026-11-09.md)
is retained after full-app P4 validation (dense sampled DOC cost about
26–33% lower), exact native checks and S3/P4 builds. The subsequent
[metadata-layout trial](../../esp32gs/docs/doc-metadata-layout-experiment-2026-11-09.md)
is deferred: smaller code regresses the dense P4 scanline kernel about 3%.
Production DOC layout remains unchanged. S3 runtime gains remain unmeasured.

Earlier checkpoint:

Hardware investigations now cover Acorn, STE, Mac2, Mac3, Amiga, MSX2,
Commodore and Apple IIgs. The earlier
[normalization integration](../../esp32gs/docs/doc-normalization-integration-2026-11-08.md)
retains a 1 KB shared float table after exact native/state checks and P4
reference/candidate/reference validation. Dense static idle rises
31.0–33.2% to 35.4–37.5%, animated 12.8–13.4% to 16.7–17.6%, near the frame
cap. Mixer frame grows 16 bytes on both targets; S3 runtime remains
unmeasured. Next replay real software playing DOC audio using throwaway
media before another local arithmetic trial. The earlier
[IIgs pointer integration](../../esp32gs/docs/doc-pointer-integration-2026-11-08.md)
retains equivalent mask-select C, saving 50 linked P4 hot-function text bytes
and removing the oscillator's 16-byte frame, with exact native/state and
reference-bracketed P4 validation. The earlier
[VIC character trial](../../esp32cbm/docs/char-pixels-experiment-2026-11-08.md)
retains a 15–17% multicolor row-kernel saving and 24 fewer P4 text bytes,
without an established whole-app gain.
The Mac2
work retains paired indexed reads and a full-row RGB555 fast path; its
[LC report](../../esp32mac2/docs/lc-video-experiment-2026-11-07.md) records
converter savings, exact pixels and the small CPU-throughput tradeoffs.
The subsequent [Mac3 converter trial](../../esp32mac3/docs/video-optimization-experiment-2026-11-07.md)
retains paired indexed reads and masked RGB555 arithmetic with original
Sonora/V8 bounds and blanking. Converter costs fall, with measured adaptive
CPU-speed tradeoffs recorded separately from the frame cap. The
[Amiga planar trial](../../esp32ami/docs/planar-expansion-experiment-2026-11-07.md)
retains split 32-bit expansion entries: complete-row cost falls 3.1–12.5%
in active-plane cases, with 80 fewer P4 text bytes and unchanged table size.
Full synthetic throughput remains within 0.2% of reference; no application
speed increase is established. The subsequent
[blitter copy trial](../../esp32ami/docs/blitter-copy-experiment-2026-11-07.md)
reduces large-copy host cost about 65–67% and core-1 blitter sample share
from 19.6% to 7.0–7.1%, preserving guest timing. The OCS control measures
0.28–0.72% lower throughput; this tradeoff is retained and documented.
The [software follow-up](../../esp32ami/docs/software-profile-2026-11-07.md)
checks 96 games at 672 picture checkpoints and compares 230 final
disk/package files; all match baseline. Nine longer three-build runs also
match. The actual Aerial Racers race profile puts complete blitter work at
only 0.1–0.3% of core 1, so defer further blitter specialization and profile
MSX2 graphics next. The
[MSX2 bank-pair trial](../../esp32msx/docs/bitmap-banks-experiment-2026-11-07.md)
now retains paired SCREEN 7/8 VRAM reads: isolated P4 row costs fall
18.5–21.8% / about 38.5%, with increased core-1 idle headroom and exact
pixels. S3 and P4 builds pass; only P4 hardware was measured. The
[SCREEN 6 follow-up](../../esp32msx/docs/screen6-palette-experiment-2026-11-07.md)
now retains a separate four-color helper: mixed-row cost falls 37.9–39.5%,
with exact pixels and lower combined renderer sample share. The
[SCREEN 5/7 follow-up](../../esp32msx/docs/screen57-palette-experiment-2026-11-07.md)
retains a sixteen-color helper: isolated P4 row costs fall 20.6–26.7% /
24.2–28.3%, with exact LCD output and lower combined renderer shares.
The [command follow-up](../../esp32msx/docs/command-stride-experiment-2026-11-07.md)
retains byte-stride HMMV/HMMM iteration: large-command isolated P4 costs
fall 14.6–67.2%, with exact VRAM/state/LCD output and faster synthetic
stress programs. YMMM controls cost 1.3–2.5% more, and the common register
writer adds 104 text bytes and sixteen stack bytes. The
[logical-pixel trial](../../esp32msx/docs/logical-pixel-experiment-2026-11-07.md)
rejects two packed-bit rewrites: exhaustive checks pass, but selective wins
come with common COPY/NOT regressions. The retained implementation is
restored. The [MSX software follow-up](../../esp32msx/docs/software-profile-2026-11-07.md)
now compares six real-software replays: all 114 pictures, eighteen visible
memory/VRAM snapshots and six supported-audio captures match the original
VDP. Lilly's first-level SCREEN 4 gameplay stays near 60 FPS in both P4
builds, with slightly lower retained core-1 idle. Tiles/sprites occupy
about 41% of core 1; host command counts are zero after frame 3000 in the
tested replays. Tile and sprite follow-ups are recorded below. Broader software,
physical input and speaker coverage remain open. Shared S3 paths have been built; the P4 measurements do not
establish S3 runtime performance.

All percentages below describe individual trials against their stated
reference builds. They are not cumulative whole-emulator speed gains.

## Measurement method

- Capture baseline and candidate with the same compiler, settings, workload,
  ROM, CPU model and display mode; repeat and report variation.
- Use `tools/emu_console.py PORT profile ELF 20000 25 1` for emulation and
  repeat with core `0` for the presenter. The ELF must match flashed firmware.
- CBM has `CBM_PROFILE`, CPC has `CPC_PROFILE`, GS has `gs_profile` counters.
- Compare kernel costs and whole-frame time / real-time speed and load.
- Preserve emulated instruction counts, timing, interrupts, memory side
  effects and pixel output; use existing CPU cases and differential tests.
- Compare optimized C and assembly, inspect generated instructions, and
  reject changes that only help microbenchmarks or increase cache pressure.

Hardware findings and Acorn measurements will be recorded in
`../../esp32arc/docs/assembly-experiment-2026-11-05.md`.

## Acorn hardware follow-up (2026-11-05)

Tab5 testing selected CPU execution over static desktop rendering:
`arm2_run` was 35.8% of core 1 time (about 90% of its non-idle samples),
while core 0 was 94.5% idle. A P4 inline assembly subtraction/CMP NZCV
candidate passed 20,162 target checks but gave no useful improvement in
a steady guest ADD/CMP/BNE workload: C 59.5-60.5% sampled CPU residency
across the initial/restored runs, assembly 60.4-61.0%. Both kept real time.
The candidate grew `arm2_run` by 314 bytes and was rejected; the C core
and baseline firmware were restored. See the Acorn experiment document
for the patch, raw summaries, workload and measurement limitations.

The subsequent [operand-decode experiments](../../esp32arc/docs/operand-decode-experiment-2026-11-05.md)
tested three compact C changes on Lander and the guest loop. Grouping the
register operand's shift type and amount into one field was retained:
the minute-long comparison improved Lander from 98.294% to 99.003% real
time (0.721% relative throughput), for 14 bytes more interpreter code.
All 190 CPU checks, 100,000 differential instructions and the deterministic
host Lander replay passed. An immediate fast path and direct conditional
dispatch were rejected for lack of useful gain. This is a small measured
improvement for the tested workload, not a general assembly speedup.

The subsequent [flag-setting dispatch experiment](../../esp32arc/docs/flags-dispatch-experiment-2026-11-05.md)
split data-processing handlers by the S bit already present in the dispatch
index. This removes unused flag work from the generated code. Two fresh-boot
Lander runs per build improved pooled real-time speed from 99.122% to 99.589%
(0.471% relative throughput); paired gains were 0.752% and 0.192%. The change
was retained, with a 6,482-byte internal code RAM cost and no table growth.
All 190 checks, 100,000 differential instructions and the identical host
replay passed. The workload is near its 50 FPS cap, and the small observed
gain varies between runs. Fetch/dispatch is the next target: a merged
dispatch tail accounted for 8.46% of a separate baseline core 1 profile.

The [dispatch-tail compiler experiments](../../esp32arc/docs/dispatch-tail-experiment-2026-11-05.md)
then tested `-fno-crossjumping -fno-gcse` and `-fno-crossjumping` alone.
Both increased code and were rejected: Lander reached 95.753% and 99.628%
real time versus 100.009% for the baseline. The paced ADD/CMP/BNE loop
used 63.4% and 59.7% mean `arm2_run` residency versus 56.8% after restoring
the committed settings. Both passed CPU checks, differential tests and
the host replay; the restored firmware's interpreter exactly matches the
baseline. More dispatch sites did not improve these P4 workloads.

The [unshifted-register experiment](../../esp32arc/docs/unshifted-register-experiment-2026-11-06.md)
then combined bits 4–11 into one dispatch check and reused that decoded
field for shifted operands. The retained refinement reduces register-loop
mean `arm2_run` residency from 62.5% to 59.9%. On supported 8 MHz A440
Lander, where idle time makes headroom measurable, it reduces sampled
interpreter residency from 71.2% to 70.6%; both maintain real time. A3020
Lander remains near its 50 FPS limit. The change costs 4,700 bytes of code
RAM and passes 190 CPU checks, 100,000 differential instructions and the
identical host replay. Host mix reporting now counts unshifted operands:
20.4% of all Lander instructions and 51.2% of register-operand instructions.

The [block-transfer count experiment](../../esp32arc/docs/block-count-experiment-2026-11-06.md)
replaced the software 16-bit population count with two byte-table lookups
in internal RAM. The retained change reduces A440 Lander mean ARM2
residency from 70.82% to 70.10% and the eight-register LDM/STM loop from
42.80% to 41.97%. Both remain paced at real time; these are sampled CPU
cost reductions, not uncapped FPS gains. The cost is 256 bytes of data
RAM and 18 more bytes of interpreter code. All 190 CPU checks, 100,000
differential instructions, 262,144 exhaustive helper checks and the
identical host replay passed; target ELF table contents and internal-RAM
placement were independently verified.

The [contiguous-register copy experiment](../../esp32arc/docs/block-copy-experiment-2026-11-06.md)
then measured the block-transfer lists used by Lander. STM `{R0-R9}`
accounts for 41.3% of its stores; short R0-prefix lists also occur often.
The retained direct-page path recognizes contiguous lists starting at R0,
excluding PC, and copies them with a counted loop. A440 Lander mean ARM2
residency falls from 69.88% to 68.50% (1.97% relatively) while both builds
hold 50 FPS; `arm2_run` shrinks by 42 bytes. All 190 CPU checks, 100,000
Unicorn instructions, 8,192 targeted baseline comparisons and the
identical deterministic replay pass. Host mix reporting now lists the
most frequent LDM and STM masks to guide further workload-based changes.

The [single-transfer direction experiment](../../esp32arc/docs/address-direction-experiment-2026-11-06.md)
then used the dispatch table's existing bit 23 to select separate add/subtract
LDR/STR handlers. It passed 190 CPU checks, 100,000 Unicorn instructions,
65,536 targeted baseline comparisons including cycles and full PSR, and
the identical host replay. It was rejected: A440 Lander mean ARM2 residency
increased from 68.60% to 69.54% (1.37% relatively), while interpreter code
grew by 2,780 bytes. Both builds held 50 FPS. The committed source and
previously tested firmware were restored; specializing more dispatch
bits does not reliably improve this P4 workload.

The [condition lookup experiment](../../esp32arc/docs/condition-lookup-experiment-2026-11-06.md)
then removed eager condition-mask updates and read the condition table
only when a conditional instruction executed. It reduced interpreter
code by 2,764 bytes but showed no game CPU improvement: A440 Lander mean
ARM2 residency was 69.24% versus 68.64% for the baseline (0.87% relatively
higher). The candidate passed 190 CPU checks, 100,000 Unicorn instructions,
1,536 exhaustive condition/flag cases including in-run TEQP, and the
identical host replay. It was rejected and the committed core, header
and firmware were restored. Host reporting now counts condition fields;
34.8% of Lander instructions use a checked condition.

The [constant-condition dispatch experiment](../../esp32arc/docs/condition-dispatch-experiment-2026-11-06.md)
then selected fixed condition-mask bits through the dispatch table, while
retaining eager flag-cache updates. Both variants were rejected: A440
Lander mean ARM2 residency was 71.68% for all fourteen checked conditions
and 70.30% for EQ/NE alone, versus 68.70% baseline (4.34% and 2.33%
relatively higher). Interpreter code shrank by 834 and 2,072 bytes,
respectively. Both passed 190 CPU checks, 100,000 Unicorn instructions,
1,536 exhaustive condition cases and the identical host replay. The P4
disassembly confirmed shorter constant-bit checks, but the compiled game
workload regressed. The committed core and firmware were restored.

The [immediate-operand table experiment](../../esp32arc/docs/immediate-table-experiment-2026-11-06.md)
then predecoded all 4,096 byte/rotation combinations into a 16 KB internal
RAM table. It passed 190 CPU checks, 100,000 Unicorn instructions, 24,576
exhaustive value/carry checks, target ELF table verification and the
identical host replay. It was rejected: Tab5 A440 Lander fell to 23.170
FPS (46.340% real time), while the baseline held approximately 50 FPS.
Startup was also much slower. CPU frequency remained 360 MHz and the
page table remained internal; the precise cause of the large target
regression was not isolated. The committed core and firmware were
restored. Host reporting now counts nonzero immediate rotations: 36.4%
of Lander's immediate operands, with 63.6% using zero rotation.

The [branchless rotate check](../../esp32arc/docs/branchless-rotate-experiment-2026-11-06.md)
then replaced the rotate-zero conditional with explicitly masked shift
counts. Both forms compile to identical P4 interpreter bytes: 43,154
bytes, SHA256 `05d946e3fad29dab88f699c4c0a8e4586385e594576d7c7d1598809a8b33940d`.
No speedup is possible from this source rewrite in the tested build, so
the original source was restored without flashing the equivalent build.
The candidate passed 190 CPU checks, 100,000 Unicorn instructions, 18,270
rotate cases under UBSan, 24,576 immediate/carry cases and the identical
host replay. Compiler output should be checked before further attempts
to optimize this helper's zero-count handling.

The [fetch and register-selection experiments](../../esp32arc/docs/arm2-fetch-register-experiments-2026-11-06.md)
followed instruction-address profiling on A3020 Lander (95.59% of core-1
samples in ARM2). All three candidates were rejected. A cached host fetch
pointer raised A440 Lander mean ARM2 residency from 68.52% to 71.50%,
with 932 more code bytes. Loading Rn before the R15 check raised residency
from 68.70% to 71.40% with an unlikely hint and 70.60% without it; code
changed by +346 and -366 bytes. Each passed the existing 190 checks,
100,000 differential instructions, focused PC/register cases and the
identical host replay. The baseline core and firmware were restored.
Sharing rare data-processing PC writes and PSR updates was then tested
and rejected: interpreter plus helper code shrank by 12,382 bytes, but
combined ARM2 residency rose from 68.68% to 72.94% (6.20% relatively).
The comparison includes the outlined helper samples. All 190 checks,
100,000 differential instructions, 40,960 PC/PSR state comparisons and
the identical host replay passed. The original core and baseline firmware
were restored. Smaller code or fewer source operations alone do not
establish a gain.

Isolating PSR-only outlining also failed in two follow-ups: sharing
`run_write_psr` alone raised combined ARM2 residency from 68.68% to 73.62%,
and sharing only inner `write_psr` while retaining inline refresh raised
it to 71.52% (7.19% and 4.14% relatively). Interpreter plus helper code
shrank by 12,114 and 8,630 bytes, respectively. Both passed 190 checks,
100,000 Unicorn instructions, 40,960 PC/PSR state comparisons and the
identical host replay. These variants still alter dispatch-tail merging;
the measurements do not isolate call overhead. Both were rejected and
the baseline restored. Details are in the same experiment document.

Combining the inner PSR helper with `no-crossjumping` on `arm2_run` alone
was also rejected. It retains 111 indirect jump sites versus baseline 88
and reduces interpreter-plus-helper code by 4,006 bytes, yet A440 Lander
combined ARM2 residency rises from 68.42% to 73.40% (7.28% relatively).
Both hold 50 FPS. All 190 CPU checks, 100,000 Unicorn instructions, 40,960
PC/PSR state comparisons and the deterministic replay pass. More tails
and smaller code together still do not establish a gain; the measurement
does not isolate prediction, register allocation or layout. Original core
and saved baseline firmware restored; see the same experiment document.

The [block-copy follow-up](../../esp32arc/docs/block-copy-experiment-2026-11-06.md)
then extended contiguous copies to lists starting above R0, preserving the
accepted prefix path. It was rejected: A440 Lander mean ARM2 residency
rose from 68.62% to 69.90% (1.87% relatively), with 204 more code bytes
and both builds holding 50 FPS. All 190 checks, 100,000 Unicorn cases,
65,536 exhaustive list classifications, 222,720 block-transfer state
comparisons and the identical replay passed. The original core and exact
baseline firmware were restored; the accepted R0-prefix optimization
remains. Added detection and compiler layout effects were not isolated.

Narrow single-register paths for STM `{LR}` and LDM `{PC}` were also
rejected. They add 44 code bytes and preserve 88 indirect jump sites,
but A440 Lander ARM2 residency rises to 69.80% versus 68.68% from the
saved baseline and 68.84% from a fresh post-candidate baseline rebuild.
All 6,241 defined functions in the rebuilt control match the saved
firmware at identical addresses and bytes. The candidate passes 190
checks, 100,000 Unicorn instructions, 222,720 extended block-transfer
state comparisons (including mode changes and newly enabled interrupts)
and the identical replay. The original source and freshly rebuilt
baseline firmware were restored. Details are in the block-copy document.

An opt-in [whole machine-frame cycle capture](../../esp32arc/docs/frame-cycle-measurement-2026-11-06.md)
now supplements PC sampling without instrumenting ARM2 instructions.
It times `arc_run_frame`, including device work and any preemption, while
excluding runtime pacing. Re-testing the single-register paths with this
hook raises mean cost from 4.815 to 4.855 million cycles/frame (+0.835%),
with guest instruction counts within 0.038%. This supports the rejection
using a different metric; the percentages are not interchangeable with
sampled CPU residency. The CPU rewrite was reverted and the timing tool
retained for future comparisons.

A subsequent [plain LDR/STR register-offset fast path](../../esp32arc/docs/address-direction-experiment-2026-11-06.md#follow-up-plain-register-offsets-no-useful-gain)
also showed no useful gain. An early return for unshifted Rm adds 104
interpreter bytes; A440 Lander averages 4.808 million frame-call cycles,
versus 4.805 million initially and 4.799 million after restoring baseline.
The candidate costs 0.064–0.201% more cycles/frame, with overlapping
capture ranges and similar guest instruction counts. It passed 190 CPU
checks, 100,000 Unicorn instructions, 4,718,592 offset-helper reference
comparisons under UBSan and identical 2,000-frame host desktop output.
The source and saved baseline firmware were restored; the small timing
differences do not establish a precise regression.

The next retained change is in the machine's
[VIDC sound integration](../../esp32arc/docs/sound-accumulator-experiment-2026-11-06.md):
32-bit accumulators replace unnecessary 64-bit arithmetic, with a proven
maximum magnitude of 258,998,272. A440 Lander frame-call cost falls 0.329%
and 0.479% in two fresh-boot paired comparisons (0.429% pooled), while
both builds retain approximately 50 FPS. The longer pair has identical
guest instruction totals. P4 `sound_run` shrinks by 84 bytes and private
state by eight bytes. Baseline/candidate PCM and sound state match under
UBSan across extreme samples, all frequency settings, DMA transitions,
ring overflow and live writes; a 4,000-frame BASIC SOUND replay also has
identical WAV bytes, screenshots and instruction counts. ARM2 source is
unchanged. Linker address changes and whole-machine timing prevent
attributing every saved cycle solely to the mixer arithmetic.

Reusing the machine's existing
[VIDC palette lookup tables](../../esp32arc/docs/palette-cache-experiment-2026-11-06.md)
is also retained. Separate validity bits for 4-bit paired pixels and
8-bit colours are cleared by every palette write and reset; each copied
frame still owns its tables. On the sound-optimized baseline, A440 Lander
frame-call cost falls 0.524% versus the initial control and 0.577% versus
a freshly restored control, with guest instruction counts within 0.002%.
All five candidate windows are below all ten control windows, while
all builds maintain approximately 50 FPS. A one-byte validity variable
adds eight aligned BSS bytes and eight padding bytes in this link; the
affected VIDC functions grow by 94 bytes (128 linked flash-text bytes). Original
and candidate state/render traces match under UBSan across 131,072
palette-write cases and 548,864 snapshots, including mode changes,
resets and delayed copies. A 6,000-frame host replay also matches all
seven screenshots, device state and instruction counts. A fresh A3020
comparison also reduces frame-call cost by 0.200%, with guest instruction
counts within 0.0031% and all three candidate windows below all three
baseline windows. Both retain approximately 50 FPS. ARM2 and sound source
are unchanged; whole-machine results include linker effects.

A [write-refreshed VIDC line-timing cache](../../esp32arc/docs/line-cache-experiment-2026-11-06.md)
is retained next. Reset, HCR/control writes and IOEB clock writes now
refresh the whole-tick/remainder values, removing four dependency checks
from every timing query. A440 Lander frame-call cost falls 0.439% versus
the initial baseline and 0.222% versus a restored control; baseline means
vary by 0.218%, so the small gain should not be treated as a precise
general speedup. All candidate windows are below both controls. A fresh
A3020 pair is effectively tied: 0.097% lower mean cycles/frame is within
capture variation, with no demonstrated throughput gain. The timing
reader shrinks from 258 to 52 bytes, total linked flash-text size stays
unchanged and BSS shrinks by 16 bytes. UBSan timing/raster traces and a
6,000-frame host mode-switch replay match exactly. New timing-input
writers must refresh the cache; repeated timing writes now recalculate
at the write. ARM2, sound and palette sources are otherwise unchanged.

A subsequent [IOC timer-count divide experiment](../../esp32arc/docs/ioc-count-experiment-2026-11-06.md)
was reverted. A guarded 32-bit divide compiles to a hardware `divu`,
retaining the original 64-bit overdue/fallback paths, and passes more
than eight million timer/state reference cases under UBSan plus the
identical 6,000-frame host replay. A440 Lander frame cost is 0.255% higher
than the first baseline and 0.107% lower than the restored control;
the baseline means differ by 0.362%. There is no consistent useful gain
and no precise regression is established. Original IOC source and saved
baseline firmware were restored. All previously accepted changes remain.
A subsequent 32,000-sample restored-baseline scout still places 68.881%
of all core-1 samples in ARM2, with approximately 28.1% idle. The hottest
ARM2 PC is the shared tail's guest-opcode load; IOC arithmetic remains a
lower priority. This single profile is not a before/after speed comparison.

The subsequent [compact relative-dispatch experiment](../../esp32arc/docs/relative-dispatch-experiment-2026-11-06.md)
was also reverted. A checked P4 table replaces 4,096 absolute pointers
with signed 16-bit byte displacements, saving 8,192 static RAM bytes while
interpreter code grows 190 bytes. Every encoded entry matches the full-width
relocation address. Wider-relative host logic passes 190 CPU checks,
100,000 Unicorn comparisons under UBSan and the identical 6,000-frame
mode-switch replay; this is separate from the target-width ELF check.
A3020 Lander frame cost rises 5.007% against the initial baseline and
4.868% against a restored control, whose means differ by 0.132%.
The table's smaller footprint does not improve this workload. The extra
address dependency, register pressure and code-layout changes are not
isolated. Signed offset range would also require an enforced check for
other builds. Original core and saved firmware were restored.

The subsequent [pre-indexed transfer dispatch experiment](../../esp32arc/docs/preindexed-transfer-experiment-2026-11-06.md)
is retained. The existing table selects eight additional LDR/STR handlers
only for P=1/W=0, eliminating dynamic indexing/user-transfer/writeback
checks for that form. Direction and offset decoding remain unchanged.
A3020 Lander frame-call cycles fall 2.622% against the initial baseline and
2.429% against a restored control; A440 falls 2.270% and 2.498%, respectively.
Baseline means differ by -0.198% and +0.233%. All five candidate windows
are below all ten baseline windows on each model, with nearly unchanged
guest instruction counts and paced frame rates near 50 FPS. This compares
whole firmware and does not isolate individual removed checks from layout
or register-allocation effects. Interpreter code grows 2,550 bytes and
allocated internal code including padding grows 2,560; table/data and total
BSS sizes are unchanged. All 190 CPU checks, 100,000 Unicorn instructions,
2,883,584 UBSan baseline comparisons and the identical 6,000-frame host
mode-switch replay pass. The formatted source rebuild matches all 6,244
measured candidate functions at identical addresses and bytes.

The [current Acorn status and bounded remainder](../../esp32arc/docs/optimization-status-2026-11-06.md)
sets the remaining pass to normal post-indexed transfer specialization,
one fresh profile and at most one further distinct profile-supported CPU
candidate, followed by broader workload/regression coverage. Lander's
retained frame calls average 12.960 ms on A440 and 18.629 ms on A3020 at
360 MHz; outside-call runtime work is additional. There is no supported
percentage-complete estimate or guaranteed remaining speedup. Physical
speaker/input validation and unsupported-machine/device features are
separate from this performance pass.

The [normal post-indexed transfer trial](../../esp32arc/docs/postindexed-transfer-experiment-2026-11-06.md)
is rejected. Eight additional P=0/W=0 handlers pass all CPU checks,
100,000 Unicorn cases, 2,883,584 UBSan reference comparisons and the
identical host replay, but add 5,012 interpreter bytes (4,992 internal
code bytes including padding). The usual fixed-delay launch first fails
to reach Lander; a fresh retry with extended startup reaches the game.
Against a matched extended-startup restored control, frame-call cycles
rise 109.072% and FPS falls from 49.999 to 25.731, with guest instructions
per frame within 0.065%. All six matched captures are valid at 360 MHz.
Generated code, layout/register allocation and BSS placement are coupled;
no individual root cause is established. A440 performance is not tested.
The original committed pre-indexed core and saved firmware are restored;
a rebuild matches all 6,244 baseline functions at identical addresses/bytes.

A subsequent 32,000-sample A3020 scout on that retained build still places
95.891% of periodic samples in ARM2. The hottest PC, `0x4ff025d8`, is the
shared NEXT guest-opcode load. One burst is not an exact utilization or
speed comparison and does not justify reviving prior rejected fetch/dispatch
variants. The currently identified CPU micro-optimization trials are done;
broader workload/regression coverage is next, with a further bounded CPU
trial conditional on a distinct profile-supported hypothesis.

## Atari STE hardware follow-up (2026-11-06)

The first Tab5 scout used TOS 1.62 monochrome GEM with continuous cursor
motion: core 0 sampled 4.2% PSG synthesis versus 1.1% planar rendering;
core 1 sampled 8.1% `slow_read16`, 6.1% Musashi execution and 52.7% idle.
A fixed-volume PSG cache passed differential output/state checks and native
three-resolution desktop replays but was rejected: P4 kernel measurements
saved 3.37% on 64-sample fixed-volume calls while one-sample calls regressed
13–29% and envelope-only calls also regressed. Original STE source and
firmware were restored. Profile animated four-plane software before planar
work; profile active music and event-split call lengths before further PSG
work. These are workload-specific P4 findings, with no S3 result. See the
[STE test report](../../esp32ste/docs/optimization-tests-2026-11-06.md).

The subsequent [four-plane STE experiment](../../esp32ste/docs/planar-conversion-experiment-2026-11-06.md)
retains a four-pixel lookup decoder in portable C. A full-screen synthetic
guest animation first raises renderer residency to 8.1% of core 0. The
revised path cuts isolated P4 low-resolution rendering cost by 35.685%;
three full-firmware captures put renderer residency at 5.0–5.2% versus
7.3–7.5% for a fresh restored reference. This is not a whole-emulator or
FPS speedup. The one-/two-plane kernel costs rise about 0.76%, a documented
tradeoff; P4 renderer code grows 272 bytes plus a 32-byte table. All
347,680 UBSan scanline comparisons and the identical three-resolution
native desktop pixel/audio replays pass. Both full firmware targets build;
S3 runtime/performance and animated real-software coverage remain open.

The [one-/two-plane follow-up](../../esp32ste/docs/one-two-plane-experiment-2026-11-06.md)
retains direct monochrome bit extraction and two-plane nibble expansion
using the same table. Relative to the committed four-plane build, P4
isolated rendering costs fall 53.840% (medium) and 54.937% (monochrome),
with four-plane cost effectively unchanged. Medium renderer residency
falls from 13.6–13.7% to 6.6–6.8%; monochrome from 21.6–22.1% to
11.4–11.6% in three periodic full-firmware captures per build/mode. These
synthetic workloads do not establish game or whole-emulator speedups.
All 609,824 UBSan scanline comparisons and identical native three-mode
pixel/audio replays pass; both full firmware targets build. Renderer code
grows another 124 P4 bytes, reusing the same 32-byte table. S3 hardware,
animated real software and displayed update-rate coverage remain open.

## Macintosh LC / II hardware follow-up (2026-11-06)

The [first Mac2 video trials](../../esp32mac2/docs/video-optimization-experiment-2026-11-06.md)
use isolated, diskless synthetic guest fills. LC converter residency is
11.0% at 8 bits and 14.9% at 16 bits; shared presentation takes 41.2%
and 37.0%. The II converter is a stronger local target: 23.2% at 8 bits,
40.6% at 24 bits. Its 24-bit CPU writes also spend substantial time in
the slow memory path. No original disk images are mounted or changed.

An early 8-bit-only prefix is rejected for large 4-/24-bit regressions.
Moving the entire mode switch outside the pixel loop is retained in C:
isolated P4 converter costs fall 5.755%, 14.312%, 16.489%, 27.787% and
13.262% for 1/2/4/8/24 bits. Repeated full-firmware renderer residency
falls from 23.2–23.4% to 17.8–18.6% at 8 bits, and 39.2–40.5% to
36.6–37.4% at 24 bits. The short 24-bit captures stay around 33 emulated
FPS; these are not whole-emulator or System performance gains.

All 100,960 UBSan scanline/canary comparisons and identical native LC/II
ROM-boot pixels pass; full P4 test and production builds pass. The converter
shrinks 56 bytes without added tables or allocation. Mac2 is P4-only.
Next: separately measure packed RGB-to-RGB565 conversion and the II's
slow packed writes; LC paired indexed reads / RGB555 conversion remain
candidates. OS/application, physical input and audio-by-ear coverage is open.

The [paired packed-RGB follow-up](../../esp32mac2/docs/packed-rgb-experiment-2026-11-07.md)
on 2026-11-07 retains three native word reads per pair of RGB pixels.
Relative to the committed mode-selection build, isolated P4 24-bit converter
cost falls 40.108%; indexed medians stay within 0.1% of the reference.
Three full-firmware captures reduce converter residency from 36.3–36.8%
to 26.7–27.8%, without an overall FPS claim. All 153,453 UBSan scanline /
canary cases pass, including every RGB color in both pixel positions, mixed
pairs, two-byte alignment and VRAM boundaries. LC/II native ROM-boot
pixels match; P4 test and production builds pass. Cost: 122 extra bytes
outside IRAM, no table or allocation. Next measure the guest's packed
writes; the renderer change leaves that slow CPU memory path intact.

A profiler-free follow-up to the paired-RGB trial records two matched
30-second windows per build. Emulated throughput is 32.977–32.980 FPS for
the reference and 32.821–32.823 for the candidate (0.474% lower pooled).
Screen-signature increments rise from about 10.549/s to 12.837–12.882/s
(21.9% higher), a presentation-change proxy rather than panel FPS. Retained
for rendering, with that small observed CPU-throughput tradeoff; no broad
statistical confidence or application performance claim.

The [packed CPU-write follow-up](../../esp32mac2/docs/packed-write-experiment-2026-11-07.md)
on 2026-11-07 handles aligned slot-9 RGB longwords as one three-byte store.
The original unused byte lane, partial final pixel and dirty-flag behavior
are preserved; other accesses retain their fallback. The differential
source fixture passes 5,345,760 writes under UBSan and repeats successfully
with heap-backed VRAM under ASan plus UBSan. LC/II native ROM-boot pixels
match, and P4 test and production builds pass. Added text is 176 bytes,
with no table or allocation and no shared runtime source change.
Profiler-free RGB fill throughput rises from 32.785 to 51.536 emulated FPS
(+57.2%); indexed control stays at 60.15 FPS. There is a presentation
tradeoff: screen-change signature increments fall from 12.848 to 10.420/s
(−18.9%). These are dirty-screen detections, not panel FPS. Retained for
the CPU gain, without a visible-update or general application speed claim.
Next investigate rendering/presentation with the faster writes and real
software on copied disks; LC indexed/RGB555 conversion is still a local
candidate. OS/application, physical input and audio coverage remains open.

The [presentation counter follow-up](../../esp32mac2/docs/presentation-followup-2026-11-07.md)
clarifies that tradeoff. With temporary machine-local wrappers, the fast
RGB fill raises source-row drawing calls from 552.46 to 841.21/s (+52.3%),
while changed-screen passes fall from 12.649 to 10.302/s. Converter time
per call stays near 42 microseconds. Inserting three guest NOPs per write
matches the reference write rate to 0.103%; pass frequency returns to
12.659/s and drawing row calls to 555.27/s. More changed-row work per pass,
rather than slower conversion, explains the lower counter. This is not a
panel FPS or smoothness result. No further production code is retained.
Next target: shared `scale_row` / `blend565` and vertical RGB565 blending;
preserve exact pixels and unchanged-row skipping, then run the shared
build matrix for any retained runtime change. Real-software coverage on
copied disks and LC converters remain open.

The [uniform half-weight RGB565 trial](rgb565-half-blend-experiment-2026-11-07.md)
retains an isolated vertical averaging loop selected once per row. On P4,
960-pixel half-weight rows cost about 51–52% less for mixed/palette colors
and 14% less for equal colors, with measured other-weight controls within
0.02%. One-multiply and per-pixel half-weight variants were rejected.
Actual-source ASan/UBSan checks pass 18,504,256 color/weight cases,
12,194 horizontal rows and 7,260 vertical row/canary cases; full P4 LCD
captures match exactly in RGB and indexed modes. Added P4 text: 72 bytes,
no table or allocation. Two full RGB runs show a +3.63% dirty-screen
signature proxy and −0.31% emulated CPU-frame throughput; indexed CPU
throughput remains at 60.15 FPS. No panel-FPS or general application gain
is claimed. All 24 sibling builds pass (10 S3, 14 P4); S3 timing remains
unmeasured.
Next investigate exact 3:2 horizontal scaling with fixed tap weights and
clamped/cropped edges; LC converters and real-software coverage remain open.

The [exact 3:2 horizontal RGB565 trial](rgb565-3to2-experiment-2026-11-07.md)
is rejected at integration. Its final P4 kernel reduces isolated row cost
about 46% on mixed colors and 35% on flat colors. Actual-source ASan/UBSan
checks pass 245,760 cropped row/canary cases and 48,776 dispatch/fallback
cases; full RGB, indexed and fallback LCD captures match. All 24 candidate
builds pass. However, two full RGB runs show +8.08% dirty-screen signature
increments and −1.56% CPU-frame throughput. Two non-3:2 fallback runs also
lose 1.27% CPU throughput, despite never using the new kernel. Indexed
CPU throughput stays at 60.159 FPS. Correct pixels and faster eligible rows
do not justify that general-path cost. Restore runtime `784bba1`; retain
only documentation. No panel-FPS or general application gain is claimed.
Next try direct conditional dispatch preserving the old inlined general
loop; LC paired indexed reads and RGB555 conversion remain local targets.
Real-software tests on copied disks and S3 hardware timing remain open.

The [direct-dispatch follow-up](rgb565-direct-dispatch-experiment-2026-11-07.md)
retains a boolean geometry selector and direct specialized call while the
compiler preserves the general loop inline. The P4 fallback control stays
within 0.05% of the reference, avoiding the measured indirect-version cost.
Two eligible RGB runs show +11.13% dirty-screen signature increments and
−1.13% emulated CPU-frame throughput; indexed CPU throughput stays about
60.15 FPS. Retained for graphics, with that CPU tradeoff and no panel-FPS
or general application-speed claim. All 245,760 cropped row/canary checks,
48,776 dispatch/fallback checks, full RGB/indexed/fallback LCD comparisons
and 24 sibling builds pass. P4 adds 344 text bytes, no tables or allocation.
S3 timing is unmeasured. Next: LC paired indexed reads and RGB555 conversion,
plus representative software on copied disks.

The [LC converter follow-up](../../esp32mac2/docs/lc-video-experiment-2026-11-07.md)
retains paired 8-bit source reads and a full-row RGB555 bounds fast path,
keeping the original partial-row fallback. Isolated P4 cost falls
23.1–23.3% at 8 bits and 37.3–39.2% at RGB555; 1/2/4-bit controls stay
within 0.13%. All 317,280 native row/canary cases per candidate, four
LC/II host ROM-boot pixel comparisons, full LCD captures and P4 builds pass.
Full-firmware converter sample shares fall from 11.9–12.9% to 10.1–10.6%
at 8 bits, and 15.1–15.8% to 9.8–10.2% at RGB555. Signature observations
rise 7.05%/6.28%, with CPU-frame throughput −0.18%/−0.33%. These are graphics
tradeoffs, not panel-FPS or general application-speed gains. Cost: 68 P4
text bytes, no table or allocation. Shared runtime source is unchanged;
Mac2 is P4-only. Next profile the Mac3 LC II/III converter independently,
preserving Sonora row strides and whole-row bounds/blanking semantics.
Copied-disk software, physical input and audio coverage remain open.

The [Mac3 video converter trial](../../esp32mac3/docs/video-optimization-experiment-2026-11-07.md)
retains paired 8-bit native reads and simpler RGB555 masks; the pre-existing
whole-row bounds checks, Sonora packed strides and blanking are unchanged.
P4 kernel costs fall 23–29% at 8 bits and about 19% at RGB555; other-depth
controls stay within 0.05%. All 1,246,912 ASan/UBSan row/canary cases per
candidate, four host ROM-boot pixel comparisons, four full LCD comparisons
and P4 builds pass. Added text: ten bytes, no table/allocation. Converter
sample shares fall in all four workloads. Primary signature rates rise
9.35% (LC II indexed) and 7.67% (LC III RGB555); separate thirty-second
windows show mean adaptive CPU speed 0.58% and 1.77% lower. The near-60 FPS
cap is not a CPU-speed result, and signatures are not panel FPS. Retained
as a graphics tradeoff, without a general application-speed claim. Mac3
is P4-only and shared runtime source is unchanged. Next profile Amiga
bitplanes/blitter work; Mac3 CPU/MMU needs separate, representative tests.


The [Amiga planar expansion trial](../../esp32ami/docs/planar-expansion-experiment-2026-11-07.md)
retains split 32-bit lookup halves, avoiding unnecessary variable 64-bit
shift work on RV32. Complete-row costs fall 3.1–12.5% for active planes;
zero-plane control stays within 0.11%. Render text shrinks 80 bytes and the
lookup remains 2 KB. All 163,328 ASan/UBSan full draw/index/canary comparisons,
524,288 independent word/plane oracle cases, fourteen generated-disk host
picture comparisons, existing timing/pattern checks and three full LCD
comparisons pass. P4 firmware and host floppy/chip-bus checks pass. Renderer
profile share falls in all three hardware modes. Profiler-free OCS/AGA
frame throughput changes −0.047%/−0.161%, with no whole-emulator speed gain
claimed. Amiga signatures are content hashes, not update counters. Local
P4-only source changes leave shared runtime unchanged. Next investigate
an unshifted blitter A-to-D copy fast path with exact masks, overlap,
wrapping and busy timing; representative software coverage remains open.


The [Amiga blitter copy trial](../../esp32ami/docs/blitter-copy-experiment-2026-11-07.md)
retains a guarded A-only, unshifted A-to-D path. Word-by-word access preserves
masks, direction, overlap, chip wrapping and register side effects; original
busy/interrupt/steal scheduling is unchanged. Large-copy P4 kernel cost
falls 65–67%, tiny 2×2 copies 31%; measured fallbacks show no slowdown.
All 100,000 ASan/UBSan memory/register/timing-state comparisons, fourteen
generated-disk host pixel comparisons, timing/pattern regressions, two
full LCD comparisons and P4 builds pass. Added text in blitter `start`:
302 bytes, no table/allocation. Full-firmware copy sample share falls from
19.6% to 7.0–7.1% of core 1, idle rises from 47.7% to about 60%, while copy
throughput remains about 474/475 copies/s and 49.9 emulated FPS. OCS control
throughput is 0.72% lower initially and 0.28% lower in the fresh-boot
reverse-order repeat. Retain for copy headroom with that measured cost;
no general application-speed increase is claimed. Shared runtime source
is unchanged. Next profile cookie-cut/minterm usage on representative
copied-disk software before adding more guarded paths.


The [Amiga software follow-up](../../esp32ami/docs/software-profile-2026-11-07.md)
compares both recent optimizations with the pre-planar/pre-copy host build.
All 96 games reach 7,000 frames; all 672 paired picture checkpoints and
230 final disk/package files match. Nine selected titles reach 15,000
frames on baseline, optimized and temporary census builds with identical
pictures at all 27 three-way checkpoints, including Aerial Racers' race.
The host census finds substantial cookie-cut word shares, including shifted
ABCD and A-disabled configurations. Word counts are not execution costs:
on the Tab5 race, complete blitter `start` accounts for only 0.1–0.3% of
core-1 samples, versus 14.6–15.1% instruction reads, 12.2–12.8% CPU dispatch,
8.1–8.6% display fetching and 7.0–7.9% audio-line work. Core-0 `render`
accounts for 24.6–25.9%, with 52.8–54.3% idle. These are optimized-build
profiles, not A/B speed comparisons. Defer another Amiga blitter fast path;
continue with MSX2 bitmap rendering/VDP commands. No production probe or
source change is retained from the census. Game/menu checkpoint agreement
does not establish complete gameplay, physical-input or audio correctness.


The [MSX2 paired-bank trial](../../esp32msx/docs/bitmap-banks-experiment-2026-11-07.md)
retains a two-byte loop for SCREEN 7/8: compute the physical row base once,
then alternate matching offsets in the two 64 KB VRAM banks. Palette,
transparency, byte masking, sprites, commands and dirty-frame control are
unchanged. All 262,144 ASan/UBSan pixel/canary cases, 262,144 independent
address checks, ten host pictures and four full P4 LCD comparisons pass.
S3, P4 test and normal P4 builds pass. Isolated P4 row cost falls 18.5–21.8%
in SCREEN 7 and about 38.5% in SCREEN 8. Full core-1 `vdp_line` shares fall
40.7–41.7% to 38.5–38.8% and 34.4–34.7% to 27.7–28.2%, respectively;
idle rises. Unchanged SCREEN 5/6 controls show about half a percentage
point higher `vdp_line` share with overlapping idle ranges; isolated
SCREEN 6 cost rises 0.47%, SCREEN 5 falls 3.85%. Added P4 function text:
398 bytes, no new table/allocation. SCREEN 8 remains at about 59.9 emulated
FPS, near its cap: no panel-FPS or application-speed gain is claimed.
Shared source is unchanged, S3 runtime and real-software coverage remain
open. Next try row-level effective palette entries for SCREEN 6's four
colors, keeping backdrop/transparency exact; command workloads need their
own profile.


The [MSX2 SCREEN 6 palette trial](../../esp32msx/docs/screen6-palette-experiment-2026-11-07.md)
retains a separate no-inline helper that resolves zero/backdrop/TP once
per row, then decodes through four local colors. Mixed P4 row cost falls
37.9–39.5%, flat-byte cases 15.6–29.1%; three isolated controls remain
within 0.13%. An initially inlined form is rejected after increasing
SCREEN 5/7 kernel costs 2.34%/0.94% and enlarging the common stack frame.
The selected helper keeps that frame at 64 bytes, adding 16 stack bytes
only while SCREEN 6 executes; combined P4 text shrinks 318 bytes. All
262,144 ASan/UBSan differential rows, 16,384 independent oracle rows,
ten host pictures, four full P4 LCD comparisons and S3/P4 builds pass.
The comparable core-1 renderer share (`vdp_line` plus helper) falls from
33.9–34.4% to 27.6–27.9%, idle rises from 37.5–38.8% to 43.2–44.9%, and
emulated FPS stays about 59.9. Full-firmware control shares vary, including
lower SCREEN 5 idle despite similar renderer share; no zero-cost control
or general application-speed claim is made. No new global table/heap
allocation or shared source change. Next measure sixteen-color row
palettes for SCREEN 5/7; VDP commands need their own workload, and S3
runtime/game/input/audio coverage remains open.

The [MSX2 SCREEN 5/7 palette trial](../../esp32msx/docs/screen57-palette-experiment-2026-11-07.md)
retains a separate sixteen-color row helper. It copies the palette and
resolves index zero's backdrop/TP rule once per row, preserving masked
VRAM reads and paired-bank ordering. P4 row cost falls 20.6–26.7% in
SCREEN 5 and 24.2–28.3% in SCREEN 7; SCREEN 6/8 isolated controls stay
within 0.2%. Combined renderer text shrinks 286 bytes. SCREEN 5/7 uses
sixteen more stack bytes during its helper call, while the maximum across
bitmap paths stays 80 bytes; there is no new global table or allocation.
All 262,144 ASan/UBSan differential rows, 32,768 independent oracle rows,
ten host pictures, four full P4 LCD comparisons and S3/P4 builds pass.
Comparable core-1 renderer shares fall from 25.5–26.5% to 23.6–23.8% in
SCREEN 5 and from 37.0–37.2% to 33.6–33.9% in SCREEN 7. Idle rises in
both; SCREEN 6/8 controls remain similar. Core-0 SCREEN 7 idle is slightly
lower (77.4–79.5% versus 76.0–78.1%), with presentation share 3.1–3.4%
versus 3.4%; no presentation benefit or cause is established. Throughput
stays about 59.9 emulated FPS near its cap, so no general application
speedup is claimed. Next profile generated VDP command fill/copy workloads
before choosing a command optimization. Shared source is unchanged;
S3 runtime, representative software, physical input and audio remain open.

The [MSX2 command-stride trial](../../esp32msx/docs/command-stride-experiment-2026-11-07.md)
retains HMMV/HMMM loops that advance directly between packed-byte accesses,
removing intervening pixel iterations and their remainder test. Direction,
clipping, partial/default widths, wrapping, overlap order and complete
command/register/status behavior match reference. All 65,248 ASan/UBSan
full-VRAM/state/canary comparisons, 58,224 independent byte-command
oracles, eighteen host pictures, four full P4 LCD comparisons and S3/P4
builds pass. Eight large-command isolated P4 costs fall 14.6–67.2%; reverse
partial/overlap cases fall 37.9–61.8%. Logical controls improve 2.5–6.7%
in this build, while unchanged YMMM controls cost 1.3–2.5% more. P4
`reg_write` grows 104 text bytes and its common frame grows from 112 to
128 stack bytes, with no new persistent state/allocation. Synthetic
SCREEN 6 fill throughput rises from about 1.13 to 3.29 emulated FPS;
SCREEN 7 copy rises from 0.53 to 0.80. These deliberately overloaded
programs remain command-bound and are not representative software speed
results. Short low-frame-count windows limit precision. More guest frames
also increase presentation work; no panel-FPS benefit is claimed. Commands
still complete synchronously, preserving existing emulation timing rather
than adding hardware accuracy. Shared source is unchanged. Next investigate
logical-command `pset`/`point` work using existing controls, with exhaustive
operation/transparency and ordered-overlap coverage. S3 runtime, games,
physical input and speaker checks remain open.

The [MSX2 logical-pixel trial](../../esp32msx/docs/logical-pixel-experiment-2026-11-07.md)
rejects two packed-bit rewrites of `pset`; no production code is retained.
Both pass 25,165,824 ASan/UBSan independent packed-byte/pixel cases,
67,296 complete VRAM/state/canary command comparisons, 58,224 byte-command
oracles and 49,920 CPU-transfer checkpoints. The first form also matches
eighteen host pictures and builds as a full P4 test app. Its ordinary
OR/XOR copies cost about 1.9–7.2% less, but several COPY/NOT paths cost
about 1% more and transparent-zero fills regress up to 4%. A revised form
delays physical addressing until after the transparency return: those
zero fills cost 55.0–61.3% less, but ordinary COPY costs up to 7.1% more
and SCREEN 6 NOT fill costs 8.5% more. The first linked form reduces combined
P4 helper/dispatcher text by 56 bytes; comparable component objects show
a 96-byte reduction for the revised form, without stack growth. Earlier
mixed object/linked accounting was corrected on 2026-11-08; smaller code
does not establish faster common paths. The revised form is kernel/object
and native-tested only; neither rejected form receives S3 or candidate
full-firmware timing/LCD comparisons. All fifteen reference profiles have
the requested 32,000 samples/core. Production source, normal P4/host builds
and Tab5 BASIC are restored to the retained implementation. Next profile
representative MSX software on copied images, with generated fixtures as
controls, before choosing more command specialization. S3 runtime,
physical input and speaker checks remain open.

The [MSX real-software follow-up](../../esp32msx/docs/software-profile-2026-11-07.md)
compares the pre-optimization VDP (`c486454`) with the retained one
(`5fc331a`), using the same host machine/runtime sources. Four official
releases produce six MSX1/MSX2 cases: Lilly's Saga, Zevimodoki's disk,
The Menace from Triton and Tales of Popolon. All 114 picture pairs,
eighteen visible-CPU-memory/VRAM checkpoints, periodic state logs and six
supported-audio WAVs match. A temporary host counter build matches those
outputs too. Lilly, Zevimodoki and Popolon reach gameplay; Triton's replay
covers intro/galaxy menu only. Zevimodoki's 64 KB ROM returns to BASIC in
both builds; the disk works. This boot limitation predates the VDP changes.

After frame 3000, host counters record SCREEN 4 rows for the two MSX2
titles, SCREEN 2 for the MSX1 controls and no VDP command/pixel operations
in any case. Lilly uses SCREEN 5 and launches twenty HMMV / 43 HMMM before
that interval, so previous kernel savings are relevant to presentation
work without establishing a gameplay speedup. On Tab5, original/retained
Lilly first-level entry pictures and visible 64 KB memory match exactly.
Both stay near 60 FPS (pooled 59.928/59.959); retained core-1 renderer share
is slightly higher (40.5–41.2% versus 41.2–41.9%) and idle lower
(34.8–35.4% versus 33.0–34.6%). Record this control tradeoff; short runs do
not isolate its cause or establish application throughput differences.
All twelve 32,000-sample captures have the requested count/core labels.
Core-0 presentation is 3.0–3.2% in both builds. Next measure SCREEN 4 tile
expansion (`pattern8`) and sprites, preserving the real-game replay and
bitmap/command controls. No production code changes. The temporary board
cartridge is removed and retained MSX2 BASIC restored. Later levels,
MSX-Music, physical controls/speaker output and S3 runtime remain open.

The [MSX doubled-tile follow-up](../../esp32msx/docs/tile-pairs-experiment-2026-11-08.md)
replaces each doubled RGB565 tile pixel's two halfword stores with an
aligned word copy, outlining and unrolling the eight-pixel helper. P4
aligned tile kernels cost 17.4–26.3% less; the halfword-only fallback is
about 12% slower, while normal framebuffer rows are word-aligned. Linked
text grows 314 bytes and the common line frame 32 bytes. The first inline
variant gives mixed results and is not selected. Retain the second variant.
Native tile/pattern and
bitmap checks, all six real-software replays and S3/P4 builds pass.

Lilly's valid Tab5 repeat lowers combined core-1 renderer share from
41.3–42.1% to 38.8–39.1%, counting the new `pattern8` helper. Core-0
presentation stays 3.1–3.3%; pooled frame rates remain about 59.93 FPS,
with no established FPS gain at the 60 Hz cap. An earlier stale-LCD run
had no presenter samples and is excluded, not counted as a speedup.
The report records its unresolved cause and subsequent display checks.
SCREEN 6/8 static LCD pixels match exactly; dynamic control renderer
shares show no consistent regression, with one lower-idle SCREEN 8 sample.
The temporary cartridge is removed; the normal app is restored to MSX2
BASIC and its LCD verified. No shared production code changes.
Next profile sprite scanning and
compositing, preserving priority, overflow and collision semantics.
S3 runtime, later stages, MSX-Music and physical input/audio remain open.

The [MSX sprite bounds trial](../../esp32msx/docs/sprite-bounds-experiment-2026-11-08.md)
bounds the final 256-pixel composite pass while retaining sprite scanning,
priority, combine, collision and overflow handling. More than two million
native differential/oracle cases, six software replay pairs and S3/P4
builds pass. Captured P4 rows cost 10.55% less; sparse rows save 31–34%,
spread rows gain little, and empty lists regress 1.44%. Cost is +62 bytes
text with unchanged stack. Fresh Lilly gameplay has overlapping renderer
shares (38.4–38.8% / 37.7–38.5%) and stays near 60 FPS. Retain it for the
kernel result, without claiming a significant whole-app speedup or S3 gain.

That trial also exposes a JTAG-confirmed Tab5 PPA completion wait and an
independently reproduced presenter hash/snapshot race. The shared
[display fixes](display-reliability-2026-11-08.md) use eight-row flush
boundaries and cache the hash of the source actually scaled. Actual-source
ASan/UBSan regressions and all 24 project/board builds pass. New sprite
baseline/candidate runs both include these fixes; older runtime timings
are not directly compared. Before further assembly work, obtain S3 timing
or profile another emulator's real workload; later MSX game stages,
MSX-Music and physical input/speaker coverage remain open.

The [Commodore baseline](../../esp32cbm/docs/profile-baseline-2026-11-08.md)
continues the survey with C64/C128 controls and an original animated
character/eight-sprite/three-voice C64 workload. The generated case runs
at 48.77–48.85 FPS on Tab5; DWARF inline-frame attribution places
44.88–45.03% of core 1 in VIC `sprite_collisions`, versus 11.72–12.32% in
graphics conversion and 16.19–16.50% in line commit. These nested source
shares come from separate complete captures; do not add them to flat
symbol shares. SID synthesis stays much smaller on core 0. Select a
bounded collision-scan C experiment before assembly or shared CPU changes,
with independent collision/IRQ oracles and later real-software replay.
No production emulator change is made in this baseline. The generated
workload is reproducible but does not represent ordinary game costs.

The [Commodore collision-span follow-up](../../esp32cbm/docs/sprite-collision-experiment-2026-11-08.md)
retains clipped active descriptors for the VIC collision pass. Full-width
P4 kernel cost falls 43–96%; eight dense sprites over four pixels cost
7.8% more. The linked draw function adds 170 text bytes and 80 stack bytes.
819,200 native differential/oracle cases, 16,384 split-call properties,
seven host replay pairs, S3/P4 builds and C64/C128 board controls pass.
The generated workload rises from 48.71–48.75 to 50.05–50.17 FPS, with
31–32% core-1 idle. This is a synthetic gain, not a game-wide speed claim.
Fresh candidate inline attribution puts collision work at 5.49–7.01% and
line commit at 18.67–18.80%: profile `commit_line`'s hash and RGB565
conversion next. Shared production C is unchanged. S3 runtime and real-game
performance remain unmeasured; the attached Tab5 is restored to its pre-test
MSX firmware/settings.

The [Commodore row-conversion follow-up](../../esp32cbm/docs/vic-conversion-experiment-2026-11-08.md)
attributes about 89% of line-commit samples to RGB565 conversion and 10% to
hashing. Advancing pointers and processing eight pixels per iteration cuts
changed-row P4 kernel cycles by 29.65–30.06%, at +74 text bytes with no stack
frame or new RAM. Two- and four-pixel loops save about 19% and 26%.
73,760 native oracle/differential cases per variant, the existing collision
regression, seven exact host replay pairs and S3/P4 builds pass. A fixed
hardware checkpoint matches all 64 KB of CPU-visible memory and guest pixels.
The generated workload stays near the 50 FPS PAL cap: line commit falls
from 18.5–19.3% to 15.1–15.3% of core 1, and idle rises from 30.6–32.2% to
34.4–35.3%. This supports headroom in this synthetic case, without a game-wide
or S3 runtime claim. Shared production code is unchanged.
C64/C128 40-/80-column title controls match, all 36 new full-app captures are
complete, and Tab5 is restored to its previous MSX firmware/settings. Next
obtain real C64 game coverage or S3 runtime timing before another low-level
optimization; hashing is a much smaller cost in this workload.

The [ARE-TYPE game follow-up](../../esp32cbm/docs/aretype-validation-2026-11-08.md)
compares the original VIC, collision-span build and final conversion build.
Three 6,000-frame host runs match pictures, full visible memory, PCM and CPU
status through gameplay, game over and restarts. Two fixed P4 checkpoints
match all three builds' memory and pixels. Live sessions stay near 50 FPS;
original draw/idle ranges are 35.0–51.1% / 17.8–39.7%, versus final
17.9–21.4% / 47.8–51.9%. Scene/state timing varies, so these support observed
game headroom without a precise kernel-speed ratio. The collision-only and
final ranges overlap: no additional game-wide conversion gain is isolated.
Final inline attribution places `char_pixels` at 4.73–10.71%, within graphics
generation's 5.29–11.82%. Select its multicolor mask/palette expansion for the
next bounded portable-C experiment, separating arithmetic from VRAM access
before considering assembly. All 21 new board captures are complete; no
production code changes in this validation. Broader games and S3 runtime
remain open.
The attached Tab5 is restored to its saved MSX firmware/settings, with flash
hash verification and matching guest/LCD captures.

The [VIC character-expansion trial](../../esp32cbm/docs/char-pixels-experiment-2026-11-08.md)
retains four local 16-bit palette-index pairs. Multicolor P4 character cost
falls 18–21% and full graphics-window cost 15–17%; representative partial
spans also improve. Small control regressions remain, including 1.52% in
hires arithmetic and 2.59% in an invalid/display-off full window. Full P4
draw text shrinks 24 bytes with its 272-byte frame unchanged. Each candidate
and the selected production source pass 17,039,360 native oracle/differential
cases; existing replays, ARE-TYPE replays and S3/P4 builds pass. Fixed hardware
memory/pictures and clean C64/C128 title controls match. A queued-typing
C128 harness attempt is excluded and rerun with inputs cleared.
Fresh generated-workload idle ranges overlap (34.6–37.6% / 34.6–37.5%);
retain for kernel/footprint gains without a whole-app or S3 runtime speed
claim. All 33 new profile captures are complete. Resume the project survey
with Apple IIgs active-voice audio and 65C816 profiling before another local
C64 kernel experiment; broader games and S3 timing remain open.
Tab5 is restored from a fresh full-flash backup with hash verification;
final guest/LCD captures match MSX Disk BASIC outside the cursor area.

The [Apple IIgs DOC/SHR baseline](../../esp32gs/docs/profile-baseline-2026-11-08.md)
adds original 0/1/8/32-voice free-running fixtures and animated 320-/640-mode
controls. Six native cases pass twice with exact PCM/VRAM/RGB565 hashes;
Tab5 guest GLU readbacks and full source-pixel checks pass. Seven cases run
59.17–59.76 FPS, with zero additional missing speaker frames in fourteen
steady timing windows. All 48 captures contain 32,000 samples each.
32-voice DOC scan/advance/mix occupies 34.5–35.5% of core 1 in the static
case. Separate inline captures put waveform-pointer calculation at
8.38–8.65% static and 9.18–9.93% animated, within oscillator advancement;
SHR expansion is 2.34–2.39% in animated-640. Select equivalent portable-C
waveform-address arithmetic next, followed by mode/IRQ/state differential
coverage and hardware A/B before retaining a change. Normal S3/P4 builds
pass; S3 runtime and real software remain open. No production change is
made in this baseline.
Tab5 is restored from its fresh full-flash MSX backup with hash verification;
MSX Disk BASIC guest and LCD captures exactly match the restored control.

The [IIgs pointer arithmetic trial](../../esp32gs/docs/doc-pointer-experiment-2026-11-08.md)
compares combined-shift arithmetic, XOR masked selection and table masks.
All pass 6,242,880 independent oracle cases each and complete DOC state,
IRQ/mixing differential checks across 224 states / 57,344 events per variant
under ASan/UBSan. 216 P4 ABBA cases select masked selection: 13.28–13.30%
fewer pointer-kernel cycles in warm internal RAM/PSRAM, versus about 10% for
the others. Normal project flags yield 50 fewer P4 audio-module object text
bytes and a 16-to-0-byte oscillator frame; S3 text falls 18 bytes with stack
unchanged. Production remains unchanged. Next run full-emulator native
PCM/state/pixel replay, full S3/P4 builds and P4 sparse/dense/animated A/B
before retaining the candidate. No whole-app or S3 runtime gain is claimed.
Tab5 is restored from a fresh full-flash MSX backup with hash verification;
MSX Disk BASIC guest/LCD controls match outside the cursor rectangle.

The [IIgs pointer integration](../../esp32gs/docs/doc-pointer-integration-2026-11-08.md)
retains the selected mask merge. Six native reference/candidate replays match
PCM, video RAM, RGB565, CPU fields, DOC state and full RAM; the actual
production module passes historical-reference sanitized mode/IRQ/mixing tests
and the bit oracle. Normal S3/P4 and isolated P4 builds pass. Ten fixed board
checkpoints match VRAM/pixels; all 42 captures are complete and all 24 timing
windows have zero additional missing speaker frames. Reference-bracketed
static-32 idle is 27.0–27.4% versus candidate 31.4–33.4%; animated-640 is
7.3–7.8% versus 12.8–13.8%. FPS stays capped near 60: this is synthetic
headroom, not a real-application or S3 runtime speed claim. Linked P4 hot
functions save 50 text bytes and oscillator stack falls 16 to 0 bytes;
S3 object text saves 18 bytes with its frame unchanged. Fresh candidate
inline samples put pointer calculation at 7.27–7.39% and voice mixing at
10.74–10.88%. Next test bit-exact float normalization lookup in
`clem_ensoniq_voices`; disassembly has two per-voice P4 FP divides and two S3
software-division call sites. Measure the 1 KB table/cache tradeoff and keep
exact float/PCM checks before retention. Broader software and S3 runtime
remain open.
Tab5 is restored from a fresh full-flash MSX backup with hash verification;
MSX Disk BASIC guest and LCD captures exactly match the restored controls.

The [IIgs normalization trial](../../esp32gs/docs/doc-normalization-experiment-2026-11-08.md)
compares shared 1 KB and dual 2 KB float tables. Both pass 524,288 exhaustive
mixer cases each on the host under ASan/UBSan and again on P4, including
float accumulation and clipped mono bits. Full native DOC state/IRQ/mode
checks and six complete emulator PCM/CPU/RAM/pixel replays per form match.
216 P4 ABBA cases select the smaller form for a full-app trial: 43.18%
fewer cycles with 32 voices, 27.31% with eight and 30.64–30.66% with paired AM
in the warm mixer kernel. The dual form is slightly faster active at twice
the table size. Both remove the two per-voice normalization divides and add
16 stack bytes; shared object text is +6 P4 / -25 S3 bytes plus 1024 bytes
of read-only data. Production is unchanged. Next integrate the shared form
in an isolated candidate, then exact replay, normal S3/P4 builds and full P4
reference/candidate/reference timing before retention. No full-app or S3
runtime gain is claimed; real software audio remains open.
Tab5 is restored from a fresh full-flash MSX backup with hash verification;
MSX Disk BASIC guest and LCD captures exactly match restored controls.

The [IIgs normalization integration](../../esp32gs/docs/doc-normalization-integration-2026-11-08.md)
retains the shared 1 KB table. Six native reference/candidate PCM, CPU, DOC,
RAM and pixel replays match exactly; the complete production mixer passes
524,288 exhaustive float cases and historical-reference sanitized mode/IRQ
checks. Full S3/P4 builds and ten fixed board checkpoints pass. All 42
captures are complete; all 24 timing windows have zero additional missing
speaker frames. Reference-bracketed static-32 idle rises 31.0–33.2% to
35.4–37.5%, animated-640 12.8–13.4% to 16.7–17.6%. Voice mixing's animated
inline share falls 10.69–10.82% to 6.37–6.60%. FPS stays near its 60 Hz cap;
this is synthetic headroom, not a real-program or S3 runtime speed claim.
The linked P4 binary grows 1024 bytes, and mixer frames grow 16 bytes on
both targets; S3 object text saves 25 bytes. Next validate actual software
playing DOC audio on throwaway media, including exact native PCM/state/media
writes and controlled P4 checkpoints, before more local arithmetic. Real
software throughput, S3 runtime and listening assessment remain open.
Tab5 is restored from its fresh full-flash MSX backup with hash verification;
MSX Disk BASIC guest and LCD captures exactly match restored controls.


### IIgs real-software DOC validation (2026-11-09)

The Tour boot/music replays 21,601 native frames against audio code before
both retained optimizations. PCM, final CPU/DOC/RAM/pixels and disposable
media bytes match exactly; up to 16 audible oscillators at frame boundaries.
Three candidate Tab5 music captures total 96,000 core-1 samples: scheduler
8.7–9.0%, oscillator 2.8–4.1%, mixer 2.6–3.2%, idle 38.2–39.3%. Five observed
speaker checkpoints add no misses; this does not cover boot's earlier gaps.
No matched real-software speedup or S3 runtime is established. Next investigate
DOC scheduling with an independent slot/cycle oracle and real-software replay
before a matched board trial. Production code is unchanged in this step.
Original SD media is unchanged, the test copy is removed, and the full MSX
backup and exact guest/LCD captures are restored.
See [the report](../../esp32gs/docs/doc-software-replay-2026-11-09.md).


### IIgs DOC scheduler trial and IRQ queue repair (2026-11-09)

Expanded scheduling checks reveal an existing full-queue IRQ bug: after an
acknowledgement, a duplicated tail can make the next enqueue write outside
the array. Production now clears the vacated tail. An independent FIFO
regression passes 18,000 native sanitizer and P4 events, while the committed
code fails; legacy DOC and real Tour replay still match. Normal S3/P4 builds
pass. Sanitizer runs now halt on errors.

Two scheduler experiments remain outside production. Both pass 245,760
boundary calls and real-software replay. Across 756 P4 ABBA kernel cases,
span saves 12.9–13.0% dense and 21–30% sparse with scanline deltas; it costs
stack (+32 bytes P4 / +16 S3) and regresses short/quiet controls. All 821,738
active scheduler calls in the instrumented native Tour advance 32–63 slots.
Next run full-app span A/B, including real music and quiet controls. No
full-app speedup or S3 runtime result is established.
See [the report](../../esp32gs/docs/doc-scheduler-experiment-2026-11-09.md).

Tab5 MSX firmware/settings are restored with a verified full-flash digest
and exact guest/LCD captures. Original SD media is untouched.


### IIgs DOC span integration (2026-11-09)

Retain span after exact native fixture/Tour replays, sanitized production
state/IRQ/boundary checks, normal S3/P4 builds and P4 A/B with dense references
before/after. Ten fixed checkpoints and five Tour source pairs/final LCD pair
match. Across 42 complete captures / 32 timing windows, no new speaker misses
occur in the observed windows. Scheduler share drops 7.0–7.2% to 5.0–5.3%
static dense, 7.5–8.0% to 5.8–5.9% animated, and 7.1–9.1% to 5.1–5.7% in
Tour music. Idle varies, including overlapping ROM/quiet controls; capped
FPS is unchanged. P4 text grows 36 bytes and stack 32; S3 text grows 14 and
stack 16, runtime unmeasured. Next inspect oscillator metadata/address loads
(12.5–14.3% of core 1 in dense candidate cases) before adding derived caches.
See [the report](../../esp32gs/docs/doc-span-integration-2026-11-09.md).

Original SD media is untouched, test copies are removed, and the full MSX
backup is digest-verified with exact restored guest/LCD captures.


### IIgs local oscillator-helper trial (2026-11-09)

The oscillator helper has one internal caller. Local scope lets GCC 14.2
inline it, saving 78 P4 / 57 S3 audio text bytes and 48 P4 / 16 S3 scheduler
stack bytes. Sanitized state/boundary/IRQ checks, six native fixture pairs
and a real Tour replay pass exactly. Across 378 P4 ABBA cases, complete DOC
scanline kernel savings are 37.5–37.7% dense, 24.1–25.4% eight active and
7.3–7.5% one active; short/quiet medians improve too. Production remains
unchanged. Next full-app A/B with inline-aware profiling; full-app and S3
runtime gains are unmeasured. Metadata relocation saves more code in an
isolated probe but needs canonical state snapshots and separate validation.
See [the report](../../esp32gs/docs/doc-helper-scope-experiment-2026-11-09.md).

The full MSX backup is restored and digest-verified, with exact guest/LCD
captures. Original SD media is unmounted throughout the kernel trial.

### IIgs local oscillator-helper integration (2026-11-09)

The local helper is retained after exact native fixtures, sanitized DOC/IRQ
checks, a 21,601-frame real Tour replay, normal S3/P4 builds and full-app P4
reference/candidate/reference validation. Inline-aware DOC kernel attribution
avoids confusing the inlined oscillator body with scheduler overhead. Across
42 full captures (1,344,000 samples), static dense DOC share falls from
17.66–17.95% to 12.68–13.17%, animated from 19.84–20.41% to 13.34–13.62%.
Idle rises to 44.6–44.8% / 24.9–26.1%; live Tour cost also falls. All ten
fixture checkpoints and six Tour image pairs match, and all 32 observed
timing windows add no missing-speaker frames. Sparse one-voice cost does not
improve; FPS remains capped, and S3 runtime is unmeasured. Audio text saves
78 P4 / 57 S3 bytes and scheduler stack saves 48 / 16 bytes. Shared runtime
source is unchanged. Next inspect remaining metadata/address loads in the
inlined scheduler; layout experiments need canonical field-based snapshots.
See [the report](../../esp32gs/docs/doc-helper-scope-integration-2026-11-09.md).

Original SD media remains unmounted; both test copies are verified unchanged
and removed.

The fresh full MSX backup is restored and independently digest-verified;
Disk BASIC guest/LCD captures match exactly after the final restart.

### IIgs DOC metadata-layout trial (2026-11-09)

The metadata-first DOC layout is deferred. With the retained local helper,
normal-flags audio text saves 238 P4 / 190 S3 bytes, but P4 dense scanline
kernel calls regress about 3.0%, and eight-oscillator sync regresses
2.2–2.5%; short calls improve. S3 scheduler stack grows 16 bytes and runtime
is unmeasured. All 378 ABBA groups / 1,512 timings agree on canonical state.
New field-based snapshots cover all fourteen DOC fields with exact float
payloads and explicit byte order, allowing different native layouts to be
compared. Both layouts pass fail-fast ASan/UBSan state/boundary/IRQ checks,
six complete-core native fixtures and a 21,601-frame Tour replay with exact
PCM/CPU/DOC/RAM/VRAM/pixel/media output. Only test tools change; production
layout and shared runtime source stay unchanged. Next investigate shared
65C816 fetch/fetch16, with bus/timing/IRQ equivalence before any code change.
See [the report](../../esp32gs/docs/doc-metadata-layout-experiment-2026-11-09.md).

The fresh full MSX backup is restored and digest-verified; guest and LCD
captures match the known Disk BASIC controls exactly.

### IIgs / shared 65C816 fetch trial (2026-11-09)

The combined paired-word and split-byte fetch candidate is selected for
full-app validation. It preserves the original fallback at page/bank ends
and callbacks; only the small cached byte-fetch wrapper is forced inline.
All candidates pass 129,748 sanitized frozen-core comparisons with exact
CPU/RAM and ordered callback traces, debugger register edits, the existing
IRQ suite, six complete-IIgs fixtures and a 21,601-frame Tour replay.
Across 672 P4 ABBA groups / 2,688 timings, combined mixed-code kernel cycles
fall 18.7–20.3%, 16-bit immediate 27.1–27.4%; bus-only costs about 1.8% more.
CPU text grows 1,020 P4 / 926 S3 bytes, reported run/fetch-helper frames stay
unchanged. Both target objects compile; S3 runtime and whole-app cache effects
remain unmeasured. Shared runtime stays unchanged. Next full-app A/B/C,
real Tour audio, inline-aware attribution and the required project/board build
matrix before retaining the CPU change.
See [the report](../../esp32gs/docs/cpu-fetch-experiment-2026-11-09.md).

The fresh full MSX backup is restored and digest-verified; final guest/LCD
captures match Disk BASIC controls exactly. Original SD media is untouched.

### IIgs / shared 65C816 full-app A/B — USB pending (2026-11-09)

The combined fetch candidate is applied byte-for-byte for production checks.
All 24 project/board builds pass, including GS through the shared CPU source;
other project working trees are unchanged. The actual production host object
also matches six fixtures and a 21,601-frame Tour replay. Source-based CPU
profiling is verified against exact linked CPU text ranges, avoiding duplicate
helper names in machine callbacks. Completed A/B results: static idle
44.9–45.2% → 50.1–50.2%, animated 25.4–25.9% → 33.5–33.9%; Tour CPU share
~30.5% → 26.9–27.2%. Eight fixture checkpoints and six live Tour image pairs
match; 36 captures / 28 observed timing windows add no missing-speaker frames.
All disposable media is verified unchanged and removed. Final reference C
and MSX restoration remain pending USB reconnection; the fresh full backup
is safe outside the repositories. No final drift or restoration pass is
claimed. Frame rate is capped and S3 runtime is unmeasured.
See [the report](../../esp32gs/docs/cpu-fetch-integration-2026-11-09.md).
