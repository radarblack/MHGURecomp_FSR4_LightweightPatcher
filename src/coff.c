#include <stdint.h>
#include <string.h>

#include "coff.h"

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

bool coff_find_symbol(const uint8_t *file, size_t size, const char *name, uint32_t *rva) {
    if (size < 0x40 || file[0] != 'M' || file[1] != 'Z') return false;
    uint32_t pe = rd32(file + 0x3c);
    if ((size_t)pe + 24 > size || memcmp(file + pe, "PE\0\0", 4)) return false;
    const uint8_t *coff = file + pe + 4;
    uint16_t sections = rd16(coff + 2);
    uint32_t sym_ptr = rd32(coff + 8);
    uint32_t sym_count = rd32(coff + 12);
    uint16_t opt_size = rd16(coff + 16);
    size_t section_table = (size_t)pe + 24 + opt_size;
    if (!sym_ptr || !sym_count || section_table + (size_t)sections * 40 > size) return false;
    if ((size_t)sym_ptr + (size_t)sym_count * 18 + 4 > size) return false;
    const uint8_t *strings = file + sym_ptr + (size_t)sym_count * 18;
    uint32_t strings_size = rd32(strings);
    size_t want = strlen(name);
    for (uint32_t i = 0; i < sym_count; i++) {
        const uint8_t *s = file + sym_ptr + (size_t)i * 18;
        uint8_t aux = s[17];
        bool match = false;
        if (rd32(s) == 0) {
            uint32_t off = rd32(s + 4);
            if (off + want + 1 <= strings_size && (size_t)off + want + 1 <= size - (size_t)(strings - file))
                match = !memcmp(strings + off, name, want) && strings[off + want] == 0;
        } else if (want <= 8) {
            match = !memcmp(s, name, want) && (want == 8 || s[want] == 0);
        }
        if (match) {
            int16_t section = (int16_t)rd16(s + 12);
            if (section > 0 && section <= sections) {
                const uint8_t *sec = file + section_table + (size_t)(section - 1) * 40;
                *rva = rd32(sec + 12) + rd32(s + 8);
                return true;
            }
        }
        i += aux;
    }
    return false;
}
