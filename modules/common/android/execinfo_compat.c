/* See execinfo_compat.h. Walks the stack with the unwinder (available on every API level); symbol names come from
   dladdr where the library exports them. */
#include "execinfo_compat.h"
#if defined(__ANDROID__) && __ANDROID_API__ < 33
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <unwind.h>

struct plm_bt_state { void** buffer; int size; int count; };

static _Unwind_Reason_Code plm_bt_step(struct _Unwind_Context* context, void* arg) {
    struct plm_bt_state* state = (struct plm_bt_state*)arg;
    void* ip = (void*)_Unwind_GetIP(context);
    if (ip == NULL) return _URC_NO_REASON;
    if (state->count >= state->size) return _URC_END_OF_STACK;
    state->buffer[state->count++] = ip;
    return _URC_NO_REASON;
}

int backtrace(void** buffer, int size) {
    struct plm_bt_state state = { buffer, size, 0 };
    if (buffer == NULL || size <= 0) return 0;
    _Unwind_Backtrace(plm_bt_step, &state);
    return state.count;
}

char** backtrace_symbols(void* const* buffer, int size) {
    if (buffer == NULL || size <= 0) return NULL;
    /* One allocation: the pointer table followed by the strings, freed with a single free() as glibc's is. */
    const size_t line_max = 256;
    char** result = (char**)malloc(size * sizeof(char*) + size * line_max);
    if (result == NULL) return NULL;
    char* text = (char*)(result + size);
    for (int i = 0; i < size; ++i) {
        Dl_info info;
        result[i] = text + i * line_max;
        if (dladdr(buffer[i], &info) && info.dli_sname != NULL) {
            snprintf(result[i], line_max, "%s(%s+0x%lx) [%p]", info.dli_fname ? info.dli_fname : "?", info.dli_sname,
                     (unsigned long)((char*)buffer[i] - (char*)info.dli_saddr), buffer[i]);
        } else {
            snprintf(result[i], line_max, "%s [%p]", (dladdr(buffer[i], &info) && info.dli_fname) ? info.dli_fname : "?",
                     buffer[i]);
        }
    }
    return result;
}

void backtrace_symbols_fd(void* const* buffer, int size, int fd) {
    char** lines = backtrace_symbols(buffer, size);
    if (lines == NULL) return;
    for (int i = 0; i < size; ++i) {
        write(fd, lines[i], strlen(lines[i]));
        write(fd, "\n", 1);
    }
    free(lines);
}
#endif
