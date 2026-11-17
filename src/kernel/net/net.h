/**
 * @file net.h
 * @brief Сетевая подсистема ядра LibreAcronium (lwip + e1000).
 */

#ifndef KERNEL_NET_NET_H
#define KERNEL_NET_NET_H

#include <stdint.h>

/* Инициализация сети: драйвер e1000, стек lwip (tcpip_thread), DHCP и
 * задача опроса приёмного кольца. Вызывается после sched_init(). */
void net_init(void);

/* Показать состояние интерфейса (команда ifconfig). */
void net_ifconfig(void);

/* Показать статистику интерфейса и задач (команда netstat). */
void net_stat(void);

/* Перезапустить DHCP-клиент. Возвращает 0 при успехе. */
int net_dhcp_renew(void);

/* Отправить ICMP echo на адрес ip (текст) count раз. Возвращает число
 * полученных ответов или -1 при ошибке. */
int net_ping(const char *ip, uint32_t count);

#endif /* KERNEL_NET_NET_H */
