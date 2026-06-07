/*
 * uintptr_t memtest_run_slice_critical_asm(uint8_t *chunk, uint32_t test_pass, uint32_t seed)
 *
 * ABI:
 *   a0 = chunk pointer (uncached KSEG1 address, 8 KiB aligned region)
 *   a1 = packed [pass:24 | test_id:8]
 *   a2 = seed
 *
 * Return:
 *   v0 = 0 on success
 *   v0 = first failing address (test mismatch or restore mismatch)
 *
 * Design constraints:
 *   - no stack usage
 *   - no subcalls (single monolithic routine)
 *   - no J/JAL; only PC-relative branches + JR RA at exit
 *   - fixed chunk size = 8 KiB (4 KiB DMEM + 4 KiB IMEM)
 *   - report mode always enabled via failing address in v0
 */

    .set noreorder
    .text
    .globl memtest_run_slice_critical_asm
    .ent memtest_run_slice_critical_asm

/* Registers with fixed semantic role across the whole routine. */
#define arg_chunk               $a0
#define arg_test_pass           $a1
#define arg_seed                $a2
#define ret_fail_addr           $v0
#define reg_base_word_index     $t3
#define reg_pass                $t6
#define reg_sp_base             $t7
#define reg_test_id             $t8
#define reg_pass_mix            $t9

memtest_run_slice_critical_asm:
    /*
     * Prime this function body into I-cache before running any test logic.
     *
     * for (p = &memtest_run_slice_critical_asm; p < &memtest_run_slice_critical_asm_end; p += 32) {
     *     cache HIT_FILL, 0(p);
     * }
     *
     * - 32 bytes = R4300 I-cache line size.
     * - HIT_FILL for I-cache is CACHE op 0x14.
     */
#define icache_ptr              $t0
#define icache_end              $t1
    la      icache_ptr, memtest_run_slice_critical_asm
    la      icache_end, memtest_run_slice_critical_asm_end

icache_fill_loop:
    sltu    $t2, icache_ptr, icache_end
    beqz    $t2, icache_fill_done
    nop
    cache   0x14, 0(icache_ptr)
    addiu   icache_ptr, icache_ptr, 32
    b       icache_fill_loop
    nop
#undef icache_ptr
#undef icache_end

icache_fill_done:
    /* fail_addr = 0; */
    move    ret_fail_addr, $zero

    /*
     * Decode packed arguments:
     *   test_id = test_pass & 0xFF;
     *   pass    = test_pass >> 8;
     */
    andi    reg_test_id, arg_test_pass, 0x00FF
    srl     reg_pass, arg_test_pass, 8

    /*
     * Precompute pass * 0x9e3779b9 used by RANDOM pattern.
     * pass_mix = pass * 0x9e3779b9U;
     */
    li      $t5, 0x9E3779B9
    multu   reg_pass, $t5
    mflo    reg_pass_mix

    /*
     * base_word_index = (PhysicalAddr(chunk) >> 2);
     */
    li      $t5, 0x1FFFFFFF
    and     reg_base_word_index, arg_chunk, $t5
    srl     reg_base_word_index, reg_base_word_index, 2

    /* ---------- PHASE 1: Save original chunk to RSP DMEM/IMEM ---------- */
    li      reg_sp_base, 0xA4040000

wait_dma_idle_before_save_dmem:
    lw      $t5, 0x0010(reg_sp_base)
    andi    $t5, $t5, 0x000C
    bnez    $t5, wait_dma_idle_before_save_dmem
    nop

    /* DMA IN: DRAM chunk[0..4095] -> SP DMEM[0..4095] */
    sw      arg_chunk, 0x0004(reg_sp_base)           /* SP_DMA_RAMADDR */
    move    $t5, $zero
    sw      $t5, 0x0000(reg_sp_base)           /* SP_DMA_SPADDR = 0 (DMEM) */
    li      $t5, 4095
    sw      $t5, 0x0008(reg_sp_base)           /* SP_DMA_RDLEN = size-1 */

wait_dma_done_save_dmem:
    lw      $t5, 0x0010(reg_sp_base)
    andi    $t5, $t5, 0x000C
    bnez    $t5, wait_dma_done_save_dmem
    nop

    /* DMA IN: DRAM chunk[4096..8191] -> SP IMEM[0..4095] */
    addiu   $t5, arg_chunk, 4096
    sw      $t5, 0x0004(reg_sp_base)           /* SP_DMA_RAMADDR = chunk + 4K */
    li      $t5, 4096
    sw      $t5, 0x0000(reg_sp_base)           /* SP_DMA_SPADDR = 4096 (IMEM base) */
    li      $t5, 4095
    sw      $t5, 0x0008(reg_sp_base)           /* SP_DMA_RDLEN = size-1 */

wait_dma_done_save_imem:
    lw      $t5, 0x0010(reg_sp_base)
    andi    $t5, $t5, 0x000C
    bnez    $t5, wait_dma_done_save_imem
    nop

    /* ---------- PHASE 2: Execute selected test algorithm ---------- */
    /*
     * Dispatch by test_id:
     *   0..7  = pattern tests (fill + verify)
     *   8..17 = March family
     */
    li      $t5, 8
    sltu    $t4, reg_test_id, $t5
    bnez    $t4, dispatch_pattern_tests
    nop
    b       dispatch_march_tests
    nop

/* ---------------------------------------------------------------------- */
/* Pattern tests (IDs 0..7)                                               */
/* ---------------------------------------------------------------------- */
dispatch_pattern_tests:
    /*
     * Fill loop skeleton:
     *   ptr = chunk; end = chunk + 8192; word_index = base_word_index;
     *   while (ptr != end) { *ptr = pattern(test_id, pass, word_index, seed); ptr++; word_index++; }
     */
    move    $t0, $a0
    addiu   $t1, $a0, 8192
    move    $t2, $t3

pattern_fill_loop:
    beq     $t0, $t1, pattern_verify_setup
    nop

    /* ---------- memtest_pattern_word switch(test_id) ---------- */
    beq     $t8, $zero, pattern_case_random_fill
    nop
    li      $t5, 1
    beq     $t8, $t5, pattern_case_checker_fill
    nop
    li      $t5, 2
    beq     $t8, $t5, pattern_case_solid_fill
    nop
    li      $t5, 3
    beq     $t8, $t5, pattern_case_walk1_fill
    nop
    li      $t5, 4
    beq     $t8, $t5, pattern_case_walk0_fill
    nop
    li      $t5, 5
    beq     $t8, $t5, pattern_case_seqinc_fill
    nop
    li      $t5, 6
    beq     $t8, $t5, pattern_case_bitspread_fill
    nop
    li      $t5, 7
    beq     $t8, $t5, pattern_case_bitflip_fill
    nop

    /* default pattern = 0 */
    move    $t4, $zero
    b       pattern_store_fill
    nop

pattern_case_random_fill:
    /* x = seed ^ pass_mix ^ word_index; x = mix32(x) */
    xor     $t4, $a2, $t9
    xor     $t4, $t4, $t2
    srl     $t5, $t4, 16
    xor     $t4, $t4, $t5
    li      $t5, 0x7FEB352D
    multu   $t4, $t5
    mflo    $t4
    srl     $t5, $t4, 15
    xor     $t4, $t4, $t5
    li      $t5, 0x846CA68B
    multu   $t4, $t5
    mflo    $t4
    srl     $t5, $t4, 16
    xor     $t4, $t4, $t5
    b       pattern_store_fill
    nop

pattern_case_checker_fill:
    addu    $t5, $t2, $t6
    andi    $t5, $t5, 1
    beq     $t5, $zero, pattern_checker_even_fill
    nop
    li      $t4, 0xAAAAAAAA
    b       pattern_store_fill
    nop
pattern_checker_even_fill:
    li      $t4, 0x55555555
    b       pattern_store_fill
    nop

pattern_case_solid_fill:
    andi    $t5, $t6, 1
    beq     $t5, $zero, pattern_solid_zero_fill
    nop
    li      $t4, 0xFFFFFFFF
    b       pattern_store_fill
    nop
pattern_solid_zero_fill:
    move    $t4, $zero
    b       pattern_store_fill
    nop

pattern_case_walk1_fill:
    addu    $t5, $t2, $t6
    andi    $t5, $t5, 31
    li      $t4, 1
    sllv    $t4, $t4, $t5
    b       pattern_store_fill
    nop

pattern_case_walk0_fill:
    addu    $t5, $t2, $t6
    andi    $t5, $t5, 31
    li      $t4, 1
    sllv    $t4, $t4, $t5
    nor     $t4, $t4, $zero
    b       pattern_store_fill
    nop

pattern_case_seqinc_fill:
    li      $t5, 0x01010101
    multu   $t6, $t5
    mflo    $t5
    addu    $t4, $t2, $t5
    b       pattern_store_fill
    nop

pattern_case_bitspread_fill:
    andi    $t5, $t6, 15
    li      $t4, 1
    sllv    $t4, $t4, $t5
    addiu   $v1, $t5, 16
    li      $t5, 1
    sllv    $t5, $t5, $v1
    or      $t4, $t4, $t5
    andi    $t5, $t2, 1
    beq     $t5, $zero, pattern_store_fill
    nop
    nor     $t4, $t4, $zero
    b       pattern_store_fill
    nop

pattern_case_bitflip_fill:
    andi    $t5, $t6, 31
    li      $t4, 1
    sllv    $t4, $t4, $t5
    addu    $t5, $t2, $t6
    andi    $t5, $t5, 1
    beq     $t5, $zero, pattern_store_fill
    nop
    nor     $t4, $t4, $zero

pattern_store_fill:
    sw      $t4, 0x0000($t0)
    addiu   $t0, $t0, 4
    addiu   $t2, $t2, 1
    b       pattern_fill_loop
    nop

pattern_verify_setup:
    /*
     * Verify loop:
     *   ptr = chunk; end = chunk + 8192; word_index = base_word_index;
     *   while (ptr != end) {
     *       expected = pattern(...);
     *       got = *ptr;
     *       if (got != expected) { fail_addr = ptr; goto restore; }
     *       ptr++; word_index++;
     *   }
     */
    move    $t0, $a0
    addiu   $t1, $a0, 8192
    move    $t2, $t3

pattern_verify_loop:
    beq     $t0, $t1, phase_restore_dma
    nop

    beq     $t8, $zero, pattern_case_random_verify
    nop
    li      $t5, 1
    beq     $t8, $t5, pattern_case_checker_verify
    nop
    li      $t5, 2
    beq     $t8, $t5, pattern_case_solid_verify
    nop
    li      $t5, 3
    beq     $t8, $t5, pattern_case_walk1_verify
    nop
    li      $t5, 4
    beq     $t8, $t5, pattern_case_walk0_verify
    nop
    li      $t5, 5
    beq     $t8, $t5, pattern_case_seqinc_verify
    nop
    li      $t5, 6
    beq     $t8, $t5, pattern_case_bitspread_verify
    nop
    li      $t5, 7
    beq     $t8, $t5, pattern_case_bitflip_verify
    nop

    move    $t4, $zero
    b       pattern_compare_verify
    nop

pattern_case_random_verify:
    xor     $t4, $a2, $t9
    xor     $t4, $t4, $t2
    srl     $t5, $t4, 16
    xor     $t4, $t4, $t5
    li      $t5, 0x7FEB352D
    multu   $t4, $t5
    mflo    $t4
    srl     $t5, $t4, 15
    xor     $t4, $t4, $t5
    li      $t5, 0x846CA68B
    multu   $t4, $t5
    mflo    $t4
    srl     $t5, $t4, 16
    xor     $t4, $t4, $t5
    b       pattern_compare_verify
    nop

pattern_case_checker_verify:
    addu    $t5, $t2, $t6
    andi    $t5, $t5, 1
    beq     $t5, $zero, pattern_checker_even_verify
    nop
    li      $t4, 0xAAAAAAAA
    b       pattern_compare_verify
    nop
pattern_checker_even_verify:
    li      $t4, 0x55555555
    b       pattern_compare_verify
    nop

pattern_case_solid_verify:
    andi    $t5, $t6, 1
    beq     $t5, $zero, pattern_solid_zero_verify
    nop
    li      $t4, 0xFFFFFFFF
    b       pattern_compare_verify
    nop
pattern_solid_zero_verify:
    move    $t4, $zero
    b       pattern_compare_verify
    nop

pattern_case_walk1_verify:
    addu    $t5, $t2, $t6
    andi    $t5, $t5, 31
    li      $t4, 1
    sllv    $t4, $t4, $t5
    b       pattern_compare_verify
    nop

pattern_case_walk0_verify:
    addu    $t5, $t2, $t6
    andi    $t5, $t5, 31
    li      $t4, 1
    sllv    $t4, $t4, $t5
    nor     $t4, $t4, $zero
    b       pattern_compare_verify
    nop

pattern_case_seqinc_verify:
    li      $t5, 0x01010101
    multu   $t6, $t5
    mflo    $t5
    addu    $t4, $t2, $t5
    b       pattern_compare_verify
    nop

pattern_case_bitspread_verify:
    andi    $t5, $t6, 15
    li      $t4, 1
    sllv    $t4, $t4, $t5
    addiu   $v1, $t5, 16
    li      $t5, 1
    sllv    $t5, $t5, $v1
    or      $t4, $t4, $t5
    andi    $t5, $t2, 1
    beq     $t5, $zero, pattern_compare_verify
    nop
    nor     $t4, $t4, $zero
    b       pattern_compare_verify
    nop

pattern_case_bitflip_verify:
    andi    $t5, $t6, 31
    li      $t4, 1
    sllv    $t4, $t4, $t5
    addu    $t5, $t2, $t6
    andi    $t5, $t5, 1
    beq     $t5, $zero, pattern_compare_verify
    nop
    nor     $t4, $t4, $zero

pattern_compare_verify:
    lw      $t5, 0x0000($t0)
    beq     $t5, $t4, pattern_verify_next
    nop
    b       record_fail_and_restore
    nop

pattern_verify_next:
    addiu   $t0, $t0, 4
    addiu   $t2, $t2, 1
    b       pattern_verify_loop
    nop

/* ---------------------------------------------------------------------- */
/* March tests (IDs 8..17)                                                 */
/* ---------------------------------------------------------------------- */
dispatch_march_tests:
    li      $t5, 8
    beq     $t8, $t5, march_mats_plus
    nop
    li      $t5, 9
    beq     $t8, $t5, march_mats_plus_plus
    nop
    li      $t5, 10
    beq     $t8, $t5, march_a
    nop
    li      $t5, 11
    beq     $t8, $t5, march_b
    nop
    li      $t5, 12
    beq     $t8, $t5, march_c_minus
    nop
    li      $t5, 13
    beq     $t8, $t5, march_c_plus
    nop
    li      $t5, 14
    beq     $t8, $t5, march_sr
    nop
    li      $t5, 15
    beq     $t8, $t5, march_ss
    nop
    li      $t5, 16
    beq     $t8, $t5, march_x
    nop
    li      $t5, 17
    beq     $t8, $t5, march_y
    nop

    b       phase_restore_dma
    nop

/* ---------------------------------------------------------------------- */
/* March MATS+:                                                            */
/*   up: w0; up: r0,w1; down: r1,w0                                        */
/* ---------------------------------------------------------------------- */
march_mats_plus:
    /* up: write 0 */
    move    $t0, $a0
    addiu   $t1, $a0, 8192
march_mats_plus_up_w0:
    beq     $t0, $t1, march_mats_plus_up_r0w1_prep
    nop
    sw      $zero, 0($t0)
    addiu   $t0, $t0, 4
    b       march_mats_plus_up_w0
    nop

march_mats_plus_up_r0w1_prep:
    move    $t0, $a0
march_mats_plus_up_r0w1:
    beq     $t0, $t1, march_mats_plus_down_r1w0_prep
    nop
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    li      $t5, 0xFFFFFFFF
    sw      $t5, 0($t0)
    addiu   $t0, $t0, 4
    b       march_mats_plus_up_r0w1
    nop

march_mats_plus_down_r1w0_prep:
    addiu   $t0, $a0, 8188
march_mats_plus_down_r1w0:
    sltu    $t5, $t0, $a0
    bnez    $t5, phase_restore_dma
    nop
    lw      $t4, 0($t0)
    li      $t5, 0xFFFFFFFF
    bne     $t4, $t5, record_fail_and_restore
    nop
    sw      $zero, 0($t0)
    addiu   $t0, $t0, -4
    b       march_mats_plus_down_r1w0
    nop

/* ---------------------------------------------------------------------- */
/* March MATS++:                                                           */
/*   up: w0; up: r0,w1; down: r1,w0,r0                                     */
/* ---------------------------------------------------------------------- */
march_mats_plus_plus:
    move    $t0, $a0
    addiu   $t1, $a0, 8192
march_mats_pp_up_w0:
    beq     $t0, $t1, march_mats_pp_up_r0w1_prep
    nop
    sw      $zero, 0($t0)
    addiu   $t0, $t0, 4
    b       march_mats_pp_up_w0
    nop

march_mats_pp_up_r0w1_prep:
    move    $t0, $a0
march_mats_pp_up_r0w1:
    beq     $t0, $t1, march_mats_pp_down_prep
    nop
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    li      $t5, 0xFFFFFFFF
    sw      $t5, 0($t0)
    addiu   $t0, $t0, 4
    b       march_mats_pp_up_r0w1
    nop

march_mats_pp_down_prep:
    addiu   $t0, $a0, 8188
march_mats_pp_down:
    sltu    $t5, $t0, $a0
    bnez    $t5, phase_restore_dma
    nop
    lw      $t4, 0($t0)
    li      $t5, 0xFFFFFFFF
    bne     $t4, $t5, record_fail_and_restore
    nop
    sw      $zero, 0($t0)
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    addiu   $t0, $t0, -4
    b       march_mats_pp_down
    nop

/* ---------------------------------------------------------------------- */
/* March A                                                                 */
/* ---------------------------------------------------------------------- */
march_a:
    move    $t0, $a0
    addiu   $t1, $a0, 8192
march_a_up_w0:
    beq     $t0, $t1, march_a_up_seq2_prep
    nop
    sw      $zero, 0($t0)
    addiu   $t0, $t0, 4
    b       march_a_up_w0
    nop

march_a_up_seq2_prep:
    move    $t0, $a0
march_a_up_seq2:
    beq     $t0, $t1, march_a_up_seq3_prep
    nop
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    li      $t5, 0xFFFFFFFF
    sw      $t5, 0($t0)
    sw      $zero, 0($t0)
    sw      $t5, 0($t0)
    addiu   $t0, $t0, 4
    b       march_a_up_seq2
    nop

march_a_up_seq3_prep:
    move    $t0, $a0
march_a_up_seq3:
    beq     $t0, $t1, march_a_down_seq4_prep
    nop
    lw      $t4, 0($t0)
    li      $t5, 0xFFFFFFFF
    bne     $t4, $t5, record_fail_and_restore
    nop
    sw      $zero, 0($t0)
    sw      $t5, 0($t0)
    addiu   $t0, $t0, 4
    b       march_a_up_seq3
    nop

march_a_down_seq4_prep:
    addiu   $t0, $a0, 8188
march_a_down_seq4:
    sltu    $t5, $t0, $a0
    bnez    $t5, march_a_down_seq5_prep
    nop
    lw      $t4, 0($t0)
    li      $t5, 0xFFFFFFFF
    bne     $t4, $t5, record_fail_and_restore
    nop
    sw      $zero, 0($t0)
    li      $t5, 0xFFFFFFFF
    sw      $t5, 0($t0)
    sw      $zero, 0($t0)
    addiu   $t0, $t0, -4
    b       march_a_down_seq4
    nop

march_a_down_seq5_prep:
    addiu   $t0, $a0, 8188
march_a_down_seq5:
    sltu    $t5, $t0, $a0
    bnez    $t5, phase_restore_dma
    nop
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    li      $t5, 0xFFFFFFFF
    sw      $t5, 0($t0)
    sw      $zero, 0($t0)
    addiu   $t0, $t0, -4
    b       march_a_down_seq5
    nop

/* ---------------------------------------------------------------------- */
/* March B                                                                 */
/* ---------------------------------------------------------------------- */
march_b:
    move    $t0, $a0
    addiu   $t1, $a0, 8192
march_b_up_w0:
    beq     $t0, $t1, march_b_up_seq2_prep
    nop
    sw      $zero, 0($t0)
    addiu   $t0, $t0, 4
    b       march_b_up_w0
    nop

march_b_up_seq2_prep:
    move    $t0, $a0
march_b_up_seq2:
    beq     $t0, $t1, march_b_up_seq3_prep
    nop
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    li      $t5, 0xFFFFFFFF
    sw      $t5, 0($t0)
    lw      $t4, 0($t0)
    bne     $t4, $t5, record_fail_and_restore
    nop
    sw      $zero, 0($t0)
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    li      $t5, 0xFFFFFFFF
    sw      $t5, 0($t0)
    addiu   $t0, $t0, 4
    b       march_b_up_seq2
    nop

march_b_up_seq3_prep:
    move    $t0, $a0
march_b_up_seq3:
    beq     $t0, $t1, march_b_down_seq4_prep
    nop
    lw      $t4, 0($t0)
    li      $t5, 0xFFFFFFFF
    bne     $t4, $t5, record_fail_and_restore
    nop
    sw      $zero, 0($t0)
    li      $t5, 0xFFFFFFFF
    sw      $t5, 0($t0)
    addiu   $t0, $t0, 4
    b       march_b_up_seq3
    nop

march_b_down_seq4_prep:
    addiu   $t0, $a0, 8188
march_b_down_seq4:
    sltu    $t5, $t0, $a0
    bnez    $t5, march_b_down_seq5_prep
    nop
    lw      $t4, 0($t0)
    li      $t5, 0xFFFFFFFF
    bne     $t4, $t5, record_fail_and_restore
    nop
    sw      $zero, 0($t0)
    li      $t5, 0xFFFFFFFF
    sw      $t5, 0($t0)
    sw      $zero, 0($t0)
    addiu   $t0, $t0, -4
    b       march_b_down_seq4
    nop

march_b_down_seq5_prep:
    addiu   $t0, $a0, 8188
march_b_down_seq5:
    sltu    $t5, $t0, $a0
    bnez    $t5, phase_restore_dma
    nop
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    li      $t5, 0xFFFFFFFF
    sw      $t5, 0($t0)
    sw      $zero, 0($t0)
    addiu   $t0, $t0, -4
    b       march_b_down_seq5
    nop

/* ---------------------------------------------------------------------- */
/* March C-                                                                */
/* ---------------------------------------------------------------------- */
march_c_minus:
    move    $t0, $a0
    addiu   $t1, $a0, 8192
march_c_minus_up_w0:
    beq     $t0, $t1, march_c_minus_up_r0w1_prep
    nop
    sw      $zero, 0($t0)
    addiu   $t0, $t0, 4
    b       march_c_minus_up_w0
    nop

march_c_minus_up_r0w1_prep:
    move    $t0, $a0
march_c_minus_up_r0w1:
    beq     $t0, $t1, march_c_minus_up_r1w0_prep
    nop
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    li      $t5, 0xFFFFFFFF
    sw      $t5, 0($t0)
    addiu   $t0, $t0, 4
    b       march_c_minus_up_r0w1
    nop

march_c_minus_up_r1w0_prep:
    move    $t0, $a0
march_c_minus_up_r1w0:
    beq     $t0, $t1, march_c_minus_down_r0w1_prep
    nop
    lw      $t4, 0($t0)
    li      $t5, 0xFFFFFFFF
    bne     $t4, $t5, record_fail_and_restore
    nop
    sw      $zero, 0($t0)
    addiu   $t0, $t0, 4
    b       march_c_minus_up_r1w0
    nop

march_c_minus_down_r0w1_prep:
    addiu   $t0, $a0, 8188
march_c_minus_down_r0w1:
    sltu    $t5, $t0, $a0
    bnez    $t5, march_c_minus_down_r1w0_prep
    nop
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    li      $t5, 0xFFFFFFFF
    sw      $t5, 0($t0)
    addiu   $t0, $t0, -4
    b       march_c_minus_down_r0w1
    nop

march_c_minus_down_r1w0_prep:
    addiu   $t0, $a0, 8188
march_c_minus_down_r1w0:
    sltu    $t5, $t0, $a0
    bnez    $t5, march_c_minus_up_r0_final_prep
    nop
    lw      $t4, 0($t0)
    li      $t5, 0xFFFFFFFF
    bne     $t4, $t5, record_fail_and_restore
    nop
    sw      $zero, 0($t0)
    addiu   $t0, $t0, -4
    b       march_c_minus_down_r1w0
    nop

march_c_minus_up_r0_final_prep:
    move    $t0, $a0
march_c_minus_up_r0_final:
    beq     $t0, $t1, phase_restore_dma
    nop
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    addiu   $t0, $t0, 4
    b       march_c_minus_up_r0_final
    nop

/* ---------------------------------------------------------------------- */
/* March C+                                                                */
/* ---------------------------------------------------------------------- */
march_c_plus:
    move    $t0, $a0
    addiu   $t1, $a0, 8192
march_c_plus_up_w0:
    beq     $t0, $t1, march_c_plus_up_seq2_prep
    nop
    sw      $zero, 0($t0)
    addiu   $t0, $t0, 4
    b       march_c_plus_up_w0
    nop

march_c_plus_up_seq2_prep:
    move    $t0, $a0
march_c_plus_up_seq2:
    beq     $t0, $t1, march_c_plus_up_seq3_prep
    nop
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    li      $t5, 0xFFFFFFFF
    sw      $t5, 0($t0)
    lw      $t4, 0($t0)
    bne     $t4, $t5, record_fail_and_restore
    nop
    addiu   $t0, $t0, 4
    b       march_c_plus_up_seq2
    nop

march_c_plus_up_seq3_prep:
    move    $t0, $a0
march_c_plus_up_seq3:
    beq     $t0, $t1, march_c_plus_down_seq4_prep
    nop
    lw      $t4, 0($t0)
    li      $t5, 0xFFFFFFFF
    bne     $t4, $t5, record_fail_and_restore
    nop
    sw      $zero, 0($t0)
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    addiu   $t0, $t0, 4
    b       march_c_plus_up_seq3
    nop

march_c_plus_down_seq4_prep:
    addiu   $t0, $a0, 8188
march_c_plus_down_seq4:
    sltu    $t5, $t0, $a0
    bnez    $t5, march_c_plus_down_seq5_prep
    nop
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    li      $t5, 0xFFFFFFFF
    sw      $t5, 0($t0)
    lw      $t4, 0($t0)
    bne     $t4, $t5, record_fail_and_restore
    nop
    addiu   $t0, $t0, -4
    b       march_c_plus_down_seq4
    nop

march_c_plus_down_seq5_prep:
    addiu   $t0, $a0, 8188
march_c_plus_down_seq5:
    sltu    $t5, $t0, $a0
    bnez    $t5, march_c_plus_up_r0_final_prep
    nop
    lw      $t4, 0($t0)
    li      $t5, 0xFFFFFFFF
    bne     $t4, $t5, record_fail_and_restore
    nop
    sw      $zero, 0($t0)
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    addiu   $t0, $t0, -4
    b       march_c_plus_down_seq5
    nop

march_c_plus_up_r0_final_prep:
    move    $t0, $a0
march_c_plus_up_r0_final:
    beq     $t0, $t1, phase_restore_dma
    nop
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    addiu   $t0, $t0, 4
    b       march_c_plus_up_r0_final
    nop

/* ---------------------------------------------------------------------- */
/* March SR                                                                */
/* ---------------------------------------------------------------------- */
march_sr:
    addiu   $t0, $a0, 8188
march_sr_down_w0:
    sltu    $t5, $t0, $a0
    bnez    $t5, march_sr_up_seq2_prep
    nop
    sw      $zero, 0($t0)
    addiu   $t0, $t0, -4
    b       march_sr_down_w0
    nop

march_sr_up_seq2_prep:
    move    $t0, $a0
    addiu   $t1, $a0, 8192
march_sr_up_seq2:
    beq     $t0, $t1, march_sr_down_seq3_prep
    nop
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    li      $t5, 0xFFFFFFFF
    sw      $t5, 0($t0)
    lw      $t4, 0($t0)
    bne     $t4, $t5, record_fail_and_restore
    nop
    sw      $zero, 0($t0)
    addiu   $t0, $t0, 4
    b       march_sr_up_seq2
    nop

march_sr_down_seq3_prep:
    addiu   $t0, $a0, 8188
march_sr_down_seq3:
    sltu    $t5, $t0, $a0
    bnez    $t5, march_sr_up_w1_prep
    nop
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    addiu   $t0, $t0, -4
    b       march_sr_down_seq3
    nop

march_sr_up_w1_prep:
    move    $t0, $a0
march_sr_up_w1:
    beq     $t0, $t1, march_sr_down_seq5_prep
    nop
    li      $t5, 0xFFFFFFFF
    sw      $t5, 0($t0)
    addiu   $t0, $t0, 4
    b       march_sr_up_w1
    nop

march_sr_down_seq5_prep:
    addiu   $t0, $a0, 8188
march_sr_down_seq5:
    sltu    $t5, $t0, $a0
    bnez    $t5, march_sr_up_seq6_prep
    nop
    lw      $t4, 0($t0)
    li      $t5, 0xFFFFFFFF
    bne     $t4, $t5, record_fail_and_restore
    nop
    sw      $zero, 0($t0)
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    li      $t5, 0xFFFFFFFF
    sw      $t5, 0($t0)
    addiu   $t0, $t0, -4
    b       march_sr_down_seq5
    nop

march_sr_up_seq6_prep:
    move    $t0, $a0
march_sr_up_seq6:
    beq     $t0, $t1, phase_restore_dma
    nop
    lw      $t4, 0($t0)
    li      $t5, 0xFFFFFFFF
    bne     $t4, $t5, record_fail_and_restore
    nop
    lw      $t4, 0($t0)
    li      $t5, 0xFFFFFFFF
    bne     $t4, $t5, record_fail_and_restore
    nop
    addiu   $t0, $t0, 4
    b       march_sr_up_seq6
    nop

/* ---------------------------------------------------------------------- */
/* March SS                                                                */
/* ---------------------------------------------------------------------- */
march_ss:
    addiu   $t0, $a0, 8188
march_ss_down_w0:
    sltu    $t5, $t0, $a0
    bnez    $t5, march_ss_up_seq2_prep
    nop
    sw      $zero, 0($t0)
    addiu   $t0, $t0, -4
    b       march_ss_down_w0
    nop

march_ss_up_seq2_prep:
    move    $t0, $a0
    addiu   $t1, $a0, 8192
march_ss_up_seq2:
    beq     $t0, $t1, march_ss_up_seq3_prep
    nop
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    sw      $zero, 0($t0)
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    li      $t5, 0xFFFFFFFF
    sw      $t5, 0($t0)
    addiu   $t0, $t0, 4
    b       march_ss_up_seq2
    nop

march_ss_up_seq3_prep:
    move    $t0, $a0
march_ss_up_seq3:
    beq     $t0, $t1, march_ss_down_seq4_prep
    nop
    lw      $t4, 0($t0)
    li      $t5, 0xFFFFFFFF
    bne     $t4, $t5, record_fail_and_restore
    nop
    lw      $t4, 0($t0)
    li      $t5, 0xFFFFFFFF
    bne     $t4, $t5, record_fail_and_restore
    nop
    li      $t5, 0xFFFFFFFF
    sw      $t5, 0($t0)
    lw      $t4, 0($t0)
    li      $t5, 0xFFFFFFFF
    bne     $t4, $t5, record_fail_and_restore
    nop
    sw      $zero, 0($t0)
    addiu   $t0, $t0, 4
    b       march_ss_up_seq3
    nop

march_ss_down_seq4_prep:
    addiu   $t0, $a0, 8188
march_ss_down_seq4:
    sltu    $t5, $t0, $a0
    bnez    $t5, march_ss_down_seq5_prep
    nop
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    sw      $zero, 0($t0)
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    li      $t5, 0xFFFFFFFF
    sw      $t5, 0($t0)
    addiu   $t0, $t0, -4
    b       march_ss_down_seq4
    nop

march_ss_down_seq5_prep:
    addiu   $t0, $a0, 8188
march_ss_down_seq5:
    sltu    $t5, $t0, $a0
    bnez    $t5, march_ss_up_final_prep
    nop
    lw      $t4, 0($t0)
    li      $t5, 0xFFFFFFFF
    bne     $t4, $t5, record_fail_and_restore
    nop
    lw      $t4, 0($t0)
    li      $t5, 0xFFFFFFFF
    bne     $t4, $t5, record_fail_and_restore
    nop
    li      $t5, 0xFFFFFFFF
    sw      $t5, 0($t0)
    lw      $t4, 0($t0)
    li      $t5, 0xFFFFFFFF
    bne     $t4, $t5, record_fail_and_restore
    nop
    sw      $zero, 0($t0)
    addiu   $t0, $t0, -4
    b       march_ss_down_seq5
    nop

march_ss_up_final_prep:
    move    $t0, $a0
march_ss_up_final:
    beq     $t0, $t1, phase_restore_dma
    nop
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    addiu   $t0, $t0, 4
    b       march_ss_up_final
    nop

/* ---------------------------------------------------------------------- */
/* March X                                                                 */
/* ---------------------------------------------------------------------- */
march_x:
    move    $t0, $a0
    addiu   $t1, $a0, 8192
march_x_up_w0:
    beq     $t0, $t1, march_x_up_r0w1_prep
    nop
    sw      $zero, 0($t0)
    addiu   $t0, $t0, 4
    b       march_x_up_w0
    nop

march_x_up_r0w1_prep:
    move    $t0, $a0
march_x_up_r0w1:
    beq     $t0, $t1, march_x_down_r1w0_prep
    nop
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    li      $t5, 0xFFFFFFFF
    sw      $t5, 0($t0)
    addiu   $t0, $t0, 4
    b       march_x_up_r0w1
    nop

march_x_down_r1w0_prep:
    addiu   $t0, $a0, 8188
march_x_down_r1w0:
    sltu    $t5, $t0, $a0
    bnez    $t5, march_x_up_r0_final_prep
    nop
    lw      $t4, 0($t0)
    li      $t5, 0xFFFFFFFF
    bne     $t4, $t5, record_fail_and_restore
    nop
    sw      $zero, 0($t0)
    addiu   $t0, $t0, -4
    b       march_x_down_r1w0
    nop

march_x_up_r0_final_prep:
    move    $t0, $a0
march_x_up_r0_final:
    beq     $t0, $t1, phase_restore_dma
    nop
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    addiu   $t0, $t0, 4
    b       march_x_up_r0_final
    nop

/* ---------------------------------------------------------------------- */
/* March Y                                                                 */
/* ---------------------------------------------------------------------- */
march_y:
    move    $t0, $a0
    addiu   $t1, $a0, 8192
march_y_up_w0:
    beq     $t0, $t1, march_y_up_seq2_prep
    nop
    sw      $zero, 0($t0)
    addiu   $t0, $t0, 4
    b       march_y_up_w0
    nop

march_y_up_seq2_prep:
    move    $t0, $a0
march_y_up_seq2:
    beq     $t0, $t1, march_y_up_seq3_prep
    nop
    sw      $zero, 0($t0)
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    li      $t5, 0xFFFFFFFF
    sw      $t5, 0($t0)
    sw      $t5, 0($t0)
    lw      $t4, 0($t0)
    li      $t5, 0xFFFFFFFF
    bne     $t4, $t5, record_fail_and_restore
    nop
    addiu   $t0, $t0, 4
    b       march_y_up_seq2
    nop

march_y_up_seq3_prep:
    move    $t0, $a0
march_y_up_seq3:
    beq     $t0, $t1, march_y_down_seq4_prep
    nop
    li      $t5, 0xFFFFFFFF
    sw      $t5, 0($t0)
    lw      $t4, 0($t0)
    li      $t5, 0xFFFFFFFF
    bne     $t4, $t5, record_fail_and_restore
    nop
    sw      $zero, 0($t0)
    sw      $zero, 0($t0)
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    addiu   $t0, $t0, 4
    b       march_y_up_seq3
    nop

march_y_down_seq4_prep:
    addiu   $t0, $a0, 8188
march_y_down_seq4:
    sltu    $t5, $t0, $a0
    bnez    $t5, march_y_down_seq5_prep
    nop
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    sw      $zero, 0($t0)
    li      $t5, 0xFFFFFFFF
    sw      $t5, 0($t0)
    sw      $t5, 0($t0)
    lw      $t4, 0($t0)
    li      $t5, 0xFFFFFFFF
    bne     $t4, $t5, record_fail_and_restore
    nop
    addiu   $t0, $t0, -4
    b       march_y_down_seq4
    nop

march_y_down_seq5_prep:
    addiu   $t0, $a0, 8188
march_y_down_seq5:
    sltu    $t5, $t0, $a0
    bnez    $t5, march_y_up_final_prep
    nop
    lw      $t4, 0($t0)
    li      $t5, 0xFFFFFFFF
    bne     $t4, $t5, record_fail_and_restore
    nop
    li      $t5, 0xFFFFFFFF
    sw      $t5, 0($t0)
    sw      $zero, 0($t0)
    sw      $zero, 0($t0)
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    addiu   $t0, $t0, -4
    b       march_y_down_seq5
    nop

march_y_up_final_prep:
    move    $t0, $a0
march_y_up_final:
    beq     $t0, $t1, phase_restore_dma
    nop
    lw      $t4, 0($t0)
    bnez    $t4, record_fail_and_restore
    nop
    addiu   $t0, $t0, 4
    b       march_y_up_final
    nop

/* ---------------------------------------------------------------------- */
/* Failure bookkeeping (first failing address only)                        */
/* ---------------------------------------------------------------------- */
record_fail_and_restore:
    /*
     * if (fail_addr == 0) fail_addr = ptr;
     *   goto restore;
     */
    bnez    ret_fail_addr, phase_restore_dma
    nop
    move    ret_fail_addr, $t0
    b       phase_restore_dma
    nop

/* ---------------------------------------------------------------------- */
/* PHASE 3: Restore original chunk from RSP DMEM/IMEM                      */
/* ---------------------------------------------------------------------- */
phase_restore_dma:
    /* Re-use SP DMA register block for the restore direction (WRLEN). */
    li      reg_sp_base, 0xA4040000

wait_dma_idle_before_restore_dmem:
    lw      $t5, 0x0010(reg_sp_base)
    andi    $t5, $t5, 0x000C
    bnez    $t5, wait_dma_idle_before_restore_dmem
    nop

    /* DMA OUT: SP DMEM[0..4095] -> DRAM chunk[0..4095] */
    sw      arg_chunk, 0x0004(reg_sp_base)
    move    $t5, $zero
    sw      $t5, 0x0000(reg_sp_base)
    li      $t5, 4095
    sw      $t5, 0x000C(reg_sp_base)

wait_dma_done_restore_dmem:
    lw      $t5, 0x0010(reg_sp_base)
    andi    $t5, $t5, 0x000C
    bnez    $t5, wait_dma_done_restore_dmem
    nop

    /* DMA OUT: SP IMEM[0..4095] -> DRAM chunk[4096..8191] */
    addiu   $t5, arg_chunk, 4096
    sw      $t5, 0x0004(reg_sp_base)
    li      $t5, 4096
    sw      $t5, 0x0000(reg_sp_base)
    li      $t5, 4095
    sw      $t5, 0x000C(reg_sp_base)

wait_dma_done_restore_imem:
    lw      $t5, 0x0010(reg_sp_base)
    andi    $t5, $t5, 0x000C
    bnez    $t5, wait_dma_done_restore_imem
    nop

    /* ---------- PHASE 4: Verify restored data against SP memories ---------- */
    /*
     * for i in [0..1023]: assert(chunk_words[i] == SP_DMEM[i]);
     * for i in [0..1023]: assert(chunk_words[1024+i] == SP_IMEM[i]);
     *
     * Any mismatch here means restore integrity failed.
     */
#define restore_ptr             $t0
#define restore_end             $t1
#define restore_sp_ptr          $t2
    move    restore_ptr, arg_chunk
    addiu   restore_end, arg_chunk, 4096
    li      restore_sp_ptr, 0xA4000000

verify_restored_dmem_loop:
    beq     restore_ptr, restore_end, verify_restored_imem_setup
    nop
    lw      $t4, 0(restore_ptr)
    lw      $t5, 0(restore_sp_ptr)
    beq     $t4, $t5, verify_restored_dmem_next
    nop
    bnez    ret_fail_addr, verify_restored_dmem_next
    nop
    move    ret_fail_addr, restore_ptr
verify_restored_dmem_next:
    addiu   restore_ptr, restore_ptr, 4
    addiu   restore_sp_ptr, restore_sp_ptr, 4
    b       verify_restored_dmem_loop
    nop

verify_restored_imem_setup:
    addiu   restore_ptr, arg_chunk, 4096
    addiu   restore_end, arg_chunk, 8192
    li      restore_sp_ptr, 0xA4001000

verify_restored_imem_loop:
    beq     restore_ptr, restore_end, function_epilogue
    nop
    lw      $t4, 0(restore_ptr)
    lw      $t5, 0(restore_sp_ptr)
    beq     $t4, $t5, verify_restored_imem_next
    nop
    bnez    ret_fail_addr, verify_restored_imem_next
    nop
    move    ret_fail_addr, restore_ptr
verify_restored_imem_next:
    addiu   restore_ptr, restore_ptr, 4
    addiu   restore_sp_ptr, restore_sp_ptr, 4
    b       verify_restored_imem_loop
    nop
#undef restore_ptr
#undef restore_end
#undef restore_sp_ptr

function_epilogue:
    jr      $ra
    nop

    .globl memtest_run_slice_critical_asm_end
memtest_run_slice_critical_asm_end:
    nop

    .end memtest_run_slice_critical_asm
