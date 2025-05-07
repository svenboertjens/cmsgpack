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


#if defined(Py_GIL_DISABLED)

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

    // Simulate a false return when we don't have a file truncate function
    #define _ftruncate(file, size) \
        false

#endif


#endif // CMSGPACK_INTERNALS_H
