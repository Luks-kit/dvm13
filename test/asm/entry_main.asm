; entry_main.asm — uses extern compute, exports _start as entry point
; _start: ax = compute(6) = 6^2+1 = 37
; result = 37

extern compute

.data
result: dq 0

.text
global _start
_start:
    mov  ax, 6
    call compute, die
    mov  bx, result
    mov  [bx], ax
    halt

die:
    halt
