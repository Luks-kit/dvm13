; fib.asm — fib(10) = 55 via call/ret
; result = 55

.data
result: dq 0

.text
    mov  ax, 10
    call fib, die
    mov  bx, result
    mov  [bx], ax
    halt

die:
    halt

; fib(n): n in ax, result in ax
; base: n<=1 → return n
fib:
    mov  bx, 1
    cmp  ax, bx
    jle  fib_base

    push ax              ; save n
    mov  bx, 1
    sub  ax, bx          ; ax = n-1
    call fib, die        ; ax = fib(n-1)
    push ax              ; save fib(n-1)

    pop  ex              ; ex = fib(n-1)
    pop  ax              ; ax = n
    mov  bx, 2
    sub  ax, bx          ; ax = n-2
    push ex              ; re-save fib(n-1)
    call fib, die        ; ax = fib(n-2)

    pop  bx              ; bx = fib(n-1)
    add  ax, bx          ; ax = fib(n-1) + fib(n-2)
    ret

fib_base:
    ret
