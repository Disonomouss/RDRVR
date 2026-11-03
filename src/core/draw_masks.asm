; R3 [Compat] DrawMaskInit (dual_pass.cpp): entry stub for FUN_140706050. The function builds four bucket masks from
; stack slots it never initialises (three in its own frame, one in its caller's home slot for rcx): each is zeroed only
; when a TLS slot is null, then OR'd with its bucket bit, so on the render thread the drawn buckets depend on whatever an
; earlier call left there. The stub zeroes the four slots, so every call starts from what a fresh stack would hold (later
; blocks still accumulate onto the first block's bits, as in vanilla), then continues into the original (the hook's
; trampoline) on the same stack.
; Offsets from the entry rsp: the prologue pushes four registers and subtracts 748h, so the body's [rsp+54h], [rsp+58h]
; and [rsp+5Ch] are entry-714h, -710h and -70Ch, and [rbp+670h] (rbp = rsp+100h) is entry+8. The anchors check the
; prologue and the eight CMOVZ sites.

;
; [Stereo] PrepassWalkCut (prepass_arrays.cpp; test only, off): in pass 2 of a frame whose pass 1 drew the pre-pass for
; both eyes, the pre-pass call (the pass object, rcx, with +7Fh = 1; the sun cascades' and cube faces' calls have 0)
; returns at once on the render thread: eye 2's pre-pass is not walked or recorded (after optimumbox's ViSkipWalk, the
; single-pass-stereo fork). Off, rdrvr_prepass_cut stays 0 and the first compare falls through to the masks as before.
; The cut touches rax and the flags only, and returns 1; nothing reads it: the walk's call is the shadow object's vt+8
; (0x1409e9020), which tail-jumps here, back to 0x1405cc2d4, which writes rax before reading it (run 25, Ghidra).
; (Run 25, research\run25\item7.md 7) The footprints reach 0x706050 through the same pass object (renderer+0x1d0,
; +7Fh still 1) with +18h = renderer+0x868, set at 0x1405cc595 and cleared at 0x1405cc619; the walk's own call has
; +18h = 0 (the constructor's; no other writer). So the cut and the wrap also require +18h = 0.
;
; [Stereo] RecorderReemit (rec_state.cpp; run 18 P2, research\run18\p2.md; test only, off): in pass 2 the same call (the
; render thread, +7Fh = 1, not cut) goes to the C++ wrapper rdrvr_prepass_walk_wrap instead: it runs the walk through
; rdrvr_draw_masks_entry below (the masks as here), then re-emits the record side's cached state (and, under [Stereo]
; PrepassRecordSkip, skips the recorder's state and draw writers while the walk runs). The function takes rcx, rdx, r8 and
; r9d only (no stack or vector arguments: p2.md section 8). Off, rdrvr_prepass_wrap stays 0 and nothing changes.

EXTERN rdrvr_draw_masks_original:QWORD
EXTERN rdrvr_draw_masks_on:BYTE
EXTERN rdrvr_prepass_cut:BYTE
EXTERN rdrvr_prepass_cut_tid:DWORD
EXTERN rdrvr_prepass_cuts:QWORD
EXTERN rdrvr_prepass_wrap:BYTE
EXTERN rdrvr_prepass_wrap_tid:DWORD
EXTERN rdrvr_prepass_walk_wrap:PROC

.code
rdrvr_draw_masks_stub PROC
    cmp byte ptr [rdrvr_prepass_cut], 0
    jne cut_check
wrap_test:
    cmp byte ptr [rdrvr_prepass_wrap], 0
    jne wrap_check
masks:
    cmp byte ptr [rdrvr_draw_masks_on], 0
    je skip
    mov dword ptr [rsp - 714h], 0
    mov dword ptr [rsp - 710h], 0
    mov dword ptr [rsp - 70Ch], 0
    mov dword ptr [rsp + 8h], 0
skip:
    jmp qword ptr [rdrvr_draw_masks_original]
cut_check:
    mov eax, dword ptr gs:[48h]                 ; this thread's id (TEB ClientId.UniqueThread)
    cmp eax, dword ptr [rdrvr_prepass_cut_tid]
    jne wrap_test
    cmp byte ptr [rcx + 7Fh], 1                 ; the pre-pass's call only
    jne masks
    cmp qword ptr [rcx + 18h], 0                ; (run 25, item7.md 7) not the footprints' call (0x5cc595 sets +18h)
    jne masks
    lock inc qword ptr [rdrvr_prepass_cuts]
    mov eax, 1
    ret
wrap_check:
    mov eax, dword ptr gs:[48h]
    cmp eax, dword ptr [rdrvr_prepass_wrap_tid]
    jne masks
    cmp byte ptr [rcx + 7Fh], 1
    jne masks
    cmp qword ptr [rcx + 18h], 0                ; (run 25) not the footprints' call
    jne masks
    jmp rdrvr_prepass_walk_wrap                 ; (a tail jump: the game's four register arguments and its return address)
rdrvr_draw_masks_stub ENDP

; The wrapper's call of the walk: the masks zeroed against this entry's rsp (the original's, as above), then the original.
rdrvr_draw_masks_entry PROC
    cmp byte ptr [rdrvr_draw_masks_on], 0
    je entry_skip
    mov dword ptr [rsp - 714h], 0
    mov dword ptr [rsp - 710h], 0
    mov dword ptr [rsp - 70Ch], 0
    mov dword ptr [rsp + 8h], 0
entry_skip:
    jmp qword ptr [rdrvr_draw_masks_original]
rdrvr_draw_masks_entry ENDP
END
