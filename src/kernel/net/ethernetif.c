/**
 * @file ethernetif.c
 * @brief Сетевой интерфейс lwip поверх драйвера Intel e1000 (LibreAcronium).
 *
 * netif работает в режиме опроса: аппаратные прерывания NIC не используются.
 * Передача идёт через e1000_transmit(), приём — через e1000_receive() из
 * отдельной задачи ядра (см. net.c).
 */

#include "lwip/opt.h"
#include "lwip/def.h"
#include "lwip/err.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"

#include "ethernetif.h"
#include "drivers/e1000.h"

#include <string.h>

#define IFNAME0        'e'
#define IFNAME1        'n'
#define ETH_FRAME_MAX  1600

/* Буферы подготовки кадра. Один netif — можно держать статически. */
static uint8_t tx_stage[ETH_FRAME_MAX];
static uint8_t rx_stage[ETH_FRAME_MAX];

static uint32_t rx_packets = 0;
static uint32_t tx_packets = 0;

/* ------------------------------------------------------------------ */
/* Низкоуровневая передача                                            */
/* ------------------------------------------------------------------ */

static err_t low_level_output(struct netif *netif, struct pbuf *p) {
    (void)netif;

    if (p->tot_len > ETH_FRAME_MAX) {
        return ERR_MEM;
    }

    /* Собираем цепочку pbuf в непрерывный буфер. */
    uint32_t off = 0;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        memcpy(&tx_stage[off], q->payload, q->len);
        off += q->len;
    }

    if (e1000_transmit(tx_stage, off) != 0) {
        return ERR_IF;
    }
    tx_packets++;
    return ERR_OK;
}

/* ------------------------------------------------------------------ */
/* Инициализация интерфейса                                           */
/* ------------------------------------------------------------------ */

err_t ethernetif_init(struct netif *netif) {
    netif->name[0] = IFNAME0;
    netif->name[1] = IFNAME1;

    /* ARP + Ethernet: вывод кадров через ARP-разрешение адресов. */
    netif->output     = etharp_output;
    netif->linkoutput = low_level_output;

    netif->mtu        = 1500;
    netif->hwaddr_len = 6;
    netif->flags      = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
                        NETIF_FLAG_ETHERNET | NETIF_FLAG_LINK_UP;

    e1000_get_mac(netif->hwaddr);
    return ERR_OK;
}

/* ------------------------------------------------------------------ */
/* Приём                                                              */
/* ------------------------------------------------------------------ */

void ethernetif_poll(struct netif *netif) {
    /* Ограничиваем число кадров за один проход, чтобы не «зависнуть». */
    for (int guard = 0; guard < 64; guard++) {
        uint32_t len = ETH_FRAME_MAX;
        if (e1000_receive(rx_stage, &len) == 0) {
            return; /* кадров больше нет */
        }

        struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
        if (p == NULL) {
            continue;
        }
        if (pbuf_take(p, rx_stage, (u16_t)len) != ERR_OK) {
            pbuf_free(p);
            continue;
        }

        rx_packets++;
        /* netif->input == tcpip_input: кадр будет обработан в tcpip_thread. */
        if (netif->input(p, netif) != ERR_OK) {
            pbuf_free(p);
        }
    }
}

uint32_t ethernetif_rx_packets(void) {
    return rx_packets;
}

uint32_t ethernetif_tx_packets(void) {
    return tx_packets;
}
