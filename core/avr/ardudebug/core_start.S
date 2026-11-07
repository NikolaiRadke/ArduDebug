/* ArduDebug: marks the start of the core's code. platform.txt links this object
   between the user's code (sketch, libraries) and the core archive, so with
   -ffunction-sections everything below this address is the user's code.
   The stub uses it to stop a pause in the sketch (see avr8-stub.c). In normal
   builds nothing refers to it, and --gc-sections removes it. */
	.section .text.ardudebug_core_start,"ax",@progbits
	.global __ardudebug_core_start
__ardudebug_core_start:
	nop
