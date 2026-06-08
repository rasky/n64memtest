/*
 * uintptr_t mem_test(uint8_t *chunk, uint32_t test_pass, uint32_t seed)
 *
 * Arguments:
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
 *   - one call depth (main routine -> leaf helpers), no nested calls
 */

    .set noreorder
    .text
    .globl mem_test
    .ent mem_test

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
#define ra2                     $v1

mem_test:
    /*
     * Prime this function body into I-cache before running any test logic.
     *
     * for (p = &mem_test; p < &mem_test_end; p += 32) {
     *     cache HIT_FILL, 0(p);
     * }
     *
     * - 32 bytes = R4300 I-cache line size.
     * - HIT_FILL for I-cache is CACHE op 0x14.
     */
#define icache_ptr              $t0
#define icache_end              $t1
    la      icache_ptr, mem_test
    la      icache_end, mem_test_end

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
    /* Preserve caller return address because jal is used below. */
    move    ra2, $ra

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

    jal     sp_dma_wait
    nop

    /* DMA IN: DRAM chunk[0..4095] -> SP DMEM[0..4095] */
    move    $t0, arg_chunk      /* dram addr */
    move    $t1, $zero          /* sp addr (DMEM) */
    li      $t2, 4095           /* len-1 */
    jal     sp_dma_read
    nop

    /* DMA IN: DRAM chunk[4096..8191] -> SP IMEM[0..4095] */
    addiu   $t0, arg_chunk, 4096 /* dram addr */
    li      $t1, 4096            /* sp addr (IMEM base) */
    li      $t2, 4095            /* len-1 */
    jal     sp_dma_read
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
     *   while (ptr != end) { 
     *     *ptr = pattern(test_id, pass, word_index, seed); 
     *     ptr++; word_index++; 
     *   }
     */
    move    $t0, $a0
    addiu   $t1, $a0, 8192
    move    $t2, $t3

pattern_fill_loop:
    beq     $t0, $t1, pattern_verify_setup
    nop

    /* pattern = pattern_word_compute(test_id, pass, word_index, seed); */
    jal     pattern_word_compute
    nop
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

    /* expected = pattern_word_compute(test_id, pass, word_index, seed); */
    jal     pattern_word_compute
    nop
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

/*
 * pattern_word_compute
 * Input:
 *   reg_test_id = test id
 *   reg_pass    = pass
 *   $t2         = word index
 *   arg_seed    = seed
 *   reg_pass_mix= pass * 0x9e3779b9
 * Output:
 *   $t4         = pattern word
 */
pattern_word_compute:
    beq     reg_test_id, $zero, pcase_random
    nop
    li      $t5, 1
    beq     reg_test_id, $t5, pcase_checker
    nop
    li      $t5, 2
    beq     reg_test_id, $t5, pcase_solid
    nop
    li      $t5, 3
    beq     reg_test_id, $t5, pcase_walk1
    nop
    li      $t5, 4
    beq     reg_test_id, $t5, pcase_walk0
    nop
    li      $t5, 5
    beq     reg_test_id, $t5, pcase_seqinc
    nop
    li      $t5, 6
    beq     reg_test_id, $t5, pcase_bitspread
    nop
    li      $t5, 7
    beq     reg_test_id, $t5, pcase_bitflip
    nop
    move    $t4, $zero
    jr      $ra
    nop

pcase_random:
    /* x = seed ^ pass_mix ^ word_index; x = mix32(x); */
    xor     $t4, arg_seed, reg_pass_mix
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
    jr      $ra
    nop

pcase_checker:
    /* pattern = ((word_index + pass) & 1) ? 0xAAAAAAAA : 0x55555555; */
    addu    $t5, $t2, reg_pass
    andi    $t5, $t5, 1
    bnez    $t5, pcase_checker_odd
    nop
    li      $t4, 0x55555555
    jr      $ra
    nop
pcase_checker_odd:
    li      $t4, 0xAAAAAAAA
    jr      $ra
    nop

pcase_solid:
    /* pattern = (pass & 1) ? 0xFFFFFFFF : 0x00000000; */
    andi    $t5, reg_pass, 1
    beqz    $t5, pcase_solid_zero
    nop
    li      $t4, 0xFFFFFFFF
    jr      $ra
    nop
pcase_solid_zero:
    move    $t4, $zero
    jr      $ra
    nop

pcase_walk1:
    /* pattern = 1U << ((word_index + pass) & 31); */
    addu    $t5, $t2, reg_pass
    andi    $t5, $t5, 31
    li      $t4, 1
    sllv    $t4, $t4, $t5
    jr      $ra
    nop

pcase_walk0:
    /* pattern = ~(1U << ((word_index + pass) & 31)); */
    addu    $t5, $t2, reg_pass
    andi    $t5, $t5, 31
    li      $t4, 1
    sllv    $t4, $t4, $t5
    nor     $t4, $t4, $zero
    jr      $ra
    nop

pcase_seqinc:
    /* pattern = word_index + (pass * 0x01010101U); */
    li      $t5, 0x01010101
    multu   reg_pass, $t5
    mflo    $t5
    addu    $t4, $t2, $t5
    jr      $ra
    nop

pcase_bitspread:
    /* pos = pass & 15; pat = (1<<pos) | (1<<(pos+16)); pattern = (word_index & 1) ? ~pat : pat; */
    andi    $t5, reg_pass, 15
    li      $t4, 1
    sllv    $t4, $t4, $t5
    addiu   $a3, $t5, 16
    li      $t5, 1
    sllv    $t5, $t5, $a3
    or      $t4, $t4, $t5
    andi    $t5, $t2, 1
    beqz    $t5, pcase_bitspread_done
    nop
    nor     $t4, $t4, $zero
pcase_bitspread_done:
    jr      $ra
    nop

pcase_bitflip:
    /* pos = pass & 31; base = 1<<pos; pattern = ((word_index + pass) & 1) ? ~base : base; */
    andi    $t5, reg_pass, 31
    li      $t4, 1
    sllv    $t4, $t4, $t5
    addu    $t5, $t2, reg_pass
    andi    $t5, $t5, 1
    beqz    $t5, pcase_bitflip_done
    nop
    nor     $t4, $t4, $zero
pcase_bitflip_done:
    jr      $ra
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

    jal     sp_dma_wait
    nop

    /* DMA OUT: SP DMEM[0..4095] -> DRAM chunk[0..4095] */
    move    $t0, arg_chunk
    move    $t1, $zero
    li      $t2, 4095
    jal     sp_dma_write
    nop

    /* DMA OUT: SP IMEM[0..4095] -> DRAM chunk[4096..8191] */
    addiu   $t0, arg_chunk, 4096
    li      $t1, 4096
    li      $t2, 4095
    jal     sp_dma_write
    nop

    /* ---------- PHASE 4: Verify restored data against SP memories ---------- */
    /*
     * for i in [0..1023]: assert(chunk_words[i] == SP_DMEM[i]);
     * for i in [0..1023]: assert(chunk_words[1024+i] == SP_IMEM[i]);
     *
     * Any mismatch here means restore integrity failed.
     */
    move    $t0, arg_chunk
    addiu   $t1, arg_chunk, 4096
    li      $t2, 0xA4000000
    jal     compare_region_with_sp
    nop

    addiu   $t0, arg_chunk, 4096
    addiu   $t1, arg_chunk, 8192
    li      $t2, 0xA4001000
    jal     compare_region_with_sp
    nop

function_epilogue:
    jr      ra2
    nop

/*
 * Leaf helper: wait until SP DMA/IO are idle.
 */
sp_dma_wait:
sp_dma_wait_loop:
    lw      $t5, 0x0010(reg_sp_base)
    andi    $t5, $t5, 0x000C
    bnez    $t5, sp_dma_wait_loop
    nop
    jr      $ra
    nop

/*
 * Leaf helper: DMA from DRAM -> SP memory.
 * Input:
 *   t0 = dram address
 *   t1 = sp address (0=DMEM, 4096=IMEM)
 *   t2 = length-1
 */
sp_dma_read:
    sw      $t0, 0x0004(reg_sp_base)
    sw      $t1, 0x0000(reg_sp_base)
    sw      $t2, 0x0008(reg_sp_base)
sp_dma_read_wait_loop:
    lw      $t5, 0x0010(reg_sp_base)
    andi    $t5, $t5, 0x000C
    bnez    $t5, sp_dma_read_wait_loop
    nop
    jr      $ra
    nop

/*
 * Leaf helper: DMA from SP memory -> DRAM.
 * Input:
 *   t0 = dram address
 *   t1 = sp address (0=DMEM, 4096=IMEM)
 *   t2 = length-1
 */
sp_dma_write:
    sw      $t0, 0x0004(reg_sp_base)
    sw      $t1, 0x0000(reg_sp_base)
    sw      $t2, 0x000C(reg_sp_base)
sp_dma_write_wait_loop:
    lw      $t5, 0x0010(reg_sp_base)
    andi    $t5, $t5, 0x000C
    bnez    $t5, sp_dma_write_wait_loop
    nop
    jr      $ra
    nop

/*
 * Leaf helper: compare [t0, t1) against SP mirror starting at t2.
 * Input:
 *   t0 = chunk pointer start
 *   t1 = chunk pointer end (exclusive)
 *   t2 = SP mirror pointer start
 * Side effects:
 *   if first mismatch is found and ret_fail_addr == 0, stores pointer in ret_fail_addr.
 */
compare_region_with_sp:
compare_region_with_sp_loop:
    beq     $t0, $t1, compare_region_with_sp_done
    nop
    lw      $t4, 0($t0)
    lw      $t5, 0($t2)
    beq     $t4, $t5, compare_region_with_sp_next
    nop
    bnez    ret_fail_addr, compare_region_with_sp_next
    nop
    move    ret_fail_addr, $t0
compare_region_with_sp_next:
    addiu   $t0, $t0, 4
    addiu   $t2, $t2, 4
    b       compare_region_with_sp_loop
    nop
compare_region_with_sp_done:
    jr      $ra
    nop

    .globl mem_test_end
mem_test_end:
    nop

    .end mem_test
