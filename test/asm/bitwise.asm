; bitwise.asm
; result = (~0xFF00 & 0xFFFF) | 0x002A = 0x00FF | 0x002A = 0x00FF

.data
result: dq 0

.text
    mov  ax, 0xFF00
    not  ax             ; ax = 0xFFFFFFFFFFFF00FF
    mov  bx, 0xFFFF
    and  ax, bx         ; ax = 0x00FF
    mov  bx, 0x002A
    or   ax, bx         ; ax = 0x00FF (0x2A already covered)
    ; just verify: ax = 0x00FF
    mov  bx, result
    mov  [bx], ax
    halt
