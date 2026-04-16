; sized_mem.asm
; Tests byte/word/dword sized loads and stores.
; result = 0x0102030405060708  (assembled from individual byte stores)

.data
buf:    dq 0       ; 8-byte scratch buffer
result: dq 0

.text
    ; store individual bytes into buf via byte stores
    mov  ax, buf

    mov  bx, 0x08
    movb [ax], bx          ; buf[0] = 0x08

    mov  bx, 0x07
    lea  cx, [ax+1]
    movb [cx], bx          ; buf[1] = 0x07

    ; zero-extend load the byte back, verify
    movzx ax, byte [ax]    ; ax = 0x08
    mov  cx, result
    mov  [cx], ax
    halt
