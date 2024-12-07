#ifndef INTERNALS_H
#define INTERNALS_H

#include <Python.h>

#if Py_BIG_ENDIAN == 1
#define IS_BIG_ENDIAN
#endif

#ifdef IS_BIG_ENDIAN

    #define BIG_64(x) (x)

    #define BIG_DOUBLE(x) (x)

#else

    // GCC and CLANG intrinsics
    #if defined(__GNUC__) || defined(__clang__)

        #define BIG_64(x) (__builtin_bswap64((uint64_t)(x)))
        #define BIG_32(x) (__builtin_bswap32((uint32_t)(x)))
        #define BIG_16(x) (__builtin_bswap16((uint16_t)(x)))

    // MSCV intrinsics
    #elif defined(_MSC_VER)

        #include <intrin.h>
        #define BIG_64(x) (_byteswap_uint64((uint64_t)(x)))
        #define BIG_32(x) (_byteswap_uint32((uint32_t)(x)))
        #define BIG_16(x) (_byteswap_uint16((uint16_t)(x)))

    #else

        #define BIG_64(x) ( \
            (((x) >> 56) & 0x00000000000000FF) | \
            (((x) >> 40) & 0x000000000000FF00) | \
            (((x) >> 24) & 0x0000000000FF0000) | \
            (((x) >>  8) & 0x00000000FF000000) | \
            (((x) <<  8) & 0x000000FF00000000) | \
            (((x) << 24) & 0x0000FF0000000000) | \
            (((x) << 40) & 0x00FF000000000000) | \
            (((x) << 56) & 0xFF00000000000000)   \
        )

        #define BIG_32(x) ( \
            (((x) >> 24) & 0x000000FF) | \
            (((x) >>  8) & 0x0000FF00) | \
            (((x) <<  8) & 0x00FF0000) | \
            (((x) << 24) & 0xFF000000)   \
        )

        #define BIG_16(x) ( \
            (((x) >> 8) & 0x00FF) | \
            (((x) << 8) & 0xFF00)   \
        )

    #endif

    #define BIG_DOUBLE(x) do { \
        uint64_t __temp; \
        memcpy(&__temp, &(x), 8); \
        __temp = BIG_64(__temp); \
        memcpy(&(x), &__temp, 8); \
    } while (0)

#endif


#if (defined(__GNUC__) || defined(__clang__))

    #define LEADING_ZEROES_64(x) (__builtin_clzll((unsigned long long)(x)))
    #define LEADING_ZEROES_32(x) (__builtin_clz((unsigned int)(x)))

    //#define LEADING_ZEROES(x) (sizeof(x) == 8 ? (LEADING_ZEROES_64(x)) : (LEADING_ZEROES_32(x)))

#elif defined(_MSC_VER)

    #include <intrin.h>
    #define LEADING_ZEROES_64(x) (8 - _BitScanReverse64(x))

#else

    inline int LEADING_ZEROES_64(uint64_t x)
    {
        int n = 64;

        if (x <= 0x00000000FFFFFFFF) { n -= 32; x <<= 32; }
        if (x <= 0x0000FFFFFFFFFFFF) { n -= 16; x <<= 16; }
        if (x <= 0x00FFFFFFFFFFFFFF) { n -=  8; x <<=  8; } 
        if (x <= 0x0FFFFFFFFFFFFFFF) { n -=  4; x <<=  4; } 
        if (x <= 0x3FFFFFFFFFFFFFFF) { n -=  2; x <<=  2; } 
        if (x <= 0x7FFFFFFFFFFFFFFF) { n -=  1; }

        return n;
    }

#endif

// Count the number of used bytes in an integer
#define USED_BYTES(x) (x == 0 ? 1 : (sizeof(x) == 8 ? (8 - (LEADING_ZEROES_64(x) >> 3)) : (4 - (LEADING_ZEROES_32(x)) >> 3)))

#if (defined(__GNUC__) || defined(__clang__)) 

    #define _LIKELY(cond) __builtin_expect(!!(cond), 1)
    #define _UNLIKELY(cond) __builtin_expect(!!(cond), 0)

#else

    #define _LIKELY(cond) (cond)
    #define _UNLIKELY(cond) (cond)

#endif

#endif // INTERNALS_H