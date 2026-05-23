/*
 * runtime/tools/blyt-luac.c — Lua bytecode compiler for blyt.
 *
 * Compiled host-native with -DLUA_32BITS=1 so the bytecode it produces
 * matches the RV32 and WASM blyt Lua VMs (both also -DLUA_32BITS=1).
 *
 * Usage: blyt-luac -o <output.luac> <file.lua> [file2.lua ...]
 *
 * Multiple source files are concatenated and compiled as one chunk.
 * Globals defined in earlier files are visible in later ones.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

typedef struct {
    unsigned char *buf;
    size_t         size;
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

int main(int argc, char *argv[]) {
    const char *output = NULL;
    int strip = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output = argv[++i];
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
        if (strcmp(argv[i], "-o") == 0) {
            i++;
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
    WBuf wb = {NULL, 0};

    if (nfiles == 1) {
        if (luaL_loadfile(L, files[0]) != LUA_OK) {
            fprintf(stderr, "blyt-luac: %s\n", lua_tostring(L, -1));
            rc = 1;
            goto done;
        }
    } else {
        /* Concatenate all source files and compile as one chunk.
         * Globals defined in each file are visible to subsequent files
         * since they share the same environment. */
        size_t total = 0;
        char **contents = malloc((size_t)nfiles * sizeof(char *));
        size_t *sizes   = malloc((size_t)nfiles * sizeof(size_t));
        if (!contents || !sizes) {
            free(contents);
            free(sizes);
            rc = 1;
            goto done;
        }
        for (i = 0; i < nfiles; i++) {
            contents[i] = read_file(files[i], &sizes[i]);
            if (!contents[i]) {
                fprintf(stderr, "blyt-luac: cannot read '%s'\n", files[i]);
                for (int j = 0; j < i; j++)
                    free(contents[j]);
                free(contents);
                free(sizes);
                rc = 1;
                goto done;
            }
            total += sizes[i] + 1; /* +1 for newline between files */
        }
        char *combined = malloc(total + 1);
        if (!combined) {
            for (i = 0; i < nfiles; i++)
                free(contents[i]);
            free(contents);
            free(sizes);
            rc = 1;
            goto done;
        }
        size_t pos = 0;
        for (i = 0; i < nfiles; i++) {
            memcpy(combined + pos, contents[i], sizes[i]);
            pos += sizes[i];
            combined[pos++] = '\n';
            free(contents[i]);
        }
        combined[pos] = '\0';
        free(contents);
        free(sizes);

        if (luaL_loadbuffer(L, combined, pos, "@cart") != LUA_OK) {
            fprintf(stderr, "blyt-luac: %s\n", lua_tostring(L, -1));
            free(combined);
            rc = 1;
            goto done;
        }
        free(combined);
    }

    if (lua_dump(L, writer_cb, &wb, strip) != 0) {
        fprintf(stderr, "blyt-luac: dump failed\n");
        rc = 1;
        goto done;
    }

    {
        FILE *out = fopen(output, "wb");
        if (!out) {
            fprintf(stderr, "blyt-luac: cannot open '%s' for writing\n", output);
            rc = 1;
            goto done;
        }
        if (fwrite(wb.buf, 1, wb.size, out) != wb.size) {
            fprintf(stderr, "blyt-luac: write error on '%s'\n", output);
            fclose(out);
            rc = 1;
            goto done;
        }
        fclose(out);
    }

done:
    free(wb.buf);
    free(files);
    lua_close(L);
    return rc;
}
