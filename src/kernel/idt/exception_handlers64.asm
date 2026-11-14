;
; Файл: exception_handlers64.asm (x86_64)
; Описание: Обработчики-заглушки для первых 32 исключений процессора в long mode.
;
; Форкуется стандартный стековый фрейм, соответствующий структуре registers_t
; из exceptions.h (64-битный вариант):
;
;   [rsp+ 0]  r15      [rsp+ 8]  r14      [rsp+16] r13 ...
;   ...                                    [rsp+..] rax
;   [rsp+..]  int_no   [rsp+..] err_code
;   [rsp+..]  rip      cs         rflags    (rsp, ss — только при смене кольца)
;

[BITS 64]

extern exception_handler

; Макрос для исключения БЕЗ кода ошибки — кладём фиктивный код ошибки.
%macro ISR_NO_ERR_CODE 1
[GLOBAL isr%1]
isr%1:
    push 0          ; фиктивный код ошибки (8 байт)
    push %1         ; номер прерывания
    jmp common_isr_stub
%endmacro

; Макрос для исключения С кодом ошибки — код ошибки уже в стеке.
%macro ISR_ERR_CODE 1
[GLOBAL isr%1]
isr%1:
    push %1         ; номер прерывания
    jmp common_isr_stub
%endmacro

;
; Заглушки для всех 32 исключений
;
ISR_NO_ERR_CODE 0
ISR_NO_ERR_CODE 1
ISR_NO_ERR_CODE 2
ISR_NO_ERR_CODE 3
ISR_NO_ERR_CODE 4
ISR_NO_ERR_CODE 5
ISR_NO_ERR_CODE 6
ISR_NO_ERR_CODE 7
ISR_ERR_CODE    8
ISR_NO_ERR_CODE 9
ISR_ERR_CODE    10
ISR_ERR_CODE    11
ISR_ERR_CODE    12
ISR_ERR_CODE    13
ISR_ERR_CODE    14
ISR_NO_ERR_CODE 15
ISR_NO_ERR_CODE 16
ISR_ERR_CODE    17
ISR_NO_ERR_CODE 18
ISR_NO_ERR_CODE 19
ISR_NO_ERR_CODE 20
ISR_NO_ERR_CODE 21
ISR_NO_ERR_CODE 22
ISR_NO_ERR_CODE 23
ISR_NO_ERR_CODE 24
ISR_NO_ERR_CODE 25
ISR_NO_ERR_CODE 26
ISR_NO_ERR_CODE 27
ISR_NO_ERR_CODE 28
ISR_NO_ERR_CODE 29
ISR_NO_ERR_CODE 30
ISR_NO_ERR_CODE 31

;
; Общая заглушка
;
common_isr_stub:
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

    ; Загружаем сегмент данных ядра
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    cld

    ; rdi = указатель на registers_t (вершина стека)
    mov rdi, rsp
    call exception_handler

    ; Восстанавливаем регистры (на случай, если обработчик вернётся)
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

    add rsp, 16     ; убираем int_no и err_code
    iretq
