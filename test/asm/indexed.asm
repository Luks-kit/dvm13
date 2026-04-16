; indexed.asm
; Tests [reg+offset] indexed memory access.
; Stores three u64 values into an array, reads them back via offsets.
; result = arr[0] + arr[1] + arr[2] = 10 + 20 + 30 = 60

.data
arr:    dq 0
        dq 0
        dq 0
result: dq 0

.text
    mov  ax, arr

    ; store arr[0]=10, arr[1]=20, arr[2]=30
    mov  bx, 10
    mov  [ax+0], bx
    mov  bx, 20
    mov  [ax+8], bx
    mov  bx, 30
    mov  [ax+16], bx

    ; load and sum
    mov  bx, [ax+0]
    mov  cx, [ax+8]
    add  bx, cx
    mov  cx, [ax+16]
    add  bx, cx          ; bx = 60

    mov  cx, result
    mov  [cx], bx
    halt
