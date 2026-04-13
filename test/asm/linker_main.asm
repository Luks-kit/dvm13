; linker_main.asm — uses extern symbols from math_lib.asm
; square(6) = 36, add_one(36) = 37  → result = 37

extern square
extern add_one

.data
result: dq 0

.text
global _start
_start:
    mov  ax, 6
    call square, die      ; ax = 36
    call add_one, die     ; ax = 37
    mov  bx, result
    mov  [bx], ax
    halt

die:
    halt
