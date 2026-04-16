; entry_lib.asm — library that exports 'compute'
; compute: ax = ax * ax + 1  (square plus one)

.text
global compute
compute:
    mov  bx, ax
    mul  ax, bx     ; ax = ax * ax
    mov  bx, 1
    add  ax, bx     ; ax = ax^2 + 1
    ret
