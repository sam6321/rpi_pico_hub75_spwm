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

// Control pins wired to connector signals
#define PIN_CLK 11  // DP3364S DCLK
#define PIN_LE  12  // DP3364S LE (LATCH)
#define PIN_ROW 13  // DP3364S ROW

// Row-address / DP32020 control pins (HUB75 connector A/B/C)
#define PIN_ROW_DCK   6   // on DP32020 this is DCK (data clock)
// #define PIN_ROW_RCK   7   // on DP32020 this is RCK (register clock)
#define PIN_ROW_DAT   8   // on DP32020 this is DIN (data) mapping

#define CHAINED_DP3364_NUM 8

#define clock_delay() {asm volatile("nop"); asm volatile("nop");}

#define clock_pulse() \
    do { \
        gpio_put(PIN_CLK, 1); \
        clock_delay(); \
        gpio_put(PIN_CLK, 0); \
        clock_delay(); \
    } while (0)

void dp3364_set_dual_edge_mode(bool dual_edge);
void dp3364_vsync(void);
void dp3364_pre_act(void);
void dp3364_write_reg(uint8_t reg, uint8_t val, bool latch);

// High-level functions
void dp3364_init(void);
void dp3364_panel_draw(uint16_t rgb_lo[3], uint16_t rgb_hi[3], bool latch);
void led_panel_advance_row(uint line);

#endif // LED_PANEL_H
