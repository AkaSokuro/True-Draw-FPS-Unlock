; Hooks in the draw code for simple objects (props, cones, signs, ...). Four identical sites (PROP_COPIES in
; props.cpp) copy an object's world matrix [reg+20..5F] into a newly allocated draw packet (rax). The matrix is
; written once per simulation step, and reg is rbx or rdi depending on the site. After the copy, the original code
; sets up the next packet call (lea rcx,[1412DBA30h]; mov edx,8), which the stub repeats.
; Each stub calls blendProp(reg+20, rax) to write the blended matrix instead.
EXTERN blendProp:PROC
EXTERN propReturns:QWORD

PROP_STUB MACRO name, reg, slot
name PROC
    pushfq
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    push rbp
    mov rbp, rsp
    and rsp, -16
    sub rsp, 80h
    movdqu [rsp+20h], xmm0
    movdqu [rsp+30h], xmm1
    movdqu [rsp+40h], xmm2
    movdqu [rsp+50h], xmm3
    movdqu [rsp+60h], xmm4
    movdqu [rsp+70h], xmm5
    lea rcx, [reg+20h]
    mov rdx, rax
    call blendProp
    movdqu xmm0, [rsp+20h]
    movdqu xmm1, [rsp+30h]
    movdqu xmm2, [rsp+40h]
    movdqu xmm3, [rsp+50h]
    movdqu xmm4, [rsp+60h]
    movdqu xmm5, [rsp+70h]
    mov rsp, rbp
    pop rbp
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    pop rax
    popfq
    mov rcx, 1412DBA30h
    mov edx, 8
    jmp qword ptr [propReturns + slot*8]
name ENDP
ENDM

.code
PROP_STUB propStub0, rbx, 0
PROP_STUB propStub1, rbx, 1
PROP_STUB propStub2, rdi, 2
PROP_STUB propStub3, rdi, 3
END
