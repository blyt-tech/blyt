/*
 * runtime/tools/blyt-luac.c — Lua bytecode compiler for blyt.
 *
 * Compiled host-native with -DLUA_32BITS=1 so the bytecode it produces
 * matches the RV32 and WASM blyt Lua VMs (both also -DLUA_32BITS=1).
 *
 * Usage: blyt-luac -o <output.luac> [-n <chunkname>] [-P <project_dir>]
 *                  <file.lua> [file2.lua ...]
 *
 * Single file: plain Lua bytecode output.
 *   -n <chunkname>  sets the source name embedded in the bytecode.
 *   -P <project_dir>  derives the canonical name as /blyt/cart/<rel>.
 *
 * Multiple files (-P required for canonical names): each file is compiled as
 * its own chunk so each proto keeps its own source name and per-file line
 * numbers.  Output uses the BLMC multi-chunk format (see below).  Globals
 * defined in earlier files remain visible in later ones because the runtime
 * executes each chunk in order in the same Lua state.
 *
 * BLMC format (multi-chunk output):
 *   [0..3]   "BLMC" magic
 *   [4..7]   uint32 little-endian chunk count N
 *   for each chunk:
 *     [0..3] uint32 little-endian byte count S
 *     [4..]  S bytes of Lua 5.4 bytecode
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

typedef struct {
    unsigned char *buf;
    size_t size;
} WBuf;

static int writer_cb(lua_State *L, const void *p, size_t sz, void *ud) {
    (void)L;
    WBuf *w = (WBuf *)ud;
    unsigned char *nb = realloc(w->buf, w->size + sz);
    if (!nb)
        return 1;
    w->buf = nb;
    memcpy(w->buf + w->size, p, sz);
    w->size += sz;
    return 0;
}

static char *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[sz] = '\0';
    fclose(f);
    *out_size = (size_t)sz;
    return buf;
}

static void write_u32le(FILE *f, uint32_t v) {
    unsigned char b[4] = {v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, v >> 24};
    fwrite(b, 1, 4, f);
}

/* Build the "@/blyt/cart/<rel>" source name for a file given the project dir.
 * Falls back to "@/blyt/cart/<basename>" if the prefix doesn't match. */
static void make_namebuf(const char *file, const char *prefix_dir, char *buf, size_t buf_size) {
    buf[0] = '@';
    if (prefix_dir) {
        size_t plen = strlen(prefix_dir);
        if (strncmp(file, prefix_dir, plen) == 0 && file[plen] == '/') {
            snprintf(buf + 1, buf_size - 1, "/blyt/cart/%s", file + plen + 1);
            return;
        }
    }
    const char *slash = strrchr(file, '/');
    snprintf(buf + 1, buf_size - 1, "/blyt/cart/%s", slash ? slash + 1 : file);
}

int main(int argc, char *argv[]) {
    const char *output = NULL;
    const char *chunkname = NULL;
    const char *prefix_dir = NULL;
    int strip = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            chunkname = argv[++i];
        } else if (strcmp(argv[i], "-P") == 0 && i + 1 < argc) {
            prefix_dir = argv[++i];
        } else if (strcmp(argv[i], "-s") == 0) {
            strip = 1;
        }
    }

    if (!output) {
        fprintf(stderr, "blyt-luac: -o <output.luac> required\n");
        return 1;
    }

    const char **files = malloc((size_t)argc * sizeof(char *));
    if (!files)
        return 1;
    int nfiles = 0;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "-n") == 0 ||
            strcmp(argv[i], "-P") == 0) {
            i++; /* skip the option's value too */
            continue;
        }
        if (argv[i][0] == '-')
            continue;
        files[nfiles++] = argv[i];
    }

    if (nfiles == 0) {
        fprintf(stderr, "blyt-luac: no input files\n");
        free(files);
        return 1;
    }

    lua_State *L = luaL_newstate();
    if (!L) {
        fprintf(stderr, "blyt-luac: cannot create Lua state\n");
        free(files);
        return 1;
    }

    int rc = 0;

    if (nfiles == 1) {
        /* Single file: plain Lua bytecode output (no BLMC wrapper). */
        WBuf wb = {NULL, 0};

        if (!chunkname && !prefix_dir) {
            if (luaL_loadfile(L, files[0]) != LUA_OK) {
                fprintf(stderr, "blyt-luac: %s\n", lua_tostring(L, -1));
                rc = 1;
                goto done_single;
            }
        } else {
            char namebuf[4096];
            if (chunkname) {
                size_t nlen = strlen(chunkname);
                namebuf[0] = '@';
                memcpy(namebuf + 1, chunkname, nlen + 1);
            } else {
                make_namebuf(files[0], prefix_dir, namebuf, sizeof(namebuf));
            }
            size_t sz = 0;
            char *content = read_file(files[0], &sz);
            if (!content) {
                fprintf(stderr, "blyt-luac: cannot read '%s'\n", files[0]);
                rc = 1;
                goto done_single;
            }
            int lrc = luaL_loadbuffer(L, content, sz, namebuf);
            free(content);
            if (lrc != LUA_OK) {
                fprintf(stderr, "blyt-luac: %s\n", lua_tostring(L, -1));
                rc = 1;
                goto done_single;
            }
        }

        if (lua_dump(L, writer_cb, &wb, strip) != 0) {
            fprintf(stderr, "blyt-luac: dump failed\n");
            rc = 1;
            goto done_single;
        }

        {
            FILE *out = fopen(output, "wb");
            if (!out) {
                fprintf(stderr, "blyt-luac: cannot open '%s' for writing\n", output);
                rc = 1;
                goto done_single;
            }
            if (fwrite(wb.buf, 1, wb.size, out) != wb.size) {
                fprintf(stderr, "blyt-luac: write error on '%s'\n", output);
                fclose(out);
                rc = 1;
                goto done_single;
            }
            fclose(out);
        }

    done_single:
        free(wb.buf);
        lua_close(L);
        free(files);
        return rc;
    }

    /* Multi-file: compile each file as its own chunk, output BLMC format.
     * Each chunk keeps its own source name and line numbers so debuggers can
     * map stack frames back to the correct file (issue #54). */
    WBuf *chunks = calloc((size_t)nfiles, sizeof(WBuf));
    if (!chunks) {
        lua_close(L);
        free(files);
        return 1;
    }

    for (i = 0; i < nfiles; i++) {
        char namebuf[4096];
        if (prefix_dir) {
            make_namebuf(files[i], prefix_dir, namebuf, sizeof(namebuf));
        } else if (chunkname) {
            /* -n with multiple files: use the given name for all (legacy path). */
            size_t nlen = strlen(chunkname);
            namebuf[0] = '@';
            memcpy(namebuf + 1, chunkname, nlen + 1);
        } else {
            namebuf[0] = '\0'; /* let luaL_loadfile use the file path */
        }

        size_t sz = 0;
        char *content = read_file(files[i], &sz);
        if (!content) {
            fprintf(stderr, "blyt-luac: cannot read '%s'\n", files[i]);
            rc = 1;
            goto done_multi;
        }

        lua_settop(L, 0);
        int lrc = namebuf[0] ? luaL_loadbuffer(L, content, sz, namebuf)
                             : luaL_loadbuffer(L, content, sz, files[i]);
        free(content);
        if (lrc != LUA_OK) {
            fprintf(stderr, "blyt-luac: %s\n", lua_tostring(L, -1));
            rc = 1;
            goto done_multi;
        }

        if (lua_dump(L, writer_cb, &chunks[i], strip) != 0) {
            fprintf(stderr, "blyt-luac: dump failed for '%s'\n", files[i]);
            rc = 1;
            goto done_multi;
        }
        lua_pop(L, 1);
    }

    {
        FILE *out = fopen(output, "wb");
        if (!out) {
            fprintf(stderr, "blyt-luac: cannot open '%s' for writing\n", output);
            rc = 1;
            goto done_multi;
        }
        fwrite("BLMC", 1, 4, out);
        write_u32le(out, (uint32_t)nfiles);
        for (i = 0; i < nfiles; i++) {
            write_u32le(out, (uint32_t)chunks[i].size);
            if (fwrite(chunks[i].buf, 1, chunks[i].size, out) != chunks[i].size) {
                fprintf(stderr, "blyt-luac: write error on '%s'\n", output);
                fclose(out);
                rc = 1;
                goto done_multi;
            }
        }
        fclose(out);
    }

done_multi:
    for (i = 0; i < nfiles; i++)
        free(chunks[i].buf);
    free(chunks);
    lua_close(L);
    free(files);
    return rc;
}
