
.syntax unified
.cpu cortex-m4
.thumb
.section RESET,"a",%progbits
.global __Vectors
__Vectors:
.word __initial_sp
.word Reset_Handler
.section .text.Reset_Handler,"ax",%progbits
.global Reset_Handler
.type Reset_Handler,%function
.thumb_func
Reset_Handler:
b __main
.section .stack,"aw",%nobits
.balign 8
.space 32768
.global __initial_sp
__initial_sp:
.section .heap,"aw",%nobits
.balign 8
.global __heap_base
__heap_base:
.space 2048
.global __heap_limit
__heap_limit:
