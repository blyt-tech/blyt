/* blyt_elf_section.c — see blyt_elf_section.h.  Freestanding (runtime/shared). */

#include "blyt_elf_section.h"

/* ELF32 layout constants (no <elf.h> — freestanding). */
#define ELF32_EHDR_MIN 52u /* smallest valid ELF32 header */
#define ELF32_E_SHOFF 32u
#define ELF32_E_SHNUM 48u
#define ELF32_E_SHSTRNDX 50u
#define ELF32_SHDR_SIZE 40u
#define ELF32_SH_NAME 0u
#define ELF32_SH_OFFSET 16u
#define ELF32_SH_SIZE 20u

int blyt_elf32_find_section(const uint8_t *elf, size_t elf_size, const char *name,
                            uint32_t *out_offset, uint32_t *out_size) {
    if (!elf || !name || elf_size < ELF32_EHDR_MIN)
        return 0;

    uint32_t e_shoff;
    uint16_t e_shnum, e_shstrndx;
    __builtin_memcpy(&e_shoff, elf + ELF32_E_SHOFF, 4);
    __builtin_memcpy(&e_shnum, elf + ELF32_E_SHNUM, 2);
    __builtin_memcpy(&e_shstrndx, elf + ELF32_E_SHSTRNDX, 2);

    if (e_shoff == 0u || e_shnum == 0u || e_shstrndx >= e_shnum)
        return 0;
    if ((uint64_t)e_shoff + (uint64_t)e_shnum * ELF32_SHDR_SIZE > elf_size)
        return 0;

    const uint8_t *shdrs = elf + e_shoff;

    /* Section-header string table, located via e_shstrndx. */
    const uint8_t *strtab_shdr = shdrs + (uint32_t)e_shstrndx * ELF32_SHDR_SIZE;
    uint32_t strtab_off, strtab_size;
    __builtin_memcpy(&strtab_off, strtab_shdr + ELF32_SH_OFFSET, 4);
    __builtin_memcpy(&strtab_size, strtab_shdr + ELF32_SH_SIZE, 4);
    if (strtab_off == 0u || strtab_size == 0u || (uint64_t)strtab_off + strtab_size > elf_size)
        return 0;
    const char *strtab = (const char *)(elf + strtab_off);

    for (uint16_t i = 0; i < e_shnum; i++) {
        const uint8_t *shdr = shdrs + (uint32_t)i * ELF32_SHDR_SIZE;
        uint32_t sh_name;
        __builtin_memcpy(&sh_name, shdr + ELF32_SH_NAME, 4);
        if (sh_name >= strtab_size)
            continue;

        /* Compare `name` against the candidate within the string table, never
         * reading past strtab_size (the table need not be NUL-terminated). */
        const char *cand = strtab + sh_name;
        size_t avail = (size_t)(strtab_size - sh_name);
        size_t k = 0;
        while (k < avail && name[k] != '\0' && cand[k] == name[k])
            k++;
        if (name[k] != '\0' || k >= avail || cand[k] != '\0')
            continue;

        uint32_t sh_off, sh_size;
        __builtin_memcpy(&sh_off, shdr + ELF32_SH_OFFSET, 4);
        __builtin_memcpy(&sh_size, shdr + ELF32_SH_SIZE, 4);
        if ((uint64_t)sh_off + sh_size > elf_size)
            return 0;
        if (out_offset)
            *out_offset = sh_off;
        if (out_size)
            *out_size = sh_size;
        return 1;
    }
    return 0;
}
