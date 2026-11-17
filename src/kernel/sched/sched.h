/**
 * @file sched.h
 * @brief Простой вытесняющий планировщик задач ядра (round-robin по таймеру).
 *
 * Планировщик даёт ядру потоки (задачи). Он используется сетевой подсистемой
 * (lwip в режиме NO_SYS=0 требует потоки, семафоры и очереди), а также для
 * фоновых задач (опрос сетевой карты).
 *
 * Особенности реализации:
 *  - вытеснение по тику PIT (IRQ0), переключение контекста прямо в обработчике;
 *  - добровольная отдача процессора через программное прерывание INT 0x30;
 *  - все задачи выполняются в ring0 и разделяют одно адресное пространство;
 *  - блокировка/пробуждение задач для примитивов синхронизации.
 */

#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>

#define SCHED_MAX_TASKS   16   /* Максимум задач в системе */
#define TASK_NAME_MAX     16   /* Максимальная длина имени задачи */

/* Состояния задачи */
typedef enum {
    TASK_UNUSED = 0,   /* Слот свободен */
    TASK_READY,        /* Готова к выполнению */
    TASK_RUNNING,      /* Выполняется прямо сейчас */
    TASK_BLOCKED,      /* Заблокирована (ждёт события) */
    TASK_SLEEPING,     /* Спит до wake_tick */
    TASK_DEAD          /* Завершилась */
} task_state_t;

/* Задача (поток ядра) */
typedef struct task {
    uint32_t         id;                    /* Индекс в таблице задач */
    char             name[TASK_NAME_MAX];   /* Имя (для отладки) */
    volatile task_state_t state;            /* Текущее состояние */
    void            *esp;                   /* Сохранённый указатель стека */
    uint32_t         stack_base;            /* Выровненная база стека (низ) */
    uint32_t         stack_size;            /* Размер стека в байтах */
    uint32_t         wake_tick;             /* Тик пробуждения (для SLEEPING) */
    struct task     *qnext;                 /* Связь в очереди ожидания */
    void            *tramp[2];              /* [0]=entry, [1]=arg для старта */
} task_t;

/* Инициализация подсистемы планирования. Текущий контекст становится
 * задачей 0 ("idle"). Вызывается до создания остальных задач. */
void sched_init(void);

/* Включить вытеснение (после создания всех стартовых задач). */
void sched_start(void);

/* Создать задачу. Возвращает задачу или NULL при ошибке. */
task_t *sched_create(const char *name, void (*entry)(void *), void *arg,
                     uint32_t stack_size);

/* Добровольно отдать процессор. */
void sched_yield(void);

/* Заблокировать текущую задачу до явного sched_wake(). */
void sched_block(void);

/* Разбудить задачу (перевести в READY, если она ждала). */
void sched_wake(task_t *t);

/* Спать не менее ms миллисекунд. */
void sched_sleep_ms(uint32_t ms);

/* Текущая задача. */
task_t *sched_current(void);

/* Количество тиков таймера с момента загрузки. */
uint32_t sched_ticks(void);

/* Вывести список задач. */
void sched_dump(void);

/* Прервать/восстановить прерывания, вернуть предыдущее состояние флагов. */
uint32_t sched_irq_save(void);
void sched_irq_restore(uint32_t flags);

/* Точки входа, вызываемые из ассемблерных заглушек (см. sched_switch*.asm). */
uintptr_t sched_timer_isr(uintptr_t esp);
uintptr_t sched_yield_isr(uintptr_t esp);

/* Завершение текущей задачи (вызывается из трамплина). */
void sched_task_exit(void);

#endif /* SCHED_H */
