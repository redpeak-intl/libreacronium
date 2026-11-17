/**
 * @file string.h
 * @brief Минимальный freestanding-заголовок <string.h> для ядра и lwip.
 *
 * Ядро собрано с -ffreestanding -nostdlib, поэтому системный glibc-заголовок
 * <string.h> недоступен (при -m32 у него нет заголовков bits). Здесь объявлены
 * только те функции, которые реализованы в sys_libc.c и используются lwip/ядром.
 */

#ifndef LWIP_PORT_STRING_H
#define LWIP_PORT_STRING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memchr(const void *s, int c, size_t n);

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strncat(char *dst, const char *src, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);

int    atoi(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* LWIP_PORT_STRING_H */
