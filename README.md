# rpi_pico_hub75_spwm

## Preamble
This repository is the result of a very long sequence of reverse engineering work using Claude to get a 128x64 RGB LED Matrix panel working that I bought off AliExpress. 
It is forked from [illia-lykhoshvai/rpi_pico_hub75_spwm](https://github.com/illia-lykhoshvai/rpi_pico_hub75_spwm) which did not work for me out of the box, despite that
project targetting the same board type and chips.

All of the modifications in this repository are AI generated, so please take everything with an appropriate grain, pinch, or bucket of salt. 
There are likely significant improvements that could be made.

The remainder of the readme from here on down is written by Claude.

## Introduction

Driving a 128×64 HUB75 LED matrix panel — built around **DP3364S** constant-current
column drivers and **DP32020B** row drivers — from a Raspberry Pi Pico (RP2040),
with two interchangeable display drivers built from the same source tree:

- **PIO driver** (`pico_dp3364_pio`) — hardware-accelerated via the RP2040's PIO
  peripheral. This is the one you want: ~130 Hz refresh, no visible flicker,
  no colour dropouts.
- **CPU driver** (`pico_dp3364_cpu`) — the original CPU bit-banged implementation.
  Kept around as a slower (~200 Hz theoretical, less in practice) but simpler
  reference/control build — useful if you ever need to rule out whether a
  symptom is PIO-specific or a more fundamental wiring/panel issue.

Both targets build from the exact same `main.c`/`led_panel.c`, switched at
compile time via the `HUB75_DRIVER` define (see [Building](#building) below).

This panel does **not** speak standard HUB75 timing — it needs a chip-specific
init/register sequence and precise clock timing that took a lot of trial and
error to reverse-engineer. If you have a similar DP3364S/DP32020B-based panel,
hopefully this saves you the same pain. See [Hard-won lessons](#hard-won-lessons-worth-reading-before-you-modify-timing)
before touching any of the clock timing.

## Hardware

### Wiring (GPIO → HUB75 connector)

| GPIO | Signal | Notes |
|------|--------|-------|
| 0 | R1 | Colour data, upper half |
| 1 | G1 | |
| 2 | B1 | |
| 3 | R2 | Colour data, lower half |
| 4 | G2 | |
| 5 | B2 | |
| 6 | DCK | DP32020 row-driver data clock |
| 7 | RCK | DP32020 register/latch clock (left idle — unused by this driver) |
| 8 | DIN | DP32020 row-driver data in |
| 9 | OE | Output enable, **active-LOW** (0 = display on, 1 = blanked) |
| 11 | CLK | DP3364S DCLK (serial shift clock) |
| 12 | LE | DP3364S LATCH |
| 13 | ROW | DP3364S ROW (row-advance signal) |

GPIO 10 is unused. GPIOs are driven at 12 mA / fast slew rate (see `main()`)
— the DP3364S needs sharp, well-defined edges, especially at PIO's higher
clock rate.

### Chip chain

- **24 DP3364S chips total** on the board, laid out as 8 physical columns of
  3 chips each (one R, one G, one B chip per column). Each column is
  labelled on the silkscreen as `U(R/G/B)1` through `U(R/G/B)8` — e.g. the
  first column (top to bottom) is UR1/UB1/UG1, the second is UG2/UR2/UB2, and
  so on. **The R/G/B ordering within a column is not consistent from column
  to column** — worth knowing if you're ever probing the board with a scope
  or trying to match a symptom to a physical chip.
- Each column of 3 chips corresponds to one of the 8 DP32020B row drivers
  (see below) — i.e. column *N*'s chips and row driver *N* are physically
  paired.
- Electrically, the R1/G1/B1 signal lines chain through 4 of those columns
  (`CHAINED_DP3364_NUM` = 4) to drive one half of the panel (64 columns);
  R2/G2/B2 chains through the other 4 columns to drive the other half, in
  parallel. So "4 chained chips per half" in the code refers to 4 *columns*
  (12 physical chips) per half, 24 chips total across both halves.
- DP32020B row drivers, advanced one row at a time via GPIO 6/8 (DCK/DIN),
  independent of the CLK/LE/ROW signalling used for colour data.

### Panel-specific register configuration

The DP3364S init register values in `dp3364_init()` (`led_panel.c`) came from
the reference [DMD_STM32](https://github.com/board707/DMD_STM32) DP3264 driver project and were confirmed working via
FPGA (Colorlight card) testing before being ported here — they are **not**
something we derived from the datasheet alone, and they may need adjustment
for a differently-configured panel (different scan-line count, PWM packet
count, etc).

## Building

Requires the Raspberry Pi Pico SDK (this project was developed against SDK
2.2.0 / toolchain 14_2_Rel1) and CMake. Development was done inside WSL; the
commands below assume a Pico-SDK-enabled shell (e.g. via the Raspberry Pi Pico
VS Code extension, or `PICO_SDK_PATH` set manually).

```bash
mkdir -p build && cd build
cmake ..
make -j4 pico_dp3364_pio pico_dp3364_cpu
```

This produces `pico_dp3364_pio.uf2` and `pico_dp3364_cpu.uf2`. Hold the
Pico's BOOTSEL button while plugging it in, then drag either `.uf2` onto the
mounted drive to flash it.

Both targets are defined in `CMakeLists.txt` from the same `main.c` +
`led_panel.c`, distinguished only by the `HUB75_DRIVER` compile definition
(`1` = PIO, `2` = CPU — see `led_panel.h`). There's normally no reason to
build the CPU target unless you're debugging a symptom and need to rule PIO
in or out.

## Demos

Selected at compile time via `DEMO_TYPE` in `main.c`:

| Demo | Purpose |
|------|---------|
| `RAINBOW_WAVE_DEMO` | Animated diagonal rainbow sweep. The default — good general-purpose smoke test since it exercises every colour/brightness combination and animates every frame. |
| `GIF_FILE_DEMO` | Plays back a GIF (via [minigif](https://github.com/illia-lykhoshvai/minigif)) baked into `gif.h`. |
| `STATIC_COLORS_DEMO` | Static per-16px-column block pattern with a brightness gradient down the panel — useful for spotting a specific column/chip with a colour-channel problem, since each of the 8 chip groups gets a distinct, unmoving colour. |
| `GREY_FILL_DEMO` | Uniform dim single-channel fill (see `fill_solid_pattern()`). Deliberately boring — a flat, low-brightness field makes marginal-timing flicker far more visible than it would be against a busy/animated pattern, since there's no other per-pixel motion to distract from it. |

## Architecture

### PIO driver

- `main.c` maintains a `uint16_t` RGB logical framebuffer (`logical_buf`),
  written by Core0's demo/content loop.
- Each frame, Core0 pre-serialises `logical_buf` into a byte-per-PIO-clock
  format (`convert_logical_to_ps()`) that exactly matches what
  `hub75_pio.pio`'s state machine expects, then atomically swaps it into the
  buffer Core1 reads from (`ps_display`/`ps_staging`, double-buffered, guarded
  by `ps_mutex`).
- Core1's `core1_entry()` streams the pre-serialised buffer straight into the
  PIO FIFO, one row (1024 bytes) at a time. Between rows it briefly pauses the
  PIO state machine to bit-bang the ROW pulse and DP32020 row advance on the
  CPU (see `dp3364_pio_pause()`/`dp3364_pio_resume()` in `led_panel.c`), since
  those signals need to be held across multiple CLK edges in a way the PIO
  program doesn't support.
- `hub75_pio.pio` embeds LE directly in the data stream (bit 12 of each FIFO
  word) rather than using any separate counter — this was a deliberate design
  choice after counter-based LE approaches proved fragile across PIO
  pause/resume cycles (see below).

### CPU driver

- Simpler and more direct: `main.c`'s CPU `core1_entry()` bit-bangs every
  CLK/LE/ROW pulse straight from the logical framebuffer via
  `dp3364_panel_draw()`/`dp3364_row_pulse_only()`, no PIO, no
  pre-serialisation step. This is essentially the original, pre-PIO
  implementation, kept as a working reference/control build.

### Shared code

`dp3364_init()` (the full register-write/priming sequence) and
`led_panel_advance_row()` (DP32020 row advance) are shared between both
drivers and always run at the "true" baseline CPU clock rate (`clock_delay()`
in `led_panel.h`, ~15 MHz) — see [Hard-won lessons](#hard-won-lessons-worth-reading-before-you-modify-timing)
for why that rate matters and must not drift.

## Hard-won lessons (worth reading before you modify timing)

Getting this panel stable — especially at high refresh rate via PIO — took a
long series of subtle timing bugs, each with a distinctive symptom. If you're
debugging a similar panel, these are the failure modes we actually hit, in
case the symptom rhymes with what you're seeing:

- **Insufficient CLK high time.** The DP3364S needs the full clock pulse
  width it was validated with — shaving this down (even by ~12 ns) produced
  static/garbled columns, not just dimness.
- **Insufficient data setup time before CLK rises.** Manifested as
  deterministic wrong colours on specific 16-pixel-wide column groups
  (particularly the chip groups nearest DIN), not random noise — a strong hint
  it's a setup-time margin problem rather than a logic bug.
- **Insufficient LE hold time after CLK falls.** Manifested as colour-channel
  dropout (losing red, specifically) on the chip farthest down the daisy
  chain, and got *worse* the longer the panel had been running (thermal
  effects reducing an already-marginal margin). If you see a channel drop out
  only after minutes of continuous operation, check LE hold time first.
- **Buffer race between the rendering core and the display core.** If the
  core producing frames can start overwriting a buffer the display core is
  still reading, you get "drift over time" and random per-column colour
  corruption that gets worse the longer the panel runs. The fix here was
  making the display core hold its buffer-access mutex for the *entire*
  frame, not just around the pointer swap.
- **Inter-symbol interference / crosstalk between adjacent bytes.** Flicker
  that only occurs when a pixel's value actually *changes* frame-to-frame
  (never on a static pattern) is a classic signature of this — the fix was
  adding a one-cycle "return to zero" step between bytes in the PIO program,
  mirroring what the CPU driver's bit-banging loop was already doing
  structurally (clearing all lines between every bit).
- **Mixed clock-domain beating.** The PIO driver alternates every row between
  PIO-driven colour data (fixed rate, set by `clkdiv`) and a CPU-bit-banged
  section for the ROW pulse and GCLK settling clocks. When both sections
  ran at very different rates, or when the CPU section's rate changed
  relative to the PIO rate, we saw the *entire panel* pulse in brightness
  rapidly — visually quite different from the localised/columnar symptoms
  above, and only present on the PIO driver (the CPU driver has a single
  uniform clock domain and never exhibited it). The fix was decoupling the
  PIO driver's per-row CPU-clocked section (`clock_delay_row()`/
  `clock_pulse_row()` in `led_panel.h`) from the rate the CPU driver and
  `dp3364_init()` need (`clock_delay()`/`clock_pulse()`) — the two drivers
  have different constraints and don't have to share one clock macro.
- **A single shared clock-speed macro silently drifting during development.**
  Because `clock_delay()` is used by the CPU driver's *entire* display loop,
  by `dp3364_init()` (shared by both drivers), and (originally) by the PIO
  driver's per-row CPU section, a well-intentioned speed tweak made while
  chasing a PIO-specific symptom quietly changed the CPU driver's timing too,
  and vice versa. If you're touching clock timing, check every caller of
  the macro you're changing, not just the one you're actively debugging —
  the CPU driver (`pico_dp3364_cpu`) is a great sanity check here, since any
  regression it introduces can't be blamed on PIO.
- **VSYNC / state-machine-restart transients.** Restarting the PIO state
  machine leaves its output registers in a different electrical state than
  mid-stream operation, which can produce a visible glitch on the first row
  after a pause/resume. Blanking the display (via OE) across the
  pause/resume window hides this for free, at a negligible (<0.5%) duty-cycle
  cost.

If you're chasing a new flicker/dropout symptom, the `STATIC_COLORS_DEMO` and
`GREY_FILL_DEMO` demos (above) plus a build of the CPU driver as a control
are the fastest way to localise it: does it happen on a static pattern or
only when pixel values change? Is it isolated to specific columns, or the
whole panel? Does it reproduce on the CPU driver too, or only PIO?
