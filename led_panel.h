#ifndef LED_PANEL_H
#define LED_PANEL_H

#include "pico/stdlib.h"
#include "hardware/gpio.h"

// Colour data pins (these are the HUB75 connector data pins)
#define PIN_R1 0
#define PIN_G1 1
#define PIN_B1 2
#define PIN_R2 3
#define PIN_G2 4
#define PIN_B2 5

#define PIN_MASK_COLORS ((1 << PIN_R1) | (1 << PIN_G1) | (1 << PIN_B1) | (1 << PIN_R2) | (1 << PIN_G2) | (1 << PIN_B2))

// Row-address / DP32020 control pins (HUB75 connector A/B/C)
#define PIN_ROW_DCK   6   // on DP32020 this is DCK (clock) in the datasheet mapping
#define PIN_ROW_RCK   7   // on DP32020 this is RCK (register clock)
#define PIN_ROW_DAT   8   // on DP32020 this is DIN (data) mapping

// Other control pins wired to connector signals (HUB75-style)
#define PIN_CLK 11  // DP3364S CLK (we'll use this as DCLK for DP3364S)
#define PIN_LE  12  // DP3364S LE (LAT)
#define PIN_ROW 13  // we keep a ROW pin name for clarity (if used physically)

#define DP3364_NUM_CHANNELS 8

#define clock_delay() {asm volatile("nop"); asm volatile("nop");}

#define clock_pulse() \
    do { \
        gpio_put(PIN_CLK, 1); \
        clock_delay(); \
        gpio_put(PIN_CLK, 0); \
        clock_delay(); \
    } while (0)

static inline void set_gpio_for_clk_pulses(uint gpio, uint width)
{
    gpio_put(gpio, 1);
    while (width-- > 0) {
        clock_pulse();
    }
    gpio_put(gpio, 0);
    
    clock_pulse();
}

// 2 clocks of LE high for single-edge mode
// 15 clocks of LE high for dual-edge mode
static inline void dp3364_set_dual_edge_mode(bool dual_edge)
{
    set_gpio_for_clk_pulses(PIN_LE, dual_edge ? 15 : 2);
}

// 3 clocks of LE high
static inline void dp3364_vsync(void)
{
    set_gpio_for_clk_pulses(PIN_LE, 3);
}

// 14 clocks of LE high
static inline void dp3364_pre_act(void)
{
    set_gpio_for_clk_pulses(PIN_LE, 14);
}

static inline void dp3364_write_reg(uint8_t reg, uint8_t val, bool latch)
{
    uint16_t u16 = ((uint16_t)reg << 8) | val;

    uint16_t bits = 16;
    while(bits-- > 0) {
        // data
        gpio_put(PIN_R1, (u16 & 0x8000) != 0);
        gpio_put(PIN_R2, (u16 & 0x8000) != 0);
        gpio_put(PIN_G1, (u16 & 0x8000) != 0);
        gpio_put(PIN_G2, (u16 & 0x8000) != 0);
        gpio_put(PIN_B1, (u16 & 0x8000) != 0);
        gpio_put(PIN_B2, (u16 & 0x8000) != 0);
        u16 <<= 1;
        // last 5 clocks of LE high
        gpio_put(PIN_LE, latch && (bits < 5));
        clock_pulse();
        gpio_clr_mask(PIN_MASK_COLORS);
    }
    gpio_put(PIN_LE, 0);
}

// val + 1 clock (last bit of val) of LE high
static inline void dp3364_panel_draw(uint16_t rgb_lo[3], uint16_t rgb_hi[3], bool latch)
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

static inline void dp3364_init(void)
{
    dp3364_set_dual_edge_mode(true);

    struct reg_vals_t { uint8_t addr; uint8_t val; };
    struct reg_vals_t init_regs[] = {
        {0x02, 31}, // 31 + 1 lines
        {0x03, 63}, // group num
        {0x04, 0x7f}, // max time of active PWM
        // provided from .doc file:
        {0x05, 0x08},
        {0x06, 0x3a},
        {0x07, 0x00},
        {0x08, 0xb9},
        {0x09, 0x67},
        {0x0a, 0xbe},
        {0x0b, 0x27},
        {0x0c, 0x18 | 0b11000000},
        {0x0d, 0x08},
        {0x0e, 0x27},
        {0x0f, 0x20},
        {0x10, 0x00},
        {0x11, 0x80},
        {0x12, 0x00},
        {0x13, 0x00},
        {0x14, 0x00},
    };

    dp3364_vsync();
    dp3364_pre_act();
    
    for (size_t i = 0; i < sizeof(init_regs)/sizeof(init_regs[0]); i++) {
        for (uint ch = 1; ch <= DP3364_NUM_CHANNELS; ch++) {
            dp3364_write_reg(init_regs[i].addr, init_regs[i].val, ch == DP3364_NUM_CHANNELS);
        }
    }

    // stop writing registers
    for (uint ch = 1; ch <= DP3364_NUM_CHANNELS; ch++) {
        uint16_t val = 0;
        uint16_t rgb_hi[3] = {val, val, val};
        uint16_t rgb_lo[3] = {val, val, val};
        bool latch = (ch == DP3364_NUM_CHANNELS);
        dp3364_panel_draw(rgb_hi, rgb_lo, latch);
    }
}

// row != 0 ? ROW high during 4 CLK pulses : 12 CLK pulses 
static inline void dp3364_change_row(uint row)
{
    set_gpio_for_clk_pulses(PIN_ROW, (row == 0) ? 12 : 4);
}

// Advance the DP32020A row outputs by issuing ONE DCK (per datasheet).
// DP32020 datasheet specifies DCK high width >= 500 ns (Tshadow).
static inline void dp32020_advance_row(uint line) 
{
    gpio_put(PIN_ROW_DAT, line == 0);
    gpio_put(PIN_ROW_DCK, 1);
    // Tshadow >= 500 ns -> use 1 us margin
    asm("\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n");
    gpio_put(PIN_ROW_DCK, 0);
    gpio_put(PIN_ROW_DAT, 0);
}

#endif // LED_PANEL_H
