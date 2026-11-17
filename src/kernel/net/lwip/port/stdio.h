/**
 * @file stdio.h
 * @brief Минимальный freestanding-заголовок <stdio.h> для ядра и lwip.
 *
 * Системный glibc-заголовок недоступен при -ffreestanding -m32. lwip включает
 * <stdio.h> в core/mem.c (для snprintf при проверке целостности кучи), поэтому
 * здесь объявлена только функция snprintf.
 */

#ifndef LWIP_PORT_STDIO_H
#define LWIP_PORT_STDIO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int snprintf(char *buf, size_t size, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* LWIP_PORT_STDIO_H */
