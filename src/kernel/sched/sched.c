/**
 * @file sched.c
 * @brief Реализация простого вытесняющего планировщика задач.
 *
 * Планировщик round-robin: по каждому тику PIT (IRQ0) либо при добровольной
 * отдаче (INT 0x30) выбирается следующая готовая задача. Переключение
 * контекста выполняется в ассемблерной заглушке: ей передаётся текущий ESP,
 * она получает из C новый ESP (сохранённый фрейм другой задачи) и продолжает
 * восстановление регистров уже с чужого стека.
 *
 * Формат сохранённого фрейма (снизу вверх по адресам):
 *   32-бит: EDI,ESI,EBP,ESP,EBX,EDX,ECX,EAX | EIP,CS,EFLAGS
 *   64-бит: R15..R8,RBP,RDI,RSI,RDX,RCX,RBX,RAX | RIP,CS,RFLAGS
 */

#include "sched.h"
#include "../memory/memory.h"
#include "../drivers/pit.h"
#include "../video/video.h"

/* Внешние символы точек входа для новых задач (реализованы в asm-заглушках) */
extern void task_entry_stub(void);

/* Таблица задач и текущее состояние планировщика */
static task_t   tasks[SCHED_MAX_TASKS];
static int      current_id = 0;
static int      sched_enabled = 0;
static uint32_t task_counter = 0;

/* ------------------------------------------------------------------ */
/* Управление прерываниями                                            */
/* ------------------------------------------------------------------ */

uint32_t sched_irq_save(void) {
    uint32_t flags;
#ifdef __x86_64__
    uint64_t flags64;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags64) :: "memory");
    flags = (uint32_t)flags64;
#else
    __asm__ volatile("pushfl; popl %0" : "=r"(flags) :: "memory");
#endif
    __asm__ volatile("cli" ::: "memory");
    return flags;
}

void sched_irq_restore(uint32_t flags) {
#ifdef __x86_64__
    __asm__ volatile("pushq %0; popfq" :: "r"((uint64_t)flags) : "memory", "cc");
#else
    __asm__ volatile("pushl %0; popfl" :: "r"(flags) : "memory", "cc");
#endif
}

/* ------------------------------------------------------------------ */
/* Вспомогательные функции                                            */
/* ------------------------------------------------------------------ */

static void str_copy_n(char *dst, const char *src, uint32_t max) {
    uint32_t i = 0;
    if (!src) { dst[0] = '\0'; return; }
    while (src[i] != '\0' && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void zero_mem(void *p, uint32_t n) {
    uint8_t *b = (uint8_t *)p;
    for (uint32_t i = 0; i < n; i++) b[i] = 0;
}

/* ------------------------------------------------------------------ */
/* Инициализация                                                      */
/* ------------------------------------------------------------------ */

void sched_init(void) {
    zero_mem(tasks, sizeof(tasks));
    current_id = 0;
    sched_enabled = 0;
    task_counter = 0;

    /* Задача 0 — это текущий контекст (kmain). Она никогда не блокируется
       и служит задачей простоя (idle): когда нет готовых задач, планировщик
       возвращается в неё. */
    tasks[0].id = 0;
    str_copy_n(tasks[0].name, "idle", TASK_NAME_MAX);
    tasks[0].state = TASK_RUNNING;
    tasks[0].esp = 0;
    tasks[0].tramp[0] = 0;
    tasks[0].tramp[1] = 0;
}

void sched_start(void) {
    sched_enabled = 1;
}

/* ------------------------------------------------------------------ */
/* Создание задачи                                                    */
/* ------------------------------------------------------------------ */

task_t *sched_create(const char *name, void (*entry)(void *), void *arg,
                     uint32_t stack_size) {
    uint32_t flags = sched_irq_save();

    int slot = -1;
    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED) { slot = i; break; }
    }
    if (slot < 0) {
        sched_irq_restore(flags);
        return 0; /* Нет свободных слотов */
    }
    if (stack_size < 4096) stack_size = 4096;
    stack_size = (stack_size + 15) & ~15u;

    /* Стек выделяем из кучи ядра с запасом для выравнивания по 16 байт. */
    uint8_t *raw = (uint8_t *)kmalloc(stack_size + 32);
    if (!raw) {
        sched_irq_restore(flags);
        return 0;
    }
    uintptr_t base = ((uintptr_t)raw + 15) & ~(uintptr_t)15;
    uintptr_t top  = base + stack_size;

    task_t *t = &tasks[slot];
    t->id = (uint32_t)slot;
    str_copy_n(t->name, name ? name : "task", TASK_NAME_MAX);
    t->stack_base = (uint32_t)base;
    t->stack_size = stack_size;
    t->wake_tick = 0;
    t->qnext = 0;
    t->tramp[0] = (void *)entry;
    t->tramp[1] = arg;

    /* Формируем стартовый фрейм, чтобы первое переключение "вернулось"
       прямо в task_entry_stub как из прерывания. */
#ifdef __x86_64__
    /* В long mode iretq всегда снимает полный фрейм из пяти слотов:
       RIP, CS, RFLAGS, RSP, SS — и загружает RSP именно из слота RSP.
       Поэтому стартовый фрейм обязан содержать также SS и RSP, иначе
       iretq возьмёт нулевой RSP и первое же обращение к стеку упадёт. */
    uint64_t *sp = (uint64_t *)top;
    *--sp = 0x0010ULL;                         /* SS (селектор данных ядра) */
    *--sp = (uint64_t)(uintptr_t)top;          /* RSP (вершина стека задачи) */
    *--sp = 0x0000000000000202ULL;             /* RFLAGS (IF=1) */
    *--sp = 0x0008ULL;                         /* CS */
    *--sp = (uint64_t)(uintptr_t)task_entry_stub; /* RIP */
    *--sp = 0;   /* RAX */
    *--sp = 0;   /* RBX */
    *--sp = 0;   /* RCX */
    *--sp = 0;   /* RDX */
    *--sp = 0;   /* RSI */
    *--sp = (uint64_t)(uintptr_t)t->tramp;      /* RDI -> аргументы трамплина */
    *--sp = 0;   /* RBP */
    *--sp = 0;   /* R8  */
    *--sp = 0;   /* R9  */
    *--sp = 0;   /* R10 */
    *--sp = 0;   /* R11 */
    *--sp = 0;   /* R12 */
    *--sp = 0;   /* R13 */
    *--sp = 0;   /* R14 */
    *--sp = 0;   /* R15 */
    t->esp = (void *)sp;
#else
    uint32_t *sp = (uint32_t *)top;
    *--sp = 0x00000202;                        /* EFLAGS (IF=1) */
    *--sp = 0x08;                              /* CS */
    *--sp = (uint32_t)(uintptr_t)task_entry_stub; /* EIP */
    *--sp = 0;   /* EAX */
    *--sp = 0;   /* ECX */
    *--sp = 0;   /* EDX */
    *--sp = 0;   /* EBX */
    *--sp = 0;   /* ESP (игнорируется popad) */
    *--sp = 0;   /* EBP */
    *--sp = 0;   /* ESI */
    *--sp = (uint32_t)(uintptr_t)t->tramp;      /* EDI -> аргументы трамплина */
    t->esp = (void *)sp;
#endif

    t->state = TASK_READY;
    task_counter++;

    sched_irq_restore(flags);
    return t;
}

/* ------------------------------------------------------------------ */
/* Выбор следующей задачи                                             */
/* ------------------------------------------------------------------ */

static uintptr_t sched_schedule(uintptr_t esp) {
    task_t *cur = &tasks[current_id];

    if (esp != 0) {
        cur->esp = (void *)esp;
    }
    if (cur->state == TASK_RUNNING) {
        cur->state = TASK_READY;
    }

    int next = -1;
    for (int i = 1; i <= SCHED_MAX_TASKS; i++) {
        int k = (current_id + i) % SCHED_MAX_TASKS;
        if (tasks[k].state == TASK_READY) { next = k; break; }
    }

    if (next < 0) {
        /* Нет готовых задач — работаем в текущей или уходим в idle. */
        if (cur->state == TASK_READY) {
            cur->state = TASK_RUNNING;
            return (uintptr_t)cur->esp;
        }
        next = 0; /* idle */
        if (tasks[0].state == TASK_UNUSED || tasks[0].state == TASK_DEAD) {
            /* Аварийная ситуация: некуда переключаться. */
            for (;;) __asm__ volatile("hlt");
        }
    }

    current_id = next;
    tasks[next].state = TASK_RUNNING;
    return (uintptr_t)tasks[next].esp;
}

/* ------------------------------------------------------------------ */
/* Точки входа из ассемблера                                          */
/* ------------------------------------------------------------------ */

uintptr_t sched_timer_isr(uintptr_t esp) {
    pit_handler();          /* ticks++ и EOI в PIC */

    if (!sched_enabled) {
        return esp;
    }

    /* Пробуждаем задачи, у которых истёк таймер сна. */
    uint32_t now = pit_get_ticks();
    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        if (tasks[i].state == TASK_SLEEPING &&
            (int32_t)(now - tasks[i].wake_tick) >= 0) {
            tasks[i].state = TASK_READY;
        }
    }

    return sched_schedule(esp);
}

uintptr_t sched_yield_isr(uintptr_t esp) {
    if (!sched_enabled) {
        return esp;
    }
    return sched_schedule(esp);
}

/* ------------------------------------------------------------------ */
/* Блокировка / пробуждение / сон                                     */
/* ------------------------------------------------------------------ */

void sched_block(void) {
    task_t *cur = &tasks[current_id];
    cur->state = TASK_BLOCKED;
    sched_yield();
}

void sched_wake(task_t *t) {
    if (!t) return;
    if (t->state == TASK_BLOCKED || t->state == TASK_SLEEPING) {
        t->state = TASK_READY;
    }
}

void sched_sleep_ms(uint32_t ms) {
    uint32_t flags = sched_irq_save();
    uint32_t freq = pit_get_frequency();
    if (freq == 0) freq = 100;
    uint32_t ticks = (ms * freq) / 1000;
    if (ticks == 0) ticks = 1;

    task_t *cur = &tasks[current_id];
    cur->wake_tick = pit_get_ticks() + ticks;
    cur->state = TASK_SLEEPING;
    sched_yield();
    sched_irq_restore(flags);
}

void sched_yield(void) {
    __asm__ volatile("int $0x30" ::: "memory");
}

void sched_task_exit(void) {
    uint32_t flags = sched_irq_save();
    tasks[current_id].state = TASK_DEAD;
    sched_irq_restore(flags);
    /* Завершённая задача больше не должна выполняться. */
    for (;;) {
        sched_yield();
    }
}

/* ------------------------------------------------------------------ */
/* Информация                                                         */
/* ------------------------------------------------------------------ */

task_t *sched_current(void) {
    return &tasks[current_id];
}

uint32_t sched_ticks(void) {
    return pit_get_ticks();
}

void sched_dump(void) {
    print_string("Tasks:\n");
    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED) continue;
        print_string("  [");
        print_dec(i);
        print_string("] ");
        print_string(tasks[i].name);
        print_string(" state=");
        switch (tasks[i].state) {
            case TASK_READY:    print_string("READY");   break;
            case TASK_RUNNING:  print_string("RUNNING"); break;
            case TASK_BLOCKED:  print_string("BLOCKED"); break;
            case TASK_SLEEPING: print_string("SLEEP");   break;
            case TASK_DEAD:     print_string("DEAD");    break;
            default:            print_string("UNUSED");  break;
        }
        if (i == current_id) print_string(" *");
        print_string("\n");
    }
}
