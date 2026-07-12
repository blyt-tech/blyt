#pragma once

/*
 * Portable ELF32 types and RISC-V constants.
 * Defined here rather than relying on <elf.h> for cross-platform builds.
 */

#include <stdint.h>

/* ELF32 primitive types */
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Word;
typedef int32_t Elf32_Sword;
typedef uint32_t Elf32_Addr;
typedef uint32_t Elf32_Off;

/* ELF identity index */
#define EI_MAG0 0
#define EI_MAG1 1
#define EI_MAG2 2
#define EI_MAG3 3
#define EI_CLASS 4
#define EI_DATA 5
#define EI_VERSION 6
#define EI_OSABI 7
#define EI_NIDENT 16

/* EI_MAG values */
#define ELFMAG0 0x7fu
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

/* EI_CLASS values */
#define ELFCLASS32 1

/* EI_DATA values */
#define ELFDATA2LSB 1

/* EI_OSABI values */
#define ELFOSABI_NONE 0

/* e_machine values */
#define EM_RISCV 243

/* RISC-V e_flags */
#define EF_RISCV_RVC 0x0001u
#define EF_RISCV_FLOAT_ABI_MASK 0x0006u
#define EF_RISCV_FLOAT_ABI_SOFT 0x0000u
#define EF_RISCV_FLOAT_ABI_SINGLE 0x0002u
#define EF_RISCV_FLOAT_ABI_DOUBLE 0x0004u

/*
 * Expected e_flags for blyt carts: RVC + ILP32D (Spike U).
 * The console is moving to RV32IMAFDC / ilp32d with hardware doubles; the whole
 * sysroot and all carts are rebuilt ilp32d (-march=rv32imafdc -mabi=ilp32d).
 * (Previously ILP32F per the earlier spikes / ADR-0024 — superseded by the
 * Spike U hardware-double work.)
 */
#define BLYT_CART_EF_FLAGS (EF_RISCV_RVC | EF_RISCV_FLOAT_ABI_DOUBLE)

/* ELF32 header */
typedef struct {
    uint8_t e_ident[EI_NIDENT];
    Elf32_Half e_type;
    Elf32_Half e_machine;
    Elf32_Word e_version;
    Elf32_Addr e_entry;
    Elf32_Off e_phoff;
    Elf32_Off e_shoff;
    Elf32_Word e_flags;
    Elf32_Half e_ehsize;
    Elf32_Half e_phentsize;
    Elf32_Half e_phnum;
    Elf32_Half e_shentsize;
    Elf32_Half e_shnum;
    Elf32_Half e_shstrndx;
} Elf32_Ehdr;

/* ELF32 section header */
typedef struct {
    Elf32_Word sh_name;
    Elf32_Word sh_type;
    Elf32_Word sh_flags;
    Elf32_Addr sh_addr;
    Elf32_Off sh_offset;
    Elf32_Word sh_size;
    Elf32_Word sh_link;
    Elf32_Word sh_info;
    Elf32_Word sh_addralign;
    Elf32_Word sh_entsize;
} Elf32_Shdr;

/* ELF32 dynamic entry */
typedef struct {
    Elf32_Sword d_tag;
    union {
        Elf32_Word d_val;
        Elf32_Addr d_ptr;
    } d_un;
} Elf32_Dyn;

/* ELF32 program header */
typedef struct {
    Elf32_Word p_type;
    Elf32_Off p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
} Elf32_Phdr;

/* Program header types */
#define PT_NULL 0u
#define PT_LOAD 1u
#define PT_DYNAMIC 2u
#define PT_INTERP 3u
#define PT_NOTE 4u
#define PT_PHDR 6u
#define PT_TLS 7u
#define PT_GNU_EH_FRAME 0x6474e550u
#define PT_GNU_STACK 0x6474e551u
#define PT_GNU_RELRO 0x6474e552u

/* Program header flags */
#define PF_X 0x1u
#define PF_W 0x2u
#define PF_R 0x4u

/* ELF32 symbol table entry */
typedef struct {
    Elf32_Word st_name;
    Elf32_Addr st_value;
    Elf32_Word st_size;
    unsigned char st_info;
    unsigned char st_other;
    Elf32_Half st_shndx;
} Elf32_Sym;

/* Symbol binding extracted from st_info */
#define ELF32_ST_BIND(i) ((i) >> 4)
#define STB_LOCAL 0u
#define STB_GLOBAL 1u
#define STB_WEAK 2u

/* Symbol type extracted from st_info */
#define ELF32_ST_TYPE(i) ((i) & 0xfu)
#define STT_FUNC 2u

/* Special section indices */
#define SHN_UNDEF 0u

/* Section header types */
#define SHT_NULL 0u
#define SHT_PROGBITS 1u
#define SHT_SYMTAB 2u
#define SHT_STRTAB 3u
#define SHT_RELA 4u
#define SHT_HASH 5u
#define SHT_DYNAMIC 6u
#define SHT_NOTE 7u
#define SHT_NOBITS 8u
#define SHT_REL 9u
#define SHT_DYNSYM 11u
#define SHT_GNU_HASH 0x6ffffff6u
#define SHT_GNU_VERSYM 0x6fffffffu
#define SHT_GNU_VERNEED 0x6ffffffeu
#define SHT_GNU_VERDEF 0x6ffffffdu

/* ELF object types */
#define ET_EXEC 2u
#define ET_DYN 3u

/* Dynamic tags */
#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_STRSZ 10
#define DT_SYMENT 11
#define DT_PLTREL 20
#define DT_JMPREL 23
#define DT_BIND_NOW 24

/* ELF32 relocation with explicit addend */
typedef struct {
    Elf32_Addr r_offset;
    Elf32_Word r_info;
    Elf32_Sword r_addend;
} Elf32_Rela;

#define ELF32_R_SYM(i) ((i) >> 8)
#define ELF32_R_TYPE(i) ((i) & 0xFFu)

/* RISC-V relocation types */
#define R_RISCV_32 1u
#define R_RISCV_RELATIVE 3u
#define R_RISCV_JUMP_SLOT 5u
#define R_RISCV_GLOB_DAT 19u
