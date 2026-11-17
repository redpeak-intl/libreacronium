;
; sched_switch.asm — переключение контекста задач (i686).
;
; Здесь определены три точки входа:
;   pit_handler_asm   — обработчик IRQ0 (таймер): тик + вытеснение;
;   sched_yield_asm   — обработчик INT 0x30: добровольная отдача процессора;
;   task_entry_stub   — трамплин первой активации новой задачи.
;
; Формат сохранённого фрейма 32-бит (снизу вверх по адресам):
;   EDI,ESI,EBP,ESP,EBX,EDX,ECX,EAX | EIP,CS,EFLAGS
;

[BITS 32]

global pit_handler_asm
global sched_yield_asm
global task_entry_stub

extern sched_timer_isr
extern sched_yield_isr
extern sched_task_exit

; --- Обработчик таймера (IRQ0) ---
pit_handler_asm:
    pushad                      ; сохраняем EAX..EDI
    cld
    mov eax, esp                ; аргумент: указатель на сохранённый фрейм
    push eax
    call sched_timer_isr        ; возвращает новый ESP (может быть тем же)
    add esp, 4
    mov esp, eax                ; переключаемся на стек выбранной задачи
    popad
    iret

; --- Добровольная отдача процессора (INT 0x30) ---
sched_yield_asm:
    pushad
    cld
    mov eax, esp
    push eax
    call sched_yield_isr
    add esp, 4
    mov esp, eax
    popad
    iret

; --- Трамплин первой активации задачи ---
; EDI указывает на task->tramp = { entry, arg }.
task_entry_stub:
    mov eax, [edi]              ; entry
    mov edx, [edi + 4]          ; arg
    push edx
    call eax                    ; entry(arg)
    call sched_task_exit        ; если entry вернулась — задача завершена
    hlt
