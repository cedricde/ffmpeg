/*
 * This copyright notice applies to this header file only:
 *
 * Copyright (c) 2022
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the software, and to permit persons to whom the
 * software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef FFNV_FBC_DYNLINK_LOADER_H
#define FFNV_FBC_DYNLINK_LOADER_H

#include <stdlib.h>

#include "NvFBC.h"

#if defined(_WIN32)
# error "This platform is not supported."
#endif

#ifndef FFNV_LIB_HANDLE
# define FFNV_LIB_HANDLE void*
#endif

#define NVFBC_LIBNAME "libnvidia-fbc.so.1"

#if !defined(FFNV_LOAD_FUNC) || !defined(FFNV_SYM_FUNC)
# include <dlfcn.h>
# define FFNV_LOAD_FUNC(path) dlopen((path), RTLD_LAZY)
# define FFNV_SYM_FUNC(lib, sym) dlsym((lib), (sym))
# define FFNV_FREE_FUNC(lib) dlclose(lib)
#endif

#if !defined(FFNV_LOG_FUNC) || !defined(FFNV_DEBUG_LOG_FUNC)
# include <stdio.h>
# define FFNV_LOG_FUNC(logctx, msg, ...) fprintf(stderr, (msg), __VA_ARGS__)
# define FFNV_DEBUG_LOG_FUNC(logctx, msg, ...)
#endif

#define LOAD_LIBRARY(l, path)                                  \
    do {                                                       \
        if (!((l) = FFNV_LOAD_FUNC(path))) {                   \
            FFNV_LOG_FUNC(logctx, "Cannot load %s\n", path);   \
            ret = -1;                                          \
            goto error;                                        \
        }                                                      \
        FFNV_DEBUG_LOG_FUNC(logctx, "Loaded lib: %s\n", path); \
    } while (0)

#define LOAD_SYMBOL(fun, tp, symbol)                             \
    do {                                                         \
        if (!((f->fun) = (tp*)FFNV_SYM_FUNC(f->lib, symbol))) {  \
            FFNV_LOG_FUNC(logctx, "Cannot load %s\n", symbol);   \
            ret = -1;                                            \
            goto error;                                          \
        }                                                        \
        FFNV_DEBUG_LOG_FUNC(logctx, "Loaded sym: %s\n", symbol); \
    } while (0)

#define LOAD_SYMBOL_OPT(fun, tp, symbol)                                      \
    do {                                                                      \
        if (!((f->fun) = (tp*)FFNV_SYM_FUNC(f->lib, symbol))) {               \
            FFNV_DEBUG_LOG_FUNC(logctx, "Cannot load optional %s\n", symbol); \
        } else {                                                              \
            FFNV_DEBUG_LOG_FUNC(logctx, "Loaded sym: %s\n", symbol);          \
        }                                                                     \
    } while (0)

#define GENERIC_LOAD_FUNC_PREAMBLE(T, n, N)     \
    T *f;                                       \
    int ret;                                    \
                                                \
    n##_free_functions(functions);              \
                                                \
    f = *functions = (T*)calloc(1, sizeof(*f)); \
    if (!f)                                     \
        return -1;                              \
                                                \
    LOAD_LIBRARY(f->lib, N);

#define GENERIC_LOAD_FUNC_FINALE(n) \
    return 0;                       \
error:                              \
    n##_free_functions(functions);  \
    return ret;

#define GENERIC_FREE_FUNC()                \
    if (!functions)                        \
        return;                            \
    if (*functions && (*functions)->lib)   \
        FFNV_FREE_FUNC((*functions)->lib); \
    free(*functions);                      \
    *functions = NULL;


typedef NVFBCSTATUS NVFBCAPI tNvFBCCreateInstance(NVFBC_API_FUNCTION_LIST *pFunctionList);

typedef struct NvfbcFunctions {
    tNvFBCCreateInstance *NvFBCCreateInstance;

    FFNV_LIB_HANDLE lib;
} NvfbcFunctions;


static inline void nvfbc_free_functions(NvfbcFunctions **functions)
{
    GENERIC_FREE_FUNC();
}

static inline int nvfbc_load_functions(NvfbcFunctions **functions, void *logctx)
{
    GENERIC_LOAD_FUNC_PREAMBLE(NvfbcFunctions, nvfbc, NVFBC_LIBNAME);

    LOAD_SYMBOL(NvFBCCreateInstance, tNvFBCCreateInstance, "NvFBCCreateInstance");

    GENERIC_LOAD_FUNC_FINALE(nvfbc);
}

#undef GENERIC_LOAD_FUNC_PREAMBLE
#undef LOAD_LIBRARY
#undef LOAD_SYMBOL
#undef GENERIC_LOAD_FUNC_FINALE
#undef GENERIC_FREE_FUNC
#undef NVFBC_LIBNAME

#endif
