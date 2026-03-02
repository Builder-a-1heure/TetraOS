#ifndef GLOBAL_H
#define GLOBAL_H

#include <stdint.h>
#include <stddef.h>
#include "../fs/fs.h" 
#include "../gfx/screen.h"
#include "../drivers/ata.h"

// Déclarations externes
extern char input_buffer[512];
extern int input_index;
extern int shift_pressed;

// Déclaration des types de base
typedef uint8_t bool;
#define true 1
#define false 0

#endif