/* Minimal bare-metal C/C++ runtime glue for linking RadioLib (C++).
 *
 *  - _sbrk: heap for newlib malloc / operator new, bounded by the linker's
 *    __HeapBase/__HeapLimit (nrf_common.ld). nosys.specs' _sbrk just fails.
 *  - operator new/delete: backed by malloc/free (RadioLib is mostly heap-free,
 *    but provide them so any reachable allocation links).
 *  - __cxa_pure_virtual: pure-virtual call trap (should never run).
 *
 * Built with -fno-exceptions -fno-rtti -fno-use-cxa-atexit, so no __cxa_throw /
 * typeinfo / __cxa_atexit are needed.
 */
#include <cstddef>
#include <cstdlib>
#include <cerrno>

extern "C" {

extern char __HeapBase;
extern char __HeapLimit;

void *_sbrk(int incr)
{
    static char *heap = &__HeapBase;
    char *prev = heap;
    if (heap + incr > &__HeapLimit) {
        errno = ENOMEM;
        return (void *)-1;
    }
    heap += incr;
    return prev;
}

void __cxa_pure_virtual(void)
{
    for (;;) {
    }
}

} /* extern "C" */

void *operator new(std::size_t size) { return malloc(size); }
void *operator new[](std::size_t size) { return malloc(size); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, std::size_t) noexcept { free(p); }
void operator delete[](void *p, std::size_t) noexcept { free(p); }
