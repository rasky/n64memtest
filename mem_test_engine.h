#ifndef MEM_TESTS_H
#define MEM_TESTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BANK_SIZE_BYTES      (1024U * 1024U)
#define MAX_BANKS            8
#define CHUNK_SIZE_BYTES     (8U * 1024U)

#if ((BANK_SIZE_BYTES % CHUNK_SIZE_BYTES) != 0U)
#error "CHUNK_SIZE_BYTES must exactly divide BANK_SIZE_BYTES"
#endif
#if (CHUNK_SIZE_BYTES < 4096U || CHUNK_SIZE_BYTES > 8192U)
#error "CHUNK_SIZE_BYTES must stay in [4096, 8192] for DMEM/IMEM staging"
#endif

typedef enum {
    RUN_IDLE = 0,
    RUN_RUNNING,
    RUN_PAUSED,
    RUN_STOPPING,
    RUN_DONE,
} run_state_t;

typedef enum {
    MEMTEST_PROFILE_QUICK = 0,
    MEMTEST_PROFILE_FULL,
    MEMTEST_PROFILE_BURNIN,
    MEMTEST_PROFILE_CUSTOM,
} memtest_profile_t;

typedef enum {
    MEMTEST_RANDOM = 0,
    MEMTEST_CHECKERBOARD,
    MEMTEST_SOLID_BITS,
    MEMTEST_WALKING_ONES,
    MEMTEST_WALKING_ZEROES,
    MEMTEST_SEQ_INCREMENT,
    MEMTEST_BIT_SPREAD,
    MEMTEST_BIT_FLIP,
    MEMTEST_MARCH_MATS_PLUS,
    MEMTEST_MARCH_MATS_PLUS_PLUS,
    MEMTEST_MARCH_A,
    MEMTEST_MARCH_B,
    MEMTEST_MARCH_C_MINUS,
    MEMTEST_MARCH_C_PLUS,
    MEMTEST_MARCH_SR,
    MEMTEST_MARCH_SS,
    MEMTEST_MARCH_X,
    MEMTEST_MARCH_Y,
    MEMTEST_ID_COUNT,
} memtest_id_t;

typedef struct {
    const char *name;
    uint32_t passes;
    bool full_only;
} memtest_desc_t;

typedef struct {
    bool valid;
    uint32_t address;
    uint32_t expected;
    uint32_t actual;
    uint32_t pass;
    uint8_t bank;
    memtest_id_t test;
} mem_failure_t;

extern int g_total_banks;
extern int g_current_test;
extern uint32_t g_current_pass;
extern int g_current_bank;
extern uint32_t g_current_bank_offset;
extern uint32_t g_rng_seed;
extern run_state_t g_run_state;
extern bool g_stop_requested;
extern uint32_t g_bytes_tested;
extern uint32_t g_slices_done;
extern uint32_t g_restore_error_count;
extern void *g_moving_stack_buffer;
extern uint32_t g_moving_stack_size;

void run_moving_stack(void *stack_buffer, uint32_t stack_size, void (*func)(void));

void memtest_capture_failure(uintptr_t addr, uint32_t exp, uint32_t got);
bool memtest_advance_to_next_work_item(void);

size_t memtest_count(void);
const memtest_desc_t *memtest_desc(memtest_id_t id);
const char *memtest_profile_name(memtest_profile_t profile);
void memtest_apply_profile_defaults(memtest_profile_t profile, bool enabled[MEMTEST_ID_COUNT]);
uint32_t memtest_pattern_word(memtest_id_t id, uint32_t pass, uint32_t word_index, uint32_t seed);
void memtest_run_slice(void);

// Run a single test on a chunk of memory (mem_test.S)
uintptr_t mem_test(uint8_t *chunk, uint32_t test_pass, uint32_t seed);

#endif
