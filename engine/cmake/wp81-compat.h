// Force-included (/FI) in every engine translation unit built for WP8.1 ARM32 with clang-cl.
// clang's intrin.h declares these MSVC bit-count intrinsics only for ARM64, but the MSVC STL also uses them on ARM32.
#pragma once

#if defined(_M_ARM) && defined(__clang__)
#ifdef __cplusplus
extern "C" {
#endif

static __inline__ __attribute__((always_inline, nodebug)) unsigned int _CountLeadingZeros(unsigned long value)
{
    return value ? (unsigned int)__builtin_clzl(value) : 32u;
}

static __inline__ __attribute__((always_inline, nodebug)) unsigned int _CountLeadingZeros64(unsigned long long value)
{
    return value ? (unsigned int)__builtin_clzll(value) : 64u;
}

static __inline__ __attribute__((always_inline, nodebug)) unsigned int _CountOneBits(unsigned long value)
{
    return (unsigned int)__builtin_popcountl(value);
}

static __inline__ __attribute__((always_inline, nodebug)) unsigned int _CountOneBits64(unsigned long long value)
{
    return (unsigned int)__builtin_popcountll(value);
}

#ifdef __cplusplus
}
#endif
#endif
