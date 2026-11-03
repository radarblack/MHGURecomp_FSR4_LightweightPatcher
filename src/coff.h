#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool coff_find_symbol(const uint8_t *file, size_t size, const char *name, uint32_t *rva);
