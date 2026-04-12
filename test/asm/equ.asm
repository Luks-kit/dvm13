; equ.asm — .equ constant definitions
; result = WIDTH * HEIGHT - BORDER = 80 * 24 - 2 = 1918

.equ WIDTH,  80
.equ HEIGHT, 24
.equ BORDER, 2

.data
result: dq 0

.text
    mov  ax, WIDTH
    mov  bx, HEIGHT
    mul  ax, bx          ; ax = 1920
    mov  bx, BORDER
    sub  ax, bx          ; ax = 1918
    mov  bx, result
    mov  [bx], ax
    halt
