#ifndef GUARD_PLATFORM_SHARED_PPC_MEMORY_H
#define GUARD_PLATFORM_SHARED_PPC_MEMORY_H

#include <stdint.h>

static inline uint16_t PpcLoadU16LE(const void *ptr)
{
    uint16_t value;
    __asm__ volatile("lhbrx %0, 0, %1" : "=r"(value) : "r"(ptr) : "memory");
    return value;
}

static inline uint32_t PpcLoadU32LE(const void *ptr)
{
    uint32_t value;
    __asm__ volatile("lwbrx %0, 0, %1" : "=r"(value) : "r"(ptr) : "memory");
    return value;
}

static inline void PpcStoreU16LE(void *ptr, uint16_t value)
{
    __asm__ volatile("sthbrx %0, 0, %1" : : "r"(value), "r"(ptr) : "memory");
}

static inline void PpcStoreU32LE(void *ptr, uint32_t value)
{
    __asm__ volatile("stwbrx %0, 0, %1" : : "r"(value), "r"(ptr) : "memory");
}

#endif // GUARD_PLATFORM_SHARED_PPC_MEMORY_H
