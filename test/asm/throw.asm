; throw.asm — test error path via THROW
; result = 0xCAFE (error handler ran)
; if result = 0xBAD, ok path incorrectly ran

.data
result: dq 0

.text
    call thrower, on_err

    ; ok path — should NOT reach here
    mov  ax, 0xBAD
    mov  bx, result
    mov  [bx], ax
    halt

on_err:
    mov  ax, 0xCAFE
    mov  bx, result
    mov  [bx], ax
    halt

thrower:
    throw
