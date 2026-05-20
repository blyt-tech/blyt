#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "blyt_runtime.h"
#include "cart_load.h"
#include "elf32.h"

#define FLATCC_BUILDER_ASSERT(cond, reason) assert(cond)
#include "cart_config_builder.h"
#include "cart_info_builder.h"
#include "flatcc/flatcc_builder.h"

/* -------------------------------------------------------------------------
 * FlatBuffers fixture helpers
 * ------------------------------------------------------------------------- */

typedef struct {
    uint8_t *buf;
    size_t size;
} Blob;

static void blob_free(Blob *b) {
    free(b->buf);
    b->buf = NULL;
    b->size = 0;
}

static Blob build_cart_info(uint16_t api_major, uint16_t api_minor) {
    flatcc_builder_t B;
    flatcc_builder_init(&B);
    blyt_CartInfo_start_as_root(&B);
    blyt_CartInfo_api_version_major_add(&B, api_major);
    blyt_CartInfo_api_version_minor_add(&B, api_minor);
    blyt_CartInfo_end_as_root(&B);
    size_t size;
    void *raw = flatcc_builder_finalize_buffer(&B, &size);
    flatcc_builder_clear(&B);
    Blob b = {malloc(size), size};
    memcpy(b.buf, raw, size);
    free(raw);
    return b;
}

static Blob build_cart_config(void) {
    flatcc_builder_t B;
    flatcc_builder_init(&B);
    blyt_CartConfig_start_as_root(&B);
    blyt_CartConfig_fps_add(&B, 60);
    blyt_CartConfig_end_as_root(&B);
    size_t size;
    void *raw = flatcc_builder_finalize_buffer(&B, &size);
    flatcc_builder_clear(&B);
    Blob b = {malloc(size), size};
    memcpy(b.buf, raw, size);
    free(raw);
    return b;
}

static Blob add_preamble(const Blob *fb, const char *tag) {
    Blob b = {calloc(1, SECT_PREAMBLE_SIZE + fb->size), SECT_PREAMBLE_SIZE + fb->size};
    memcpy(b.buf, tag, 4);
    memcpy(b.buf + SECT_PREAMBLE_SIZE, fb->buf, fb->size);
    return b;
}

static size_t align4(size_t n) {
    return (n + 3u) & ~3u;
}

/* -------------------------------------------------------------------------
 * Minimal valid ELF construction
 *
 * Layout (all offsets computed at build time):
 *
 *   [Elf32_Ehdr]
 *   [Elf32_Phdr * PHNUM]        program headers
 *   [.shstrtab]                 section name strings
 *   [.cart.info data]
 *   [.cart.config data]
 *   [code stub]                 4 zero bytes — the PF_X LOAD segment data
 *   [Section header table]
 *
 * Program headers:
 *   [0] PT_LOAD PF_R|PF_X — covers the code stub
 *   [1] PT_GNU_RELRO       — required by ADR-0112
 *
 * e_entry = LOAD_VADDR (within the PF_X segment)
 * ------------------------------------------------------------------------- */

/* Interpreter path required in every valid cart (must match BLYT_INTERP_PATH
 * in cart_load.c; kept in sync by test_interp_path_exact). */
#define TEST_INTERP_PATH "/lib/ld-blyt.so.1"
#define TEST_INTERP_LEN sizeof(TEST_INTERP_PATH) /* includes NUL */

/* Section name string table:
 *   offset 0:  ""           (null)
 *   offset 1:  ".interp"
 *   offset 9:  ".shstrtab"
 *   offset 19: ".cart.info"
 *   offset 30: ".cart.config"
 */
#define SHSTR "\0.interp\0.shstrtab\0.cart.info\0.cart.config\0"
#define SHSTR_LEN (sizeof(SHSTR) - 1)
#define SHSTR_IDX_INTERP 1
#define SHSTR_IDX_SHSTRTAB 9
#define SHSTR_IDX_CART_INFO 19
#define SHSTR_IDX_CART_CONFIG 30

#define PHNUM 4u
#define PH_INTERP 0 /* PT_INTERP — /lib/ld-blyt.so.1 */
#define PH_CODE 1 /* PT_LOAD PF_R|PF_X */
#define PH_RELRO 2 /* PT_GNU_RELRO */
#define PH_SPARE 3 /* PT_NULL spare slot for overlap/etc. tests */

#define CODE_SIZE 4u /* 4 zero bytes — not ecall/ebreak */
#define LOAD_VADDR 0x10000u /* arbitrary virtual address for the code LOAD */

typedef struct {
    Blob cart_info_sect;
    Blob cart_config_sect;
} CartSections;

static Blob build_valid_elf(const CartSections *sects) {
    size_t ehdr_sz = sizeof(Elf32_Ehdr);
    size_t phdrs_sz = PHNUM * sizeof(Elf32_Phdr);
    size_t interp_sz = TEST_INTERP_LEN; /* "/lib/ld-blyt.so.1\0" */
    size_t shstr_sz = SHSTR_LEN;
    size_t ci_sz = sects->cart_info_sect.size;
    size_t cc_sz = sects->cart_config_sect.size;

    size_t off_phdrs = ehdr_sz;
    size_t off_interp = off_phdrs + phdrs_sz;
    size_t off_shstrtab = align4(off_interp + interp_sz);
    size_t off_ci = align4(off_shstrtab + shstr_sz);
    size_t off_cc = align4(off_ci + ci_sz);
    size_t off_code = align4(off_cc + cc_sz);
    size_t off_shdrs = align4(off_code + CODE_SIZE);
    size_t shnum = 5; /* null, interp, shstrtab, cart.info, cart.config */
    size_t total = off_shdrs + shnum * sizeof(Elf32_Shdr);

    Blob b = {calloc(1, total), total};

    /* ELF header */
    Elf32_Ehdr *eh = (Elf32_Ehdr *)b.buf;
    eh->e_ident[EI_MAG0] = ELFMAG0;
    eh->e_ident[EI_MAG1] = ELFMAG1;
    eh->e_ident[EI_MAG2] = ELFMAG2;
    eh->e_ident[EI_MAG3] = ELFMAG3;
    eh->e_ident[EI_CLASS] = ELFCLASS32;
    eh->e_ident[EI_DATA] = ELFDATA2LSB;
    eh->e_ident[EI_OSABI] = ELFOSABI_NONE;
    eh->e_machine = EM_RISCV;
    eh->e_flags = BLYT_CART_EF_FLAGS;
    eh->e_ehsize = (Elf32_Half)sizeof(Elf32_Ehdr);
    eh->e_phentsize = (Elf32_Half)sizeof(Elf32_Phdr);
    eh->e_phnum = PHNUM;
    eh->e_phoff = (Elf32_Off)off_phdrs;
    eh->e_shentsize = (Elf32_Half)sizeof(Elf32_Shdr);
    eh->e_shnum = (Elf32_Half)shnum;
    eh->e_shstrndx = 2; /* .shstrtab is section 2 */
    eh->e_shoff = (Elf32_Off)off_shdrs;
    eh->e_entry = LOAD_VADDR;

    /* Program headers */
    Elf32_Phdr *ph = (Elf32_Phdr *)(b.buf + off_phdrs);

    /* PT_INTERP — /lib/ld-blyt.so.1 */
    ph[PH_INTERP].p_type = PT_INTERP;
    ph[PH_INTERP].p_offset = (Elf32_Off)off_interp;
    ph[PH_INTERP].p_filesz = (Elf32_Word)interp_sz;
    ph[PH_INTERP].p_memsz = (Elf32_Word)interp_sz;
    ph[PH_INTERP].p_flags = PF_R;
    ph[PH_INTERP].p_align = 1;

    /* PT_LOAD PF_R|PF_X — covers the code stub */
    ph[PH_CODE].p_type = PT_LOAD;
    ph[PH_CODE].p_offset = (Elf32_Off)off_code;
    ph[PH_CODE].p_vaddr = LOAD_VADDR;
    ph[PH_CODE].p_paddr = LOAD_VADDR;
    ph[PH_CODE].p_filesz = CODE_SIZE;
    ph[PH_CODE].p_memsz = CODE_SIZE;
    ph[PH_CODE].p_flags = PF_R | PF_X;
    ph[PH_CODE].p_align = 4;

    /* PT_GNU_RELRO — required by ADR-0112 */
    ph[PH_RELRO].p_type = PT_GNU_RELRO;
    ph[PH_RELRO].p_flags = PF_R;

    /* Section data */
    memcpy(b.buf + off_interp, TEST_INTERP_PATH, interp_sz);
    memcpy(b.buf + off_shstrtab, SHSTR, shstr_sz);
    memcpy(b.buf + off_ci, sects->cart_info_sect.buf, ci_sz);
    memcpy(b.buf + off_cc, sects->cart_config_sect.buf, cc_sz);
    /* code stub: zero bytes — not ecall (0x73...) so opcode scan passes */

    /* Section headers: sh[0]=null, sh[1]=.interp, sh[2]=.shstrtab,
     *                  sh[3]=.cart.info, sh[4]=.cart.config */
    Elf32_Shdr *sh = (Elf32_Shdr *)(b.buf + off_shdrs);
    sh[1].sh_name = SHSTR_IDX_INTERP;
    sh[1].sh_type = SHT_PROGBITS;
    sh[1].sh_offset = (Elf32_Off)off_interp;
    sh[1].sh_size = (Elf32_Word)interp_sz;
    sh[2].sh_name = SHSTR_IDX_SHSTRTAB;
    sh[2].sh_type = SHT_STRTAB;
    sh[2].sh_offset = (Elf32_Off)off_shstrtab;
    sh[2].sh_size = (Elf32_Word)shstr_sz;
    sh[3].sh_name = SHSTR_IDX_CART_INFO;
    sh[3].sh_type = SHT_PROGBITS;
    sh[3].sh_offset = (Elf32_Off)off_ci;
    sh[3].sh_size = (Elf32_Word)ci_sz;
    sh[4].sh_name = SHSTR_IDX_CART_CONFIG;
    sh[4].sh_type = SHT_PROGBITS;
    sh[4].sh_offset = (Elf32_Off)off_cc;
    sh[4].sh_size = (Elf32_Word)cc_sz;

    return b;
}

static Blob default_valid_elf(void) {
    Blob ci_fb = build_cart_info(0, 0);
    Blob cc_fb = build_cart_config();
    Blob ci = add_preamble(&ci_fb, CART_INFO_TAG);
    Blob cc = add_preamble(&cc_fb, CART_CONFIG_TAG);
    CartSections s = {ci, cc};
    Blob elf = build_valid_elf(&s);
    blob_free(&ci);
    blob_free(&cc);
    blob_free(&ci_fb);
    blob_free(&cc_fb);
    return elf;
}

static char *write_temp(const Blob *b) {
    char *path = strdup("/tmp/blyt_test_XXXXXX");
    int fd = mkstemp(path);
    assert(fd >= 0);
    ssize_t written = write(fd, b->buf, b->size);
    assert((size_t)written == b->size);
    close(fd);
    return path;
}

/* -------------------------------------------------------------------------
 * Test harness
 * ------------------------------------------------------------------------- */

static int failures = 0;

static void check(const char *name, const char *path, blyt_cart_err_t expected) {
    blyt_cart_t *cart = NULL;
    blyt_cart_err_t got = blyt_cart_open(path, &cart);
    blyt_cart_close(cart);
    if (got != expected) {
        fprintf(stderr, "FAIL %s: expected %s, got %s\n", name, blyt_cart_err_str(expected),
                blyt_cart_err_str(got));
        failures++;
    } else {
        printf("PASS %s\n", name);
    }
}

/* Build the default valid ELF, apply a mutation, write to temp, check, cleanup. */
static void check_mutated(const char *name, blyt_cart_err_t expected, void (*mutate)(Blob *)) {
    Blob elf = default_valid_elf();
    if (mutate)
        mutate(&elf);
    char *path = write_temp(&elf);
    check(name, path, expected);
    unlink(path);
    free(path);
    blob_free(&elf);
}

/* -------------------------------------------------------------------------
 * ELF identity mutations
 * ------------------------------------------------------------------------- */

static void mut_bad_magic(Blob *b) {
    b->buf[0] = 0x7e;
}
static void mut_bad_class(Blob *b) {
    b->buf[EI_CLASS] = 2;
}
static void mut_bad_endian(Blob *b) {
    b->buf[EI_DATA] = 2;
}
static void mut_bad_osabi(Blob *b) {
    b->buf[EI_OSABI] = 3;
}
static void mut_bad_machine(Blob *b) {
    ((Elf32_Ehdr *)b->buf)->e_machine = 0x3e;
}
static void mut_bad_flags(Blob *b) {
    ((Elf32_Ehdr *)b->buf)->e_flags = 0x0000;
}

/* -------------------------------------------------------------------------
 * Segment mutations
 * ------------------------------------------------------------------------- */

static Elf32_Phdr *get_ph(Blob *b, int idx) {
    Elf32_Ehdr *eh = (Elf32_Ehdr *)b->buf;
    return (Elf32_Phdr *)(b->buf + eh->e_phoff) + idx;
}

static void mut_wx_segment(Blob *b) {
    get_ph(b, PH_CODE)->p_flags = PF_W | PF_X;
}
static void mut_no_relro(Blob *b) {
    get_ph(b, PH_RELRO)->p_type = PT_NULL;
}
static void mut_no_interp(Blob *b) {
    /* Remove PT_INTERP — validator requires it */
    get_ph(b, PH_INTERP)->p_type = PT_NULL;
}
static void mut_wrong_interp(Blob *b) {
    /* Corrupt the interpreter path to a non-blyt value */
    Elf32_Phdr *ph = get_ph(b, PH_INTERP);
    uint8_t *interp = b->buf + ph->p_offset;
    interp[0] = 'X'; /* "/lib/ld-blyt.so.1" → "Xlib/ld-blyt.so.1" */
}
static void mut_entry_not_in_exec(Blob *b) {
    /* Move entry point outside the PF_X LOAD segment */
    ((Elf32_Ehdr *)b->buf)->e_entry = 0xdeadbeef;
}
static void mut_segment_past_eof(Blob *b) {
    get_ph(b, PH_CODE)->p_filesz = (Elf32_Word)b->size + 1;
}
static void mut_overlapping_segments(Blob *b) {
    /* Use the spare slot: make a PT_LOAD with the same VA as PH_CODE */
    Elf32_Phdr *spare = get_ph(b, PH_SPARE);
    spare->p_type = PT_LOAD;
    spare->p_vaddr = LOAD_VADDR; /* same VA as PH_CODE → overlap */
    spare->p_memsz = CODE_SIZE;
    spare->p_flags = PF_R;
}
static void mut_gnu_stack_exec(Blob *b) {
    Elf32_Phdr *relro = get_ph(b, PH_RELRO);
    relro->p_type = PT_GNU_STACK;
    relro->p_flags = PF_R | PF_W | PF_X;
}

/* -------------------------------------------------------------------------
 * Opcode scan mutation: plant an ecall in the code stub
 * ------------------------------------------------------------------------- */

static void mut_ecall_in_code(Blob *b) {
    Elf32_Ehdr *eh = (Elf32_Ehdr *)b->buf;
    Elf32_Phdr *ph = get_ph(b, PH_CODE);
    /* Write ecall (0x73 0x00 0x00 0x00) at the start of the code segment */
    uint8_t *code = b->buf + ph->p_offset;
    (void)eh;
    code[0] = 0x73;
    code[1] = 0x00;
    code[2] = 0x00;
    code[3] = 0x00;
}

static void mut_ebreak_in_code(Blob *b) {
    Elf32_Phdr *ph = get_ph(b, PH_CODE);
    uint8_t *code = b->buf + ph->p_offset;
    code[0] = 0x73;
    code[1] = 0x00;
    code[2] = 0x10;
    code[3] = 0x00;
}

/* -------------------------------------------------------------------------
 * Section / FlatBuffers mutations
 * ------------------------------------------------------------------------- */

static void mut_unknown_section(Blob *b) {
    Elf32_Ehdr *eh = (Elf32_Ehdr *)b->buf;
    /* Use e_shstrndx so this stays correct regardless of section ordering */
    size_t shstrtab_off = (size_t)((Elf32_Shdr *)(b->buf + eh->e_shoff))[eh->e_shstrndx].sh_offset;
    /* Replace ".cart.config\0" (13 bytes at index SHSTR_IDX_CART_CONFIG) */
    memcpy(b->buf + shstrtab_off + SHSTR_IDX_CART_CONFIG, ".unknown_sec", 13);
}
static void mut_bad_ci_preamble(Blob *b) {
    Elf32_Ehdr *eh = (Elf32_Ehdr *)b->buf;
    Elf32_Shdr *sh = (Elf32_Shdr *)(b->buf + eh->e_shoff);
    /* sh[3] = .cart.info (sh[0]=null, sh[1]=.interp, sh[2]=.shstrtab) */
    b->buf[sh[3].sh_offset] = 'X';
}
/* build_elf_bad_api_version: valid ELF but api_version_major = 9 */
static Blob build_elf_bad_api_version(void) {
    Blob ci_fb = build_cart_info(9, 0);
    Blob cc_fb = build_cart_config();
    Blob ci = add_preamble(&ci_fb, CART_INFO_TAG);
    Blob cc = add_preamble(&cc_fb, CART_CONFIG_TAG);
    CartSections s = {ci, cc};
    Blob elf = build_valid_elf(&s);
    blob_free(&ci);
    blob_free(&cc);
    blob_free(&ci_fb);
    blob_free(&cc_fb);
    return elf;
}

/* -------------------------------------------------------------------------
 * Dedicated ELF: bad DT_NEEDED
 * ------------------------------------------------------------------------- */

static Blob build_elf_bad_needed(void) {
    static const char SHSTR2[] = "\0.shstrtab\0.cart.info\0.dynamic\0.dynstr\0";
    size_t shstr2_sz = sizeof(SHSTR2) - 1;

    static const char DYNSTR[] = "\0libforbidden.so";
    size_t dynstr_sz = sizeof(DYNSTR) - 1;

    Blob ci_fb = build_cart_info(0, 0);
    Blob ci = add_preamble(&ci_fb, CART_INFO_TAG);

    Elf32_Dyn dyn[] = {
        {DT_STRTAB, {0}},
        {DT_STRSZ, {(Elf32_Word)dynstr_sz}},
        {DT_NEEDED, {1}},
        {DT_NULL, {0}},
    };
    size_t dyn_sz = sizeof(dyn);
    size_t shnum = 5;
    size_t phsz = PHNUM * sizeof(Elf32_Phdr);

    size_t off_ph = sizeof(Elf32_Ehdr);
    size_t off_interp = off_ph + phsz;
    size_t off_shstr = align4(off_interp + TEST_INTERP_LEN);
    size_t off_ci = align4(off_shstr + shstr2_sz);
    size_t off_dyn = align4(off_ci + ci.size);
    size_t off_dynstr = align4(off_dyn + dyn_sz);
    size_t off_code = align4(off_dynstr + dynstr_sz);
    size_t off_shdrs = align4(off_code + CODE_SIZE);
    size_t total = off_shdrs + shnum * sizeof(Elf32_Shdr);

    Blob b = {calloc(1, total), total};

    Elf32_Ehdr *eh = (Elf32_Ehdr *)b.buf;
    eh->e_ident[EI_MAG0] = ELFMAG0;
    eh->e_ident[EI_MAG1] = ELFMAG1;
    eh->e_ident[EI_MAG2] = ELFMAG2;
    eh->e_ident[EI_MAG3] = ELFMAG3;
    eh->e_ident[EI_CLASS] = ELFCLASS32;
    eh->e_ident[EI_DATA] = ELFDATA2LSB;
    eh->e_ident[EI_OSABI] = ELFOSABI_NONE;
    eh->e_machine = EM_RISCV;
    eh->e_flags = BLYT_CART_EF_FLAGS;
    eh->e_ehsize = (Elf32_Half)sizeof(Elf32_Ehdr);
    eh->e_phentsize = (Elf32_Half)sizeof(Elf32_Phdr);
    eh->e_phnum = PHNUM;
    eh->e_phoff = (Elf32_Off)off_ph;
    eh->e_shentsize = (Elf32_Half)sizeof(Elf32_Shdr);
    eh->e_shnum = (Elf32_Half)shnum;
    eh->e_shstrndx = 1;
    eh->e_shoff = (Elf32_Off)off_shdrs;
    eh->e_entry = LOAD_VADDR;

    Elf32_Phdr *ph = (Elf32_Phdr *)(b.buf + off_ph);
    ph[PH_INTERP].p_type = PT_INTERP;
    ph[PH_INTERP].p_offset = (Elf32_Off)off_interp;
    ph[PH_INTERP].p_filesz = (Elf32_Word)TEST_INTERP_LEN;
    ph[PH_INTERP].p_memsz = (Elf32_Word)TEST_INTERP_LEN;
    ph[PH_INTERP].p_flags = PF_R;
    ph[PH_INTERP].p_align = 1;
    ph[PH_CODE].p_type = PT_LOAD;
    ph[PH_CODE].p_offset = (Elf32_Off)off_code;
    ph[PH_CODE].p_vaddr = LOAD_VADDR;
    ph[PH_CODE].p_paddr = LOAD_VADDR;
    ph[PH_CODE].p_filesz = CODE_SIZE;
    ph[PH_CODE].p_memsz = CODE_SIZE;
    ph[PH_CODE].p_flags = PF_R | PF_X;
    ph[PH_CODE].p_align = 4;
    ph[PH_RELRO].p_type = PT_GNU_RELRO;
    ph[PH_RELRO].p_flags = PF_R;

    memcpy(b.buf + off_interp, TEST_INTERP_PATH, TEST_INTERP_LEN);
    memcpy(b.buf + off_shstr, SHSTR2, shstr2_sz);
    memcpy(b.buf + off_ci, ci.buf, ci.size);
    memcpy(b.buf + off_dyn, dyn, dyn_sz);
    memcpy(b.buf + off_dynstr, DYNSTR, dynstr_sz);

    Elf32_Shdr *sh = (Elf32_Shdr *)(b.buf + off_shdrs);
    sh[1].sh_name = 1;
    sh[1].sh_type = SHT_STRTAB;
    sh[1].sh_offset = (Elf32_Off)off_shstr;
    sh[1].sh_size = (Elf32_Word)shstr2_sz;
    sh[2].sh_name = 11;
    sh[2].sh_type = SHT_PROGBITS;
    sh[2].sh_offset = (Elf32_Off)off_ci;
    sh[2].sh_size = (Elf32_Word)ci.size;
    sh[3].sh_name = 22;
    sh[3].sh_type = SHT_DYNAMIC;
    sh[3].sh_offset = (Elf32_Off)off_dyn;
    sh[3].sh_size = (Elf32_Word)dyn_sz;
    sh[3].sh_entsize = sizeof(Elf32_Dyn);
    sh[4].sh_name = 31;
    sh[4].sh_type = SHT_STRTAB;
    sh[4].sh_addr = 0;
    sh[4].sh_offset = (Elf32_Off)off_dynstr;
    sh[4].sh_size = (Elf32_Word)dynstr_sz;

    blob_free(&ci);
    blob_free(&ci_fb);
    return b;
}

/* -------------------------------------------------------------------------
 * Dedicated ELF: foreign symbol in .dynsym
 * ------------------------------------------------------------------------- */

static Blob build_elf_bad_import(void) {
    /* .dynsym with one STB_GLOBAL/SHN_UNDEF symbol "forbidden_fn" */
    static const char SHSTR3[] = "\0.shstrtab\0.cart.info\0.dynsym\0.dynstr\0";
    size_t shstr3_sz = sizeof(SHSTR3) - 1;
    /* offsets: shstrtab=1, cart.info=11, dynsym=22, dynstr=30 */

    static const char SYMSTR[] = "\0forbidden_fn";
    size_t symstr_sz = sizeof(SYMSTR) - 1;

    Blob ci_fb = build_cart_info(0, 0);
    Blob ci = add_preamble(&ci_fb, CART_INFO_TAG);

    /* Two symbol entries: [0] STB_LOCAL null, [1] STB_GLOBAL SHN_UNDEF */
    Elf32_Sym syms[2];
    memset(syms, 0, sizeof(syms));
    syms[1].st_name = 1; /* "forbidden_fn" in SYMSTR */
    syms[1].st_info = (STB_GLOBAL << 4);
    syms[1].st_shndx = SHN_UNDEF;

    size_t shnum = 5;
    size_t phsz = PHNUM * sizeof(Elf32_Phdr);
    size_t off_ph = sizeof(Elf32_Ehdr);
    size_t off_interp2 = off_ph + phsz;
    size_t off_shstr = align4(off_interp2 + TEST_INTERP_LEN);
    size_t off_ci = align4(off_shstr + shstr3_sz);
    size_t off_syms = align4(off_ci + ci.size);
    size_t off_symstr = align4(off_syms + sizeof(syms));
    size_t off_code = align4(off_symstr + symstr_sz);
    size_t off_shdrs = align4(off_code + CODE_SIZE);
    size_t total = off_shdrs + shnum * sizeof(Elf32_Shdr);

    Blob b = {calloc(1, total), total};

    Elf32_Ehdr *eh = (Elf32_Ehdr *)b.buf;
    eh->e_ident[EI_MAG0] = ELFMAG0;
    eh->e_ident[EI_MAG1] = ELFMAG1;
    eh->e_ident[EI_MAG2] = ELFMAG2;
    eh->e_ident[EI_MAG3] = ELFMAG3;
    eh->e_ident[EI_CLASS] = ELFCLASS32;
    eh->e_ident[EI_DATA] = ELFDATA2LSB;
    eh->e_ident[EI_OSABI] = ELFOSABI_NONE;
    eh->e_machine = EM_RISCV;
    eh->e_flags = BLYT_CART_EF_FLAGS;
    eh->e_ehsize = (Elf32_Half)sizeof(Elf32_Ehdr);
    eh->e_phentsize = (Elf32_Half)sizeof(Elf32_Phdr);
    eh->e_phnum = PHNUM;
    eh->e_phoff = (Elf32_Off)off_ph;
    eh->e_shentsize = (Elf32_Half)sizeof(Elf32_Shdr);
    eh->e_shnum = (Elf32_Half)shnum;
    eh->e_shstrndx = 1;
    eh->e_shoff = (Elf32_Off)off_shdrs;
    eh->e_entry = LOAD_VADDR;

    Elf32_Phdr *ph = (Elf32_Phdr *)(b.buf + off_ph);
    ph[PH_INTERP].p_type = PT_INTERP;
    ph[PH_INTERP].p_offset = (Elf32_Off)off_interp2;
    ph[PH_INTERP].p_filesz = (Elf32_Word)TEST_INTERP_LEN;
    ph[PH_INTERP].p_memsz = (Elf32_Word)TEST_INTERP_LEN;
    ph[PH_INTERP].p_flags = PF_R;
    ph[PH_INTERP].p_align = 1;
    ph[PH_CODE].p_type = PT_LOAD;
    ph[PH_CODE].p_offset = (Elf32_Off)off_code;
    ph[PH_CODE].p_vaddr = LOAD_VADDR;
    ph[PH_CODE].p_paddr = LOAD_VADDR;
    ph[PH_CODE].p_filesz = CODE_SIZE;
    ph[PH_CODE].p_memsz = CODE_SIZE;
    ph[PH_CODE].p_flags = PF_R | PF_X;
    ph[PH_CODE].p_align = 4;
    ph[PH_RELRO].p_type = PT_GNU_RELRO;
    ph[PH_RELRO].p_flags = PF_R;

    memcpy(b.buf + off_interp2, TEST_INTERP_PATH, TEST_INTERP_LEN);
    memcpy(b.buf + off_shstr, SHSTR3, shstr3_sz);
    memcpy(b.buf + off_ci, ci.buf, ci.size);
    memcpy(b.buf + off_syms, syms, sizeof(syms));
    memcpy(b.buf + off_symstr, SYMSTR, symstr_sz);

    Elf32_Shdr *sh = (Elf32_Shdr *)(b.buf + off_shdrs);
    sh[1].sh_name = 1;
    sh[1].sh_type = SHT_STRTAB;
    sh[1].sh_offset = (Elf32_Off)off_shstr;
    sh[1].sh_size = (Elf32_Word)shstr3_sz;
    sh[2].sh_name = 11;
    sh[2].sh_type = SHT_PROGBITS;
    sh[2].sh_offset = (Elf32_Off)off_ci;
    sh[2].sh_size = (Elf32_Word)ci.size;
    sh[3].sh_name = 22;
    sh[3].sh_type = SHT_DYNSYM;
    sh[3].sh_offset = (Elf32_Off)off_syms;
    sh[3].sh_size = (Elf32_Word)sizeof(syms);
    sh[3].sh_entsize = sizeof(Elf32_Sym);
    sh[3].sh_link = 4;
    sh[4].sh_name = 30;
    sh[4].sh_type = SHT_STRTAB;
    sh[4].sh_offset = (Elf32_Off)off_symstr;
    sh[4].sh_size = (Elf32_Word)symstr_sz;

    blob_free(&ci);
    blob_free(&ci_fb);
    return b;
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void) {
    /* Valid cart */
    check_mutated("valid cart", BLYT_CART_OK, NULL);

    /* ELF identity */
    check_mutated("bad ELF magic", BLYT_CART_ERR_NOT_ELF, mut_bad_magic);
    check_mutated("bad ELF class", BLYT_CART_ERR_BAD_CLASS, mut_bad_class);
    check_mutated("bad ELF endian", BLYT_CART_ERR_BAD_ENDIAN, mut_bad_endian);
    check_mutated("bad EI_OSABI", BLYT_CART_ERR_BAD_OSABI, mut_bad_osabi);
    check_mutated("bad e_machine", BLYT_CART_ERR_BAD_MACHINE, mut_bad_machine);
    check_mutated("bad e_flags", BLYT_CART_ERR_BAD_FLAGS, mut_bad_flags);

    /* Segment layout */
    check_mutated("W+X segment", BLYT_CART_ERR_BAD_SEGMENT, mut_wx_segment);
    check_mutated("no PT_GNU_RELRO", BLYT_CART_ERR_NO_RELRO, mut_no_relro);
    check_mutated("PT_INTERP missing", BLYT_CART_ERR_BAD_INTERP, mut_no_interp);
    check_mutated("PT_INTERP wrong path", BLYT_CART_ERR_BAD_INTERP, mut_wrong_interp);
    check_mutated("entry not in exec", BLYT_CART_ERR_BAD_SEGMENT, mut_entry_not_in_exec);
    check_mutated("segment past EOF", BLYT_CART_ERR_BAD_SEGMENT, mut_segment_past_eof);
    check_mutated("overlapping segments", BLYT_CART_ERR_BAD_SEGMENT, mut_overlapping_segments);
    check_mutated("GNU_STACK+PF_X", BLYT_CART_ERR_BAD_SEGMENT, mut_gnu_stack_exec);

    /* Opcode scan */
    check_mutated("ecall in code", BLYT_CART_ERR_BAD_OPCODE, mut_ecall_in_code);
    check_mutated("ebreak in code", BLYT_CART_ERR_BAD_OPCODE, mut_ebreak_in_code);

    /* Section / FlatBuffers */
    check_mutated("unknown section", BLYT_CART_ERR_UNKNOWN_SECT, mut_unknown_section);
    check_mutated("bad .cart.info preamble", BLYT_CART_ERR_BAD_PREAMBLE, mut_bad_ci_preamble);
    {
        Blob elf = build_elf_bad_api_version();
        char *path = write_temp(&elf);
        check("unsupported api_version", path, BLYT_CART_ERR_API_VERSION);
        unlink(path);
        free(path);
        blob_free(&elf);
    }

    /* DT_NEEDED */
    {
        Blob elf = build_elf_bad_needed();
        char *path = write_temp(&elf);
        check("bad DT_NEEDED", path, BLYT_CART_ERR_BAD_NEEDED);
        unlink(path);
        free(path);
        blob_free(&elf);
    }

    /* Symbol import allowlist */
    {
        Blob elf = build_elf_bad_import();
        char *path = write_temp(&elf);
        check("bad symbol import", path, BLYT_CART_ERR_BAD_IMPORT);
        unlink(path);
        free(path);
        blob_free(&elf);
    }

    /* Non-existent file */
    {
        blyt_cart_t *cart = NULL;
        blyt_cart_err_t err = blyt_cart_open("/tmp/blyt_no_such_file_xyz", &cart);
        blyt_cart_close(cart);
        if (err != BLYT_CART_ERR_IO) {
            fprintf(stderr, "FAIL missing file: expected IO error, got %s\n",
                    blyt_cart_err_str(err));
            failures++;
        } else {
            printf("PASS missing file\n");
        }
    }

    if (failures > 0) {
        fprintf(stderr, "\n%d test(s) FAILED\n", failures);
        return 1;
    }
    printf("\nAll tests passed.\n");
    return 0;
}
