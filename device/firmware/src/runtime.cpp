/* Bare-metal C/C++ runtime for RadioLib (it mallocs two short-lived buffers per SPI command):
 * _sbrk stops at __HeapLimit (nosys.specs' never does); pure virtuals trap to the fault handler. */
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
    __builtin_trap();
}

} /* extern "C" */

void *operator new(std::size_t size) { return malloc(size); }
void *operator new[](std::size_t size) { return malloc(size); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, std::size_t) noexcept { free(p); }
void operator delete[](void *p, std::size_t) noexcept { free(p); }
