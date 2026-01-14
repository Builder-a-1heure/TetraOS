#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>
#include <stdint.h>

// Fonctions de mémoire
void* memcpy(void* dest, const void* src, size_t n);
void* memset(void* ptr, int value, size_t num);
int memcmp(const void* s1, const void* s2, size_t n);

// Fonctions de chaînes
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, size_t n);
char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, size_t n);
size_t strlen(const char* str);
char* strcat(char* dest, const char* src);

// Fonction de snprint
int snprintf(char* out, size_t n, const char* fmt, ...);

int strcasecmp(const char* s1, const char* s2);

#endif