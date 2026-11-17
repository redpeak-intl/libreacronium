/**
 * @file cc.h
 * @brief Архитектурный слой lwip для LibreAcronium (архитектура компилятора).
 *
 * Здесь описываются зависимости от платформы/компилятора, которые lwip
 * ожидает увидеть в "arch/cc.h". Ядро собрано с -ffreestanding -nostdlib,
 * поэтому стандартная библиотека C недоступна и часть функций/заголовков
 * приходится объявлять самостоятельно.
 */

#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* В ядре нет <errno.h> — пусть lwip определит errno сам (LWIP_PROVIDE_ERRNO). */
#define LWIP_PROVIDE_ERRNO 1

/* Доступные заголовки: только компиляторные (freestanding). */
#define LWIP_NO_INTTYPES_H 1
#define LWIP_NO_CTYPE_H    1
#define LWIP_NO_UNISTD_H   1
/* <limits.h> из системного glibc недоступен при -m32, поэтому определяем
 * нужные lwip пределы самостоятельно (см. комментарий в lwip/arch.h). */
#define LWIP_NO_LIMITS_H   1
#define CHAR_BIT  8
#define SCHAR_MAX 127
#define UCHAR_MAX 255
#define SHRT_MAX  32767
#define USHRT_MAX 65535
#define INT_MAX   2147483647
#define UINT_MAX  4294967295U

/* Форматтеры для отладочных сообщений (используются редко). */
#ifndef X8_F
#define X8_F  "02x"
#endif
#ifndef U16_F
#define U16_F "u"
#endif
#ifndef S16_F
#define S16_F "d"
#endif
#ifndef X16_F
#define X16_F "x"
#endif
#ifndef U32_F
#define U32_F "u"
#endif
#ifndef S32_F
#define S32_F "d"
#endif
#ifndef X32_F
#define X32_F "x"
#endif
#ifndef SZT_F
#define SZT_F "u"
#endif

/* Диагностика и ассерты lwip перенаправляются в ядро. */
void kernel_panic(const char *msg);

#define LWIP_PLATFORM_DIAG(x)   do { } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { kernel_panic(x); } while (0)

/* Генератор псевдослучайных чисел (начальное значение TCP ISN и т.п.). */
uint32_t lwip_arch_rand(void);
#define LWIP_RAND() ((u32_t)lwip_arch_rand())

#ifdef __cplusplus
}
#endif

#endif /* LWIP_ARCH_CC_H */
