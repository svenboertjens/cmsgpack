#ifndef CMSGPACK_INTERNALS_H
#define CMSGPACK_INTERNALS_H


//////////////////////
//  SUPPORT CHECKS  //
//////////////////////

// MSVC is not supported
#ifdef _MSC_VER
    #error "The MSVC compiler is not supported due to its lack of C standard support"
#endif

// Dummy __has_builtin macro always resulting in false
#ifndef __has_builtin
    #define __has_builtin(name) 0
#endif


///////////////////
//   INTERNALS   //
///////////////////

#include <Python.h>
#include <stdbool.h>

#if Py_BIG_ENDIAN == 1
    #define _big_endian
#endif

#ifdef __always_inline
    #define _always_inline __always_inline
#else
    #define _always_inline inline
#endif


#ifdef Py_NOGIL

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


#ifdef _big_endian

    #define BIG_64(x) (x)
    #define BIG_32(x) (x)
    #define BIG_DOUBLE(x) (NULL)

#else

    #if __has_builtin(__builtin_bswap64) && __has_builtin(__builtin_bswap32)

        #define BIG_64(x) (__builtin_bswap64((uint64_t)(x)))
        #define BIG_32(x) (__builtin_bswap32((uint32_t)(x)))

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
        uint64_t tmp; \
        memcpy(&tmp, &(x), 8); \
        tmp = BIG_64(tmp); \
        memcpy(&(x), &tmp, 8); \
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


static _always_inline bool memcmp_small(const void *s1, const void *s2, size_t size)
{
    char *buf1 = (char *)s1;
    char *buf2 = (char *)s2;

    if (size == 0)
        return true;

    while (size >= 8)
    {
        uint64_t v1, v2;
        memcpy(&v1, buf1, 8);
        memcpy(&v2, buf2, 8);

        if (v1 != v2)
            return false;
        
        buf1 += 8;
        buf2 += 8;
        size -= 8;
    }

    while (size >= 4)
    {
        uint32_t v1, v2;
        memcpy(&v1, buf1, 4);
        memcpy(&v2, buf2, 4);

        if (v1 != v2)
            return false;
        
        buf1 += 4;
        buf2 += 4;
        size -= 4;
    }

    while (size--)
    {
        if (*buf1++ != *buf2++)
            return false;
    }

    return true;
}

#endif // CMSGPACK_INTERNALS_H
