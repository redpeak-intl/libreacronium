/**
 * @file net.c
 * @brief Сетевая подсистема ядра LibreAcronium.
 *
 * Здесь сетевой стек lwip запускается в режиме NO_SYS=0: создаётся поток
 * tcpip_thread (через tcpip_init), регистрируется интерфейс eth0 поверх
 * драйвера e1000 и отдельная задача ядра опрашивает приёмное кольцо NIC.
 * Поверх netconn/raw API реализованы команды ifconfig, netstat, dhcp и ping.
 */

#include "lwip/opt.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "lwip/def.h"
#include "lwip/sys.h"
#include "lwip/raw.h"
#include "lwip/ip.h"
#include "lwip/prot/icmp.h"
#include "lwip/inet_chksum.h"
#include "netif/ethernet.h"

#include "net.h"
#include "ethernetif.h"
#include "drivers/e1000.h"
#include "drivers/pit.h"
#include "sched/sched.h"
#include "video/video.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Состояние                                                          */
/* ------------------------------------------------------------------ */

static struct netif  netif_eth0;
static volatile int  net_initialized = 0;
static volatile int  dhcp_active = 0;

/* ------------------------------------------------------------------ */
/* Задача опроса приёмного кольца                                     */
/* ------------------------------------------------------------------ */

static void net_rx_thread(void *arg) {
    (void)arg;
    for (;;) {
        ethernetif_poll(&netif_eth0);
        sched_sleep_ms(5);
    }
}

/* ------------------------------------------------------------------ */
/* Настройка интерфейса (выполняется в потоке tcpip_thread)            */
/* ------------------------------------------------------------------ */

static void netif_setup(void *arg) {
    (void)arg;

    ip4_addr_t ipaddr, netmask, gw;
    /* Стартовые адреса (используются до получения ответа DHCP). */
    IP4_ADDR(&ipaddr,  192, 168, 1, 128);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw,      192, 168, 1, 1);

    /* input = tcpip_input: приём обрабатывается в потоке tcpip_thread. */
    netif_add(&netif_eth0, &ipaddr, &netmask, &gw, NULL,
              ethernetif_init, tcpip_input);
    netif_set_default(&netif_eth0);
    netif_set_up(&netif_eth0);
    netif_set_link_up(&netif_eth0);

#if LWIP_DHCP
    if (dhcp_start(&netif_eth0) == ERR_OK) {
        dhcp_active = 1;
    }
#endif
}

/* ------------------------------------------------------------------ */
/* Инициализация                                                      */
/* ------------------------------------------------------------------ */

void net_init(void) {
    if (e1000_init() != 0) {
        print_string_color("net: e1000 не инициализирован, сеть отключена\n",
                           COLOR_RED, COLOR_BLACK);
        return;
    }

    /* Запускаем стек: lwip_init, tcpip_thread и колбэк настройки netif. */
    tcpip_init(netif_setup, NULL);

    /* Задача ядра, опрашивающая приёмное кольцо. */
    sched_create("netrx", net_rx_thread, NULL, 4096);

    net_initialized = 1;
}

/* ------------------------------------------------------------------ */
/* Вспомогательный вывод                                              */
/* ------------------------------------------------------------------ */

static void print_mac(const uint8_t mac[6]) {
    const char *hex = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        if (i) print_string(":");
        char s[3];
        s[0] = hex[(mac[i] >> 4) & 0xF];
        s[1] = hex[mac[i] & 0xF];
        s[2] = '\0';
        print_string(s);
    }
}

/* ------------------------------------------------------------------ */
/* ifconfig / netstat                                                 */
/* ------------------------------------------------------------------ */

void net_ifconfig(void) {
    if (!net_initialized) {
        print_string("eth0: сеть не инициализирована\n");
        return;
    }

    uint8_t mac[6];
    e1000_get_mac(mac);

    print_string("eth0: MAC ");
    print_mac(mac);
    print_string("\n");
    print_string("      link ");
    print_string(e1000_link_up() ? "UP" : "DOWN");
    print_string("\n");
    print_string("      inet ");
    print_string(ip4addr_ntoa(&netif_eth0.ip_addr));
    print_string("  mask ");
    print_string(ip4addr_ntoa(&netif_eth0.netmask));
    print_string("  gw ");
    print_string(ip4addr_ntoa(&netif_eth0.gw));
    print_string("\n");
#if LWIP_DHCP
    print_string("      dhcp ");
    if (dhcp_supplied_address(&netif_eth0)) {
        print_string("bound");
    } else if (dhcp_active) {
        print_string("running");
    } else {
        print_string("off");
    }
    print_string("\n");
#endif
}

void net_stat(void) {
    net_ifconfig();
    print_string("      rx=");
    print_dec((int)ethernetif_rx_packets());
    print_string(" packets  tx=");
    print_dec((int)ethernetif_tx_packets());
    print_string(" packets\n");
    sched_dump();
}

/* ------------------------------------------------------------------ */
/* DHCP                                                               */
/* ------------------------------------------------------------------ */

#if LWIP_DHCP
static void do_dhcp_renew(void *arg) {
    struct netif *n = (struct netif *)arg;
    if (dhcp_supplied_address(n)) {
        dhcp_renew(n);
    } else {
        dhcp_start(n);
    }
    dhcp_active = 1;
}
#endif

int net_dhcp_renew(void) {
#if LWIP_DHCP
    if (!net_initialized) return -1;
    tcpip_callback_wait(do_dhcp_renew, &netif_eth0);
    return 0;
#else
    return -1;
#endif
}

/* ------------------------------------------------------------------ */
/* ICMP ping (raw API, выполняется в контексте tcpip_thread)          */
/* ------------------------------------------------------------------ */

#if LWIP_RAW && LWIP_ICMP

#define PING_ID        0x4C41u   /* 'LA' */
#define PING_DATA_LEN  32
#define PING_TIMEOUT   1000u     /* мс */

struct ping_ctx {
    ip_addr_t        target;
    struct raw_pcb  *pcb;
    sys_sem_t        sem;
    u16_t            seq;
    volatile int     got_reply;
    volatile u16_t   reply_seq;
};

static struct ping_ctx ping;

/* Приём ответа: вызывается в потоке tcpip_thread. */
static u8_t ping_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p,
                      const ip_addr_t *addr) {
    struct ping_ctx *ctx = (struct ping_ctx *)arg;
    (void)pcb;
    (void)addr;

    /* raw_recv получает pbuf с IP-заголовком в начале. */
    if (p->len < (IP_HLEN + sizeof(struct icmp_echo_hdr))) {
        return 0;
    }
    struct icmp_echo_hdr *iecho =
        (struct icmp_echo_hdr *)((u8_t *)p->payload + IP_HLEN);

    if (ICMPH_TYPE(iecho) == ICMP_ER &&
        lwip_ntohs(iecho->id) == (u16_t)PING_ID &&
        lwip_ntohs(iecho->seqno) == ctx->seq) {
        ctx->reply_seq = ctx->seq;
        ctx->got_reply = 1;
        sys_sem_signal(&ctx->sem);
    }
    return 0; /* пакет не забираем */
}

static void ping_setup(void *arg) {
    struct ping_ctx *ctx = (struct ping_ctx *)arg;
    ctx->pcb = raw_new(IP_PROTO_ICMP);
    if (ctx->pcb != NULL) {
        raw_recv(ctx->pcb, ping_recv, ctx);
    }
}

static void ping_teardown(void *arg) {
    struct ping_ctx *ctx = (struct ping_ctx *)arg;
    if (ctx->pcb != NULL) {
        raw_remove(ctx->pcb);
        ctx->pcb = NULL;
    }
}

static void ping_send(void *arg) {
    struct ping_ctx *ctx = (struct ping_ctx *)arg;
    if (ctx->pcb == NULL) return;

    struct pbuf *p = pbuf_alloc(PBUF_IP,
                                (u16_t)(sizeof(struct icmp_echo_hdr) + PING_DATA_LEN),
                                PBUF_RAM);
    if (p == NULL) return;

    struct icmp_echo_hdr *iecho = (struct icmp_echo_hdr *)p->payload;
    ICMPH_TYPE_SET(iecho, ICMP_ECHO);
    ICMPH_CODE_SET(iecho, 0);
    iecho->chksum = 0;
    iecho->id     = lwip_htons((u16_t)PING_ID);
    iecho->seqno  = lwip_htons(ctx->seq);

    u8_t *data = (u8_t *)(iecho + 1);
    for (u16_t i = 0; i < PING_DATA_LEN; i++) {
        data[i] = (u8_t)i;
    }
    iecho->chksum = inet_chksum(iecho, p->tot_len);

    raw_sendto(ctx->pcb, p, &ctx->target);
    pbuf_free(p);
}

int net_ping(const char *ip, uint32_t count) {
    if (!net_initialized) {
        print_string("ping: сеть не инициализирована\n");
        return -1;
    }
    if (count == 0) count = 4;

    ip_addr_t target;
    if (!ipaddr_aton(ip, &target)) {
        print_string("ping: некорректный IP-адрес\n");
        return -1;
    }

    ping.pcb       = NULL;
    ping.got_reply = 0;
    ping.seq       = 0;
    ip_addr_copy(ping.target, target);
    sys_sem_new(&ping.sem, 0);

    tcpip_callback_wait(ping_setup, &ping);
    if (ping.pcb == NULL) {
        print_string("ping: не удалось создать ICMP-сокет\n");
        sys_sem_free(&ping.sem);
        return -1;
    }

    print_string("PING ");
    print_string(ip);
    print_string("\n");

    int replies = 0;
    for (uint32_t i = 0; i < count; i++) {
        ping.seq++;
        ping.got_reply = 0;

        tcpip_callback_wait(ping_send, &ping);
        u32_t t = sys_arch_sem_wait(&ping.sem, PING_TIMEOUT);

        if (t == SYS_ARCH_TIMEOUT || !ping.got_reply) {
            print_string("  seq=");
            print_dec((int)ping.seq);
            print_string(" timeout\n");
        } else {
            replies++;
            print_string("  reply seq=");
            print_dec((int)ping.reply_seq);
            print_string(" time=");
            print_dec((int)t);
            print_string(" ms\n");
        }
    }

    tcpip_callback_wait(ping_teardown, &ping);
    sys_sem_free(&ping.sem);

    print_string("ping: получено ");
    print_dec(replies);
    print_string(" из ");
    print_dec((int)count);
    print_string("\n");
    return replies;
}

#else /* !(LWIP_RAW && LWIP_ICMP) */

int net_ping(const char *ip, uint32_t count) {
    (void)ip;
    (void)count;
    print_string("ping: ICMP отключён в конфигурации lwip\n");
    return -1;
}

#endif
