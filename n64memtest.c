#include <libdragon.h>
#include <stdbool.h>
#include <stdint.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mem_tests.h"
#include "logo.h"

#define UI_LINE_HEIGHT 10
#define UI_X_PADDING 8
#define UI_Y_PADDING 8
#define FAILURE_LOG_CAP 12
#ifndef MEMTEST_CHUNKS_PER_FRAME
#define MEMTEST_CHUNKS_PER_FRAME 32
#endif

typedef enum {
    SETUP_ROW_START = 0,
    SETUP_ROW_PROFILE,
    SETUP_ROW_LOOPS,
    SETUP_ROW_BANK_BASE,
} setup_row_t;

typedef struct {
    uint32_t bg;
    uint32_t panel;
    uint32_t fg;
    uint32_t muted;
    uint32_t accent;
    uint32_t ok;
    uint32_t warn;
    uint32_t fail;
} ui_palette_t;

int g_total_memory_bytes;
int g_total_banks;
int g_usable_banks;
bool g_bank_available[MAX_BANKS];
bool g_bank_enabled[MAX_BANKS];
bool g_test_enabled[MEMTEST_ID_COUNT];
memtest_profile_t g_profile;
uint32_t g_loop_target;

run_state_t g_run_state;
bool g_stop_requested;
uint32_t g_loop_completed;
uint32_t g_bytes_tested;
uint32_t g_slices_done;
uint32_t g_error_count;
uint32_t g_restore_error_count;
uint64_t g_start_ms;
uint32_t g_elapsed_ms;

int g_setup_cursor;
int g_setup_rows_total;

int g_current_test;
uint32_t g_current_pass;
int g_current_bank;
uint32_t g_current_bank_offset;
uint32_t g_rng_seed;
mem_failure_t g_last_failure;
mem_failure_t g_failure_log[FAILURE_LOG_CAP];
uint32_t g_failure_log_head;
uint32_t g_failure_log_count;
static int g_last_logged_test = -1;
static uint32_t g_last_logged_pass = UINT32_MAX;
static bool g_summary_logged = false;

static ui_palette_t g_pal;

static inline int min_int(int a, int b) { return a < b ? a : b; }

static bool any_bank_enabled(void)
{
    int i;
    for (i = 0; i < g_total_banks; i++) {
        if (g_bank_available[i] && g_bank_enabled[i]) return true;
    }
    return false;
}

static bool any_test_enabled(void)
{
    int i;
    for (i = 0; i < MEMTEST_ID_COUNT; i++) {
        if (g_test_enabled[i]) return true;
    }
    return false;
}

static int find_next_enabled_bank(int start)
{
    int i;
    for (i = start; i < g_total_banks; i++) {
        if (g_bank_available[i] && g_bank_enabled[i]) return i;
    }
    return -1;
}

static int find_next_enabled_test(int start)
{
    int i;
    for (i = start; i < MEMTEST_ID_COUNT; i++) {
        if (g_test_enabled[i]) return i;
    }
    return -1;
}

static const char *run_state_name(run_state_t state)
{
    switch (state) {
        case RUN_IDLE: return "Idle";
        case RUN_RUNNING: return "Running";
        case RUN_PAUSED: return "Paused";
        case RUN_STOPPING: return "Stopping";
        case RUN_DONE: return "Done";
        default: return "?";
    }
}

static const char *loop_target_name(uint32_t loop_target)
{
    switch (loop_target) {
        case 0: return "Infinite";
        case 1: return "1";
        case 5: return "5";
        case 20: return "20";
        default: return "?";
    }
}

static void cycle_loop_target(void)
{
    switch (g_loop_target) {
        case 1: g_loop_target = 5; break;
        case 5: g_loop_target = 20; break;
        case 20: g_loop_target = 0; break;
        default: g_loop_target = 1; break;
    }
}

static void apply_profile(memtest_profile_t profile)
{
    g_profile = profile;
    memtest_apply_profile_defaults(profile, g_test_enabled);
    if (profile == MEMTEST_PROFILE_BURNIN) {
        g_loop_target = 0; /* Burn-in is always continuous. */
    } else if (g_loop_target == 0) {
        g_loop_target = 1;
    }
}

static uint32_t next_burnin_seed(uint32_t seed)
{
    /* Xorshift32 + timer stir for loop-to-loop variability. */
    uint32_t x = seed ? seed : 0x1337c0deU;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    x ^= (uint32_t)get_ticks_us();
    return x ? x : 0xA5A5A5A5U;
}

static void format_elapsed_compact(char *out, size_t out_size, uint32_t elapsed_ms)
{
    uint32_t total_s = elapsed_ms / 1000U;
    uint32_t s = total_s % 60U;
    uint32_t total_m = total_s / 60U;
    uint32_t m = total_m % 60U;
    uint32_t h = total_m / 60U;

    if (h > 0U) {
        snprintf(out, out_size, "%02luh%02lum%02lus",
                 (unsigned long)h, (unsigned long)m, (unsigned long)s);
    } else {
        snprintf(out, out_size, "%02lum%02lus",
                 (unsigned long)m, (unsigned long)s);
    }
}

static void init_palette(void)
{
    /* Gruvbox dark-inspired palette */
    g_pal.bg = graphics_convert_color(RGBA32(0x28, 0x28, 0x28, 0xFF));      /* bg0 */
    g_pal.panel = graphics_convert_color(RGBA32(0x3c, 0x38, 0x36, 0xFF));   /* bg1 */
    g_pal.fg = graphics_convert_color(RGBA32(0xfb, 0xf1, 0xc7, 0xFF));      /* fg0 */
    g_pal.muted = graphics_convert_color(RGBA32(0xd5, 0xc4, 0xa1, 0xFF));   /* fg2 */
    g_pal.accent = graphics_convert_color(RGBA32(0x83, 0xa5, 0x98, 0xFF));  /* bright_blue */
    g_pal.ok = graphics_convert_color(RGBA32(0xb8, 0xbb, 0x26, 0xFF));      /* bright_green */
    g_pal.warn = graphics_convert_color(RGBA32(0xfa, 0xbd, 0x2f, 0xFF));    /* bright_yellow */
    g_pal.fail = graphics_convert_color(RGBA32(0xfb, 0x49, 0x34, 0xFF));    /* bright_red */
}

static void reset_runtime_counters(void)
{
    g_loop_completed = 0;
    g_bytes_tested = 0;
    g_slices_done = 0;
    g_error_count = 0;
    g_restore_error_count = 0;
    g_elapsed_ms = 0;
    g_stop_requested = false;
    g_last_failure.valid = false;
    g_failure_log_head = 0;
    g_failure_log_count = 0;
    g_last_logged_test = -1;
    g_last_logged_pass = UINT32_MAX;
    g_summary_logged = false;
}

static void prepare_first_work_item(void)
{
    g_current_test = find_next_enabled_test(0);
    g_current_pass = 0;
    g_current_bank = find_next_enabled_bank(0);
    g_current_bank_offset = 0;
}

static bool work_item_valid(void);

static uint32_t count_enabled_banks(void)
{
    uint32_t count = 0;
    int i;
    for (i = 0; i < g_total_banks; i++) {
        if (g_bank_available[i] && g_bank_enabled[i]) count++;
    }
    return count;
}

static uint32_t count_enabled_passes_per_loop(void)
{
    uint32_t total = 0;
    int i;
    for (i = 0; i < MEMTEST_ID_COUNT; i++) {
        if (g_test_enabled[i]) {
            const memtest_desc_t *d = memtest_desc((memtest_id_t)i);
            if (d) total += d->passes;
        }
    }
    return total;
}

static uint32_t count_enabled_passes_before_test(int test_id)
{
    uint32_t total = 0;
    int i;
    for (i = 0; i < test_id && i < MEMTEST_ID_COUNT; i++) {
        if (g_test_enabled[i]) {
            const memtest_desc_t *d = memtest_desc((memtest_id_t)i);
            if (d) total += d->passes;
        }
    }
    return total;
}

static uint32_t count_enabled_banks_before_current(void)
{
    uint32_t count = 0;
    int i;
    for (i = 0; i < g_current_bank && i < g_total_banks; i++) {
        if (g_bank_available[i] && g_bank_enabled[i]) count++;
    }
    return count;
}

static bool total_progress_percent(uint32_t *out_percent)
{
    uint64_t total_units;
    uint64_t done_units = 0;
    uint32_t banks = count_enabled_banks();
    uint32_t passes_per_loop = count_enabled_passes_per_loop();

    if (!out_percent) return false;
    if (g_loop_target == 0 || banks == 0 || passes_per_loop == 0) return false;

    total_units = (uint64_t)g_loop_target * passes_per_loop * banks * BANK_SIZE_BYTES;
    done_units += (uint64_t)g_loop_completed * passes_per_loop * banks * BANK_SIZE_BYTES;

    if (work_item_valid()) {
        uint32_t passes_before = count_enabled_passes_before_test(g_current_test);
        uint32_t banks_before = count_enabled_banks_before_current();
        done_units += (uint64_t)(passes_before + g_current_pass) * banks * BANK_SIZE_BYTES;
        done_units += (uint64_t)banks_before * BANK_SIZE_BYTES;
        done_units += g_current_bank_offset;
    } else if (g_run_state == RUN_DONE) {
        done_units = total_units;
    }

    if (done_units >= total_units) {
        *out_percent = 100;
    } else {
        *out_percent = (uint32_t)((done_units * 100U) / total_units);
    }
    return true;
}

static void log_pass_start_if_needed(void)
{
    if (!work_item_valid()) return;
    if (g_current_test == g_last_logged_test && g_current_pass == g_last_logged_pass) return;
    g_last_logged_test = g_current_test;
    g_last_logged_pass = g_current_pass;
    debugf("[memtest] pass-start profile=%s loop=%lu test=%s pass=%lu banks=%d seed=%08lx\n",
           memtest_profile_name(g_profile),
           (unsigned long)(g_loop_completed + 1U),
           memtest_desc((memtest_id_t)g_current_test) ? memtest_desc((memtest_id_t)g_current_test)->name : "?",
           (unsigned long)(g_current_pass + 1U),
           g_total_banks,
           (unsigned long)g_rng_seed);
}

static void log_run_summary_once(void)
{
    if (g_summary_logged) return;
    g_summary_logged = true;
    debugf("[memtest] summary profile=%s loops=%lu bytes=%lu slices=%lu errors=%lu restore=%lu elapsed_ms=%lu state=%s\n",
           memtest_profile_name(g_profile),
           (unsigned long)g_loop_completed,
           (unsigned long)g_bytes_tested,
           (unsigned long)g_slices_done,
           (unsigned long)g_error_count,
           (unsigned long)g_restore_error_count,
           (unsigned long)g_elapsed_ms,
           run_state_name(g_run_state));
    if (g_last_failure.valid) {
        debugf("[memtest] summary-last-fail bank=%u test=%s pass=%lu addr=%08lx exp=%08lx got=%08lx\n",
               g_last_failure.bank,
               memtest_desc(g_last_failure.test) ? memtest_desc(g_last_failure.test)->name : "?",
               (unsigned long)(g_last_failure.pass + 1U),
               (unsigned long)g_last_failure.address,
               (unsigned long)g_last_failure.expected,
               (unsigned long)g_last_failure.actual);
    }
}

static bool work_item_valid(void)
{
    return g_current_test >= 0 && g_current_bank >= 0;
}

bool memtest_advance_to_next_work_item(void)
{
    const memtest_desc_t *desc;
    int next_bank;
    int next_test;

    if (!work_item_valid()) return false;

    next_bank = find_next_enabled_bank(g_current_bank + 1);
    if (next_bank >= 0) {
        g_current_bank = next_bank;
        g_current_bank_offset = 0;
        log_pass_start_if_needed();
        return true;
    }

    desc = memtest_desc((memtest_id_t)g_current_test);
    g_current_pass++;
    if (desc && g_current_pass < desc->passes) {
        g_current_bank = find_next_enabled_bank(0);
        g_current_bank_offset = 0;
        log_pass_start_if_needed();
        return work_item_valid();
    }

    next_test = find_next_enabled_test(g_current_test + 1);
    if (next_test >= 0) {
        g_current_test = next_test;
        g_current_pass = 0;
        g_current_bank = find_next_enabled_bank(0);
        g_current_bank_offset = 0;
        log_pass_start_if_needed();
        return work_item_valid();
    }

    g_loop_completed++;
    if (g_loop_target && g_loop_completed >= g_loop_target) {
        g_run_state = RUN_DONE;
        return false;
    }

    if (g_profile == MEMTEST_PROFILE_BURNIN) {
        g_rng_seed = next_burnin_seed(g_rng_seed);
    }

    prepare_first_work_item();
    log_pass_start_if_needed();
    return work_item_valid();
}

void memtest_capture_failure(uintptr_t addr, uint32_t exp, uint32_t got)
{
    g_error_count++;
    g_last_failure.valid = true;
    g_last_failure.address = (uint32_t)PhysicalAddr((void *)addr);
    g_last_failure.expected = exp;
    g_last_failure.actual = got;
    g_last_failure.pass = g_current_pass;
    g_last_failure.bank = (uint8_t)g_current_bank;
    g_last_failure.test = (memtest_id_t)g_current_test;

    g_failure_log[g_failure_log_head] = g_last_failure;
    g_failure_log_head = (g_failure_log_head + 1U) % FAILURE_LOG_CAP;
    if (g_failure_log_count < FAILURE_LOG_CAP) {
        g_failure_log_count++;
    }

    {
        debugf("[memtest] error bank=%u test=%s pass=%lu addr=%08lx exp=%08lx got=%08lx total=%lu restore=%lu\n",
               g_last_failure.bank,
               memtest_desc(g_last_failure.test) ? memtest_desc(g_last_failure.test)->name : "?",
               (unsigned long)(g_last_failure.pass + 1U),
               (unsigned long)g_last_failure.address,
               (unsigned long)g_last_failure.expected,
               (unsigned long)g_last_failure.actual,
               (unsigned long)g_error_count,
               (unsigned long)g_restore_error_count);
    }
}

static void draw_row(surface_t *disp, int x, int w, int y, bool selected, uint32_t fg, const char *txt)
{
    if (selected) {
        graphics_draw_box(disp, x - 2, y - 1, w, UI_LINE_HEIGHT, g_pal.accent);
        graphics_set_color(g_pal.bg, g_pal.accent);
    } else {
        graphics_set_color(fg, g_pal.panel);
    }
    graphics_draw_text(disp, x, y, txt);
}

static void render_setup_screen(surface_t *disp)
{
    char line[96];
    int y = UI_Y_PADDING + 12;
    int screen_w = display_get_width();
    int col_left = UI_X_PADDING;
    int col_right = screen_w / 2 + 4;
    int left_w = col_right - col_left - 6;
    int right_w = screen_w - col_right - UI_X_PADDING + 2;
    int footer_y = display_get_height() - 12;
    int tests_y_start = UI_Y_PADDING + 12 + (UI_LINE_HEIGHT * 5) + 4;
    int tests_rows_visible = (footer_y - tests_y_start) / UI_LINE_HEIGHT;
    int tests_first = 0;
    int tests_last;
    int i;
    int row = 0;
    int test_row_base = SETUP_ROW_BANK_BASE + g_total_banks;

    if (tests_rows_visible < 1) tests_rows_visible = 1;
    if (MEMTEST_ID_COUNT > tests_rows_visible && g_setup_cursor >= test_row_base) {
        int selected_test = g_setup_cursor - test_row_base;
        tests_first = selected_test - (tests_rows_visible / 2);
        if (tests_first < 0) tests_first = 0;
        if (tests_first > MEMTEST_ID_COUNT - tests_rows_visible) {
            tests_first = MEMTEST_ID_COUNT - tests_rows_visible;
        }
    }
    tests_last = tests_first + tests_rows_visible;
    if (tests_last > MEMTEST_ID_COUNT) tests_last = MEMTEST_ID_COUNT;

    snprintf(line, sizeof(line), "Start test run");
    draw_row(disp, col_left, left_w, y, g_setup_cursor == row++, g_pal.ok, line);
    y += UI_LINE_HEIGHT;

    snprintf(line, sizeof(line), "Profile: %s", memtest_profile_name(g_profile));
    draw_row(disp, col_left, left_w, y, g_setup_cursor == row++, g_pal.fg, line);
    y += UI_LINE_HEIGHT;

    snprintf(line, sizeof(line), "Loops: %s", loop_target_name(g_loop_target));
    draw_row(disp, col_left, left_w, y, g_setup_cursor == row++, g_pal.fg, line);
    y += UI_LINE_HEIGHT;

    y += 4;

    graphics_set_color(g_pal.muted, g_pal.panel);
    graphics_draw_text(disp, col_left, y, "Banks (1MiB):");
    snprintf(line, sizeof(line), "Tests %d-%d/%d:", tests_first + 1, tests_last, MEMTEST_ID_COUNT);
    graphics_draw_text(disp, col_right, y, line);

    y = tests_y_start;
    for (i = 0; i < g_total_banks; i++) {
        const char *status = g_bank_available[i] ? (g_bank_enabled[i] ? "ON " : "OFF") : "N/A";
        snprintf(line, sizeof(line), "Bank %d: %s", i, status);
        draw_row(disp, col_left, left_w, y, g_setup_cursor == (SETUP_ROW_BANK_BASE + i), g_pal.fg, line);
        y += UI_LINE_HEIGHT;
    }

    y = tests_y_start;
    for (i = tests_first; i < tests_last; i++) {
        const memtest_desc_t *desc = memtest_desc((memtest_id_t)i);
        snprintf(line, sizeof(line), "[%c] %s", g_test_enabled[i] ? 'x' : ' ', desc ? desc->name : "?");
        draw_row(disp, col_right, right_w, y, g_setup_cursor == (test_row_base + i), g_pal.fg, line);
        y += UI_LINE_HEIGHT;
    }
}

static void render_running_screen(surface_t *disp)
{
    char line[112];
    const memtest_desc_t *desc = memtest_desc((memtest_id_t)g_current_test);
    uint32_t progress_pct = 0;
    bool has_progress = total_progress_percent(&progress_pct);
    uint32_t state_color = (g_run_state == RUN_PAUSED) ? g_pal.warn : g_pal.ok;
    int left = UI_X_PADDING;
    int right = display_get_width() / 2 + 24;

    graphics_set_color(state_color, g_pal.panel);
    snprintf(line, sizeof(line), "State: %s", run_state_name(g_run_state));
    graphics_draw_text(disp, left, UI_Y_PADDING + 12, line);

    graphics_set_color(g_pal.fg, g_pal.panel);
    snprintf(line, sizeof(line), "Loop: %lu / %s", (unsigned long)(g_loop_completed + 1), loop_target_name(g_loop_target));
    graphics_draw_text(disp, left, UI_Y_PADDING + 24, line);

    graphics_set_color(g_pal.fg, g_pal.panel);
    snprintf(line, sizeof(line), "Test: %s (pass %lu)", desc ? desc->name : "?", (unsigned long)(g_current_pass + 1));
    graphics_draw_text(disp, left, UI_Y_PADDING + 36, line);

    if (has_progress) {
    snprintf(line, sizeof(line), "Bank %d/%d  Total: %lu%%", g_current_bank, g_total_banks - 1, (unsigned long)progress_pct);
    } else {
        snprintf(line, sizeof(line), "Bank %d/%d  Total: N/A", g_current_bank, g_total_banks - 1);
    }
    graphics_draw_text(disp, left, UI_Y_PADDING + 48, line);

    snprintf(line, sizeof(line), "Tested: %lu KiB  Slices: %lu",
             (unsigned long)(g_bytes_tested / 1024U), (unsigned long)g_slices_done);
    graphics_draw_text(disp, left, UI_Y_PADDING + 60, line);

    graphics_set_color(g_pal.muted, g_pal.panel);
    graphics_draw_text(disp, right, UI_Y_PADDING + 12, "Errors");

    graphics_set_color(g_error_count ? g_pal.fail : g_pal.ok, g_pal.panel);
    snprintf(line, sizeof(line), "Err: %lu  Restore: %lu",
             (unsigned long)g_error_count, (unsigned long)g_restore_error_count);
    graphics_draw_text(disp, right, UI_Y_PADDING + 24, line);

    if (g_failure_log_count > 0) {
        uint32_t shown = 0;
        uint32_t idx = g_failure_log_head;
        graphics_set_color(g_pal.muted, g_pal.panel);
        graphics_draw_text(disp, left, UI_Y_PADDING + 78, "Latest fails");
        graphics_set_color(g_pal.fail, g_pal.panel);
        while (shown < g_failure_log_count && shown < 8) {
            const mem_failure_t *entry;
            if (idx == 0) idx = FAILURE_LOG_CAP;
            idx--;
            entry = &g_failure_log[idx];
            snprintf(line, sizeof(line), "#%lu B%u P%lu A:%08lX E:%08lX G:%08lX",
                     (unsigned long)(shown + 1),
                     entry->bank,
                     (unsigned long)(entry->pass + 1),
                     (unsigned long)entry->address,
                     (unsigned long)entry->expected,
                     (unsigned long)entry->actual);
            graphics_draw_text(disp, left, UI_Y_PADDING + 90 + (int)(shown * UI_LINE_HEIGHT), line);
            shown++;
        }
    } else {
        graphics_set_color(g_pal.ok, g_pal.panel);
        graphics_draw_text(disp, left, UI_Y_PADDING + 78, "No mismatches.");
    }
}

static void render_report_screen(surface_t *disp)
{
    char line[112];
    char elapsed_compact[32];
    graphics_set_color(g_pal.ok, g_pal.panel);
    graphics_draw_text(disp, UI_X_PADDING, UI_Y_PADDING + 12, "Report");

    graphics_set_color(g_pal.fg, g_pal.panel);
    snprintf(line, sizeof(line), "Loops: %lu", (unsigned long)g_loop_completed);
    graphics_draw_text(disp, UI_X_PADDING, UI_Y_PADDING + 28, line);

    snprintf(line, sizeof(line), "Tested: %lu KiB", (unsigned long)(g_bytes_tested / 1024U));
    graphics_draw_text(disp, UI_X_PADDING, UI_Y_PADDING + 40, line);

    snprintf(line, sizeof(line), "Slices: %lu", (unsigned long)g_slices_done);
    graphics_draw_text(disp, UI_X_PADDING, UI_Y_PADDING + 52, line);

    graphics_set_color(g_error_count ? g_pal.fail : g_pal.ok, g_pal.panel);
    snprintf(line, sizeof(line), "Err: %lu  Restore: %lu",
             (unsigned long)g_error_count, (unsigned long)g_restore_error_count);
    graphics_draw_text(disp, UI_X_PADDING, UI_Y_PADDING + 64, line);

    graphics_set_color(g_pal.fg, g_pal.panel);
    format_elapsed_compact(elapsed_compact, sizeof(elapsed_compact), g_elapsed_ms);
    snprintf(line, sizeof(line), "Elapsed: %s", elapsed_compact);
    graphics_draw_text(disp, UI_X_PADDING, UI_Y_PADDING + 76, line);

    if (g_failure_log_count > 0) {
        uint32_t idx = g_failure_log_head ? g_failure_log_head - 1U : (FAILURE_LOG_CAP - 1U);
        const mem_failure_t *last = &g_failure_log[idx];
        const memtest_desc_t *desc = memtest_desc(last->test);
        graphics_set_color(g_pal.fail, g_pal.panel);
        snprintf(line, sizeof(line), "Last: B%u %s P%lu A:%08lX", last->bank,
                 desc ? desc->name : "?", (unsigned long)(last->pass + 1), (unsigned long)last->address);
        graphics_draw_text(disp, UI_X_PADDING, UI_Y_PADDING + 96, line);
    }
}

static void render_footer(surface_t *disp)
{
    const char *left_hint = "";
    int footer_y = display_get_height() - 12;

    if (g_run_state == RUN_IDLE) {
        left_hint = "A: toggle   C-L/R/U: quick/full/burn   Start: run";
    } else if (g_run_state == RUN_RUNNING || g_run_state == RUN_PAUSED || g_run_state == RUN_STOPPING) {
        left_hint = "B: stop    Start: pause/resume";
    } else {
        left_hint = "A: setup   Start: rerun";
    }

    graphics_draw_box(disp, 0, footer_y - 5, display_get_width(), footer_y+5, g_pal.accent);
    graphics_set_color(g_pal.bg, g_pal.accent);
    graphics_draw_text(disp, UI_X_PADDING, footer_y, left_hint);
}

static void render_frame(surface_t *disp)
{
    const char *title = "N64Memtest 0.8 - by Rasky with Libdragon";
    int topbar_h = 15;
    graphics_fill_screen(disp, g_pal.panel);

    graphics_draw_box(disp, 0, 0, display_get_width(), topbar_h, g_pal.accent);
    graphics_set_color(g_pal.bg, g_pal.accent);
    graphics_draw_text(disp, UI_X_PADDING, 3, title);

    if (g_run_state == RUN_IDLE) render_setup_screen(disp);
    else if (g_run_state == RUN_RUNNING || g_run_state == RUN_PAUSED || g_run_state == RUN_STOPPING) render_running_screen(disp);
    else render_report_screen(disp);

    if (g_run_state == RUN_RUNNING || g_run_state == RUN_PAUSED || g_run_state == RUN_STOPPING) {
        joypad_inputs_t inputs = joypad_get_inputs(JOYPAD_PORT_1);
        int analog_x = inputs.stick_x;
        int analog_y = inputs.stick_y;
        uint32_t logo_color = (g_run_state == RUN_PAUSED) ? g_pal.warn : g_pal.accent;
        /* Fallback to C-stick emulation when main stick is neutral. */
        if (analog_x == 0 && analog_y == 0) {
            analog_x = inputs.cstick_x;
            analog_y = inputs.cstick_y;
        }
        logo_draw(disp, display_get_width() - 60, 78, logo_color, (uint32_t)get_ticks_ms(), analog_x, analog_y);
    }

    render_footer(disp);
}

static void start_run(void)
{
    if (!any_bank_enabled() || !any_test_enabled()) return;
    reset_runtime_counters();
    prepare_first_work_item();
    if (!work_item_valid()) return;

    g_run_state = RUN_RUNNING;
    g_start_ms = get_ticks_ms();
    log_pass_start_if_needed();
    debugf("[memtest] run-start profile=%s loop_target=%s banks=%d seed=%08lx\n",
           memtest_profile_name(g_profile),
           loop_target_name(g_loop_target),
           g_total_banks,
           (unsigned long)g_rng_seed);
}

static void handle_setup_input(joypad_buttons_t pressed)
{
    int test_row_base = SETUP_ROW_BANK_BASE + g_total_banks;
    int max_row = test_row_base + MEMTEST_ID_COUNT - 1;
    int row;

    if (pressed.d_up) g_setup_cursor--;
    if (pressed.d_down) g_setup_cursor++;
    if (g_setup_cursor < 0) g_setup_cursor = max_row;
    if (g_setup_cursor > max_row) g_setup_cursor = 0;

    if (pressed.c_left) apply_profile(MEMTEST_PROFILE_QUICK);
    if (pressed.c_right) apply_profile(MEMTEST_PROFILE_FULL);
    if (pressed.c_up) apply_profile(MEMTEST_PROFILE_BURNIN);

    if (pressed.start) start_run();

    if (!pressed.a) return;
    row = g_setup_cursor;
    if (row == SETUP_ROW_PROFILE) {
        if (g_profile == MEMTEST_PROFILE_QUICK) apply_profile(MEMTEST_PROFILE_FULL);
        else if (g_profile == MEMTEST_PROFILE_FULL) apply_profile(MEMTEST_PROFILE_BURNIN);
        else if (g_profile == MEMTEST_PROFILE_BURNIN) apply_profile(MEMTEST_PROFILE_CUSTOM);
        else apply_profile(MEMTEST_PROFILE_QUICK);
    } else if (row == SETUP_ROW_LOOPS) {
        cycle_loop_target();
    } else if (row == SETUP_ROW_START) {
        start_run();
    } else if (row >= SETUP_ROW_BANK_BASE && row < test_row_base) {
        int bank = row - SETUP_ROW_BANK_BASE;
        if (g_bank_available[bank]) g_bank_enabled[bank] = !g_bank_enabled[bank];
    } else if (row >= test_row_base) {
        int test = row - test_row_base;
        if (test >= 0 && test < MEMTEST_ID_COUNT) {
            if (g_profile != MEMTEST_PROFILE_CUSTOM) g_profile = MEMTEST_PROFILE_CUSTOM;
            g_test_enabled[test] = !g_test_enabled[test];
        }
    }
}

static void handle_running_input(joypad_buttons_t pressed)
{
    if (pressed.start) {
        if (g_run_state == RUN_RUNNING) g_run_state = RUN_PAUSED;
        else if (g_run_state == RUN_PAUSED) g_run_state = RUN_RUNNING;
    }
    if (pressed.b && g_run_state != RUN_DONE) {
        g_stop_requested = true;
        if (g_run_state == RUN_RUNNING) g_run_state = RUN_STOPPING;
    }
}

static void handle_report_input(joypad_buttons_t pressed)
{
    if (pressed.a) g_run_state = RUN_IDLE;
    else if (pressed.start) start_run();
}

int main(void)
{
    surface_t *disp;
    int i;
    run_state_t prev_state = RUN_IDLE;

    debug_init_emulog();
    debug_init_usblog();
    display_init((resolution_t){
        .width = 440, .height = 240, .overscan_margin = VI_CRT_MARGIN
    }, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    joypad_init();
    rsp_init();
    joypad_poll();

    init_palette();
    g_total_memory_bytes = get_memory_size();
    g_total_banks = min_int(g_total_memory_bytes / (int)BANK_SIZE_BYTES, MAX_BANKS);
    g_usable_banks = g_total_banks;
    g_loop_target = 1;
    g_run_state = RUN_IDLE;
    g_rng_seed = 0x1337c0deU;

    for (i = 0; i < g_total_banks; i++) {
        g_bank_available[i] = true;
        g_bank_enabled[i] = true;
    }

    apply_profile(MEMTEST_PROFILE_QUICK);
    g_setup_rows_total = SETUP_ROW_BANK_BASE + g_total_banks + MEMTEST_ID_COUNT;
    if (!joypad_is_connected(JOYPAD_PORT_1)) {
        apply_profile(MEMTEST_PROFILE_BURNIN);
        debugf("[memtest] no joypad on port1; auto-starting burn-in mode\n");
        start_run();
    }

    while (1) {
        joypad_buttons_t pressed;
        joypad_poll();
        pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);

        if (g_run_state == RUN_IDLE) handle_setup_input(pressed);
        else if (g_run_state == RUN_RUNNING || g_run_state == RUN_PAUSED || g_run_state == RUN_STOPPING) handle_running_input(pressed);
        else handle_report_input(pressed);

        disp = display_get();
        render_frame(disp);
        display_show(disp);

        if (g_run_state == RUN_RUNNING || g_run_state == RUN_STOPPING) {
            uint32_t chunks_this_frame;
            for (chunks_this_frame = 0; chunks_this_frame < MEMTEST_CHUNKS_PER_FRAME; chunks_this_frame++) {
                if (!(g_run_state == RUN_RUNNING || g_run_state == RUN_STOPPING)) break;
                memtest_run_slice();
            }
            if (g_run_state == RUN_DONE) {
                g_elapsed_ms = (uint32_t)(get_ticks_ms() - g_start_ms);
            }
        }

        if (g_run_state == RUN_DONE && prev_state != RUN_DONE) {
            if (g_start_ms) g_elapsed_ms = (uint32_t)(get_ticks_ms() - g_start_ms);
            log_run_summary_once();
        }
        prev_state = g_run_state;
    }
}
