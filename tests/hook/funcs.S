// test functions for the hook engine. each starts with (or has at a known
// offset) one instruction form the relocator has to rewrite.
// called through _thunk(fn, arg): x0 = arg, flags = cmp arg, #10,
// x15 = return address (for the BL test, whose first instruction clobbers x30).
    .text
    // own 16 KB pages: write_raw makes a patched page non-executable for a
    // moment, so the test functions must never share a page with hook.c
    .p2align 14

    .globl _thunk
_thunk:
    mov     x15, x30
    mov     x16, x0
    mov     x0, x1
    cmp     x1, #10
    br      x16

    .globl _t_plain                 // ordinary first instruction
_t_plain:
    add     x0, x0, #1
    ret

    .globl _t_adrp                  // ADRP + ADD -> address of g_data
_t_adrp:
    adrp    x0, _g_data@PAGE
    add     x0, x0, _g_data@PAGEOFF
    ret

    .globl _t_adr                   // ADR -> address of a local label
_t_adr:
    adr     x0, Ladr_target
    ret
Ladr_target:
    .word   0

    .globl _t_b                     // B forward: returns 2
_t_b:
    b       Lb_dest
    mov     x0, #1
    ret
Lb_dest:
    mov     x0, #2
    ret

    .globl _t_bl                    // BL: returns arg + 7 (returns through x15)
_t_bl:
    bl      Lbl_helper
    mov     x30, x15
    ret
Lbl_helper:
    add     x0, x0, #7
    ret

    .globl _t_bcond                 // B.LT: arg < 10 -> 200, else 100
_t_bcond:
    b.lt    Lbc_taken
    mov     x0, #100
    ret
Lbc_taken:
    mov     x0, #200
    ret

    .globl _t_cbz                   // CBZ x0: 0 -> 2, else 1
_t_cbz:
    cbz     x0, Lcbz_taken
    mov     x0, #1
    ret
Lcbz_taken:
    mov     x0, #2
    ret

    .globl _t_cbnz_w                // CBNZ w0: low word nonzero -> 2, else 1
_t_cbnz_w:
    cbnz    w0, Lcbnz_taken
    mov     x0, #1
    ret
Lcbnz_taken:
    mov     x0, #2
    ret

    .globl _t_tbz                   // TBZ bit 3: clear -> 2, else 1
_t_tbz:
    tbz     x0, #3, Ltbz_taken
    mov     x0, #1
    ret
Ltbz_taken:
    mov     x0, #2
    ret

    .globl _t_tbnz_hi               // TBNZ bit 40: set -> 2, else 1
_t_tbnz_hi:
    tbnz    x0, #40, Ltbnz_taken
    mov     x0, #1
    ret
Ltbnz_taken:
    mov     x0, #2
    ret

    .globl _t_ldr_x                 // LDR X literal
_t_ldr_x:
    ldr     x0, Llit64
    ret
    .globl _t_ldr_w                 // LDR W literal (zero-extends)
_t_ldr_w:
    ldr     w0, Llit32neg
    ret
    .globl _t_ldrsw                 // LDRSW literal (sign-extends)
_t_ldrsw:
    ldrsw   x0, Llit32neg
    ret
    .globl _t_ldr_s                 // LDR S literal -> bits in x0
_t_ldr_s:
    ldr     s0, Llit32neg
    fmov    w0, s0
    ret
    .globl _t_ldr_d                 // LDR D literal -> bits in x0
_t_ldr_d:
    ldr     d0, Llit64
    fmov    x0, d0
    ret
    .globl _t_ldr_q                 // LDR Q literal -> high half in x0
_t_ldr_q:
    ldr     q0, Llit128
    mov     x0, v0.d[1]
    ret
    .p2align 4
Llit128:
    .quad   0x0102030405060708
Llit64:
    .quad   0x1122334455667788
Llit32neg:
    .word   0xFFFFFFF0
    .word   0

    .globl _t_mid                   // midhook target: x0 + 3, hooked at +4
_t_mid:
    add     x0, x0, #1
    .globl _t_mid_at
_t_mid_at:
    add     x0, x0, #2
    ret

    .globl _t_mid_flags             // flags set by the thunk survive a midhook
_t_mid_flags:
    nop
    .globl _t_mid_flags_at
_t_mid_flags_at:
    nop
    b.lt    Lmf_lt
    mov     x0, #100
    ret
Lmf_lt:
    mov     x0, #200
    ret

    .globl _t_mid_cbz               // midhook ON a CBZ (relocated branch)
_t_mid_cbz:
    mov     x1, x0
    .globl _t_mid_cbz_at
_t_mid_cbz_at:
    cbz     x1, Lmc_zero
    mov     x0, #1
    ret
Lmc_zero:
    mov     x0, #2
    ret

    .globl _t_mid_regs              // all callee-saved + NEON survive a midhook
_t_mid_regs:
    // x0 = pointer to 12 u64: writes x19..x28 and d8,d9 after the hook point
    stp     x19, x20, [sp, #-96]!
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     d8, d9, [sp, #80]
    mov     x19, #19
    mov     x20, #20
    mov     x21, #21
    mov     x22, #22
    mov     x23, #23
    mov     x24, #24
    mov     x25, #25
    mov     x26, #26
    mov     x27, #27
    mov     x28, #28
    fmov    d8, #1.0
    fmov    d9, #2.0
    .globl _t_mid_regs_at
_t_mid_regs_at:
    nop
    stp     x19, x20, [x0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     d8, d9, [x0, #80]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     d8, d9, [sp, #80]
    ldp     x19, x20, [sp], #96
    ret

    .globl _t_patch                 // returns 5; patched to return 42
_t_patch:
    mov     x0, #5
    ret
    .globl _t_marker                // a unique byte pattern for mvc_find_pattern
_t_marker:
    movz    x0, #0xBEEF
    movk    x0, #0xC0DE, lsl #16
    ret
    .p2align 14
    .space  4, 0

    .data
    .p2align 3
    .globl _g_data
_g_data:
    .quad   0
