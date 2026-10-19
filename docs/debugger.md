# The debugger protocol (emudbg)

The machines serve a debugger for the programs that run in them: breakpoints,
steps, registers and memory of every emulated CPU. The VS Code extension
[esp32emu-vscode](https://github.com/rlucencic/esp32emu-vscode) speaks it;
so can a script (`nc atari-xl.local 6502`).

- **Board:** TCP port 6502 (menuconfig "Debugger", `CONFIG_EMUDBG_PORT`),
  announced over mDNS as `_esp32emu._tcp` (TXT `machine=`). IPv4 and IPv6.
- **Host builds** (`xl_host`, `cbm_host`, ...): `127.0.0.1:<port>` when the
  machine's `<M>_DEBUG=<port>` environment variable is set; the machine
  then runs in real time until it's killed.

One client at a time. When it goes away, its breakpoints go and the machine
runs on.

## Lines

Everything is a line of text ending in `\n`.

**Requests:** `<id> <command> <arguments...>`, where `<id>` is a decimal
number of the client's choice. Numbers in arguments are decimal, `0x` hex
or `$` hex. Requests up to 16 KB.

**Answers:** one JSON object per request, `{"id":<id>,"ok":true,...}` or
`{"id":<id>,"ok":false,"error":"..."}`. Answers come in order.

**Events:** JSON objects with an `"event"` key. An answer always comes
before the events it causes (a `step`'s answer, then its `stopped`):

| Event | When |
|---|---|
| `{"event":"stopped","reason":R,"cpu":N,"pc":A,"where":"frame 812, line 103, cycle 54"}` | The machine stopped. Every CPU stops with it. `reason`: `breakpoint`, `step`, `pause`, `frames` |
| `{"event":"continued","reason":"paused"}` | The board's setup screen (or a disk change) took the machine out of a stop. It runs to the end of its frame and then pauses. Breakpoints are off meanwhile |
| `{"event":"machine","name":"..."}` | The machine was started again (the setup screen, other CPUs): breakpoints are gone. Send `hello` again |

## Commands

The CPUs are numbered from 0, in the order `hello` lists them. Commands
that read or change registers need the machine stopped. Memory can be read
and written at any time.

| Command | Answer | |
|---|---|---|
| `hello` | `protocol`, `machine`, `state`, `frame`, `cpus`, `stop` | `cpus`: `[{"name":"6502","arch":"6502","addr_bits":16,"regs":[{"name":"PC","bits":16,"flags":1},...]}]`. Register flags: 1 the program counter, 2 the stack pointer, 4 a status register (`flag_names`, one letter per bit, MSB first, `-` unused), 8 read only. `arch`: `6502`, `65c816`, `z80`, `68000`, `6809`, `arm2` (`addr_bits` 26; the PSR is its own register, the PC without it) |
| `state` | `state`, `frame`, `stop` | `state`: `none` (no machine: the setup screen before the start), `running`, `stopped`, `paused` (the setup screen). `stop`: `{"reason","cpu","pc"}` while stopped |
| `pause` | | Stops at the next instruction of any CPU. If none runs for 3 frames (WAI, STOP, HALT with interrupts off), it stops between frames instead |
| `continue` | | |
| `step <cpu>` | | One instruction of that CPU. An interrupt that comes first runs through: the stop is never inside a handler that the step didn't call |
| `next <cpu>` | | `step`, but over a call (`JSR` `JSL` `CALL` `RST` `BSR` `TRAP`, line A/F, `BL` `SWI`): it stops once a return takes the stack back to where it was. Calls with their parameters inline (ProDOS MLI, `PRIMM`, GS/OS) work |
| `out <cpu>` | | Runs until a return takes the stack above where it is now |
| `frames <n>` | | Runs n frames and stops between frames (reason `frames`). Useful after a reset, to let the machine start up |
| `reset [stop]` | | A cold reset at the end of the frame; with `stop` the machine stops at the first instruction |
| `bps <cpu> [addr...]` | `count` | Sets that CPU's breakpoints, replacing all of them (up to 128). Addresses are CPU addresses (24-bit on the 65C816 and 68000) |
| `regs <cpu>` | `regs` | The values, in `hello`'s order |
| `setreg <cpu> <name> <value>` | `value` | |
| `read <cpu> <addr> <len>` | `data` | Memory as the CPU sees it, without side effects (I/O reads `ff`). `data` is hex. Up to 4096 bytes |
| `write <cpu> <addr> <hex>` | `count` | Writes RAM as the CPU sees it (ROM and I/O are left alone) |
| `disasm <cpu> <addr> <count>` | `lines` | `[[addr,"hexbytes","LDA #$01"],...]`, up to 64 instructions |
| `detach` | | Clears everything and lets the machine run |

## Example

```
> 1 hello
< {"id":1,"ok":true,"protocol":1,"machine":"Atari 800XL","state":"running","frame":5310,"cpus":[{"name":"6502","arch":"6502","addr_bits":16,"regs":[...]}]}
> 2 bps 0 0x2000
< {"id":2,"ok":true,"count":1}
< {"event":"stopped","reason":"breakpoint","cpu":0,"pc":8192,"where":"frame 5402, line 94, cycle 31"}
> 3 regs 0
< {"id":3,"ok":true,"regs":[8192,0,4,255,251,48,9875443]}
> 4 next 0
< {"id":4,"ok":true}
< {"event":"stopped","reason":"step","cpu":0,"pc":8195,"where":"frame 5402, line 94, cycle 35"}
```

## How it works

- The CPU cores look at a map before each instruction: a pointer that is
  NULL without a debugger, otherwise a bit per address (16-bit CPUs) or per
  256-byte page (24-bit). A set bit calls `emudbg_hit()`, which checks the
  breakpoints and steps. Pausing and stepping give the CPU a map with every
  bit set. The 65C816 runs one instruction at a time while it has a map, so
  its instruction loop stays as it was.
- A stop blocks the emulation task inside `emudbg_hit()`, so every CPU
  stops between two instructions. The network task answers the commands
  meanwhile. The pacing starts afresh afterwards: the machine doesn't race
  to catch up.
- `components/emudbg/`: the protocol (`emudbg.c`), the board's server
  (`emudbg_freertos.c`), the host builds' (`emudbg_posix.c`), and a test
  against the 6502 and Z80 cores (`test/`). The cores' adapters:
  `m6502_dbg.c`, `z80_dbg.c`, `w65816_dbg.c`, `musashi/m68k_dbg.c`. Each
  machine describes its CPUs to `emudbg_set_machine()` when it starts.
