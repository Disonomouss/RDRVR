; Run 7: the entry stub for the execution gate FUN_140adebe0 (aim.cpp, [Hands] BlockExecutions and run 7 item 1e's shot
; request readback). The gate is a leaf with no .pdata entry, and the shot request FUN_140d1f960 keeps a pointer in r9
; across the call (0x140d1fa28: mov rax, [r9+0AA8h], on the path the gate's true takes): the leaf never touches r9, so
; the game's compiler kept it there. A C++ hook may clobber every volatile register; with the readback's code in the
; hook it did, and the game crashed on that path (cycle M: a read at 0x18, RDR.exe+0xd1fa36). This stub saves the
; volatile registers (rcx, rdx, r8-r11, xmm0-xmm5) around the C++ part and passes it the game's return address, so the
; caller sees every register but rax as it left them.
; A FRAME procedure (run 7 item 6's review): its unwind data describes the six pushes and the one allocation, so a stack
; walk through it (the crash handler, the hang log, a crash report) is right. The allocation is made once in the prolog:
; the callee's home slots at rsp+00h..1Fh, the xmm saves at rsp+20h..7Fh, 8 bytes to keep rsp 0 mod 16.

EXTERN rdrvr_exec_gate_impl:PROC

.code
rdrvr_exec_gate_stub PROC FRAME
    ; entry: rsp = 8 mod 16 (the caller's return address on top)
    push rcx
    .pushreg rcx
    push rdx
    .pushreg rdx
    push r8
    .pushreg r8
    push r9
    .pushreg r9
    push r10
    .pushreg r10
    push r11                       ; 6 pushes: rsp = 8 - 48 = 8 mod 16
    .pushreg r11
    sub rsp, 88h                   ; home slots (20h) + 6 xmm (60h) + 8: rsp = 0 mod 16
    .allocstack 88h
    .endprolog
    movdqu xmmword ptr [rsp + 20h], xmm0
    movdqu xmmword ptr [rsp + 30h], xmm1
    movdqu xmmword ptr [rsp + 40h], xmm2
    movdqu xmmword ptr [rsp + 50h], xmm3
    movdqu xmmword ptr [rsp + 60h], xmm4
    movdqu xmmword ptr [rsp + 70h], xmm5
    mov rdx, qword ptr [rsp + 88h + 30h]   ; the game's return address (above the allocation and the 6 pushes)
    call rdrvr_exec_gate_impl      ; (phys in rcx, the return address in rdx) -> al
    movdqu xmm0, xmmword ptr [rsp + 20h]
    movdqu xmm1, xmmword ptr [rsp + 30h]
    movdqu xmm2, xmmword ptr [rsp + 40h]
    movdqu xmm3, xmmword ptr [rsp + 50h]
    movdqu xmm4, xmmword ptr [rsp + 60h]
    movdqu xmm5, xmmword ptr [rsp + 70h]
    add rsp, 88h
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    ret
rdrvr_exec_gate_stub ENDP
END
