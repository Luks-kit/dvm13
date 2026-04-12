; float.asm — float arithmetic: (3.0 * 14.0) / 1.0 = 42
; result = 42  (via ftoi)

.data
result: dq 0

.text
    fmovi fp0, 3.0
    fmovi fp1, 14.0
    fmul  fp0, fp1      ; fp0 = 42.0

    fmovi fp2, 1.0
    fdiv  fp0, fp2      ; fp0 = 42.0

    ftoi  ax, fp0       ; ax = 42
    mov   bx, result
    mov   [bx], ax
    halt
