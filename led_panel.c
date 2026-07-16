#include "led_panel.h"
#if HUB75_DRIVER == HUB75_DRIVER_PIO
#include "hub75_pio.pio.h"
#endif

#define PIN_MASK_COLORS ((1 << PIN_R1) | (1 << PIN_G1) | (1 << PIN_B1) | \
                         (1 << PIN_R2) | (1 << PIN_G2) | (1 << PIN_B2))

#if HUB75_DRIVER == HUB75_DRIVER_PIO
// PIO state machine used by the display loop (exposed for main.c).
PIO  panel_pio    = pio0;
uint panel_pio_sm = 0;
#endif

// -----------------------------------------------------------------------
// CPU-based helpers — used only during dp3364_init(), BEFORE PIO starts.
// -----------------------------------------------------------------------

static inline void set_gpio_for_clk_pulses(uint gpio, uint width)
{
    gpio_put(gpio, 1);
    while (width-- > 0) {
        clock_pulse();
    }
    gpio_put(gpio, 0);
    clock_pulse();
}

static inline void dp3364_change_row(uint row)
{
    set_gpio_for_clk_pulses(PIN_ROW, (row == 0) ? 12 : 4);
}

// 2 clocks of LE high for single-edge mode / 15 for dual-edge mode
void dp3364_set_dual_edge_mode(bool dual_edge)
{
    set_gpio_for_clk_pulses(PIN_LE, dual_edge ? 15 : 2);
}

// 3 clocks of LE high
void dp3364_vsync(void)
{
    set_gpio_for_clk_pulses(PIN_LE, 3);
}

// 14 clocks of LE high
void dp3364_pre_act(void)
{
    set_gpio_for_clk_pulses(PIN_LE, 14);
}

void dp3364_write_reg(uint8_t reg, uint8_t val, bool latch)
{
    uint16_t u16 = ((uint16_t)reg << 8) | val;

    uint16_t bits = 16;
    while (bits-- > 0) {
        gpio_put(PIN_R1, (u16 & 0x8000) != 0);
        gpio_put(PIN_R2, (u16 & 0x8000) != 0);
        gpio_put(PIN_G1, (u16 & 0x8000) != 0);
        gpio_put(PIN_G2, (u16 & 0x8000) != 0);
        gpio_put(PIN_B1, (u16 & 0x8000) != 0);
        gpio_put(PIN_B2, (u16 & 0x8000) != 0);
        u16 <<= 1;
        gpio_put(PIN_LE, latch && (bits < 5));
        clock_pulse();
        gpio_clr_mask(PIN_MASK_COLORS);
    }
    gpio_put(PIN_LE, 0);
}

// Send all chips in one column group a zero-brightness word, with optional
// latch on the last chip.  Used by the blank scan sequence during init.
void dp3364_panel_draw(uint16_t rgb_lo[3], uint16_t rgb_hi[3], bool latch)
{
    uint16_t mask = 1 << 15;
    while (mask) {
        uint32_t pin_mask = 0;
        if (rgb_hi[0] & mask) pin_mask |= (1 << PIN_R2);
        if (rgb_hi[1] & mask) pin_mask |= (1 << PIN_G2);
        if (rgb_hi[2] & mask) pin_mask |= (1 << PIN_B2);
        if (rgb_lo[0] & mask) pin_mask |= (1 << PIN_R1);
        if (rgb_lo[1] & mask) pin_mask |= (1 << PIN_G1);
        if (rgb_lo[2] & mask) pin_mask |= (1 << PIN_B1);
        if (latch && (mask == 1)) pin_mask |= (1 << PIN_LE);

        gpio_put_masked(PIN_MASK_COLORS | (1 << PIN_LE), pin_mask);
        clock_pulse();
        gpio_clr_mask(PIN_MASK_COLORS | (1 << PIN_LE));

        mask >>= 1;
    }
}

// ROW pulse used by blank-scan init helper (and by the CPU driver's
// steady-state per-row loop — both must stay at the true ~15 MHz baseline
// rate, since dp3364_change_row()/clock_pulse() below is shared).
void dp3364_row_pulse_only(bool is_very_first_line)
{
    dp3364_change_row(is_very_first_line ? 0 : 1);
}

#if HUB75_DRIVER == HUB75_DRIVER_PIO
// PIO-driver-only variant, clocked via clock_pulse_row() (~20 MHz) instead
// of the shared clock_pulse() (~15 MHz).  See clock_delay_row() in
// led_panel.h for why this needs to stay decoupled.
static inline void dp3364_change_row_pio(uint row)
{
    uint width = (row == 0) ? 12 : 4;
    gpio_put(PIN_ROW, 1);
    while (width-- > 0) {
        clock_pulse_row();
    }
    gpio_put(PIN_ROW, 0);
    clock_pulse_row();
}

void dp3364_row_pulse_only_pio(bool is_very_first_line)
{
    dp3364_change_row_pio(is_very_first_line ? 0 : 1);
}
#endif // HUB75_DRIVER == HUB75_DRIVER_PIO

// Send a blank scan of `rows` rows including ROW signals.
static void dp3364_blank_scan_n(uint rows)
{
    static const uint16_t z[3] = {0, 0, 0};
    for (uint line = 0; line < rows; line++) {
        for (uint col = 0; col < 16; col++) {
            for (uint ch = 1; ch <= CHAINED_DP3364_NUM; ch++) {
                dp3364_panel_draw((uint16_t *)z, (uint16_t *)z,
                                  ch == CHAINED_DP3364_NUM);
            }
        }
        set_gpio_for_clk_pulses(PIN_ROW, (line == 0) ? 12 : 4);
        led_panel_advance_row(line);
        for (uint i = 0; i < (CLK_CYCLES_PER_LINE / 8 + 1); i++) {
            clock_pulse();
        }
    }
}

static inline void dp3364_blank_scan(void)      { dp3364_blank_scan_n(32); }
static inline void dp3364_blank_scan_wide(void) { dp3364_blank_scan_n(64); }

static void dp3364_write_reg_frame_n(uint8_t addr, uint8_t val,
                                     uint scan_rows, uint delay_ms)
{
    dp3364_vsync();
    dp3364_pre_act();
    for (uint ch = 1; ch <= CHAINED_DP3364_NUM; ch++) {
        dp3364_write_reg(addr, val, ch == CHAINED_DP3364_NUM);
    }
    dp3364_blank_scan_n(scan_rows);
    if (delay_ms) sleep_ms(delay_ms);
}

void dp3364_init(void)
{
    // Step 1: empirically-required 15-clock LE pulse (see original comments).
    set_gpio_for_clk_pulses(PIN_LE, 15);

    // Step 2: 64-row priming scan.
    dp3364_vsync();
    dp3364_blank_scan_wide();

    // Step 3: write all registers (values from DMD_STM32 DP3264 driver, proven
    // to work on this panel as confirmed by FPGA Colorlight card testing).
    struct reg_vals_t { uint8_t addr; uint8_t val; };
    static const struct reg_vals_t init_regs[] = {
        {0x11, 0x00},
        {0x02, 0x3f}, // scan lines-1 = 63 → 64 scans
        {0x03, 0x3f}, // PWM packet count-1: 64 groups
        {0x04, 0x3f}, // PWM display length per row-1
        {0x05, 0x04},
        {0x06, 0x42}, // GCLK PLL config
        {0x07, 0x00},
        {0x08, 0xbf}, // output current gain
        {0x09, 0x60},
        {0x0a, 0xbe},
        {0x0b, 0x8b},
        {0x0c, 0x08}, // PWM display mode 1 (General Frame Sync)
        {0x0d, 0x12},
    };

    for (size_t i = 0; i < sizeof(init_regs) / sizeof(init_regs[0]); i++) {
        dp3364_write_reg_frame_n(init_regs[i].addr, init_regs[i].val, 64, 5);
    }
}

#if HUB75_DRIVER == HUB75_DRIVER_PIO
// -----------------------------------------------------------------------
// PIO init — call once after dp3364_init(), before the display loop.
// Transfers GPIO 0-5 (colour), 11 (CLK), 12 (LE), 13 (ROW) to the PIO
// hub75_data state machine.  After this call the CPU MUST NOT directly
// toggle those pins; all CLK/LE/ROW signalling goes via the FIFO.
// -----------------------------------------------------------------------
void dp3364_pio_init(void)
{
    uint offset = pio_add_program(panel_pio, &hub75_data_program);

    pio_sm_config c = hub75_data_program_get_default_config(offset);

    // OUT: GPIO 0-12 (13 bits).
    //   GPIO 0-5:  R1/G1/B1/R2/G2/B2 (colour data).
    //   GPIO 6-10: inside OUT range but kept in SIO mode — PIO writes the
    //              internal register but the pad is SIO-owned so these bits
    //              have no physical effect.  GPIO 6-8 are the DP32020
    //              row-driver pins (DCK/RCK/DAT), GPIO 9 is OE (output
    //              enable), GPIO 10 is unused.
    //   GPIO 11:   CLK – also in OUT range, but OVERRIDDEN by the side-set.
    //   GPIO 12:   LE  – driven by OSR bit 12 (set by the CPU before push).
    sm_config_set_out_pins(&c, 0, 13);
    // SIDE: GPIO 11 (CLK).  No SET configuration needed – LE is driven by OUT.
    sm_config_set_sideset_pins(&c, PIN_CLK);
    // Shift right; AUTOPULL disabled (explicit `pull block` in program).
    sm_config_set_out_shift(&c, /*shift_right=*/true, /*autopull=*/false, 32);
    // 240 MHz / 3 = 80 MHz → 12.5 ns per PIO cycle.
    // Every byte: 9 cycles × 12.5 ns = 112.5 ns  → ~8.9 MHz DCLK.
    // CLK HIGH:   2 PIO cycles = 25 ns  ✓  (matches CPU clock_pulse baseline).
    // Data setup: 2 PIO cycles = 25 ns before CLK rising  ✓
    //   (`out pins,13` updates GPIOs at cycle-end; two `nop side 0` follow before
    //    CLK rises — matching the CPU baseline's clock_delay() ≈ 25 ns margin.)
    // LE hold:    2 PIO cycles = 25 ns after CLK falls  ✓  (symmetric with setup;
    //   see hub75_pio.pio header for why this matters for the chain-far chip.)
    // Return-to-zero: 1 PIO cycle clears colour+LE before the next byte's
    //   value is asserted, matching the CPU baseline's gpio_clr_mask() step
    //   between bits — see hub75_pio.pio header for the ISI/crosstalk theory.
    sm_config_set_clkdiv(&c, 3.0f);

    // Initialise the SM (resets PC, FIFOs, scratch regs; DOES NOT touch GPIO).
    pio_sm_init(panel_pio, panel_pio_sm, offset, &c);

    // Transfer pin control to PIO AFTER pio_sm_init.
    for (uint pin = 0; pin < 6; pin++) {
        pio_gpio_init(panel_pio, pin);
    }
    // GPIO 6-10 intentionally NOT given to PIO (they stay in SIO mode so the
    // CPU can continue to drive DCK/RCK/DAT and OE — see the pin map above).
    pio_gpio_init(panel_pio, PIN_CLK);
    pio_gpio_init(panel_pio, PIN_LE);
    pio_gpio_init(panel_pio, PIN_ROW);

    // Set output directions.
    pio_sm_set_consecutive_pindirs(panel_pio, panel_pio_sm, 0, 6, true);
    pio_sm_set_consecutive_pindirs(panel_pio, panel_pio_sm, PIN_CLK, 1, true);
    pio_sm_set_consecutive_pindirs(panel_pio, panel_pio_sm, PIN_LE,  2, true);

    // No FIFO pre-loading needed: the LE-in-data-stream program has no
    // init prologue and does not use X or Y registers.

    pio_sm_set_enabled(panel_pio, panel_pio_sm, true);
}

// -----------------------------------------------------------------------
// PIO pause / resume helpers
//
// ROW pulse (ROW held high for 4/12 CLKs) requires a signal sustained across
// multiple consecutive CLK edges.  The PIO data program only asserts LE for
// one CLK per 64-byte group and cannot hold any signal between bytes.
// Releasing GPIO 11-13 to the CPU for those short bursts lets the existing
// working CPU functions handle them correctly.
//
// Pause steps:
//   1. Drain the FIFO (let any in-flight words fully execute).
//   2. Force `out pins, 13` via pio_sm_exec while the SM is stalled.
//      The depleted OSR (always 0 for our word format) drives GPIO 0-12 = 0,
//      clearing LE (GPIO 12) in the PIO output register.  No CLK edge.
//   3. Pre-set SIO outputs to 0 (while PIO still drives the pads).
//   4. Disable the SM.  PIO output for GPIO 12 is now 0.
//   5. Switch GPIO 11-13 function to SIO → CPU takes over the pads.
//
// Resume steps:
//   1. Switch GPIO 11 (CLK) to PIO0 first (any CLK transient happens while
//      LE is still SIO = 0, preventing a spurious latch).
//   2. Switch GPIO 12 (LE) to PIO0.  PIO output register = 0 (from step 2
//      above), so the pad transitions from SIO=0 to PIO=0.  Glitch-free.
//   3. Re-enable the SM (resumes from pull-block, CLK = 0, LE = 0).
// -----------------------------------------------------------------------
void dp3364_pio_pause(void)
{
    // Drain FIFO: wait until PIO has consumed every word.
    while (!pio_sm_is_tx_fifo_empty(panel_pio, panel_pio_sm))
        tight_loop_contents();

    // SM is now stalled at `pull block` (FIFO empty).  The last `out pins, 13`
    // for a row's final LE byte left GPIO 12 (LE) = 1 in the PIO output register.
    // The subsequent `nop side 0` gave the chip its required LE hold time (≥12.5 ns
    // after CLK fell), and LE remains HIGH at the pull_block stall.
    //
    // We must clear that PIO register BEFORE handing GPIO 12 back to the CPU,
    // otherwise dp3364_pio_resume() will cause a spurious LE=1 glitch when it
    // switches GPIO 12 back to PIO0.
    //
    // Technique: forcibly execute `out pins, 13` via SM_INSTR while the SM is
    // stalled (but still running/clocked).  The OSR is depleted (remaining bits
    // are always 0 for our FIFO word format, since all values ≤ 0x1FFF and
    // `out pins, 13` already consumed the low 13 bits).  The forced instruction
    // drives GPIO 0-12 = 0 (LE = 0) without producing any CLK edge (side-set
    // bit = 0 in the encoded instruction), then PC stays at `pull block`.
    pio_sm_exec(panel_pio, panel_pio_sm, pio_encode_out(pio_pins, 13));

    // Pre-write SIO registers while PIO still owns the pads (no visible glitch).
    gpio_put(PIN_CLK, 0);
    gpio_put(PIN_LE,  0);
    gpio_put(PIN_ROW, 0);
    // Stop the SM.  PIO output for GPIO 12 is now 0, so the next resume is clean.
    pio_sm_set_enabled(panel_pio, panel_pio_sm, false);
    // Hand pads back to CPU.
    gpio_set_function(PIN_CLK, GPIO_FUNC_SIO);
    gpio_set_function(PIN_LE,  GPIO_FUNC_SIO);
    gpio_set_function(PIN_ROW, GPIO_FUNC_SIO);
}

void dp3364_pio_resume(void)
{
    // dp3364_pio_pause() cleared GPIO 12 (LE) in the PIO output register via
    // pio_sm_exec before stopping the SM, so switching GPIO 12 back to PIO0
    // here is glitch-free (PIO output = 0, same as SIO pre-set value).
    // CLK (GPIO 11) is switched first: if any transient occurs it happens
    // while LE is still SIO-driven (= 0), preventing a spurious latch.
    gpio_set_function(PIN_CLK, GPIO_FUNC_PIO0);
    gpio_set_function(PIN_LE,  GPIO_FUNC_PIO0);
    gpio_set_function(PIN_ROW, GPIO_FUNC_PIO0);
    // Restart the SM; resumes from pull_block with CLK = 0, LE = 0.
    pio_sm_set_enabled(panel_pio, panel_pio_sm, true);
}
#endif // HUB75_DRIVER == HUB75_DRIVER_PIO

// -----------------------------------------------------------------------
// Advance the external DP32020 row-driver chain by one position.
// Uses GPIO 6 (DCK) and GPIO 8 (DAT) only — never touches CLK/LE/ROW, so
// it's safe to call regardless of whether PIO currently owns those pins.
// Used both by dp3364_init()'s blank-scan priming (dp3364_blank_scan_n
// above) and by both drivers' steady-state per-row display loop.
// -----------------------------------------------------------------------
void led_panel_advance_row(uint scan_pos)
{
    gpio_put(PIN_ROW_DAT, scan_pos == 0);
    gpio_put(PIN_ROW_DCK, 1);
    delay_500ns();   // Tshadow ≥ 500 ns
    gpio_put(PIN_ROW_DCK, 0);
    gpio_put(PIN_ROW_DAT, 0);
    delay_500ns();   // extra settling margin
}
