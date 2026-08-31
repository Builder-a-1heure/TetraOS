#include "../lib/utils.h"
#include <stdarg.h>
#include <stdint.h>

// Fonctions de mémoire
void* memcpy(void* dest, const void* src, size_t n) {
    // CORRECTION PERF : avant, copie OCTET PAR OCTET (`while(n--) *d++=*s++`).
    // Pour un flip d'écran 1920x1080x32bpp (8,3 Mo), ça fait 8,3 millions
    // d'itérations d'une boucle scalaire — canon principal de la lenteur
    // "30 secondes pour changer les pixels".
    // `rep movsl` copie par mots de 4 octets via une instruction CPU dédiée
    // (bien plus rapide que la boucle C, indépendamment du niveau
    // d'optimisation de compilation) ; le reste (0-3 octets) est copié au cas par cas.
    uint8_t*       d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    size_t words = n / 4;
    size_t rem   = n % 4;

    __asm__ volatile (
        "rep movsl"
        : "+D"(d), "+S"(s), "+c"(words)
        :
        : "memory"
    );

    while (rem--) *d++ = *s++;
    return dest;
}

void* memset(void* ptr, int value, size_t num) {
    // Même optimisation que memcpy : rep stosl par mots de 4 octets.
    uint8_t* p = (uint8_t*)ptr;
    uint8_t  b = (uint8_t)value;
    uint32_t v32 = (uint32_t)b * 0x01010101u; // réplique l'octet sur les 4

    size_t words = num / 4;
    size_t rem   = num % 4;

    __asm__ volatile (
        "rep stosl"
        : "+D"(p), "+c"(words)
        : "a"(v32)
        : "memory"
    );

    while (rem--) *p++ = b;
    return ptr;
}

int memcmp(const void* s1, const void* s2, size_t n) {
    const unsigned char* p1 = s1, *p2 = s2;
    while (n--) {
        if (*p1 != *p2) return *p1 - *p2;
        p1++;
        p2++;
    }
    return 0;
}

// Fonctions de chaînes
int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

int strncmp(const char* s1, const char* s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i]) {
            return (s1[i] < s2[i]) ? -1 : 1;
        }
        if (s1[i] == '\0') {
            return 0;
        }
    }
    return 0;
}

char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while (*src) {
        *d++ = *src++;
    }
    *d = '\0';
    return dest;
}

char* strncpy(char* dest, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

size_t strlen(const char* str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}
char* strcat(char* dest, const char* src) {
    char* d = dest;
    while (*d) d++;  // Aller à la fin de dest
    while (*src) *d++ = *src++;  // Copier src
    *d = '\0';
    return dest;
}

// Gestion snprint
static void itoa_dec(int value, char* buffer) {
    char tmp[32];
    int i = 0, j = 0;
    if (value == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }
    int neg = (value < 0);
    if (neg) value = -value;
    while (value > 0) {
        tmp[i++] = (value % 10) + '0';
        value /= 10;
    }
    if (neg) buffer[j++] = '-';
    while (i > 0) buffer[j++] = tmp[--i];
    buffer[j] = '\0';
}

static void itoa_hex(uint32_t value, char* buffer) {
    const char* hex = "0123456789ABCDEF";
    char tmp[32];
    int i = 0, j = 0;
    if (value == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }
    while (value > 0) {
        tmp[i++] = hex[value & 0xF];
        value >>= 4;
    }
    buffer[j++] = '0';
    buffer[j++] = 'x';
    while (i > 0) buffer[j++] = tmp[--i];
    buffer[j] = '\0';
}

int snprintf(char* out, size_t n, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    size_t pos = 0;

    for (const char* p = fmt; *p && pos < n - 1; p++) {
        if (*p != '%') {
            out[pos++] = *p;
            continue;
        }
        p++;
        if (*p == 's') {
            const char* s = va_arg(args, const char*);
            while (*s && pos < n - 1) out[pos++] = *s++;
        } else if (*p == 'd') {
            char buf[32];
            itoa_dec(va_arg(args, int), buf);
            for (char* q = buf; *q && pos < n - 1; q++) out[pos++] = *q;
        } else if (*p == 'x') {
            char buf[32];
            itoa_hex(va_arg(args, uint32_t), buf);
            for (char* q = buf; *q && pos < n - 1; q++) out[pos++] = *q;
        } else {
            out[pos++] = *p; // caractère inconnu → on copie brut
        }
    }

    out[pos] = '\0';
    va_end(args);
    return pos;
}

int strcasecmp(const char* s1, const char* s2) {
    while (*s1 && *s2) {
        char c1 = (*s1 >= 'A' && *s1 <= 'Z') ? *s1 + 32 : *s1;
        char c2 = (*s2 >= 'A' && *s2 <= 'Z') ? *s2 + 32 : *s2;
        if (c1 != c2) return (unsigned char)c1 - (unsigned char)c2;
        s1++; s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

