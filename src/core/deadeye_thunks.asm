; Run 28 item 1c (deadeye.cpp; after optimumbox's fork, v0.8.11.9, commit ac942427, MIT): the entry thunks of three game
; functions the Dead Eye marks' UI (DeadeyeMarksUi FUN_140394300, and the duel's FUN_140d07680) calls, so the X can be
; shown and placed for the headset. Each is reached by the inline hook on its function and does nothing to any other
; caller: unless the thread is the one deadeye.cpp's marks hook let in (rdrvr_de_tid, 0 when none) AND the return
; address is one of the call sites read in research\RDR.exe (the anchors DeadeyeVisRet, DeadeyePosRet(2),
; DeadeyeUiRet(2)), the thunk jumps to the function's original code with every register as it came (rax and the flags
; aside: none of the three reads rax, and every caller takes the flags as clobbered by a call).
; - ViewportPointVisible (0x1335d0) is a LEAF: its callers keep r9-r11 live across it (0x1403945c2: r9 the element's
;   vtable, then call [r9+20h]; 0x14024eb1a: r9b, r10; 0x140361a11/0x140361a8e: r9-r11 loop counters). The leaf-hook
;   rule (run 7's crash): no C++ on that path at all. Let in, the thunk answers itself and returns: al =
;   rdrvr_de_vis_answer (1 shown; 0 hidden, deadeye.cpp's hide pass that clears X left in their slots). Only rax and
;   the flags change, as the function's own code changes them.
; - MarkWorldPos (0x3dcc50) and ScreenToUi (0x6c59f0) are not leaves, but a caller of a function whose whole call tree
;   leaves a volatile register alone may keep it live (that is what the leaf case was), so they get the same gate.
;   Let in, the thunk jumps (not calls) to the C++ part, an ordinary function the call sites see as the original: at
;   those sites nothing volatile is live across the call (checked: 0x140394597, 0x140394355, 0x1403947fd, 0x140d076db).
; The thunks touch no stack (a leaf without unwind data is unwound by its return address at [rsp]).

EXTERN rdrvr_de_tid:DWORD
EXTERN rdrvr_de_vis_ret:QWORD
EXTERN rdrvr_de_vis_orig:QWORD
EXTERN rdrvr_de_vis_hits:QWORD
EXTERN rdrvr_de_vis_answer:BYTE
EXTERN rdrvr_de_pos_ret:QWORD
EXTERN rdrvr_de_pos_orig:QWORD
EXTERN rdrvr_de_ui_ret:QWORD
EXTERN rdrvr_de_ui_orig:QWORD
EXTERN rdrvr_de_pos_impl:PROC
EXTERN rdrvr_de_ui_impl:PROC

.code
rdrvr_de_vis_thunk PROC
    mov eax, dword ptr gs:[48h]                 ; this thread (TEB ClientId.UniqueThread)
    cmp eax, dword ptr [rdrvr_de_tid]
    jne to_orig
    mov rax, qword ptr [rsp]                    ; the game's return address
    cmp rax, qword ptr [rdrvr_de_vis_ret]
    jne to_orig
    lock inc qword ptr [rdrvr_de_vis_hits]
    movzx eax, byte ptr [rdrvr_de_vis_answer]   ; 1 shown wherever the camera looks; 0 hidden (a hide pass)
    ret
to_orig:
    jmp qword ptr [rdrvr_de_vis_orig]
rdrvr_de_vis_thunk ENDP

rdrvr_de_pos_thunk PROC
    mov eax, dword ptr gs:[48h]
    cmp eax, dword ptr [rdrvr_de_tid]
    jne to_orig
    mov rax, qword ptr [rsp]
    cmp rax, qword ptr [rdrvr_de_pos_ret]
    je let_in
    cmp rax, qword ptr [rdrvr_de_pos_ret + 8]
    jne to_orig
let_in:
    jmp rdrvr_de_pos_impl
to_orig:
    jmp qword ptr [rdrvr_de_pos_orig]
rdrvr_de_pos_thunk ENDP

rdrvr_de_ui_thunk PROC
    mov eax, dword ptr gs:[48h]
    cmp eax, dword ptr [rdrvr_de_tid]
    jne to_orig
    mov rax, qword ptr [rsp]
    cmp rax, qword ptr [rdrvr_de_ui_ret]
    je let_in
    cmp rax, qword ptr [rdrvr_de_ui_ret + 8]
    jne to_orig
let_in:
    jmp rdrvr_de_ui_impl
to_orig:
    jmp qword ptr [rdrvr_de_ui_orig]
rdrvr_de_ui_thunk ENDP
END
