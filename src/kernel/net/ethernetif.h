/**
 * @file ethernetif.h
 * @brief Сетевой интерфейс lwip поверх драйвера Intel e1000.
 */

#ifndef KERNEL_NET_ETHERNETIF_H
#define KERNEL_NET_ETHERNETIF_H

#include "lwip/netif.h"

/* Инициализация netif: MAC, вывод, низкоуровневая передача. */
err_t ethernetif_init(struct netif *netif);

/* Опрос приёмного кольца e1000: принять и передать в стек все доступные
 * кадры. Вызывается из отдельной задачи ядра. */
void ethernetif_poll(struct netif *netif);

/* Счётчики для netstat. */
uint32_t ethernetif_rx_packets(void);
uint32_t ethernetif_tx_packets(void);

#endif /* KERNEL_NET_ETHERNETIF_H */
