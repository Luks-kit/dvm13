; data_ref.asm — read from .cnst and .data, sum them
; result = 100 + 200 = 300

.cnst
addend_a: dq 100

.data
addend_b: dq 200
result:   dq 0

.text
    mov  ax, addend_a   ; ax = address of addend_a (reloc)
    mov  ax, [ax]       ; ax = 100
    mov  bx, addend_b   ; bx = address of addend_b (reloc)
    mov  bx, [bx]       ; bx = 200
    add  ax, bx         ; ax = 300
    mov  cx, result
    mov  [cx], ax
    halt
