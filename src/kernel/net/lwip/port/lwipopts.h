/**
 * @file lwipopts.h
 * @brief Конфигурация lwip для ядра LibreAcronium.
 *
 * Выбран режим NO_SYS=0: lwip работает в многозадачном режиме с отдельным
 * потоком tcpip_thread и API netconn (см. sys_arch.c). Сетевой интерфейс —
 * Ethernet (Intel e1000), адресация IPv4, клиент DHCP, DNS и ICMP.
 */

#ifndef LWIP_HDR_LWIPOPTS_H
#define LWIP_HDR_LWIPOPTS_H

/* ---------- Режим работы ---------- */

#define NO_SYS                          0
#define SYS_LIGHTWEIGHT_PROT            1
#define LWIP_NETCONN                    1
#define LWIP_SOCKET                     0

/* ---------- Память ---------- */

#define MEM_LIBC_MALLOC                 0
#define MEMP_MEM_MALLOC                 0
#define MEM_ALIGNMENT                   8
#define MEM_SIZE                        (192 * 1024)

#define MEMP_NUM_PBUF                   32
#define MEMP_NUM_RAW_PCB                8
#define MEMP_NUM_UDP_PCB                8
#define MEMP_NUM_TCP_PCB                16
#define MEMP_NUM_TCP_PCB_LISTEN         8
#define MEMP_NUM_TCP_SEG                48
#define MEMP_NUM_TCPIP_MSG_API          16
#define MEMP_NUM_TCPIP_MSG_INPKT        32
#define MEMP_NUM_SYS_TIMEOUT            16

#define PBUF_POOL_SIZE                  48
#define PBUF_POOL_BUFSIZE               1600

/* ---------- Протоколы ---------- */

#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ICMP                       1
#define LWIP_RAW                        1
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_DNS                        1
#define LWIP_DHCP                       1
#define LWIP_DHCP_DOES_ACD_CHECK        0
#define LWIP_AUTOIP                     0
#define LWIP_IGMP                       0
#define LWIP_RIP                        0

#define LWIP_NETIF_HOSTNAME             1
#define LWIP_NETIF_API                  1
#define LWIP_NETIF_STATUS_CALLBACK      0
#define LWIP_NETIF_LINK_CALLBACK        0
#define LWIP_HAVE_LOOPIF                0
#define LWIP_NETIF_TX_SINGLE_PBUF       1

/* ---------- Потоки ---------- */

#define TCPIP_THREAD_NAME               "tcpip"
#define TCPIP_THREAD_STACKSIZE          4096
#define TCPIP_THREAD_PRIO               1
#define TCPIP_MBOX_SIZE                 32
#define DEFAULT_THREAD_STACKSIZE        4096
#define DEFAULT_THREAD_PRIO             1
#define DEFAULT_UDP_RECVMBOX_SIZE       16
#define DEFAULT_TCP_RECVMBOX_SIZE       16
#define DEFAULT_ACCEPTMBOX_SIZE         8

#define LWIP_TCPIP_CORE_LOCKING         0

/* ---------- Прочее ---------- */

#define LWIP_STATS                      0
#define LWIP_STATS_DISPLAY              0
#define LWIP_DEBUG                      0
#define LWIP_CHECKSUM_ON_COPY           0
#define LWIP_CHKSUM_ALGORITHM           3
#define LWIP_SO_RCVTIMEO                1
#define LWIP_SO_SNDTIMEO                1
#define LWIP_TCP_KEEPALIVE              0
#define LWIP_COMPAT_MUTEX               0
#define LWIP_TIMERS                     1

#endif /* LWIP_HDR_LWIPOPTS_H */
