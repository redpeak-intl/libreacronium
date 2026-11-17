/**
 * @file sys_arch.c
 * @brief Системный слой lwip поверх планировщика задач ядра.
 *
 * Реализованы все примитивы, которые lwip требует в режиме NO_SYS=0:
 * семафоры, мьютексы, очереди сообщений, потоки, защита критических секций
 * и функция времени. Ожидание ресурсов реализовано через блокировку задачи
 * (без активного ожидания).
 */

#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/err.h"

#include "sched/sched.h"
#include "drivers/pit.h"

/* ------------------------------------------------------------------ */
/* Вспомогательное ожидание                                           */
/* ------------------------------------------------------------------ */

/**
 * Приостановить текущую задачу. Вызывается при запрещённых прерываниях.
 * timeout == 0 — ждать бесконечно, иначе спать не более timeout мс.
 */
static void sys_wait(u32_t timeout) {
    task_t *cur = sched_current();
    if (timeout == 0) {
        cur->state = TASK_BLOCKED;
        sched_yield();
        return;
    }
    u32_t freq = pit_get_frequency();
    if (freq == 0) freq = 100;
    u32_t ticks = (timeout * freq + 999) / 1000;
    if (ticks == 0) ticks = 1;
    cur->wake_tick = pit_get_ticks() + ticks;
    cur->state = TASK_SLEEPING;
    sched_yield();
}

/* ------------------------------------------------------------------ */
/* Инициализация и время                                              */
/* ------------------------------------------------------------------ */

void sys_init(void) {
    /* Планировщик инициализируется ядром до запуска сети. */
}

u32_t sys_now(void) {
    return pit_get_time_ms();
}

u32_t sys_jiffies(void) {
    return pit_get_ticks();
}

/* sys_msleep() реализуется самим lwip (core/sys.c) поверх sys_sem_* API. */

u32_t lwip_arch_rand(void) {
    /* Простой линейный конгруэнтный генератор — годится для TCP ISN. */
    static uint32_t state = 0x12345678u;
    state = state * 1103515245u + 12345u;
    return state;
}

/* ------------------------------------------------------------------ */
/* Защита критических секций                                          */
/* ------------------------------------------------------------------ */

sys_prot_t sys_arch_protect(void) {
    return (sys_prot_t)sched_irq_save();
}

void sys_arch_unprotect(sys_prot_t pval) {
    sched_irq_restore((uint32_t)pval);
}

/* ------------------------------------------------------------------ */
/* Семафоры                                                           */
/* ------------------------------------------------------------------ */

err_t sys_sem_new(sys_sem_t *sem, u8_t count) {
    if (!sem) return ERR_ARG;
    sem->count = count;
    sem->waiter = 0;
    sem->valid = 1;
    return ERR_OK;
}

void sys_sem_free(sys_sem_t *sem) {
    if (!sem) return;
    sem->waiter = 0;
    sem->valid = 0;
}

void sys_sem_set_invalid(sys_sem_t *sem) {
    if (sem) sem->valid = 0;
}

int sys_sem_valid(sys_sem_t *sem) {
    return sem && sem->valid;
}

void sys_sem_signal(sys_sem_t *sem) {
    if (!sem) return;
    uint32_t flags = sched_irq_save();
    sem->count++;
    if (sem->waiter) {
        task_t *t = sem->waiter;
        sem->waiter = 0;
        sched_wake(t);
    }
    sched_irq_restore(flags);
}

u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout) {
    uint32_t start = sys_now();

    for (;;) {
        uint32_t flags = sched_irq_save();

        if (sem->count > 0) {
            sem->count--;
            sched_irq_restore(flags);
            return (u32_t)(sys_now() - start);
        }

        sem->waiter = sched_current();
        sys_wait(timeout);
        sem->waiter = 0;
        sched_irq_restore(flags);

        if (timeout != 0 && (u32_t)(sys_now() - start) >= timeout) {
            flags = sched_irq_save();
            if (sem->count > 0) {
                sem->count--;
                sched_irq_restore(flags);
                return (u32_t)(sys_now() - start);
            }
            sched_irq_restore(flags);
            return SYS_ARCH_TIMEOUT;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Мьютексы                                                           */
/* ------------------------------------------------------------------ */

err_t sys_mutex_new(sys_mutex_t *mutex) {
    if (!mutex) return ERR_ARG;
    mutex->locked = 0;
    mutex->owner = 0;
    mutex->waiter = 0;
    mutex->valid = 1;
    return ERR_OK;
}

void sys_mutex_free(sys_mutex_t *mutex) {
    if (mutex) mutex->valid = 0;
}

void sys_mutex_set_invalid(sys_mutex_t *mutex) {
    if (mutex) mutex->valid = 0;
}

int sys_mutex_valid(sys_mutex_t *mutex) {
    return mutex && mutex->valid;
}

void sys_mutex_lock(sys_mutex_t *mutex) {
    for (;;) {
        uint32_t flags = sched_irq_save();
        if (!mutex->locked) {
            mutex->locked = 1;
            mutex->owner = sched_current();
            sched_irq_restore(flags);
            return;
        }
        mutex->waiter = sched_current();
        sys_wait(0);
        mutex->waiter = 0;
        sched_irq_restore(flags);
    }
}

void sys_mutex_unlock(sys_mutex_t *mutex) {
    uint32_t flags = sched_irq_save();
    mutex->locked = 0;
    mutex->owner = 0;
    if (mutex->waiter) {
        task_t *t = mutex->waiter;
        mutex->waiter = 0;
        sched_wake(t);
    }
    sched_irq_restore(flags);
}

/* ------------------------------------------------------------------ */
/* Очереди сообщений                                                  */
/* ------------------------------------------------------------------ */

err_t sys_mbox_new(sys_mbox_t *mbox, int size) {
    (void)size; /* Используем фиксированный размер SYS_MBOX_SIZE. */
    if (!mbox) return ERR_ARG;
    for (int i = 0; i < SYS_MBOX_SIZE; i++) mbox->msgs[i] = 0;
    mbox->head = 0;
    mbox->tail = 0;
    mbox->count = 0;
    mbox->recv_waiter = 0;
    mbox->send_waiter = 0;
    mbox->valid = 1;
    return ERR_OK;
}

void sys_mbox_free(sys_mbox_t *mbox) {
    if (!mbox) return;
    mbox->recv_waiter = 0;
    mbox->send_waiter = 0;
    mbox->valid = 0;
}

int sys_mbox_valid(sys_mbox_t *mbox) {
    return mbox && mbox->valid;
}

void sys_mbox_set_invalid(sys_mbox_t *mbox) {
    if (mbox) mbox->valid = 0;
}

static void mbox_post_locked(sys_mbox_t *mbox, void *msg) {
    mbox->msgs[mbox->tail] = msg;
    mbox->tail = (mbox->tail + 1) % SYS_MBOX_SIZE;
    mbox->count++;
    if (mbox->recv_waiter) {
        task_t *t = mbox->recv_waiter;
        mbox->recv_waiter = 0;
        sched_wake(t);
    }
}

void sys_mbox_post(sys_mbox_t *mbox, void *msg) {
    if (!mbox) return;
    for (;;) {
        uint32_t flags = sched_irq_save();
        if (mbox->count < SYS_MBOX_SIZE) {
            mbox_post_locked(mbox, msg);
            sched_irq_restore(flags);
            return;
        }
        /* Очередь полна — ждём, пока получатель заберёт сообщение. */
        mbox->send_waiter = sched_current();
        sys_wait(0);
        mbox->send_waiter = 0;
        sched_irq_restore(flags);
    }
}

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg) {
    if (!mbox) return ERR_ARG;
    uint32_t flags = sched_irq_save();
    if (mbox->count >= SYS_MBOX_SIZE) {
        sched_irq_restore(flags);
        return ERR_MEM;
    }
    mbox_post_locked(mbox, msg);
    sched_irq_restore(flags);
    return ERR_OK;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *msg) {
    /* sched_irq_save/restore корректны и в контексте прерывания. */
    return sys_mbox_trypost(mbox, msg);
}

static void *mbox_pop_locked(sys_mbox_t *mbox) {
    void *m = mbox->msgs[mbox->head];
    mbox->head = (mbox->head + 1) % SYS_MBOX_SIZE;
    mbox->count--;
    if (mbox->send_waiter) {
        task_t *t = mbox->send_waiter;
        mbox->send_waiter = 0;
        sched_wake(t);
    }
    return m;
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout) {
    uint32_t start = sys_now();

    for (;;) {
        uint32_t flags = sched_irq_save();
        if (mbox->count > 0) {
            void *m = mbox_pop_locked(mbox);
            sched_irq_restore(flags);
            if (msg) *msg = m;
            return (u32_t)(sys_now() - start);
        }

        mbox->recv_waiter = sched_current();
        sys_wait(timeout);
        mbox->recv_waiter = 0;
        sched_irq_restore(flags);

        if (timeout != 0 && (u32_t)(sys_now() - start) >= timeout) {
            flags = sched_irq_save();
            if (mbox->count > 0) {
                void *m = mbox_pop_locked(mbox);
                sched_irq_restore(flags);
                if (msg) *msg = m;
                return (u32_t)(sys_now() - start);
            }
            sched_irq_restore(flags);
            if (msg) *msg = 0;
            return SYS_ARCH_TIMEOUT;
        }
    }
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg) {
    if (!mbox) {
        if (msg) *msg = 0;
        return SYS_MBOX_EMPTY;
    }
    uint32_t flags = sched_irq_save();
    if (mbox->count == 0) {
        sched_irq_restore(flags);
        if (msg) *msg = 0;
        return SYS_MBOX_EMPTY;
    }
    void *m = mbox_pop_locked(mbox);
    sched_irq_restore(flags);
    if (msg) *msg = m;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Потоки                                                             */
/* ------------------------------------------------------------------ */

sys_thread_t sys_thread_new(const char *name, lwip_thread_fn thread,
                            void *arg, int stacksize, int prio) {
    (void)prio;
    if (stacksize < 2048) stacksize = 2048;
    return sched_create(name, (void (*)(void *))thread, arg, (uint32_t)stacksize);
}
