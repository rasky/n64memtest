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

static uint32_t mix32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

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

uint32_t memtest_pattern_word(memtest_id_t id, uint32_t pass, uint32_t word_index, uint32_t seed)
{
    uint32_t bit;
    switch (id) {
        case MEMTEST_RANDOM:
            return mix32(seed ^ (pass * 0x9e3779b9U) ^ word_index);
        case MEMTEST_CHECKERBOARD:
            return ((word_index + pass) & 1U) ? 0xAAAAAAAAU : 0x55555555U;
        case MEMTEST_SOLID_BITS:
            return (pass & 1U) ? 0xFFFFFFFFU : 0x00000000U;
        case MEMTEST_WALKING_ONES:
            bit = (pass + word_index) & 31U;
            return 1U << bit;
        case MEMTEST_WALKING_ZEROES:
            bit = (pass + word_index) & 31U;
            return ~(1U << bit);
        case MEMTEST_SEQ_INCREMENT:
            return word_index + (pass * 0x01010101U);
        case MEMTEST_BIT_SPREAD: {
            uint32_t pos = pass & 15U;
            uint32_t pat = (1U << pos) | (1U << (pos + 16U));
            return (word_index & 1U) ? ~pat : pat;
        }
        case MEMTEST_BIT_FLIP: {
            uint32_t pos = pass & 31U;
            uint32_t base = 1U << pos;
            return ((word_index + pass) & 1U) ? ~base : base;
        }
        default:
            return 0U;
    }
}

static bool memtest_run_chunk_once(volatile uint32_t *chunk_words, uint32_t words,
    memtest_id_t test_id, uint32_t pass, uint32_t base_word_index, uint32_t seed, bool report_failures)
{
    bool is_march =
        test_id >= MEMTEST_MARCH_MATS_PLUS && test_id <= MEMTEST_MARCH_Y;
    uint32_t i;
    bool ok = true;

    if (is_march) {
        #define CHECK_READ(cell, expected) do { \
            uint32_t __got = (cell); \
            if (__got != (expected)) { \
                if (report_failures) memtest_capture_failure((uintptr_t)&chunk_words[i], (expected), __got); \
                ok = false; \
                return false; \
            } \
        } while (0)
        #define WRITE_VAL(v) do { chunk_words[i] = (v); } while (0)
        #define LOOP_UP for (i = 0; i < words; i++)
        #define LOOP_DOWN for (i = words; i-- > 0; )

        switch (test_id) {
            case MEMTEST_MARCH_MATS_PLUS:
                LOOP_UP { WRITE_VAL(0U); }
                LOOP_UP { CHECK_READ(chunk_words[i], 0U); WRITE_VAL(0xFFFFFFFFU); }
                LOOP_DOWN { CHECK_READ(chunk_words[i], 0xFFFFFFFFU); WRITE_VAL(0U); }
                break;

            case MEMTEST_MARCH_MATS_PLUS_PLUS:
                LOOP_UP { WRITE_VAL(0U); }
                LOOP_UP { CHECK_READ(chunk_words[i], 0U); WRITE_VAL(0xFFFFFFFFU); }
                LOOP_DOWN { CHECK_READ(chunk_words[i], 0xFFFFFFFFU); WRITE_VAL(0U); CHECK_READ(chunk_words[i], 0U); }
                break;

            case MEMTEST_MARCH_A:
                LOOP_UP { WRITE_VAL(0U); }
                LOOP_UP { CHECK_READ(chunk_words[i], 0U); WRITE_VAL(0xFFFFFFFFU); WRITE_VAL(0U); WRITE_VAL(0xFFFFFFFFU); }
                LOOP_UP { CHECK_READ(chunk_words[i], 0xFFFFFFFFU); WRITE_VAL(0U); WRITE_VAL(0xFFFFFFFFU); }
                LOOP_DOWN { CHECK_READ(chunk_words[i], 0xFFFFFFFFU); WRITE_VAL(0U); WRITE_VAL(0xFFFFFFFFU); WRITE_VAL(0U); }
                LOOP_DOWN { CHECK_READ(chunk_words[i], 0U); WRITE_VAL(0xFFFFFFFFU); WRITE_VAL(0U); }
                break;

            case MEMTEST_MARCH_B:
                LOOP_UP { WRITE_VAL(0U); }
                LOOP_UP { CHECK_READ(chunk_words[i], 0U); WRITE_VAL(0xFFFFFFFFU); CHECK_READ(chunk_words[i], 0xFFFFFFFFU); WRITE_VAL(0U); CHECK_READ(chunk_words[i], 0U); WRITE_VAL(0xFFFFFFFFU); }
                LOOP_UP { CHECK_READ(chunk_words[i], 0xFFFFFFFFU); WRITE_VAL(0U); WRITE_VAL(0xFFFFFFFFU); }
                LOOP_DOWN { CHECK_READ(chunk_words[i], 0xFFFFFFFFU); WRITE_VAL(0U); WRITE_VAL(0xFFFFFFFFU); WRITE_VAL(0U); }
                LOOP_DOWN { CHECK_READ(chunk_words[i], 0U); WRITE_VAL(0xFFFFFFFFU); WRITE_VAL(0U); }
                break;

            case MEMTEST_MARCH_C_MINUS:
                LOOP_UP { WRITE_VAL(0U); }
                LOOP_UP { CHECK_READ(chunk_words[i], 0U); WRITE_VAL(0xFFFFFFFFU); }
                LOOP_UP { CHECK_READ(chunk_words[i], 0xFFFFFFFFU); WRITE_VAL(0U); }
                LOOP_DOWN { CHECK_READ(chunk_words[i], 0U); WRITE_VAL(0xFFFFFFFFU); }
                LOOP_DOWN { CHECK_READ(chunk_words[i], 0xFFFFFFFFU); WRITE_VAL(0U); }
                LOOP_UP { CHECK_READ(chunk_words[i], 0U); }
                break;

            case MEMTEST_MARCH_C_PLUS:
                LOOP_UP { WRITE_VAL(0U); }
                LOOP_UP { CHECK_READ(chunk_words[i], 0U); WRITE_VAL(0xFFFFFFFFU); CHECK_READ(chunk_words[i], 0xFFFFFFFFU); }
                LOOP_UP { CHECK_READ(chunk_words[i], 0xFFFFFFFFU); WRITE_VAL(0U); CHECK_READ(chunk_words[i], 0U); }
                LOOP_DOWN { CHECK_READ(chunk_words[i], 0U); WRITE_VAL(0xFFFFFFFFU); CHECK_READ(chunk_words[i], 0xFFFFFFFFU); }
                LOOP_DOWN { CHECK_READ(chunk_words[i], 0xFFFFFFFFU); WRITE_VAL(0U); CHECK_READ(chunk_words[i], 0U); }
                LOOP_UP { CHECK_READ(chunk_words[i], 0U); }
                break;

            case MEMTEST_MARCH_SR:
                LOOP_DOWN { WRITE_VAL(0U); }
                LOOP_UP { CHECK_READ(chunk_words[i], 0U); WRITE_VAL(0xFFFFFFFFU); CHECK_READ(chunk_words[i], 0xFFFFFFFFU); WRITE_VAL(0U); }
                LOOP_DOWN { CHECK_READ(chunk_words[i], 0U); CHECK_READ(chunk_words[i], 0U); }
                LOOP_UP { WRITE_VAL(0xFFFFFFFFU); }
                LOOP_DOWN { CHECK_READ(chunk_words[i], 0xFFFFFFFFU); WRITE_VAL(0U); CHECK_READ(chunk_words[i], 0U); WRITE_VAL(0xFFFFFFFFU); }
                LOOP_UP { CHECK_READ(chunk_words[i], 0xFFFFFFFFU); CHECK_READ(chunk_words[i], 0xFFFFFFFFU); }
                break;

            case MEMTEST_MARCH_SS:
                LOOP_DOWN { WRITE_VAL(0U); }
                LOOP_UP { CHECK_READ(chunk_words[i], 0U); CHECK_READ(chunk_words[i], 0U); WRITE_VAL(0U); CHECK_READ(chunk_words[i], 0U); WRITE_VAL(0xFFFFFFFFU); }
                LOOP_UP { CHECK_READ(chunk_words[i], 0xFFFFFFFFU); CHECK_READ(chunk_words[i], 0xFFFFFFFFU); WRITE_VAL(0xFFFFFFFFU); CHECK_READ(chunk_words[i], 0xFFFFFFFFU); WRITE_VAL(0U); }
                LOOP_DOWN { CHECK_READ(chunk_words[i], 0U); CHECK_READ(chunk_words[i], 0U); WRITE_VAL(0U); CHECK_READ(chunk_words[i], 0U); WRITE_VAL(0xFFFFFFFFU); }
                LOOP_DOWN { CHECK_READ(chunk_words[i], 0xFFFFFFFFU); CHECK_READ(chunk_words[i], 0xFFFFFFFFU); WRITE_VAL(0xFFFFFFFFU); CHECK_READ(chunk_words[i], 0xFFFFFFFFU); WRITE_VAL(0U); }
                LOOP_UP { CHECK_READ(chunk_words[i], 0U); }
                break;

            case MEMTEST_MARCH_X:
                LOOP_UP { WRITE_VAL(0U); }
                LOOP_UP { CHECK_READ(chunk_words[i], 0U); WRITE_VAL(0xFFFFFFFFU); }
                LOOP_DOWN { CHECK_READ(chunk_words[i], 0xFFFFFFFFU); WRITE_VAL(0U); }
                LOOP_UP { CHECK_READ(chunk_words[i], 0U); }
                break;

            case MEMTEST_MARCH_Y:
                LOOP_UP { WRITE_VAL(0U); }
                LOOP_UP { WRITE_VAL(0U); CHECK_READ(chunk_words[i], 0U); WRITE_VAL(0xFFFFFFFFU); WRITE_VAL(0xFFFFFFFFU); CHECK_READ(chunk_words[i], 0xFFFFFFFFU); }
                LOOP_UP { WRITE_VAL(0xFFFFFFFFU); CHECK_READ(chunk_words[i], 0xFFFFFFFFU); WRITE_VAL(0U); WRITE_VAL(0U); CHECK_READ(chunk_words[i], 0U); }
                LOOP_DOWN { CHECK_READ(chunk_words[i], 0U); WRITE_VAL(0U); WRITE_VAL(0xFFFFFFFFU); WRITE_VAL(0xFFFFFFFFU); CHECK_READ(chunk_words[i], 0xFFFFFFFFU); }
                LOOP_DOWN { CHECK_READ(chunk_words[i], 0xFFFFFFFFU); WRITE_VAL(0xFFFFFFFFU); WRITE_VAL(0U); WRITE_VAL(0U); CHECK_READ(chunk_words[i], 0U); }
                LOOP_UP { CHECK_READ(chunk_words[i], 0U); }
                break;

            default:
                break;
        }
        #undef CHECK_READ
        #undef WRITE_VAL
        #undef LOOP_UP
        #undef LOOP_DOWN
    } else {
        for (i = 0; i < words; i++) {
            uint32_t pat = memtest_pattern_word(test_id, pass, base_word_index + i, seed);
            chunk_words[i] = pat;
        }

        for (i = 0; i < words; i++) {
            uint32_t exp = memtest_pattern_word(test_id, pass, base_word_index + i, seed);
            uint32_t got = chunk_words[i];
            if (got != exp) {
                if (report_failures) {
                    memtest_capture_failure((uintptr_t)&chunk_words[i], exp, got);
                }
                ok = false;
                break;
            }
        }
    }

    return ok;
}

static inline __attribute__((always_inline)) void memtest_sp_dma_wait_local(void)
{
    while (*SP_STATUS & (SP_STATUS_DMA_BUSY | SP_STATUS_IO_BUSY)) {}
}

static inline __attribute__((always_inline)) void memtest_rsp_load_data_local(
    void *start, unsigned long size, unsigned int dmem_offset)
{
    *SP_DMA_RAMADDR = (uint32_t)start;
    MEMORY_BARRIER();
    *SP_DMA_SPADDR = (uint32_t)(SP_DMEM + dmem_offset / 4);
    MEMORY_BARRIER();
    *SP_DMA_RDLEN = size - 1;
    MEMORY_BARRIER();
    memtest_sp_dma_wait_local();
}

static inline __attribute__((always_inline)) void memtest_rsp_load_code_local(
    void *start, unsigned long size, unsigned int imem_offset)
{
    *SP_DMA_RAMADDR = (uint32_t)start;
    MEMORY_BARRIER();
    *SP_DMA_SPADDR = (uint32_t)(SP_IMEM + imem_offset / 4);
    MEMORY_BARRIER();
    *SP_DMA_RDLEN = size - 1;
    MEMORY_BARRIER();
    memtest_sp_dma_wait_local();
}

static inline __attribute__((always_inline)) void memtest_rsp_read_data_local(
    void *start, unsigned long size, unsigned int dmem_offset)
{
    *SP_DMA_RAMADDR = (uint32_t)start;
    MEMORY_BARRIER();
    *SP_DMA_SPADDR = (uint32_t)(SP_DMEM + dmem_offset / 4);
    MEMORY_BARRIER();
    *SP_DMA_WRLEN = size - 1;
    MEMORY_BARRIER();
    memtest_sp_dma_wait_local();
}

static inline __attribute__((always_inline)) void memtest_rsp_read_code_local(
    void *start, unsigned long size, unsigned int imem_offset)
{
    *SP_DMA_RAMADDR = (uint32_t)start;
    MEMORY_BARRIER();
    *SP_DMA_SPADDR = (uint32_t)(SP_IMEM + imem_offset / 4);
    MEMORY_BARRIER();
    *SP_DMA_WRLEN = size - 1;
    MEMORY_BARRIER();
    memtest_sp_dma_wait_local();
}

static __attribute__((noinline, flatten, unused)) uintptr_t memtest_run_slice_critical(
    uint8_t *chunk, uint32_t test_pass, uint32_t seed)
{
    uintptr_t icache_addr;
    uintptr_t icache_start = (uintptr_t)memtest_run_slice_critical;
    uintptr_t icache_end = icache_start + 8192U;
    uintptr_t fail_addr = 0;
    bool local_test_ok;
    memtest_id_t test_id = (memtest_id_t)(test_pass & 0xFFU);
    uint32_t pass = test_pass >> 8;
    uint32_t base_word_index = ((uint32_t)PhysicalAddr(chunk)) / 4U;
    uint32_t chunk_first = 4096U;
    uint32_t chunk_second = CHUNK_SIZE_BYTES - 4096U;
    uint32_t words = CHUNK_SIZE_BYTES / sizeof(uint32_t);
    uint32_t j;
    uint32_t *chunk_words = (uint32_t *)chunk;
    uint32_t words_first = chunk_first / 4U;
    uint32_t words_second = chunk_second / 4U;

    for (icache_addr = icache_start; icache_addr < icache_end; icache_addr += 32U) {
        asm volatile("cache 0x14, 0(%0)" :: "r"(icache_addr) : "memory");
    }

    memtest_sp_dma_wait_local();
    memtest_rsp_load_data_local(chunk, chunk_first, 0);
    if (chunk_second) memtest_rsp_load_code_local(chunk + chunk_first, chunk_second, 0);

    local_test_ok = memtest_run_chunk_once(chunk_words, words, test_id, pass, base_word_index, seed, true);

    memtest_sp_dma_wait_local();
    memtest_rsp_read_data_local(chunk, chunk_first, 0);
    if (chunk_second) memtest_rsp_read_code_local(chunk + chunk_first, chunk_second, 0);

    if (!local_test_ok) {
        fail_addr = (uintptr_t)chunk;
    }

    for (j = 0; j < words_first; j++) {
        if (chunk_words[j] != SP_DMEM[j]) {
            if (!fail_addr) fail_addr = (uintptr_t)&chunk_words[j];
            break;
        }
    }
    if (!fail_addr) {
        for (j = 0; j < words_second; j++) {
            if (chunk_words[words_first + j] != SP_IMEM[j]) {
                fail_addr = (uintptr_t)&chunk_words[words_first + j];
                break;
            }
        }
    }
    return fail_addr;
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
