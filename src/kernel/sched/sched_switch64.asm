;
; sched_switch64.asm — переключение контекста задач (x86_64).
;
; Здесь определены три точки входа:
;   pit_handler_asm   — обработчик IRQ0 (таймер): тик + вытеснение;
;   sched_yield_asm   — обработчик INT 0x30: добровольная отдача процессора;
;   task_entry_stub   — трамплин первой активации новой задачи.
;
; Формат сохранённого фрейма 64-бит (снизу вверх по адресам):
;   R15..R8,RBP,RDI,RSI,RDX,RCX,RBX,RAX | RIP,CS,RFLAGS
;

[BITS 64]

global pit_handler_asm
global sched_yield_asm
global task_entry_stub

extern sched_timer_isr
extern sched_yield_isr
extern sched_task_exit

; --- Обработчик таймера (IRQ0) ---
pit_handler_asm:
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
    mov rdi, rsp                ; аргумент: указатель на сохранённый фрейм
    call sched_timer_isr        ; возвращает новый RSP
    mov rsp, rax
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

; --- Добровольная отдача процессора (INT 0x30) ---
sched_yield_asm:
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
    mov rdi, rsp
    call sched_yield_isr
    mov rsp, rax
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

; --- Трамплин первой активации задачи ---
; RDI указывает на task->tramp = { entry, arg }.
task_entry_stub:
    mov rax, [rdi]              ; entry
    mov rdi, [rdi + 8]          ; arg
    call rax                    ; entry(arg)
    call sched_task_exit        ; если entry вернулась — задача завершена
    hlt
