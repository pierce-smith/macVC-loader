// midhook stub. the hook island calls this with:
//   x16 = MidInfo* { handler, user }
//   [sp+0]  = original x30, xzr
//   [sp+16] = original x16, x17
//   then 128 bytes skipped so the hooked function's red zone is safe
// builds an MVCContext on the stack, calls handler(ctx, user), writes changes back and
// returns to the island, which restores x30/x16/x17 and runs the displaced instruction.
//


// MVCContext: x[31] @0, sp @248, nzcv @256, reserved @264 (our return address),
// q[32] @272, 784 bytes total.

    .text
    .globl _mvc_midhook_stub
    .p2align 2
_mvc_midhook_stub:
    sub     sp, sp, #784
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    add     x9, sp, #784
    ldp     x0, x1, [x9, #16]       // original x16, x17
    stp     x0, x1, [sp, #128]
    ldr     x0, [x9]                // original x30
    str     x0, [sp, #240]
    add     x0, x9, #160            // original sp = stub sp + 784 + 32 + 128
    str     x0, [sp, #248]
    mrs     x0, nzcv
    str     x0, [sp, #256]
    str     x30, [sp, #264]         // return address into the island
    stp     q0, q1, [sp, #272]
    stp     q2, q3, [sp, #304]
    stp     q4, q5, [sp, #336]
    stp     q6, q7, [sp, #368]
    stp     q8, q9, [sp, #400]
    stp     q10, q11, [sp, #432]
    stp     q12, q13, [sp, #464]
    stp     q14, q15, [sp, #496]
    stp     q16, q17, [sp, #528]
    stp     q18, q19, [sp, #560]
    stp     q20, q21, [sp, #592]
    stp     q22, q23, [sp, #624]
    stp     q24, q25, [sp, #656]
    stp     q26, q27, [sp, #688]
    stp     q28, q29, [sp, #720]
    stp     q30, q31, [sp, #752]

    mov     x0, sp                  // ctx
    ldr     x1, [x16, #8]           // user
    ldr     x9, [x16]               // handler
    blr     x9

    ldp     q0, q1, [sp, #272]
    ldp     q2, q3, [sp, #304]
    ldp     q4, q5, [sp, #336]
    ldp     q6, q7, [sp, #368]
    ldp     q8, q9, [sp, #400]
    ldp     q10, q11, [sp, #432]
    ldp     q12, q13, [sp, #464]
    ldp     q14, q15, [sp, #496]
    ldp     q16, q17, [sp, #528]
    ldp     q18, q19, [sp, #560]
    ldp     q20, q21, [sp, #592]
    ldp     q22, q23, [sp, #624]
    ldp     q24, q25, [sp, #656]
    ldp     q26, q27, [sp, #688]
    ldp     q28, q29, [sp, #720]
    ldp     q30, q31, [sp, #752]
    ldr     x0, [sp, #256]
    msr     nzcv, x0
    add     x9, sp, #784
    ldr     x0, [sp, #240]          // x30 as left by the handler
    str     x0, [x9]
    ldp     x0, x1, [sp, #128]      // x16, x17 as left by the handler
    stp     x0, x1, [x9, #16]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldp     x0, x1, [sp, #0]
    ldr     x30, [sp, #264]
    add     sp, sp, #784
    ret
