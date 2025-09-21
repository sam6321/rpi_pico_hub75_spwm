#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "led_panel.h"
#include "minigif.h"
#include "gif.h"

static mutex_t frame_buff_mtx;
static uint16_t frame_buf_a[MATRIX_WIDTH * MATRIX_HEIGHT * 3];
static uint16_t frame_buf_b[MATRIX_WIDTH * MATRIX_HEIGHT * 3];
static uint16_t *display_buf = frame_buf_a;  // used by display_routine()
static uint16_t *tmp_buf = frame_buf_b;      // used by painter_cb()

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
} memfile_t;

static long _read_bytes_cb(void *f, uint8_t *val, size_t size)
{
    memfile_t *mem = (memfile_t *)f;
    if (mem->pos + size > mem->size) {
        size = mem->size - mem->pos; // Clamp read to available bytes
    }

    memcpy(val, mem->data + mem->pos, size);
    mem->pos += size;
    return size;
}

static long _lseek_cb(void *f, long off, int seek)
{
    memfile_t *mem = (memfile_t *)f;

    size_t new_pos = 0;
    switch (seek) {
        case SEEK_SET:
            new_pos = off;
            break;
        case SEEK_CUR:
            new_pos = mem->pos + off;
            break;
        case SEEK_END:
            new_pos = mem->size + off;
            break;
        default:
            return -1;
    }

    if (new_pos > mem->size) {
        return -1;
    }
    
    mem->pos = new_pos;
    return (long)mem->pos;
}

static void _painter_cb(uint16_t x, uint16_t y, gif_rgb_t rgb, void *user_data) {
    size_t idx = (y * MATRIX_WIDTH + x) * 3;
    tmp_buf[idx + 0] = rgb.r << 6;
    tmp_buf[idx + 1] = rgb.g << 6;
    tmp_buf[idx + 2] = rgb.b << 6;
}

static void core1_entry() {
    dp3364_init();
    
    while (1) {
        mutex_enter_blocking(&frame_buff_mtx);
        uint16_t *fb = display_buf;

        dp3364_vsync();
        for (uint line = 0; line < 32; line++) {
            for (uint col = 0; col < 16; col++) { // column within driver chip
                for (uint ch = 1; ch <= CHAINED_DP3364_NUM; ch++) { // throughout each driver chip
                    uint8_t x = ((ch - 1) * 16) + col;   // global X (0..127)

                    // low half
                    size_t idx_lo = (line * MATRIX_WIDTH + x) * 3;
                    uint16_t rgb_lo[3] = {
                        fb[idx_lo + 0],
                        fb[idx_lo + 1],
                        fb[idx_lo + 2]
                    };

                    // high half
                    size_t idx_hi = ((line + 32) * MATRIX_WIDTH + x) * 3;
                    uint16_t rgb_hi[3] = {
                        fb[idx_hi + 0],
                        fb[idx_hi + 1],
                        fb[idx_hi + 2]
                    };
                    
                    bool latch = ch == CHAINED_DP3364_NUM;

                    dp3364_panel_draw(rgb_lo, rgb_hi, latch);
                }
            }
            led_panel_advance_row(line);

            const uint clk_cycles_per_line = (2 * (0x0 + 1) + 2 * (0x8 + 1) + 4 * (0x7f + 1)) / (0b010 + 1);
            
            for (uint i = 0; i < (clk_cycles_per_line/8 + 1); i++) {
                clock_pulse();
            }
        }
        mutex_exit(&frame_buff_mtx);
    }
}

static uint32_t get_time_ms() {
    return to_ms_since_boot(get_absolute_time());
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

    memset(frame_buf_a, 0, sizeof(frame_buf_a));
    memset(frame_buf_b, 0, sizeof(frame_buf_b));
    mutex_init(&frame_buff_mtx);

    static memfile_t gif_mem = {
        .data = gif_file,
        .size = sizeof(gif_file),
        .pos = 0
    };

    static uint8_t gif_buf[sizeof(gif_t)];

    gif_cb_t gif_cb = {
        .read = _read_bytes_cb,
        .lseek = _lseek_cb,
        .painter = _painter_cb,
        .user_data = NULL,
    };

    gif_handle_t gif = minigif_init(gif_buf, &gif_mem, gif_cb);
    assert(gif != NULL);

    static uint32_t core1_stack[256];
    multicore_launch_core1_with_stack(core1_entry, core1_stack, 1024);

    while (1) {
        uint32_t t0 = get_time_ms();

        gif_status_t ret = minigif_render_frame(gif);
        // assert(ret != GIF_STATUS_ERROR);

        if (ret == GIF_STATUS_GIF_END) {
            minigif_rewind(gif);
            minigif_render_frame(gif);
        }

        mutex_enter_blocking(&frame_buff_mtx);
        uint16_t *old = display_buf;
        display_buf = tmp_buf;
        tmp_buf = old;
        mutex_exit(&frame_buff_mtx);

        uint32_t t1 = get_time_ms();
        uint32_t delta = t1 - t0;
        uint32_t gif_delay_ms = minigif_get_frame_delay(gif) * 10;
        uint32_t task_delay_ms = gif_delay_ms > delta ? gif_delay_ms - delta : 1;
        
        busy_wait_ms(task_delay_ms);
    }

    return 0;
}
