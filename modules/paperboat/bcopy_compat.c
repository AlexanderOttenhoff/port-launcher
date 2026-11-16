/* PaperBoat's game code (include/common.h, nusys) calls bcopy() as a plain function. Upstream builds Android with
   NDK 30; with our NDK 26 the libc has no out-of-line bcopy to link against (bionic only offers it as a header
   inline, which common.h's own prototype bypasses), and src/port/libc_compat.c defines it for Windows only. Weak, so
   a future upstream or NDK definition wins. */
#include <string.h>

__attribute__((weak)) void bcopy(const void* src, void* dest, size_t n) {
    memmove(dest, src, n);
}
