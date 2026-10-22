; Hook in the character (driver) per-frame draw update, function around 14017EC00, rbp = character object.
; Same layout as render instances: [rbp+10] drawn world, [rbp+50] previous world, [rbp+A0] simulated matrix.
; Original code at 14017EC5D..14017EC89 copies [rbp+A0..DF] into [rbp+10..4F].
; We call blendInstance(obj = rbp, world = rbp+10) instead, then resume at 14017EC89 (which still needs rdx).
EXTERN blendInstance:PROC
EXTERN charReturn:QWORD

.code
charStub PROC
    pushfq
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    push rbp
    mov rcx, rbp                ; obj (before rbp is reused as the frame pointer)
    lea rdx, [rbp+10h]          ; world
    mov rbp, rsp
    and rsp, -16
    sub rsp, 80h
    movdqu [rsp+20h], xmm0
    movdqu [rsp+30h], xmm1
    movdqu [rsp+40h], xmm2
    movdqu [rsp+50h], xmm3
    movdqu [rsp+60h], xmm4
    movdqu [rsp+70h], xmm5
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
    jmp qword ptr [charReturn]
charStub ENDP
END
