; Hook in the vehicle part draw code, where a wheel's car-local matrix is read: wheel struct = rdi, matrix at
; [rdi+10..4F]. Replaces 1401F8EBA..1401F8EC5:
;     movups xmm4, [rdi+10]
;     lea    rax, [140000000h]          (rip-relative in the original)
; We set rdi = blendWheel(rdi) first: a private copy of the struct with the blended matrix (the draw code only
; reads [rdi+10..65] after this point, and reloads rdi later), then run both instructions.
; rsp alignment at the site is unknown, so it is aligned explicitly.
EXTERN blendWheel:PROC
EXTERN wheelReturn:QWORD

.code
wheelStub PROC
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
    mov rcx, rdi
    call blendWheel
    mov rdi, rax
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
    movups xmm4, [rdi+10h]
    mov rax, 140000000h
    jmp qword ptr [wheelReturn]
wheelStub ENDP
END
