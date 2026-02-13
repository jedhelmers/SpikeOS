.section .data
msg:
    .ascii "Howdy."
    .byte 0

.section .text
.global kprint_howdy
.type kprint_howdy, @function

kprint_howdy:
    movl $msg, %esi
    movl $0xB8000, %edi
    movb $0x0F, %bl

next_char:
    lodsb
    test %al, %al
    je done

    movb %al, (%edi)
    movb %bl, 1(%edi)
    addl $4, %edi
    jmp next_char

done:
    ret
