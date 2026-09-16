/* SPDX-License-Identifier: LGPL-2.1-or-later */
.section .text.init, "ax", @progbits
.global _start
_start:
.option push
.option norelax
    la gp, __global_pointer$
.option pop
    /* Firmware starts every hart. Nonzero harts must not touch C globals. */
    csrr t0, hartid
    bnez t0, .Lreturn_zero
    mv s0, sp                         /* preserve firmware stack exactly */
    mv s1, a0                         /* params survive boot-stack result */
    mv s2, a1                         /* environment survives validation */
    /* C dereferences only ABI/environment and checks both before this switch. */
    mv a0, s1
    mv a1, s2
    mv a2, s0
    call etaac_full_boot_stack
    beqz a0, .Lreturn_bad
    mv sp, a0
    mv a0, s1
    mv a1, s2
    call etaac_full_process
    mv t0, a0
    mv sp, s0
    mv a1, t0
    li a2, 0
    li a0, 8                           /* SYSCALL_RETURN_FROM_KERNEL */
    ecall
.Lreturn_bad:
    mv a1, a0
    li a2, 0
    li a0, 8
    ecall
.Lreturn_zero:
    li a1, 0
    li a2, 0
    li a0, 8
    ecall
