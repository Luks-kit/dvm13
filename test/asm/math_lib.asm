; math_lib.asm — exported math functions
; square(ax) → ax = ax * ax
; add_one(ax) → ax = ax + 1

.text

global square
square:
    mov  bx, ax
    mul  ax, bx
    ret

global add_one
add_one:
    mov  bx, 1
    add  ax, bx
    ret
