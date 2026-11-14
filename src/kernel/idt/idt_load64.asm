;;
;; @file idt_load64.asm  (x86_64)
;; @brief Ассемблерные функции для работы с IDT в long mode.
;;
;; Содержит низкоуровневые функции для загрузки IDT
;; и обработки прерываний клавиатуры (64-битные версии).
;;

[BITS 64]

global load_idt
global keyboard_handler

extern keyboard_handler_main

;;
;; @brief Загружает IDT и включает прерывания
;; @param rdi Указатель на 10-байтовый описатель IDT (limit + 64-битный base)
;;
load_idt:
    lidt [rdi]
    sti
    ret

;;
;; @brief Обработчик прерывания клавиатуры
;;
;; Сохраняет регистры общего назначения, вызывает C-обработчик,
;; затем восстанавливает регистры и возвращается через iretq.
;;
keyboard_handler:
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
    call keyboard_handler_main
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
    iretq
