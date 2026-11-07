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

## Current checkpoint (2026-11-07)

Hardware investigations now cover Acorn, STE and Mac2. The latest Mac2
work retains paired indexed reads and a full-row RGB555 fast path; its
[LC report](../../esp32mac2/docs/lc-video-experiment-2026-11-07.md) records
converter savings, exact pixels and the small CPU-throughput tradeoffs.
The subsequent [Mac3 converter trial](../../esp32mac3/docs/video-optimization-experiment-2026-11-07.md)
retains paired indexed reads and masked RGB555 arithmetic with original
Sonora/V8 bounds and blanking. Converter costs fall, with measured adaptive
CPU-speed tradeoffs recorded separately from the frame cap. Next establish
an Amiga bitplane/blitter baseline before selecting its optimization target.
Representative software on copied disks, physical input and audio coverage
remain open. Shared S3 paths have been built; the P4 measurements do not
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
