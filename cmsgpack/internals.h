#ifndef CMSGPACK_INTERNALS_H
#define CMSGPACK_INTERNALS_H


///////////////////
//   INTERNALS   //
///////////////////

#include <Python.h>
#include <stdbool.h>

#if Py_BIG_ENDIAN == 1
#define IS_BIG_ENDIAN
#endif

#define _always_inline Py_ALWAYS_INLINE inline


#if defined(Py_NOGIL)

// Define macro for needing thread-safety
#define _need_threadsafe

// Lock a flag
#define lock_flag(flag) \
    { while (atomic_flag_test_and_set_explicit(flag, memory_order_acquire)) { } }

// Unlock a flag
#define unlock_flag(flag) \
    { atomic_flag_clear_explicit(flag, memory_order_release); }

// Clear a flag / initialize it to unset
#define clear_flag(flag) \
    { atomic_flag_clear(flag); }

#else

// Dummy macros with no effect
#define lock_flag(flag)
#define unlock_flag(flag)
#define clear_flag(flag)

#endif


#ifdef IS_BIG_ENDIAN

    #define BIG_64(x) (x)
    #define BIG_32(x) (x)
    #define BIG_DOUBLE(x) (NULL)

#else

    #if defined(__GNUC__) || defined(__clang__)

        #define BIG_64(x) (__builtin_bswap64((uint64_t)(x)))
        #define BIG_32(x) (__builtin_bswap32((uint32_t)(x)))

    #elif defined(_MSC_VER)

        #include <intrin.h>
        #define BIG_64(x) (_byteswap_uint64((uint64_t)(x)))
        #define BIG_32(x) (_byteswap_uint32((uint32_t)(x)))

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

    #endif

    #define BIG_DOUBLE(x) do { \
        uint64_t __temp; \
        memcpy(&__temp, &(x), 8); \
        __temp = BIG_64(__temp); \
        memcpy(&(x), &__temp, 8); \
    } while (0)

#endif


#if defined(_WIN32) || defined(_WIN64)

    #include <io.h>

    #define _ftruncate(file, size) \
        _chsize_s(_fileno(file), size) != 0

#elif defined(_POSIX_VERSION)

    #include <unistd.h>

    #define _ftruncate(file, size) \
        ftruncate(fileno(file), size) != 0

#else

    #define _ftruncate(file, size) \
        false

#endif


static inline bool memcmp_small(const void *s1, const void *s2, size_t size)
{
    if (size == 0)
        return true;

    const char *p1 = s1;
    const char *p2 = s2;

    while (size >= 8)
    {
        uint64_t v1, v2;
        memcpy(&v1, p1, 8);
        memcpy(&v2, p2, 8);

        if (v1 != v2)
            return false;
        
        p1   += 8;
        p2   += 8;
        size -= 8;
    }

    while (size >= 4)
    {
        uint32_t v1, v2;
        memcpy(&v1, p1, 4);
        memcpy(&v2, p2, 4);

        if (v1 != v2)
            return false;
        
        p1   += 4;
        p2   += 4;
        size -= 4;
    }

    while (--size)
    {
        if (*p1++ != *p2++)
            return false;
    }

    return true;
}



#endif // CMSGPACK_INTERNALS_H