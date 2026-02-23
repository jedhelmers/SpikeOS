/* crt0.S — C runtime startup for SpikeOS user programs.
   Links as the entry point. Calls main(), then _exit(). */

.section .text
.global _start

_start:
    call main

    /* Pass main's return value (in EAX) as exit status */
    mov %eax, %ebx       /* EBX = status */
    mov $0, %eax          /* SYS_EXIT = 0 */
    int $0x80

    /* Unreachable */
1:  hlt
    jmp 1b
