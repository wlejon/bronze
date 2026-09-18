/*
 * The runtime's one way into compiled code on the SysV targets (fn.h,
 * rtEnterJs); enter_js_msvc_x64.asm is the Windows twin.
 *
 * Compiled code keeps the TLS block's address in a callee-saved register for
 * as long as it runs — R13 on x64, X28 on aarch64 (bronze_abi_tls.h, "the
 * pinned register"). A compiled caller never touches the register, so it
 * only has to be loaded at the boundary from C++ — and C++ is free to have
 * used the register for anything, so this saves it, loads the block, makes
 * the call, and puts it back.
 *
 * uint64_t bronze_enter_js(bronze_fn_code code, uint64_t env, uint64_t thisv,
 *                          uint32_t argc, const uint64_t* argv)
 *
 * Both bodies keep the frame-pointer chain intact (push rbp / stp x29, x30),
 * because that chain is what the Error.stack walker follows here.
 */

#if defined(__APPLE__)
#define SYM(name) _##name
#define PLT(name) _##name
#else
#define SYM(name) name
#define PLT(name) name@PLT
#endif

#if defined(__x86_64__)

/* rdi = code, rsi = env, rdx = this, ecx = argc, r8 = argv. */
    .text
    .globl SYM(bronze_enter_js)
#if !defined(__APPLE__)
    .type SYM(bronze_enter_js), @function
#endif
    .p2align 4
SYM(bronze_enter_js):
    .cfi_startproc
    pushq %rbp
    .cfi_def_cfa_offset 16
    .cfi_offset %rbp, -16
    movq %rsp, %rbp
    .cfi_def_cfa_register %rbp
    pushq %r13
    .cfi_offset %r13, -24
    pushq %r12
    .cfi_offset %r12, -32
    pushq %r14
    .cfi_offset %r14, -40
    pushq %r15
    .cfi_offset %r15, -48
    pushq %rbx
    .cfi_offset %rbx, -56
    /* One spill slot for argv, which also realigns the stack for the calls. */
    subq $8, %rsp
    movq %rdi, %rbx
    movq %rsi, %r12
    movq %rdx, %r14
    movl %ecx, %r15d
    movq %r8, (%rsp)

    call PLT(bronze_tls_enter)
    movq %rax, %r13

    movq %r12, %rdi
    movq %r14, %rsi
    movl %r15d, %edx
    movq (%rsp), %rcx
    call *%rbx

    addq $8, %rsp
    popq %rbx
    popq %r15
    popq %r14
    popq %r12
    popq %r13
    popq %rbp
    .cfi_def_cfa %rsp, 8
    ret
    .cfi_endproc
#if !defined(__APPLE__)
    .size SYM(bronze_enter_js), .-SYM(bronze_enter_js)
#endif

#elif defined(__aarch64__)

/* x0 = code, x1 = env, x2 = this, w3 = argc, x4 = argv. */
    .text
    .globl SYM(bronze_enter_js)
#if !defined(__APPLE__)
    .type SYM(bronze_enter_js), %function
#endif
    .p2align 2
SYM(bronze_enter_js):
    .cfi_startproc
    stp x29, x30, [sp, #-64]!
    .cfi_def_cfa_offset 64
    .cfi_offset x29, -64
    .cfi_offset x30, -56
    mov x29, sp
    .cfi_def_cfa x29, 64
    stp x28, x19, [sp, #16]
    .cfi_offset x28, -48
    .cfi_offset x19, -40
    stp x20, x21, [sp, #32]
    .cfi_offset x20, -32
    .cfi_offset x21, -24
    stp x22, x23, [sp, #48]
    .cfi_offset x22, -16
    .cfi_offset x23, -8
    mov x19, x0
    mov x20, x1
    mov x21, x2
    mov w22, w3
    mov x23, x4

    bl SYM(bronze_tls_enter)
    mov x28, x0

    mov x0, x20
    mov x1, x21
    mov w2, w22
    mov x3, x23
    blr x19

    ldp x22, x23, [sp, #48]
    ldp x20, x21, [sp, #32]
    ldp x28, x19, [sp, #16]
    ldp x29, x30, [sp], #64
    .cfi_def_cfa sp, 0
    ret
    .cfi_endproc
#if !defined(__APPLE__)
    .size SYM(bronze_enter_js), .-SYM(bronze_enter_js)
#endif

#else
#error "bronze_enter_js: no trampoline for this architecture"
#endif

#if defined(__ELF__) && defined(__x86_64__)
    .section .note.GNU-stack, "", @progbits
#elif defined(__ELF__) && defined(__aarch64__)
    .section .note.GNU-stack, "", %progbits
#endif
