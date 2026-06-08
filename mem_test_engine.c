#include "mem_test_engine.h"
#include <libdragon.h>
#include <string.h>

static const memtest_desc_t g_desc[MEMTEST_ID_COUNT] = {
    [MEMTEST_RANDOM] = {"Random value", 2, false},
    [MEMTEST_CHECKERBOARD] = {"Checkerboard", 2, false},
    [MEMTEST_SOLID_BITS] = {"Solid bits", 2, false},
    [MEMTEST_WALKING_ONES] = {"Walking ones", 32, false},
    [MEMTEST_WALKING_ZEROES] = {"Walking zeroes", 32, false},
    [MEMTEST_SEQ_INCREMENT] = {"Sequential increment", 2, false},
    [MEMTEST_BIT_SPREAD] = {"Bit spread", 16, true},
    [MEMTEST_BIT_FLIP] = {"Bit flip", 32, true},
    [MEMTEST_MARCH_MATS_PLUS] = {"March MATS+", 1, false},
    [MEMTEST_MARCH_MATS_PLUS_PLUS] = {"March MATS++", 1, false},
    [MEMTEST_MARCH_A] = {"March A", 1, true},
    [MEMTEST_MARCH_B] = {"March B", 1, true},
    [MEMTEST_MARCH_C_MINUS] = {"March C-", 1, false},
    [MEMTEST_MARCH_C_PLUS] = {"March C+", 1, true},
    [MEMTEST_MARCH_SR] = {"March SR", 1, true},
    [MEMTEST_MARCH_SS] = {"March SS", 1, true},
    [MEMTEST_MARCH_X] = {"March X", 1, false},
    [MEMTEST_MARCH_Y] = {"March Y", 1, true},
};

size_t memtest_count(void)
{
    return MEMTEST_ID_COUNT;
}

const memtest_desc_t *memtest_desc(memtest_id_t id)
{
    if ((unsigned)id >= MEMTEST_ID_COUNT) return 0;
    return &g_desc[id];
}

const char *memtest_profile_name(memtest_profile_t profile)
{
    switch (profile) {
        case MEMTEST_PROFILE_QUICK: return "Quick";
        case MEMTEST_PROFILE_FULL: return "Full";
        case MEMTEST_PROFILE_BURNIN: return "Burn-in";
        case MEMTEST_PROFILE_CUSTOM: return "Custom";
        default: return "?";
    }
}

void memtest_apply_profile_defaults(memtest_profile_t profile, bool enabled[MEMTEST_ID_COUNT])
{
    size_t i;
    for (i = 0; i < MEMTEST_ID_COUNT; i++) {
        enabled[i] = false;
    }

    if (profile == MEMTEST_PROFILE_QUICK) {
        /* Quick: Random + Checkerboard + March C- (+ a couple fast March tests). */
        enabled[MEMTEST_RANDOM] = true;
        enabled[MEMTEST_CHECKERBOARD] = true;
        enabled[MEMTEST_SOLID_BITS] = true;
        enabled[MEMTEST_MARCH_C_MINUS] = true;
        enabled[MEMTEST_MARCH_MATS_PLUS] = true;
        enabled[MEMTEST_MARCH_MATS_PLUS_PLUS] = true;
        enabled[MEMTEST_MARCH_X] = true;
        return;
    }

    if (profile == MEMTEST_PROFILE_FULL || profile == MEMTEST_PROFILE_BURNIN) {
        /* Full/Burn-in: all March + Bit Flip + Walking Ones/Zeroes + Random. */
        enabled[MEMTEST_RANDOM] = true;
        enabled[MEMTEST_BIT_FLIP] = true;
        enabled[MEMTEST_WALKING_ONES] = true;
        enabled[MEMTEST_WALKING_ZEROES] = true;
        for (i = MEMTEST_MARCH_MATS_PLUS; i <= MEMTEST_MARCH_Y; i++) {
            enabled[i] = true;
        }
        if (profile == MEMTEST_PROFILE_BURNIN) {
            /*
             * Burn-in extends Full to guarantee no test is orphaned
             * across presets, and to maximize long-run stress coverage.
             */
            enabled[MEMTEST_CHECKERBOARD] = true;
            enabled[MEMTEST_SOLID_BITS] = true;
            enabled[MEMTEST_SEQ_INCREMENT] = true;
            enabled[MEMTEST_BIT_SPREAD] = true;
        }
        return;
    }

    if (profile == MEMTEST_PROFILE_CUSTOM) {
        /* Keep explicit defaults lightweight for custom mode. */
        enabled[MEMTEST_RANDOM] = true;
    }
}

void memtest_run_slice(void)
{
    uint8_t *chunk;
    phys_addr_t chunk_phys;
    uint32_t chunk_bytes;
    uint32_t packed_test_pass;
    uintptr_t fail_addr;

    if (g_run_state != RUN_RUNNING && g_run_state != RUN_STOPPING) return;
    if (g_current_test < 0 || g_current_bank < 0) {
        g_run_state = RUN_DONE;
        return;
    }

    chunk_phys = (phys_addr_t)(g_current_bank * BANK_SIZE_BYTES + g_current_bank_offset);
    chunk = (uint8_t *)VirtualUncachedAddr(chunk_phys);
    chunk_bytes = CHUNK_SIZE_BYTES;
    packed_test_pass = (((uint32_t)g_current_pass & 0x00FFFFFFU) << 8)
        | ((uint32_t)g_current_test & 0xFFU);

    volatile uint32_t *SI_STATUS = (volatile uint32_t *)0xa4800018;

    disable_interrupts();

    // Wait for SI DMA to complete. Otherwise, we can get joybus packets being
    // written into RDRAM asynchronously while the test is running.
    while (*SI_STATUS & 1) {}

    //data_cache_writeback_invalidate_all();
    fail_addr = memtest_run_slice_critical_asm(chunk, packed_test_pass, g_rng_seed);

    enable_interrupts();

    if (fail_addr) {
        uint32_t got = *(volatile uint32_t *)fail_addr;
        memtest_capture_failure(fail_addr, 0U, got);
        g_restore_error_count++;
    }

    g_bytes_tested += chunk_bytes;
    g_slices_done++;
    g_current_bank_offset += chunk_bytes;

    if (g_current_bank_offset >= BANK_SIZE_BYTES) {
        g_current_bank_offset = 0;
        if (!memtest_advance_to_next_work_item()) {
            return;
        }
    }

    if (g_stop_requested) {
        g_run_state = RUN_DONE;
    }
}
