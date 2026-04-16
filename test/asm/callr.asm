; callr.asm
; Tests indirect call through a register (function pointer).
; result = double(21) = 42

.data
result: dq 0

.text
    ; load address of 'double_fn' into bx, call through it
    mov  bx, double_fn
    mov  ax, 21
    callr bx, die
    mov  cx, result
    mov  [cx], ax
    halt

die:
    halt

double_fn:
    mov  bx, ax
    add  ax, bx
    ret
