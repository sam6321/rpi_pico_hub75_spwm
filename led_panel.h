#ifndef LED_PANEL_H
#define LED_PANEL_H

#include "pico/stdlib.h"
#include "hardware/gpio.h"

// -----------------------------------------------------------------------
// Driver selection
//
// Set via CMake target_compile_definitions (see CMakeLists.txt):
//   -DHUB75_DRIVER=1   → PIO-accelerated driver   (pico_dp3364_pio target)
//   -DHUB75_DRIVER=2   → CPU bit-banged driver     (pico_dp3364_cpu target)
//
// This lets both implementations live in one source tree — build whichever
// target you want, no source edits needed to swap between them.  Useful for
// control experiments (does a symptom show up on the CPU baseline too, or
// only with PIO?) without keeping a second checkout in sync.
// -----------------------------------------------------------------------
#define HUB75_DRIVER_PIO 1
#define HUB75_DRIVER_CPU 2

#ifndef HUB75_DRIVER
#error "HUB75_DRIVER not defined -- build via the pico_dp3364_pio or pico_dp3364_cpu CMake target."
#endif

#if HUB75_DRIVER == HUB75_DRIVER_PIO
#include "hardware/pio.h"
#endif

// Colour data pins (HUB75 connector data pins)
#define PIN_R1 0
#define PIN_G1 1
#define PIN_B1 2
#define PIN_R2 3
#define PIN_G2 4
#define PIN_B2 5

// Control pins wired to HUB75 connector signals
#define PIN_CLK 11  // DP3364S DCLK
#define PIN_LE  12  // DP3364S LE (LATCH)
#define PIN_ROW 13  // DP3364S ROW

// Row-address / DP32020 control pins (HUB75 connector A/B/C)
#define PIN_ROW_DCK   6   // DP32020 DCK (data clock)
#define PIN_ROW_RCK   7   // DP32020 RCK (register/latch clock, left idle)
#define PIN_ROW_DAT   8   // DP32020 DIN (data)

// Output-enable (active-LOW).  0 = display on, 1 = display blanked.
// Used to suppress visual glitches during the VSYNC + SM-restart window.
#define PIN_OE        9

// Chain depth per half, in DP3364S *columns* (each column = 3 physical
// chips, one per R/G/B line — see README's "Chip chain" section for the
// full 24-chip board layout).  R1/G1/B1 chains through 4 columns (12 chips)
// to drive one half of the panel (64 pixel columns); R2/G2/B2 chains
// through the other 4 columns (12 chips) to drive the other half, in
// parallel — 24 chips total across both halves.
#define CHAINED_DP3364_NUM 4

// Extra DCLK/GCLK-settling cycles the DP3364S needs per scan line, on top
// of the 1024 colour-data CLKs already sent for that row.  Determined
// empirically against the register configuration in dp3364_init()
// (led_panel.c) — CLK_CYCLES_PER_LINE/8+1 CLK pulses are issued once per row
// after the ROW/DCK pulse, before the next row's colour data starts.
// Changing the init register values (init_regs in led_panel.c) may require
// re-tuning this constant.
#define CLK_CYCLES_PER_LINE 60

#if HUB75_DRIVER == HUB75_DRIVER_PIO
// Pre-serialised buffer geometry (PIO driver only).
// Per row: 16 col positions × CHAINED_DP3364_NUM chip calls = 64 chip calls,
// each sending 16 serial bit planes → 64×16 = 1024 bytes per row.
#define PS_CHIPS_PER_ROW    ((MATRIX_WIDTH / 2))          // = 64
#define PS_BIT_PLANES       16                             // 16-bit serial word
#define PS_BYTES_PER_ROW    (PS_CHIPS_PER_ROW * PS_BIT_PLANES)  // = 1024
#define PS_BUF_SIZE         (MATRIX_HEIGHT * PS_BYTES_PER_ROW)  // = 65536

// Byte format for one PIO FIFO word (zero-extended to 32 bits):
//   bit7=ROW  bit6=LE  bit5=B2  bit4=G2  bit3=R2  bit2=B1  bit1=G1  bit0=R1
#define PS_LE_BIT   0x40u
#define PS_ROW_BIT  0x80u
#endif // HUB75_DRIVER == HUB75_DRIVER_PIO

// -----------------------------------------------------------------------
// CPU-speed clock macros.
// PIO driver: used only during dp3364_init(), before PIO takes over (plus
//   the short per-row ROW/DCK/settle-clock CPU section every row).
// CPU driver: used for the ENTIRE display loop — every single CLK pulse
//   for every bit of every pixel, all day, every frame.
//
// 6 NOPs → each half-period ≈ 8 (gpio) + 25 (nop) = 33 ns → FCLK ≈ 15 MHz.
// This MUST match the original pi-fully-working baseline's value exactly
// (previously drifted to 4 NOPs / ~25 ns / ~20 MHz at some point during the
// PIO port, which went unnoticed because the PIO driver only exercises this
// macro briefly during init/per-row CPU sections — but the CPU driver's
// entire display loop runs on it, and 20 MHz turned out to be measurably
// less safe than the original's deliberately-conservative 15 MHz, causing
// colour-channel dropouts on the CPU driver once unified into one project).
// Every PIO setup/hold margin added elsewhere in this codebase was reasoned
// as "matching the CPU baseline's ~25 ns clock_delay()" — that reasoning
// was against this already-drifted 4-NOP value, not the TRUE 33 ns
// original.  Restoring 6 NOPs here is also relevant background for any
// remaining PIO-side marginal-timing symptoms.
// -----------------------------------------------------------------------
#define clock_delay() { \
    asm volatile("nop"); asm volatile("nop"); \
    asm volatile("nop"); asm volatile("nop"); \
    asm volatile("nop"); asm volatile("nop"); \
}

#define clock_pulse() \
    do { \
        gpio_put(PIN_CLK, 1); \
        clock_delay(); \
        gpio_put(PIN_CLK, 0); \
        clock_delay(); \
    } while (0)

#if HUB75_DRIVER == HUB75_DRIVER_PIO
// -----------------------------------------------------------------------
// PIO driver's per-row CPU section (ROW pulse + GCLK settling clocks in
// core1_entry) — DELIBERATELY KEPT SEPARATE from clock_delay()/clock_pulse()
// above.
//
// The PIO driver streams the bulk of each row's colour data through the PIO
// peripheral at a fixed rate set by clkdiv (unrelated to this macro), then
// briefly hands GPIO 11/13 back to the CPU every row for the ROW pulse and
// GCLK-settling clocks.  That means the PIO driver's steady-state operation
// involves switching between two different clock domains every single row,
// 64 times a frame — something the CPU driver's single-clock-domain loop
// never does.  Restoring clock_delay() to the true 15 MHz baseline (see
// above) was necessary and correct for the CPU driver and for dp3364_init()
// (proven: colour dropouts fully resolved on both drivers after that fix),
// but slowing this specific per-row CPU section to 15 MHz as a side-effect
// introduced a NEW symptom unique to the PIO driver: rapid, whole-panel
// brightness pulsing that never appears on the CPU-only baseline (which has
// no PIO, no clock-domain switching, nothing to beat against).  The
// suspected mechanism is the DP3364S's GCLK PLL (register 0x06) reacting to
// the changed ratio between the two alternating clock domains every row.
// Kept at 4 NOPs / ~20 MHz — the rate this section ran at during all of the
// earlier PIO testing before today's clock_delay fix, when this symptom was
// never observed.
// -----------------------------------------------------------------------
#define clock_delay_row() { \
    asm volatile("nop"); asm volatile("nop"); \
    asm volatile("nop"); asm volatile("nop"); \
}

#define clock_pulse_row() \
    do { \
        gpio_put(PIN_CLK, 1); \
        clock_delay_row(); \
        gpio_put(PIN_CLK, 0); \
        clock_delay_row(); \
    } while (0)
#endif // HUB75_DRIVER == HUB75_DRIVER_PIO

// NOP-based 500 ns delay for dp32020_advance_row Tshadow timing.
// At 240 MHz: 1 NOP ≈ 4.17 ns; 120 NOPs ≈ 500 ns.
static inline void delay_500ns(void)
{
    // tight nop loop — avoids sleep_us() timer peripheral overhead
    for (int _i = 0; _i < 30; _i++) {
        asm volatile("nop\nnop\nnop\nnop");
    }
}

#if HUB75_DRIVER == HUB75_DRIVER_PIO
// -----------------------------------------------------------------------
// PIO globals (defined in led_panel.c)
// -----------------------------------------------------------------------
extern PIO  panel_pio;
extern uint panel_pio_sm;
#endif // HUB75_DRIVER == HUB75_DRIVER_PIO

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

// CPU-based init — call BEFORE dp3364_pio_init (PIO driver) or before the
// CPU display loop begins (CPU driver).
void dp3364_init(void);

#if HUB75_DRIVER == HUB75_DRIVER_PIO
// Transfers GPIO 0-5, 11-13 to PIO and starts the hub75_data state machine.
// Call once, after dp3364_init(), before the display loop begins.
void dp3364_pio_init(void);

// Pause PIO and return GPIO 11-13 (CLK, LE, ROW) to CPU/SIO control.
// Call before any CPU operation that needs to hold LE or ROW HIGH across
// multiple CLK pulses (VSYNC, ROW pulse, PRE_ACT).  All three GPIOs are
// driven low before the switch to avoid glitches.
void dp3364_pio_pause(void);

// Resume PIO after a dp3364_pio_pause() / CPU operation pair.
// Returns GPIO 11-13 to PIO0 control and re-enables the state machine.
void dp3364_pio_resume(void);
#endif // HUB75_DRIVER == HUB75_DRIVER_PIO

// CPU bit-bang helpers.  PIO driver: used by init only, not safe once PIO
// owns CLK.  CPU driver: used continuously by the display loop.
void dp3364_set_dual_edge_mode(bool dual_edge);
void dp3364_vsync(void);
void dp3364_pre_act(void);
void dp3364_write_reg(uint8_t reg, uint8_t val, bool latch);
void dp3364_panel_draw(uint16_t rgb_lo[3], uint16_t rgb_hi[3], bool latch);
void dp3364_row_pulse_only(bool is_very_first_line);

#if HUB75_DRIVER == HUB75_DRIVER_PIO
// Same as dp3364_row_pulse_only() but clocked via clock_pulse_row() (~20 MHz)
// instead of clock_pulse() (~15 MHz).  Used ONLY by the PIO driver's
// steady-state per-row CPU section — see clock_delay_row() above for why
// this needs to stay decoupled from the CPU driver's row-pulse timing.
void dp3364_row_pulse_only_pio(bool is_very_first_line);
#endif

// Advance external DP32020 row-driver chain.
// Safe to call from display loop (uses GPIO 6/8 only, not CLK).
void led_panel_advance_row(uint line);

#endif // LED_PANEL_H
