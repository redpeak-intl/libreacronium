/**
 * @file sys_arch.h
 * @brief Типы системного слоя lwip (архитектура LibreAcronium).
 *
 * Реализация примитивов синхронизации (семафоры, мьютексы, очереди) построена
 * поверх простого вытесняющего планировщика задач ядра (см. sched/sched.h).
 * Задача, которой не хватает ресурса, переводится в состояние ожидания, а
 * сигнализирующая сторона будит её — активного ожидания (busy-wait) нет.
 */

#ifndef LWIP_ARCH_SYS_ARCH_H
#define LWIP_ARCH_SYS_ARCH_H

#include "sched/sched.h"

/* Уровень защиты критической секции: сохранённые флаги процессора. */
typedef unsigned long sys_prot_t;

/* --- Семафор --- */
struct sys_sem {
    volatile int valid;    /* Семафор создан */
    volatile int count;    /* Текущее значение счётчика */
    task_t      *waiter;   /* Задача, ожидающая семафор (или NULL) */
};
typedef struct sys_sem sys_sem_t;

/* --- Мьютекс --- */
struct sys_mutex {
    volatile int valid;
    volatile int locked;   /* 0 — свободен, 1 — занят */
    task_t      *owner;    /* Владелец (для отладки) */
    task_t      *waiter;   /* Ожидающая задача */
};
typedef struct sys_mutex sys_mutex_t;

/* --- Очередь сообщений (mailbox) --- */
#define SYS_MBOX_SIZE 64
struct sys_mbox {
    volatile int  valid;
    void         *msgs[SYS_MBOX_SIZE];
    volatile int  head;
    volatile int  tail;
    volatile int  count;
    task_t       *recv_waiter;   /* Задача, ждущая сообщение */
    task_t       *send_waiter;   /* Задача, ждущая место в очереди */
};
typedef struct sys_mbox sys_mbox_t;

/* lwip ожидает, что этот тип определён портом. */
typedef task_t *sys_thread_t;

#endif /* LWIP_ARCH_SYS_ARCH_H */
