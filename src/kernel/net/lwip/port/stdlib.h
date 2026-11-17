/**
 * @file stdlib.h
 * @brief Минимальный freestanding-заголовок <stdlib.h> для ядра и lwip.
 *
 * Системный glibc-заголовок недоступен при -ffreestanding -m32. lwip включает
 * <stdlib.h> в core/netif.c и api/netdb.c (для atoi). Здесь объявлена только
 * функция atoi (реализована в sys_libc.c).
 */

#ifndef LWIP_PORT_STDLIB_H
#define LWIP_PORT_STDLIB_H

#ifdef __cplusplus
extern "C" {
#endif

int atoi(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* LWIP_PORT_STDLIB_H */
