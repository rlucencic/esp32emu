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
