; alloca.asm
; Tests alloca: dynamically allocates N bytes on the stack,
; writes to the block, reads back.
; result = 0xABCD (written to alloca'd block and read back)

.data
result: dq 0

.text
    ; allocate 16 bytes — ax = ptr to block
    mov   ax, 16
    alloca

    ; write 0xABCD at offset 0 of the block
    mov   bx, 0xABCD
    mov   [ax], bx

    ; read it back via indexed load
    mov   cx, [ax+0]

    ; store result
    mov   bx, result
    mov   [bx], cx
    halt
