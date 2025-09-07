#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "led_panel.h"
#include "image.h"

static inline void rgb565_conversion(uint16_t c, uint16_t *r, uint16_t *g, uint16_t *b)
{
    *r = ((((c) >> 11) & 0x1F) << 14) >> 5;
    *g = ((((c) >>  5) & 0x3F) << 14) >> 6;
    *b = ((((c) >>  0) & 0x1F) << 14) >> 5;
}

static uint16_t fb[128 * 64 * 3];

static inline uint16_t image_get_pixel(uint x, uint y)
{
    return ((uint16_t *)mountains_128x64)[y * 128 + x];
}

static void core1_entry() {
    dp3364_init();
    
    while (1) 
    {
        dp3364_vsync();
        for (uint line = 0; line < 32; line++) 
        { // Y
            for (uint col = 0; col < 16; col++) 
            { // channel in driver chip
                for (uint ch = 1; ch <= DP3364_NUM_CHANNELS; ch++) 
                { // each driver chip
                    uint8_t x = ((ch - 1) * 16) + col;   // global X (0..127)

                    // low half
                    size_t idx_lo = (line * 128 + x) * 3;
                    uint16_t rgb_lo[3] = {
                        fb[idx_lo + 0],
                        fb[idx_lo + 1],
                        fb[idx_lo + 2]
                    };

                    // high half
                    size_t idx_hi = ((line + 32) * 128 + x) * 3;
                    uint16_t rgb_hi[3] = {
                        fb[idx_hi + 0],
                        fb[idx_hi + 1],
                        fb[idx_hi + 2]
                    };
                    
                    bool latch = ch == DP3364_NUM_CHANNELS;

                    dp3364_panel_draw(rgb_lo, rgb_hi, latch);
                }
            }
            dp3364_change_row(line);
            dp32020_advance_row(line);

            const uint clk_cycles_per_line = (2 * (0x0 + 1) + 2 * (0x8 + 1) + 4 * (0x7f + 1)) / (0b010 + 1);
            
            for (uint i = 0; i < (clk_cycles_per_line/8 + 1); i++) {
                clock_pulse();
            }
        }
    }
}

int main() {
    set_sys_clock_hz(200 * MHZ, true);
    stdio_init_all();

    // init pins 0..13 as outputs
    const uint32_t init_mask = 0x3FFF;
    gpio_init_mask(init_mask);
    gpio_set_dir_out_masked(init_mask);
    gpio_put_masked(init_mask, 0);

    for (uint i = 0; i <= 13; i++) {
        gpio_set_drive_strength(i, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(i, GPIO_SLEW_RATE_FAST);
    }

    // Ensure DP3364/DP32020 power rails are applied and stable after this point
    busy_wait_ms(2);
    
    for (size_t i = 0; i < sizeof(mountains_128x64)/sizeof(uint16_t); i++) {
        uint16_t c = image_get_pixel(i % 128, i / 128);
        rgb565_conversion(c, &fb[i*3+0], &fb[i*3+1], &fb[i*3+2]);
    }

    multicore_launch_core1(core1_entry);

    while(1);

    return 0;
}
