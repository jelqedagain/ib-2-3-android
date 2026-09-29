// Native ARM64 backend glue (Android): the game's code runs directly on the CPU.
//
// Context layout (must match cpu_native.cpp):
//   0    x0..x30        (31 x 8)
//   248  sp             the caller's stack pointer
//   256  q0..q15        (16 x 16)
//   512  target         where to continue after a handler (0 = return to x30)
//   520  index          HLE stub index
//   528  stack_args     guest calls: address of the stack arguments
//   536  nstack         guest calls: number of 8-byte stack arguments
//   544  (size)

    .text

// Reached from an HLE stub with x16 = stub index. Saves the guest's registers, runs
// ib3_hle_dispatch(Context*), then restores them (a handler may have changed any of them,
// e.g. longjmp) and returns to x30 or continues at `target` (e.g. objc_msgSend -> IMP).
    .global ib3_hle_entry
    .type ib3_hle_entry, %function
    .p2align 4
ib3_hle_entry:
    sub     sp, sp, #544
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30, [sp, #240]
    add     x9, sp, #544
    str     x9, [sp, #248]
    stp     q0, q1, [sp, #256]
    stp     q2, q3, [sp, #288]
    stp     q4, q5, [sp, #320]
    stp     q6, q7, [sp, #352]
    stp     q8, q9, [sp, #384]
    stp     q10, q11, [sp, #416]
    stp     q12, q13, [sp, #448]
    stp     q14, q15, [sp, #480]
    str     xzr, [sp, #512]
    str     x16, [sp, #520]
    mov     x0, sp
    bl      ib3_hle_dispatch
    ldp     q0, q1, [sp, #256]
    ldp     q2, q3, [sp, #288]
    ldp     q4, q5, [sp, #320]
    ldp     q6, q7, [sp, #352]
    ldp     q8, q9, [sp, #384]
    ldp     q10, q11, [sp, #416]
    ldp     q12, q13, [sp, #448]
    ldp     q14, q15, [sp, #480]
    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldr     x8, [sp, #64]
    ldr     x19, [sp, #152]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30, [sp, #240]
    ldr     x16, [sp, #512]
    ldr     x17, [sp, #248]
    mov     sp, x17
    cbz     x16, 1f
    br      x16
1:  ret
    .size ib3_hle_entry, . - ib3_hle_entry

// void ib3_guest_call(Context* ctx, u64 fn): calls guest code with the argument registers and
// stack arguments in `ctx`; stores x0, x1 and q0..q3 back into it.
    .global ib3_guest_call
    .type ib3_guest_call, %function
    .p2align 4
ib3_guest_call:
    stp     x29, x30, [sp, #-96]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    stp     x23, x24, [sp, #48]
    stp     x25, x26, [sp, #64]
    stp     x27, x28, [sp, #80]
    mov     x19, x0
    mov     x20, x1
    ldr     x9, [x19, #536]
    ldr     x10, [x19, #528]
    add     x11, x9, #1
    and     x11, x11, #0xfffffffffffffffe
    sub     sp, sp, x11, lsl #3
    mov     x12, #0
2:  cmp     x12, x9
    b.ge    3f
    ldr     x13, [x10, x12, lsl #3]
    str     x13, [sp, x12, lsl #3]
    add     x12, x12, #1
    b       2b
3:  ldp     q0, q1, [x19, #256]
    ldp     q2, q3, [x19, #288]
    ldp     q4, q5, [x19, #320]
    ldp     q6, q7, [x19, #352]
    ldp     x0, x1, [x19, #0]
    ldp     x2, x3, [x19, #16]
    ldp     x4, x5, [x19, #32]
    ldp     x6, x7, [x19, #48]
    ldr     x8, [x19, #64]
    blr     x20
    stp     x0, x1, [x19, #0]
    stp     q0, q1, [x19, #256]
    stp     q2, q3, [x19, #288]
    mov     sp, x29
    ldp     x19, x20, [sp, #16]
    ldp     x21, x22, [sp, #32]
    ldp     x23, x24, [sp, #48]
    ldp     x25, x26, [sp, #64]
    ldp     x27, x28, [sp, #80]
    ldp     x29, x30, [sp], #96
    ret
    .size ib3_guest_call, . - ib3_guest_call
