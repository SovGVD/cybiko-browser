; cy_app_call(fn in er0, arg in er1): the emulator's CY_APP_OP, result in er0.
        .h8300s
        .section .text
        .global _cy_app_call
_cy_app_call:
        .word   0x0042
        rts
        .end
