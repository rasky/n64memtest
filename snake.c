#include <libdragon.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define S_W 34
#define S_H 22
#define S_C 8
#define S_X 84
#define S_Y 42
#define S_MAX 192
#define S_STEP_BASE_US 120000U
#define S_STEP_MIN_US 70000U
#define S_STEP_DEC_US 750U
#define S_GAME_OVER_DELAY_US 1000000ULL

/* Ultra-minimal snake: all game state is static and local to this file. */
static bool s_on;
static bool s_dead;

static int s_len;
static int s_dx;
static int s_dy;
static int s_ndx;
static int s_ndy;
static int s_fx;
static int s_fy;
static int s_x[S_MAX];
static int s_y[S_MAX];

static uint64_t s_tick_us;
static uint64_t s_dead_us;

static uint32_t c_bg;
static uint32_t c_bd;
static uint32_t c_sn;
static uint32_t c_hd;
static uint32_t c_fd;
static uint32_t c_tx;
static uint32_t c_go;
static uint32_t c_panel;

static int s_rnd(int n)
{
    return rand() % n;
}

static void s_colors(void)
{
    c_bg = graphics_convert_color(RGBA32(0x22, 0x22, 0x22, 0xFF));
    c_panel = graphics_convert_color(RGBA32(0x3C, 0x38, 0x36, 0xFF));
    c_bd = graphics_convert_color(RGBA32(0x83, 0xA5, 0x98, 0xFF));
    c_sn = graphics_convert_color(RGBA32(0xB8, 0xBB, 0x26, 0xFF));
    c_hd = graphics_convert_color(RGBA32(0xFB, 0xF1, 0xC7, 0xFF));
    c_fd = graphics_convert_color(RGBA32(0xFB, 0x49, 0x34, 0xFF));
    c_tx = graphics_convert_color(RGBA32(0xFB, 0xF1, 0xC7, 0xFF));
    c_go = graphics_convert_color(RGBA32(0xFF, 0xE0, 0x00, 0xFF));
}

static void s_food(void)
{
    int i;
    bool ok;

    do {
        s_fx = s_rnd(S_W);
        s_fy = s_rnd(S_H);
        ok = true;

        for (i = 0; i < s_len; i++) {
            if (s_x[i] == s_fx && s_y[i] == s_fy) {
                ok = false;
                break;
            }
        }
    } while (!ok);
}

static void s_reset(void)
{
    int i;

    s_dead = false;
    s_len = 4;

    s_dx = 1;
    s_dy = 0;
    s_ndx = 1;
    s_ndy = 0;

    s_tick_us = get_ticks_us();
    s_dead_us = 0;
    srand(s_tick_us);

    for (i = 0; i < s_len; i++) {
        s_x[i] = S_W / 2 - i;
        s_y[i] = S_H / 2;
    }

    s_food();
}

static uint32_t s_step_us(void)
{
    uint32_t dec;
    uint32_t floor_range = S_STEP_BASE_US - S_STEP_MIN_US;

    if (s_len <= 4) return S_STEP_BASE_US;

    dec = (uint32_t)(s_len - 4) * S_STEP_DEC_US;
    if (dec >= floor_range) return S_STEP_MIN_US;

    return S_STEP_BASE_US - dec;
}

static void s_step(void)
{
    int i;
    int nx;
    int ny;

    /* Apply queued direction only if it's not a direct 180 turn. */
    if (s_ndx != -s_dx || s_ndy != -s_dy) {
        s_dx = s_ndx;
        s_dy = s_ndy;
    }

    nx = s_x[0] + s_dx;
    ny = s_y[0] + s_dy;

    if (nx < 0 || ny < 0 || nx >= S_W || ny >= S_H) {
        s_dead = true;
        if (!s_dead_us) s_dead_us = get_ticks_us();
        return;
    }

    for (i = 0; i < s_len; i++) {
        if (s_x[i] == nx && s_y[i] == ny) {
            s_dead = true;
            if (!s_dead_us) s_dead_us = get_ticks_us();
            return;
        }
    }

    for (i = s_len; i > 0; i--) {
        s_x[i] = s_x[i - 1];
        s_y[i] = s_y[i - 1];
    }

    s_x[0] = nx;
    s_y[0] = ny;

    if (nx == s_fx && ny == s_fy) {
        if (s_len < (S_MAX - 1)) s_len++;
        s_food();
    }
}

bool snake_is_active(void)
{
    return s_on;
}

void snake_stop(void)
{
    s_on = false;
}

void snake_toggle(void)
{
    s_on = !s_on;
    if (s_on) s_reset();
}

void snake_handle_input(joypad_buttons_t p)
{
    if (!s_on || s_dead) return;

    if (p.d_up && s_dy != 1) {
        s_ndx = 0;
        s_ndy = -1;
        return;
    }

    if (p.d_down && s_dy != -1) {
        s_ndx = 0;
        s_ndy = 1;
        return;
    }

    if (p.d_left && s_dx != 1) {
        s_ndx = -1;
        s_ndy = 0;
        return;
    }

    if (p.d_right && s_dx != -1) {
        s_ndx = 1;
        s_ndy = 0;
    }
}

void snake_render(surface_t *d, bool has_progress, uint32_t progress_pct, uint32_t errors)
{
    char line[64];
    int i;
    int hud_x = 8;
    int hud_y = 21;
    uint64_t now = get_ticks_us();
    uint32_t step_us;

    if (!s_on) return;

    s_colors();

    /*
     * Fixed-step update keeps speed stable across FPS.
     * Step shrinks a bit on each growth, for very gradual acceleration.
     */
    step_us = s_step_us();
    while (!s_dead && (now - s_tick_us) >= step_us) {
        s_tick_us += step_us;
        s_step();
        step_us = s_step_us();
    }

    /* Minimal HUD text on normal background (no card/border). */

    if (has_progress) {
        snprintf(line, sizeof(line), "Test: %3lu%%", (unsigned long)progress_pct);
    } else {
        snprintf(line, sizeof(line), "Test: N/A");
    }
    graphics_set_color(c_tx, c_panel);
    graphics_draw_text(d, hud_x, hud_y, line);

    graphics_set_color(errors ? c_fd : c_sn, c_panel);
    snprintf(line, sizeof(line), "Err: %lu", (unsigned long)errors);
    graphics_draw_text(d, hud_x + 108, hud_y, line);

    graphics_draw_box(d, S_X - 2, S_Y - 2, S_W * S_C + 4, S_H * S_C + 4, c_bd);
    graphics_draw_box(d, S_X, S_Y, S_W * S_C, S_H * S_C, c_bg);

    for (i = s_len - 1; i > 0; i--) {
        graphics_draw_box(d, S_X + s_x[i] * S_C, S_Y + s_y[i] * S_C,
                          S_C - 1, S_C - 1, c_sn);
    }

    graphics_draw_box(d, S_X + s_x[0] * S_C, S_Y + s_y[0] * S_C, S_C - 1, S_C - 1, c_hd);
    graphics_draw_box(d, S_X + s_fx * S_C, S_Y + s_fy * S_C, S_C - 1, S_C - 1, c_fd);

    if (s_dead && s_dead_us && (now - s_dead_us) >= S_GAME_OVER_DELAY_US) {
        const char *msg = "GAME OVER";
        int x = display_get_width() / 2 - (9 * 8) / 2;
        int y = display_get_height() / 2 - 4;
        graphics_set_color(c_go, c_bg);
        graphics_draw_text(d, x, y, msg);
    }
}
