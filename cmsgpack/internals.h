#ifndef INTERNALS_H
#define INTERNALS_H


///////////////////
//   INTERNALS   //
///////////////////

#ifdef IS_BIG_ENDIAN

    #define BIG_64(x) (x)
    #define BIG_DOUBLE(x) (x)

#else

    #if defined(__GNUC__) || defined(__clang__)

        #define BIG_64(x) (__builtin_bswap64((uint64_t)(x)))
        #define BIG_32(x) (__builtin_bswap32((uint32_t)(x)))
        #define BIG_16(x) (__builtin_bswap16((uint16_t)(x)))

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


#define _always_inline Py_ALWAYS_INLINE

#if (defined(__GNUC__) || defined(__clang__)) 

    #define _LIKELY(cond) __builtin_expect(!!(cond), 1)
    #define _UNLIKELY(cond) __builtin_expect(!!(cond), 0)

#else

    #define _LIKELY(cond) (cond)
    #define _UNLIKELY(cond) (cond)

#endif


#if defined(_WIN32) || defined(_WIN64)
    #include <io.h>

    static int _ftruncate_and_close(FILE *file, const size_t size, const char *fname)
    {
        int state = _chsize_s(_fileno(fileobj), size) != 0;
        fclose(file);
        return state;
    }
#elif defined(_POSIX_VERSION)
    #include <unistd.h>

    static int _ftruncate_and_close(FILE *file, const size_t size, const char *fname)
    {
        int state = ftruncate(fileno(file), size) != 0;
        fclose(file);
        return state;
    }
#else
    static int _ftruncate_and_close(FILE *file, const size_t size, const char *fname)
    {
        file = freopen(fname, "r+b", file);

        if (_UNLIKELY(file == NULL))
            return -1;
        
        char *tmp = (char *)malloc(size);

        if (_UNLIKELY(tmp == NULL))
        {
            fclose(file);
            return -1;
        }
        
        if (_UNLIKELY(fread(tmp, 1, size, file) != size))
        {
            fclose(file);
            free(tmp);
            return -1;
        }

        file = freopen(fname, "wb", file);

        if (_UNLIKELY(file == NULL))
        {
            free(tmp);
            return -1;
        }

        setvbuf(file, NULL, _IONBF, 0);

        const size_t written = fwrite(tmp, 1, size, file);

        free(tmp);

        return written == size ? 0 : -1;
    }
#endif

#endif // INTERNALS_H