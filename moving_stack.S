.set noreorder
.text
.align 4
.globl run_moving_stack

/* void run_moving_stack(void *stack_buffer, uint32_t stack_size, void (*func)(void)) */
run_moving_stack:
    move    $t0, $sp          /* save old stack pointer */
    move    $sp, $a0          /* switch to temporary stack buffer */
    addu    $sp, $sp, $a1     /* stack grows downward from top */
    addiu   $sp, $sp, -16     /* reserve scratch area */
    sd      $t0, 0($sp)       /* save previous stack pointer */
    sd      $ra, 8($sp)       /* save return address */

    move    $t9, $a2
    jalr    $t9
    nop

    ld      $ra, 8($sp)
    ld      $sp, 0($sp)       /* restore original stack pointer */
    jr      $ra
    nop
