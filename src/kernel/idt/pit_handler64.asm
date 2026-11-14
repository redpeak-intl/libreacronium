; Обработчик прерывания системного таймера PIT (IRQ0) — x86_64
; Вызывается при каждом тике таймера.

[BITS 64]

global pit_handler_asm

extern pit_handler

pit_handler_asm:
    ; Сохраняем регистры
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    cld

    ; Вызываем C-обработчик
    call pit_handler

    ; Восстанавливаем регистры
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; Возвращаемся из прерывания (64-битная версия iret)
    iretq
