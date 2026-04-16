; trunc.asm
; Tests trunc8/trunc16/trunc32.
; result = (0xDEADBEEFCAFE1234 & 0xFF) = 0x34

.data
result: dq 0

.text
    mov  ax, 0xDEADBEEFCAFE1234
    trunc8  ax           ; ax = 0x34

    mov  bx, result
    mov  [bx], ax
    halt
