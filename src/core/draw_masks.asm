; R3 [Compat] DrawMaskInit (dual_pass.cpp): entry stub for FUN_140706050. The function builds four bucket masks from
; stack slots it never initialises (three in its own frame, one in its caller's home slot for rcx): each is zeroed only
; when a TLS slot is null, then OR'd with its bucket bit, so on the render thread the drawn buckets depend on whatever an
; earlier call left there. The stub zeroes the four slots, so every call starts from what a fresh stack would hold (later
; blocks still accumulate onto the first block's bits, as in vanilla), then continues into the original (the hook's
; trampoline) on the same stack.
; Offsets from the entry rsp: the prologue pushes four registers and subtracts 748h, so the body's [rsp+54h], [rsp+58h]
; and [rsp+5Ch] are entry-714h, -710h and -70Ch, and [rbp+670h] (rbp = rsp+100h) is entry+8. The anchors check the
; prologue and the eight CMOVZ sites.

EXTERN rdrvr_draw_masks_original:QWORD
EXTERN rdrvr_draw_masks_on:BYTE

.code
rdrvr_draw_masks_stub PROC
    cmp byte ptr [rdrvr_draw_masks_on], 0
    je skip
    mov dword ptr [rsp - 714h], 0
    mov dword ptr [rsp - 710h], 0
    mov dword ptr [rsp - 70Ch], 0
    mov dword ptr [rsp + 8h], 0
skip:
    jmp qword ptr [rdrvr_draw_masks_original]
rdrvr_draw_masks_stub ENDP
END
