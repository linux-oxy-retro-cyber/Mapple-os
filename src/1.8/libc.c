/* libc.c - Biblioteca C propria do mapple (freestanding).
 *
 * Implementa o minimo que o kernel usa: strings, memoria e conversoes.
 * O linker resolve aqui ate chamadas implicitas que o GCC gerar
 * (memcpy/memset), entao NAO remover essas funcoes.
 */
#include <stdint.h>
#include <stddef.h>

size_t strlen(const char* s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char* a, const char* b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == '\0' || b[i] == '\0') {
            return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        }
    }
    return 0;
}

char* strcpy(char* dst, const char* src)
{
    char* d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}

char* strncpy(char* dst, const char* src, size_t n)
{
    size_t i = 0;
    while (i < n && src[i]) {
        dst[i] = src[i];
        i++;
    }
    while (i < n) {
        dst[i++] = '\0';
    }
    return dst;
}

char* strcat(char* dst, const char* src)
{
    char* d = dst;
    while (*d) d++;
    while ((*d++ = *src++)) {}
    return dst;
}

char* strchr(const char* s, int c)
{
    while (*s) {
        if (*s == (char)c) {
            return (char*)s;
        }
        s++;
    }
    return (c == '\0') ? (char*)s : 0;
}

void* memcpy(void* dst, const void* src, size_t n)
{
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

void* memset(void* dst, int c, size_t n)
{
    uint8_t* d = (uint8_t*)dst;
    for (size_t i = 0; i < n; i++) {
        d[i] = (uint8_t)c;
    }
    return dst;
}

void* memmove(void* dst, const void* src, size_t n)
{
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else if (d > s) {
        for (size_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dst;
}

int memcmp(const void* a, const void* b, size_t n)
{
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return (int)x[i] - (int)y[i];
        }
    }
    return 0;
}

/* atoi simples (espacos + sinal opcional). */
int atoi(const char* s)
{
    int neg = 0;
    int v = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return neg ? -v : v;
}

/* utoa64: decimal sem sinal em buf (cap>=21). Retorna strlen. */
size_t utoa64(uint64_t v, char* buf, size_t cap)
{
    char tmp[21];
    size_t n = 0;
    if (cap == 0) return 0;
    if (v == 0) {
        if (cap < 2) return 0;
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }
    while (v > 0 && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    if (n + 1 > cap) return 0;
    for (size_t i = 0; i < n; i++) {
        buf[i] = tmp[n - 1 - i];
    }
    buf[n] = '\0';
    return n;
}

/* itoa64: com sinal. */
size_t itoa64(int64_t v, char* buf, size_t cap)
{
    if (v < 0) {
        if (cap < 2) return 0;
        buf[0] = '-';
        return 1 + utoa64((uint64_t)(-(v + 1)) + 1, buf + 1, cap - 1);
    }
    return utoa64((uint64_t)v, buf, cap);
}
