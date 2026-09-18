; The runtime's one way into compiled code on Windows x64 (fn.h, rtEnterJs).
;
; Compiled code keeps the TLS block's address in R13 for as long as it runs
; (bronze_abi_tls.h, "the pinned register"). A compiled caller never touches
; the register, so it only has to be loaded at the boundary from C++ - and
; C++ is free to have used R13 for anything, so this saves it, loads the
; block, makes the call, and puts it back.
;
; uint64_t bronze_enter_js(bronze_fn_code code, uint64_t env, uint64_t thisv,
;                          uint32_t argc, const uint64_t* argv)
;
; rcx = code, rdx = env, r8 = this, r9d = argc, [entry rsp + 40] = argv.
;
; FRAME with the pushreg/allocstack directives makes MASM emit the .pdata and
; .xdata for this function, which is what lets RtlVirtualUnwind - the
; Error.stack walker - step through it from a JS frame to the C++ caller.

.code

EXTERN bronze_tls_enter: PROC

bronze_enter_js PROC FRAME
    ; Home the register arguments in the caller's shadow space, which is what
    ; the space is for; they are reloaded after the runtime call below.
    mov qword ptr [rsp + 8], rcx
    mov qword ptr [rsp + 16], rdx
    mov qword ptr [rsp + 24], r8
    mov qword ptr [rsp + 32], r9
    push r13
    .pushreg r13
    sub rsp, 32
    .allocstack 32
    .endprolog

    ; Entry rsp is now rsp + 40.
    call bronze_tls_enter
    mov r13, rax

    mov rax, qword ptr [rsp + 48]       ; code
    mov rcx, qword ptr [rsp + 56]       ; env
    mov rdx, qword ptr [rsp + 64]       ; this
    mov r8d, dword ptr [rsp + 72]       ; argc
    mov r9, qword ptr [rsp + 80]        ; argv
    call rax

    add rsp, 32
    pop r13
    ret
bronze_enter_js ENDP

END
