; [Stereo] PrepassRecordSkip (rec_state.cpp; run 18 P2, research\run18\p2.md sections 6 and 8; test only, off): the draw
; recorder's state and draw writers (22 slots of the shared vtable RecorderVtbl), each through its own thunk here, patched
; in once at start and only with the switch on. Off (rdrvr_rs_skip 0), or for any other recorder (rcx, the recorder, not
; the render thread's own, rdrvr_rs_rec: the DLSS deferred recorders share the vtable) or any other thread, a thunk jumps
; to the slot's original writer with every register as it came (rax and the flags aside: no writer takes rax). On, the
; writer is skipped: nothing recorded, a count, and eax = 0 (every one of the 22 writers returns void, p2.md section 8:
; nothing reads it). The thunks are leaf code in this module's .text (never written); their counters and the originals
; are in its .data (rec_state.cpp).

EXTERN rdrvr_rs_skip:BYTE
EXTERN rdrvr_rs_tid:DWORD
EXTERN rdrvr_rs_rec:QWORD
EXTERN rdrvr_rs_orig:QWORD
EXTERN rdrvr_rs_skips:QWORD

RSTHUNK MACRO n
    LOCAL check, pass
rdrvr_rs_thunk&n PROC
    cmp byte ptr [rdrvr_rs_skip], 0
    jne check
    jmp qword ptr [rdrvr_rs_orig + n * 8]
check:
    mov rax, qword ptr [rdrvr_rs_rec]
    cmp rcx, rax                                ; the render thread's recorder only
    jne pass
    mov eax, dword ptr gs:[48h]                 ; and the render thread only (TEB ClientId.UniqueThread)
    cmp eax, dword ptr [rdrvr_rs_tid]
    jne pass
    lock inc qword ptr [rdrvr_rs_skips + n * 8]
    xor eax, eax
    ret
pass:
    jmp qword ptr [rdrvr_rs_orig + n * 8]
rdrvr_rs_thunk&n ENDP
ENDM

.code
RSTHUNK 0
RSTHUNK 1
RSTHUNK 2
RSTHUNK 3
RSTHUNK 4
RSTHUNK 5
RSTHUNK 6
RSTHUNK 7
RSTHUNK 8
RSTHUNK 9
RSTHUNK 10
RSTHUNK 11
RSTHUNK 12
RSTHUNK 13
RSTHUNK 14
RSTHUNK 15
RSTHUNK 16
RSTHUNK 17
RSTHUNK 18
RSTHUNK 19
RSTHUNK 20
RSTHUNK 21

.const
PUBLIC rdrvr_rs_thunks
rdrvr_rs_thunks DQ rdrvr_rs_thunk0, rdrvr_rs_thunk1, rdrvr_rs_thunk2, rdrvr_rs_thunk3, rdrvr_rs_thunk4, rdrvr_rs_thunk5
                DQ rdrvr_rs_thunk6, rdrvr_rs_thunk7, rdrvr_rs_thunk8, rdrvr_rs_thunk9, rdrvr_rs_thunk10, rdrvr_rs_thunk11
                DQ rdrvr_rs_thunk12, rdrvr_rs_thunk13, rdrvr_rs_thunk14, rdrvr_rs_thunk15, rdrvr_rs_thunk16, rdrvr_rs_thunk17
                DQ rdrvr_rs_thunk18, rdrvr_rs_thunk19, rdrvr_rs_thunk20, rdrvr_rs_thunk21
END
