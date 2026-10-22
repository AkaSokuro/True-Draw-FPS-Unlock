; Hook in the render-instance per-frame update (vtable entry 0 of the instance class, 1400CD040).
; Original code at 1400CD0D4..1400CD0FF copies the simulated matrix [rcx+A0..DF] into the drawn world matrix [rsi].
; We call blendInstance(obj = rcx, world = rsi) instead, then resume at 1400CD0FF.
; rsp is not 16-byte aligned at the hook site, so the stub aligns it explicitly (via rbp, restored after).
EXTERN blendInstance:PROC
EXTERN instReturn:QWORD

.code
instStub PROC
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
    sub rsp, 80h                ; 32 bytes of call shadow space + room to save 6 xmm registers
    movdqu [rsp+20h], xmm0
    movdqu [rsp+30h], xmm1
    movdqu [rsp+40h], xmm2
    movdqu [rsp+50h], xmm3
    movdqu [rsp+60h], xmm4
    movdqu [rsp+70h], xmm5
    mov rdx, rsi
    call blendInstance
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
    jmp qword ptr [instReturn]
instStub ENDP
END
