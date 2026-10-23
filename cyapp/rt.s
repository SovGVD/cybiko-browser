; Runtime helpers for gcc 3.4 -ms on the H8S (its libgcc is H8/300 only),
; and the fixed-point divisions the game uses.
; Calling convention: arguments in er0, er1, er2; result in er0;
; er0-er3 may be changed, er4-er6 must be kept.
        .h8300s
        .section .text

; unsigned 32 / 32: quotient er0, remainder er2
        .global ___udivsi3
___udivsi3:
        mov.w   e1,e1
        bne     5f
        ; divisor fits in 16 bits: two divxu steps
        mov.w   e0,r2
        extu.l  er2
        divxu.w r1,er2
        mov.w   e2,e0
        divxu.w r1,er0
        mov.w   e0,r3                   ; remainder
        extu.l  er3
        mov.w   r2,e0
        mov.l   er3,er2
        rts
5:      sub.l   er2,er2
        mov.l   #32,er3
6:      shll.l  er0
        rotxl.l er2
        cmp.l   er1,er2
        blo     7f
        sub.l   er1,er2
        inc.l   #1,er0
7:      dec.l   #1,er3
        bne     6b
        rts

        .global ___umodsi3
___umodsi3:
        jsr     @___udivsi3
        mov.l   er2,er0
        rts

        .global ___divsi3
___divsi3:
        push.l  er4
        sub.l   er4,er4
        mov.l   er0,er0
        bpl     1f
        neg.l   er0
        not.b   r4l
1:      mov.l   er1,er1
        bpl     2f
        neg.l   er1
        not.b   r4l
2:      jsr     @___udivsi3
        mov.b   r4l,r4l
        beq     3f
        neg.l   er0
3:      pop.l   er4
        rts

        .global ___modsi3
___modsi3:
        push.l  er4
        sub.l   er4,er4
        mov.l   er0,er0
        bpl     1f
        neg.l   er0
        not.b   r4l
1:      mov.l   er1,er1
        bpl     2f
        neg.l   er1
2:      jsr     @___udivsi3
        mov.l   er2,er0
        mov.b   r4l,r4l
        beq     3f
        neg.l   er0
3:      pop.l   er4
        rts

; 32 x 32 -> low 32
        .global ___mulsi3
___mulsi3:
        mov.l   er0,er2
        mov.l   er1,er3
        mulxu.w r1,er0                  ; a_lo * b_lo
        mov.w   e3,r3
        mulxu.w r2,er3                  ; b_hi * a_lo
        mov.w   e2,r2
        mulxu.w r1,er2                  ; a_hi * b_lo
        add.w   r3,r2
        add.w   r2,e0
        rts

; void mset(void *d, u8 c, u32 n) and memset(d, c, n)
        .global _mset
        .global _memset
_memset:
_mset:
        mov.l   er0,er3
        mov.l   er2,er2
        beq     2f
1:      mov.b   r1l,@er3
        inc.l   #1,er3
        dec.l   #1,er2
        bne     1b
2:      rts

; void mcpy(void *d, const void *s, u32 n) and memcpy(d, s, n)
        .global _mcpy
        .global _memcpy
_memcpy:
_mcpy:
        mov.l   er0,er3
        mov.l   er2,er2
        beq     2f
        push.l  er4
1:      mov.b   @er1+,r4l
        mov.b   r4l,@er3
        inc.l   #1,er3
        dec.l   #1,er2
        bne     1b
        pop.l   er4
2:      rts

        .end
