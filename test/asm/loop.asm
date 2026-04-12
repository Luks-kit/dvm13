; loop.asm — sum 1..10 via counted loop
; result = 55

.data
result: dq 0

.text
    mov  ax, 0          ; accumulator
    mov  bx, 1          ; counter
    mov  cx, 11         ; limit

loop_top:
    add  ax, bx
    mov  dx, 1
    add  bx, dx
    cmp  bx, cx
    jl   loop_top

    mov  bx, result
    mov  [bx], ax
    halt
