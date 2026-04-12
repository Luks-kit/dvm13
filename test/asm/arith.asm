; arith.asm — basic arithmetic
; result = (10 + 32) * 2 - 42 = 42

.data
result: dq 0

.text
    mov  ax, 10
    mov  bx, 32
    add  ax, bx         ; ax = 42
    mov  cx, 2
    mul  ax, cx         ; ax = 84
    mov  cx, 42
    sub  ax, cx         ; ax = 42

    mov  bx, result
    mov  [bx], ax
    halt
