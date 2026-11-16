/* backtrace() and backtrace_symbols() for Android below API 33, where <execinfo.h> declares nothing. Ports use them
   for crash logs and debug traces; force-included by module.cmake (-include) so the ports need no patches. */
#ifndef PLM_EXECINFO_COMPAT_H
#define PLM_EXECINFO_COMPAT_H
#if defined(__ANDROID__) && __ANDROID_API__ < 33
#ifdef __cplusplus
extern "C" {
#endif
int backtrace(void** buffer, int size);
char** backtrace_symbols(void* const* buffer, int size);
void backtrace_symbols_fd(void* const* buffer, int size, int fd);
#ifdef __cplusplus
}
#endif
#endif
#endif
