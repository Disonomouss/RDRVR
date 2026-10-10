; [Render] PlaybackBatch (playback.cpp): the game's playback loop (ReplayChunk FUN_140ecdd30) reads, per packet, the
; chunk's count (+10h), decrements it with a lock and reads the chunk's data pointer (+18h): the same cache line the
; render thread's recorder writes per packet (its count increment and its reserve's used size). This replaces the loop
; head at 0x140ecdd61 (through the hook's relay): rdrvr_rb_next(chunk = rbp, &read offset = r14) returns the next
; packet's address once it is counted and claimed (in batches, or one at a time as the original), and the stub enters
; the original dispatch at 0x140ecdd8a with rdi = the packet and eax = its opcode, as the original head leaves them.
; The head's own call (SwitchToThread at 0x140ecdd70) already makes every volatile register dead across it, and the
; loop's rsp is the function body's (16-byte aligned, with its callees' home space): the call below is the same.

EXTERN rdrvr_rb_next:PROC
EXTERN rdrvr_rb_dispatch:QWORD

.code
rdrvr_playback_head PROC
    mov rcx, rbp
    mov rdx, r14
    call rdrvr_rb_next
    mov rdi, rax
    mov eax, dword ptr [rdi]
    jmp qword ptr [rdrvr_rb_dispatch]
rdrvr_playback_head ENDP
END
