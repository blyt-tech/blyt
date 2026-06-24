/*
 * blyt_elf_section.h — locate a section in a cart ELF32 image by name.
 *
 * Part of runtime/shared (freestanding; see blyt_fp_canon.h header comment).
 *
 * The host runtime parses cart section headers to find FlatBuffers sections
 * (.cart.layouts, .cart.config, …); the native bare-metal path re-parses the
 * same headers by hand before the restricted seccomp filter is installed.  Same
 * skeleton, historically two copies — this is the single bounds-checked walk
 * both sides call (issue #128).
 *
 * Operates on a caller-provided, fully-mapped ELF image; all offsets are
 * validated against `elf_size`.  Parses raw little-endian ELF32 fields by spec
 * offset (no <elf.h>): e_shoff@32 (u32), e_shnum@48 (u16), e_shstrndx@50 (u16);
 * each Elf32_Shdr is 40 bytes with sh_name@0, sh_offset@16, sh_size@20 (all
 * u32).  Reads are width-explicit and use __builtin_memcpy, so results are
 * bit-identical on the LP64 host and the ILP32 native target.
 */

#ifndef BLYT_SHARED_ELF_SECTION_H
#define BLYT_SHARED_ELF_SECTION_H

#include <stddef.h>
#include <stdint.h>

/* Find the section named `name` (exact, NUL-terminated match) in the ELF32
 * image at [elf, elf+elf_size).  On success writes the section's file offset
 * and size (when the out-pointers are non-NULL) and returns 1; returns 0 if the
 * image is too small/malformed, the section is absent, or its [offset,offset+
 * size) range would fall outside the image. */
int blyt_elf32_find_section(const uint8_t *elf, size_t elf_size, const char *name,
                            uint32_t *out_offset, uint32_t *out_size);

#endif /* BLYT_SHARED_ELF_SECTION_H */
