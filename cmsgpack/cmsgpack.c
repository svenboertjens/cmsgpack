/**
 * Copyright 2024 Sven Boertjens
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define PY_SSIZE_T_CLEAN

#include <Python.h>
#include <stdbool.h>

#include "internals.h"


////////////////////
//    TYPEDEFS    //
////////////////////

typedef struct {
    PyBytesObject *ob_base; // Base of allocated PyBytes object
    char *base;             // Base of buffer to write to
    size_t offset;          // Writing offset relative to the buffer base
    size_t allocated;       // Space allocated for the buffer (excl PyBytesObject size)
} buffer_t;


///////////////////
//     MASKS     //
///////////////////

/**
 * 0b000 (uint)
 * 0b100 (array, map)
 * 0b101 (str)
 * 0b110 (non-fixed)
 * 0b111 (int)
 */


// Integers
#define DT_UINT_FIXED 0x00ULL
#define DT_UINT_BIT08 0xCCULL
#define DT_UINT_BIT16 0xCDULL
#define DT_UINT_BIT32 0xCEULL
#define DT_UINT_BIT64 0xCFULL

#define UINT_FIXED_MAXVAL 0x7FULL
#define UINT_BIT08_MAXVAL 0xFFULL
#define UINT_BIT16_MAXVAL 0xFFFFULL
#define UINT_BIT32_MAXVAL 0xFFFFFFFFULL
#define UINT_BIT64_MAXVAL 0xFFFFFFFFFFFFFFFFULL

#define DT_INT_FIXED 0xE0ULL
#define DT_INT_BIT08 0xD0ULL
#define DT_INT_BIT16 0xD1ULL
#define DT_INT_BIT32 0xD2ULL
#define DT_INT_BIT64 0xD3ULL

#define INT_FIXED_MAXVAL -32LL
#define INT_BIT08_MAXVAL -128LL
#define INT_BIT16_MAXVAL -32768LL
#define INT_BIT32_MAXVAL -2147483648LL
#define INT_BIT64_MAXVAL -9223372036854775808LL

// Floats
#define DT_FLOAT_BIT32 0xCAULL
#define DT_FLOAT_BIT64 0xCBULL

// Strings
#define DT_STR_FIXED 0xA0ULL
#define DT_STR_SHORT 0xD9ULL
#define DT_STR_MEDIUM 0xDAULL
#define DT_STR_LARGE 0xDBULL

#define STR_FIXED_MAXSIZE 0x1FULL
#define STR_SHORT_MAXSIZE 0xFFULL
#define STR_MEDIUM_MAXSIZE 0xFFFFULL
#define STR_LARGE_MAXSIZE 0xFFFFFFFFULL

// Arrays
#define DT_ARR_FIXED 0x90ULL
#define DT_ARR_MEDIUM 0xDCULL
#define DT_ARR_LARGE 0xDDULL

#define ARR_FIXED_MAXITEMS 0x0FULL
#define ARR_MEDIUM_MAXITEMS 0xFFFFULL
#define ARR_LARGE_MAXITEMS 0xFFFFFFFFULL

// Maps
#define DT_MAP_FIXED 0x80ULL
#define DT_MAP_MEDIUM 0xDEULL
#define DT_MAP_LARGE 0xDFULL

#define MAP_FIXED_MAXPAIRS 0x0FULL
#define MAP_MEDIUM_MAXPAIRS 0xFFFFULL
#define MAP_LARGE_MAXPAIRS 0xFFFFFFFFULL

// States
#define DT_NIL   0xC0ULL
#define DT_TRUE  0xC3ULL
#define DT_FALSE 0xC2ULL

// Binary
#define DT_BIN_SHORT 0xC4ULL
#define DT_BIN_MEDIUM 0xC5ULL
#define DT_BIN_LARGE 0xC6ULL

#define BIN_SHORT_MAXSIZE 0x0FULL
#define BIN_MEDIUM_MAXSIZE 0xFFFFULL
#define BIN_LARGE_MAXSIZE 0xFFFFFFFFULL

// Extension Types
#define DT_EXT_FIX1 0xD4ULL
#define DT_EXT_FIX2 0xD5ULL
#define DT_EXT_FIX4 0xD6ULL
#define DT_EXT_FIX8 0xD7ULL
#define DT_EXT_FIX16 0xD8ULL

#define DT_EXT_SHORT 0xC7ULL
#define DT_EXT_MEDIUM 0xC8ULL
#define DT_EXT_LARGE 0xC9ULL

#define EXT_SHORT_MAXSIZE 0xFFULL
#define EXT_MEDIUM_MAXSIZE 0xFFFFULL
#define EXT_LARGE_MAXSIZE 0xFFFFFFFFULL


////////////////////
//    METADATA    //
////////////////////

#define INCBYTE (b->base + b->offset++)

static inline void write_mask(buffer_t *b, const unsigned char mask, const size_t size, const size_t nbytes)
{
    if (nbytes == 0)
    {
        INCBYTE[0] = mask | (unsigned char)size;
    }
    else if (nbytes == 1)
    {
        const uint16_t mdata = BIG_16(size | (mask << 8));
        const size_t mdata_off = 0;
        memcpy(b->base + b->offset, (char *)(&mdata) + mdata_off, 2);
        b->offset += 2;
    }
    else if (nbytes == 2)
    {
        const uint32_t mdata = BIG_32(size | (mask << 16));
        const size_t mdata_off = 1;
        memcpy(b->base + b->offset, (char *)(&mdata) + mdata_off, 3);
        b->offset += 3;
    }
    else if (nbytes == 4)
    {
        const uint64_t mdata = BIG_64((uint64_t)size | ((uint64_t)mask << 32));
        const size_t mdata_off = 3;
        memcpy(b->base + b->offset, (char *)(&mdata) + mdata_off, 5);
        b->offset += 5;
    }
}

static inline bool write_str_metadata(buffer_t *b, const size_t size)
{
    if (size <= STR_FIXED_MAXSIZE)
    {
        write_mask(b, DT_STR_FIXED, size, 0);
    }
    else if (size <= STR_SHORT_MAXSIZE)
    {
        write_mask(b, DT_STR_SHORT, size, 1);
    }
    else if (size <= STR_MEDIUM_MAXSIZE)
    {
        write_mask(b, DT_STR_MEDIUM, size, 2);
    }
    else if (_LIKELY(size <= STR_LARGE_MAXSIZE)) // Set as likely as this being negative is an error case
    {
        write_mask(b, DT_STR_LARGE, size, 4);
    }
    else
    {
        PyErr_Format(PyExc_ValueError, "String values can only hold up to 0xFFFFFFFF bytes in data, got %zu bytes", size);
        return false;
    }

    return true;
}

static inline bool write_array_metadata(buffer_t *b, const size_t nitems)
{
    if (nitems <= ARR_FIXED_MAXITEMS)
    {
        write_mask(b, DT_ARR_FIXED, nitems, 0);
    }
    else if (nitems <= ARR_MEDIUM_MAXITEMS)
    {
        write_mask(b, DT_ARR_MEDIUM, nitems, 2);
    }
    else if (_LIKELY(nitems <= ARR_LARGE_MAXITEMS))
    {
        write_mask(b, DT_ARR_LARGE, nitems, 4);
    }
    else
    {
        PyErr_Format(PyExc_ValueError, "Array values can only hold up to 0xFFFFFFFF items, got %zu items", nitems);
        return false;
    }

    return true;
}

static inline bool write_map_metadata(buffer_t *b, const size_t npairs)
{
    if (npairs <= MAP_FIXED_MAXPAIRS)
    {
        write_mask(b, DT_MAP_FIXED, npairs, 0);
    }
    else if (npairs <= MAP_MEDIUM_MAXPAIRS)
    {
        write_mask(b, DT_MAP_MEDIUM, npairs, 2);
    }
    else if (_LIKELY(npairs <= MAP_LARGE_MAXPAIRS))
    {
        write_mask(b, DT_MAP_LARGE, npairs, 4);
    }
    else
    {
        PyErr_Format(PyExc_ValueError, "Map values can only hold up to 0xFFFFFFFF pairs, got %zu pairs", npairs);
        return false;
    }

    return true;
}

static inline void write_int_metadata_and_data(buffer_t *b, const int64_t num)
{
    if (num >= 0 && (uint64_t)num <= UINT_FIXED_MAXVAL)
    {
        write_mask(b, DT_UINT_FIXED, num, 0);
        return;
    }
    else if (num < 0 && num >= INT_FIXED_MAXVAL)
    {
        write_mask(b, DT_INT_FIXED, num, 0);
        return;
    }

    // Convert negative numbers into positive ones, upscaled to match the uint max values
    const uint64_t pnum = num >= 0 ? num : ((~num + 1) << 1) - 1;

    if (pnum <= UINT_BIT08_MAXVAL)
    {
        write_mask(b, num >= 0 ? DT_UINT_BIT08 : DT_INT_BIT08, num, 1);
    }
    else if (pnum <= UINT_BIT16_MAXVAL)
    {
        write_mask(b, num >= 0 ? DT_UINT_BIT16 : DT_INT_BIT16, num, 2);
    }
    else if (pnum <= UINT_BIT32_MAXVAL)
    {
        write_mask(b, num >= 0 ? DT_UINT_BIT32 : DT_INT_BIT32, num, 4);
    }
    else
    {
        INCBYTE[0] = num >= 0 ? DT_UINT_BIT64 : DT_INT_BIT64;

        const uint64_t _num = BIG_64(num);
        memcpy(b->base + b->offset, &_num, 8);
        b->offset += 8;
    }
}

static inline bool write_bin_metadata(buffer_t *b, const size_t size)
{
    if (size <= BIN_SHORT_MAXSIZE)
    {
        write_mask(b, DT_BIN_SHORT, size, 1);
    }
    else if (size <= BIN_MEDIUM_MAXSIZE)
    {
        write_mask(b, DT_BIN_MEDIUM, size, 2);
    }
    else if (_LIKELY(size <= BIN_LARGE_MAXSIZE))
    {
        write_mask(b, DT_BIN_LARGE, size, 4);
    }
    else
    {
        PyErr_Format(PyExc_ValueError, "Binary values can only hold up to 0xFFFFFFFF bytes in data, got %zu bytes", size);
        return false;
    }

    return true;
}

static inline bool write_ext_metadata(buffer_t *b, const size_t size, const unsigned char id)
{
    const bool is_baseof2 = (size & (size - 1)) == 0;

    if (size <= 16 && is_baseof2)
    {
        const unsigned char fixmask = (
            size == 16 ? DT_EXT_FIX16 :
            DT_EXT_FIX1 | (32 - LEADING_ZEROES_32(size))
        );

        write_mask(b, fixmask, (size_t)id, 1);
        return true;
    }
    
    if (size <= EXT_SHORT_MAXSIZE)
    {
        write_mask(b, DT_EXT_SHORT, size, 1);
    }
    else if (size <= EXT_MEDIUM_MAXSIZE)
    {
        write_mask(b, DT_EXT_MEDIUM, size, 2);
    }
    else if (size <= EXT_LARGE_MAXSIZE)
    {
        write_mask(b, DT_EXT_LARGE, size, 4);
    }
    else
    {
        PyErr_Format(PyExc_ValueError, "Ext values can only hold up to 0xFFFFFFFF bytes in data, got %zu bytes", size);
        return false;
    }

    INCBYTE[0] = id;

    return true;
}


/////////////////////
//  DATA FETCHING  //
/////////////////////

static void fetch_str(PyObject *obj, char **base, size_t *size)
{
    if (_LIKELY(PyUnicode_IS_COMPACT_ASCII(obj))) {
        *size = ((PyASCIIObject *)obj)->length;
        *base = (char *)(((PyASCIIObject *)obj) + 1);
    }
    else
    {
        *size = ((PyCompactUnicodeObject *)obj)->utf8_length;
        *base = ((PyCompactUnicodeObject *)obj)->utf8;
    }

    if (_UNLIKELY(base == NULL))
        *base = (char *)PyUnicode_AsUTF8AndSize(obj, (Py_ssize_t *)size);
}

static inline void fetch_bytes(PyObject *obj, char **base, size_t *size)
{
    *base = PyBytes_AS_STRING(obj);
    *size = PyBytes_GET_SIZE(obj);
}

static bool fetch_long(PyObject *obj, int64_t *num)
{
    // Use custom integer extraction on 3.12+
    #if PY_VERSION_HEX >= 0x030c0000

    PyLongObject *lobj = (PyLongObject *)obj;

    const size_t digits = lobj->long_value.lv_tag >> _PyLong_NON_SIZE_BITS;

    switch (digits)
    {
    case 0:
    case 1:
    {
        *num = lobj->long_value.ob_digit[0];
        break;
    }
    case 2:
    {
        *num = lobj->long_value.ob_digit[0] |
               (lobj->long_value.ob_digit[1] << PyLong_SHIFT);
        
        break;
    }
    case 3:
    {
        *num = (uint64_t)lobj->long_value.ob_digit[0] |
               ((uint64_t)lobj->long_value.ob_digit[1] << (PyLong_SHIFT * 1)) |
               ((uint64_t)lobj->long_value.ob_digit[2] << (PyLong_SHIFT * 2));
        
        const digit dig3 = lobj->long_value.ob_digit[2];
        const digit dig3_overflow = ((1ULL << (64 - (2 * PyLong_SHIFT))) - 1);

        // Don't break if there's overflow
        if (_LIKELY(dig3 < dig3_overflow))
            break;
    }
    default:
    {
        PyErr_SetString(PyExc_OverflowError, "integers cannot be more than 8 bytes");
        return false;
    }
    }

    bool neg = (lobj->long_value.lv_tag & _PyLong_SIGN_MASK) != 0;

    if (neg)
        *num = -*num;

    return true;

    #else

    int overflow = 0;
    *num = PyLong_AsLongLongAndOverflow(obj, &overflow);

    if (_UNLIKELY(overflow != 0))
    {
        PyErr_SetString(PyExc_OverflowError, "Python ints cannot be more than 8 bytes");
        return false;
    }

    return true;

    #endif
}

static void fetch_float(PyObject *obj, double *num)
{
    *num = PyFloat_AS_DOUBLE(obj);
    BIG_DOUBLE(*num);
    return;
}

////////////////////
//   ALLOCATION   //
////////////////////


// Copied from cpython/Objects/bytesobject.c, as required for allocations
#include <stddef.h>
#define PyBytesObject_SIZE (offsetof(PyBytesObject, ob_sval) + 1)


// Initial size to allocate for a buffer
#define INITIAL_ALLOC_SIZE 512

// Size to allocate up front for a buffer
static size_t avg_alloc_size = 128;
// Size to allocate per item in containers
static size_t avg_item_size = 12;

// Minimum values for the dynamic allocation values to prevent severe underallocation or underflow
#define ALLOC_SIZE_MIN 64
#define ITEM_SIZE_MIN 4

static void update_allocation_settings(const size_t reallocs, const size_t offset, const size_t initial_allocated, const size_t nitems)
{
    if (reallocs != 0)
    {
        // For safety, check if the offset isn't smaller than what we allocated initially (can happen in rare cases)
        if (_UNLIKELY(offset < initial_allocated))
        {
            // Add 16 to alloc size to prevent the unnecessary realloc next time, and return
            avg_alloc_size += 16;
            return;
        }

        const size_t difference = offset - (initial_allocated / 1.25);
        const size_t med_diff = difference / nitems;

        avg_alloc_size += difference;
        avg_item_size += med_diff;
    }
    else
    {
        const size_t difference = initial_allocated - offset;
        const size_t med_diff = difference / (nitems);

        const size_t diff_small = difference >> 4;
        const size_t med_small = med_diff >> 2;

        if (diff_small + ALLOC_SIZE_MIN < avg_alloc_size)
            avg_alloc_size -= diff_small;
        else
            avg_alloc_size = ALLOC_SIZE_MIN;

        if (med_small + ITEM_SIZE_MIN < avg_item_size)
            avg_item_size -= med_small;
        else
            avg_item_size = ITEM_SIZE_MIN;
    }
}

// Ensure that there's ENSURE_SIZE space available in a buffer
static inline bool buffer_ensure_space(buffer_t *b, size_t ensure_size, size_t *reallocs)
{
    const size_t required_size = b->offset + ensure_size;
    if (_UNLIKELY(required_size >= b->allocated))
    {
        // Scale the buffer by 1.5 times the required size
        b->allocated = required_size * 1.5;

        PyBytesObject *new_ob = (PyBytesObject *)PyObject_Realloc(b->ob_base, PyBytesObject_SIZE + b->allocated);

        if (_UNLIKELY(new_ob == NULL))
        {
            PyErr_NoMemory();
            return false;
        }

        b->ob_base = new_ob;
        b->base = PyBytes_AS_STRING(new_ob);

        (*reallocs)++;
    }

    return true;
}


////////////////////
//    ENCODING    //
////////////////////

static bool encode_object(PyObject *obj, buffer_t *b, size_t *reallocs)
{
    PyTypeObject *tp = Py_TYPE(obj);

    if (tp == &PyUnicode_Type)
    {
        char *base;
        size_t size;

        fetch_str(obj, &base, &size);

        // Ensure there's enough space for metadata plus the string size
        if (_UNLIKELY(!buffer_ensure_space(b, 8 + size, reallocs)))
            return false;
        
        write_str_metadata(b, size);
        
        memcpy(b->base + b->offset, base, size);
        b->offset += size;

        return true;
    }
    else if (tp == &PyBytes_Type)
    {
        char *base;
        size_t size;

        fetch_bytes(obj, &base, &size);

        if (_UNLIKELY(!buffer_ensure_space(b, 8 + size, reallocs)))
            return false;
        
        write_bin_metadata(b, size);
        
        memcpy(b->base + b->offset, base, size);
        b->offset += size;

        return true;
    }

    if (_UNLIKELY(!buffer_ensure_space(b, 9, reallocs)))
        return false;

    if (tp == &PyLong_Type)
    {
        int64_t num;

        if (_UNLIKELY(!fetch_long(obj, &num)))
            return false;

        write_int_metadata_and_data(b, num);
    }
    else if (tp == &PyFloat_Type)
    {
        double num;
        fetch_float(obj, &num);

        INCBYTE[0] = DT_FLOAT_BIT64;

        memcpy(b->base + b->offset, &num, 8);
        b->offset += 8;
    }
    else if (tp == &PyBool_Type)
    {
        // Write a typemask based on if the value is true or false
        INCBYTE[0] = obj == Py_True ? DT_TRUE : DT_FALSE;
    }
    else if (tp == Py_TYPE(Py_None)) // Public API doesn't expose a PyNone_Type
    {
        INCBYTE[0] = DT_NIL;
    }
    else if (tp == &PyList_Type)
    {
        // Get the number of items in the list
        const size_t nitems = PyList_GET_SIZE(obj);

        // Write metadata of the array
        write_array_metadata(b, nitems);

        // Iterate over all items in the array and encode them
        for (size_t i = 0; i < nitems; ++i)
        {
            if (_UNLIKELY(!encode_object(PyList_GET_ITEM(obj, i), b, reallocs)))
                return false;
        }
    }
    else if (tp == &PyDict_Type)
    {
        // Get the number of pairs in the dict
        const size_t npairs = PyDict_GET_SIZE(obj);

        write_map_metadata(b, npairs);

        // Iterate over all pairs and encode them
        Py_ssize_t pos = 0;
        for (size_t i = 0; i < npairs; ++i)
        {
            PyObject *key;
            PyObject *val;

            PyDict_Next(obj, &pos, &key, &val);

            if (
                _UNLIKELY(!encode_object(key, b, reallocs)) ||
                _UNLIKELY(!encode_object(val, b, reallocs))
            ) return false;
        }
    }
    else
    {
        // Custom type logic will be placed here later
        PyErr_Format(PyExc_NotImplementedError, "Received unsupported type: '%s'", Py_TYPE(obj)->tp_name);
        return false;
    }

    return true;
}

static PyObject *encode(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    if (_UNLIKELY(nargs < 1))
    {
        PyErr_SetString(PyExc_ValueError, "Expected at least the 'obj' argument");
        return NULL;
    }

    PyObject *obj = args[0];

    buffer_t b;

    // Handle allocations for lists and dicts separate from singular items
    size_t nitems;
    size_t initial_alloc;

    PyTypeObject *tp = Py_TYPE(obj);
    if (tp == &PyList_Type || tp == &PyDict_Type)
    {
        // Set the base of the initial alloc size to the average alloc size
        initial_alloc = avg_alloc_size;

        // Allocate extra space based on the number of items/pairs
        if (tp == &PyList_Type)
        {
            nitems = PyList_GET_SIZE(obj);
            initial_alloc += nitems * avg_item_size;
        }
        else if (tp == &PyDict_Type)
        {
            const size_t npairs = PyDict_GET_SIZE(obj);
            nitems = npairs * 2; // One pair is two items
            initial_alloc += nitems * avg_item_size;
        }

        // Manually allocate space for an object, and include space for the bytes object data
        b.ob_base = (PyBytesObject *)PyObject_Malloc(PyBytesObject_SIZE + initial_alloc);

        if (b.ob_base == NULL)
        {
            return PyErr_NoMemory();
        }

        // Set the other data of the buffer struct
        b.base = PyBytes_AS_STRING(b.ob_base);
        b.allocated = initial_alloc;
        b.offset = 0;
    }
    else
    {
        // Set the object base to NULL. As it's a single item, the total required size will be known once we
        // ensure the buffer has enough space. The realloc function accepts NULL and will handle it as a malloc,
        // so having the buffer be malloc'd during the buffer ensure avoids over- or underallocation.
        b.ob_base = NULL;
        b.allocated = 0;
        b.offset = 0;

        // Set number of items to zero to avoid the dynamic allocation update
        nitems = 0;
    }

    size_t reallocs = 0;
    
    if (_UNLIKELY(encode_object(obj, &b, &reallocs) == false))
    {
        // Error should be set already
        PyObject_Free(b.ob_base);
        return NULL;
    }

    if (nitems != 0)
    {
        update_allocation_settings(reallocs, b.offset, initial_alloc, nitems);
    }

    // Correct the size of the bytes object and null-terminate before returning
    Py_SET_SIZE(b.ob_base, b.offset);
    b.base[b.offset] = 0;

    // Initiate the object's refcount and type
    Py_SET_REFCNT(b.ob_base, 1);
    Py_SET_TYPE(b.ob_base, &PyBytes_Type);

    // Return the bytes object holding the data
    return (PyObject *)b.ob_base;
}


////////////////////
//    DECODING    //
////////////////////

#define FIXLEN_DT(mask) ((mask >> 5) & 0b111)

#ifdef IS_BIG_ENDIAN

#define LENMODE_1BYTE if (lenmode == 0) { \
    n = n & 0xFF; \
    b->offset += 1; \
}
#define LENMODE_2BYTE if (lenmode == 1) { \
    n = n & 0xFFFF; \
    b->offset += 2; \
}
#define LENMODE_4BYTE if (lenmode == 2) { \
    n = n & 0xFFFFFFFF; \
    b->offset += 4; \
}

#else

#define LENMODE_1BYTE if (lenmode == 0) { \
    n = (n >> 56) & 0xFF; \
    b->offset += 1; \
}
#define LENMODE_2BYTE if (lenmode == 1) { \
    n = (n >> 48) & 0xFFFF; \
    b->offset += 2; \
}
#define LENMODE_4BYTE if (lenmode == 2) { \
    n = (n >> 32) & 0xFFFFFFFF; \
    b->offset += 4; \
}

#endif

#define INVALID_MSG "Received invalid encoded data"


// FNV-1a hashing algorithm
static inline uint32_t fnv1a(const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t hash = 0x811c9dc5; // FNV offset basis for 32-bit

    for (size_t i = 0; i < len; ++i) {
        hash ^= bytes[i];       // XOR the byte into the hash
        hash *= 0x01000193;     // Multiply by FNV prime
    }

    return hash;
}

#define FIXASCII_COMMON_SLOTS 256
static PyObject **fixascii_common; // Allocated on module init

static PyObject *fixstr_decode(buffer_t *b, const size_t size)
{
    const size_t hash = (size_t)(fnv1a(b->base + b->offset, size) & (FIXASCII_COMMON_SLOTS - 1));
    PyObject *match = fixascii_common[hash];

    if (match != NULL)
    {
        const char *cbase = (const char *)((PyASCIIObject *)match + 1);
        const size_t csize = ((PyASCIIObject *)match)->length;

        if (_LIKELY(csize == size && memcmp(cbase, b->base + b->offset, size) == 0))
        {
            Py_INCREF(match);
            return match;
        }
    }

    // No common value, create a new one
    PyObject *obj = PyUnicode_DecodeUTF8(b->base + b->offset, size, "strict");

    // Add to commons table if ascii
    if (PyUnicode_IS_COMPACT_ASCII(obj))
    {
        Py_INCREF(obj);
        fixascii_common[hash] = obj;
    }

    return obj;
}


static PyObject *decode_bytes(buffer_t *b);

static PyObject *decode_array(buffer_t *b, const size_t nitems)
{
    PyObject *list = PyList_New(nitems);

    if (_UNLIKELY(list == NULL))
        return NULL;

    for (size_t i = 0; i < nitems; ++i)
    {
        PyObject *item = decode_bytes(b);

        if (item == NULL)
        {
            Py_DECREF(list);
            return NULL;
        }
        
        PyList_SET_ITEM(list, i, item);
    }

    return list;
}

static PyObject *decode_map(buffer_t *b, const size_t npairs)
{
    PyObject *dict = _PyDict_NewPresized(npairs);

    if (_UNLIKELY(dict == NULL))
        return NULL;

    for (size_t i = 0; i < npairs; ++i)
    {
        PyObject *key;

        // Special case for fixsize strings
        const unsigned char mask = (b->base + b->offset)[0];
        if ((mask & 0b11100000) == DT_STR_FIXED)
        {
            b->offset++;

            const size_t size = mask & 0b11111;
            key = fixstr_decode(b, size);

            b->offset += size;
        }
        else
        {
            key = decode_bytes(b);

            if (_UNLIKELY(key == NULL))
            {
                Py_DECREF(dict);
                return NULL;
            }
        }

        PyObject *val = decode_bytes(b);

        if (_UNLIKELY(val == NULL))
        {
            Py_DECREF(key);
            Py_DECREF(dict);
            return NULL;
        }

        PyDict_SetItem(dict, (PyObject *)key, val);

        Py_DECREF(key);
        Py_DECREF(val);
    }

    return dict;
}

static PyObject *decode_bytes(buffer_t *b)
{
    if (b->offset >= b->allocated)
    {
        PyErr_SetString(PyExc_ValueError, INVALID_MSG " (overread the encoded data)");
        return NULL;
    }

    const unsigned char mask = (b->base + b->offset)[0];

    if ((mask & 0b11100000) != 0b11000000) // fixsize values
    {
        // Size inside of the fixed mask
        size_t n = (size_t)mask; // N has to be masked before use
        b->offset++;

        switch (FIXLEN_DT(mask))
        {
        case FIXLEN_DT(DT_STR_FIXED):
        {
            n &= 0x1F;
            PyObject *obj = fixstr_decode(b, n);
            b->offset += n;
            return obj;
        }
        case 0b000: // Possible combinations for fixuint as uses just 1 bit and we're checking the 3 upper bits
        case 0b001:
        case 0b010:
        case 0b011:
        {
            // No increment necessary, no bytes that follow the value
            return PyLong_FromUnsignedLong((unsigned long)n & 0x7F);
        }
        case FIXLEN_DT(DT_INT_FIXED):
        {
            // Also no increment, same-byte value
            return PyLong_FromLong((long)n & 0x1F);
        }
        case FIXLEN_DT(DT_MAP_FIXED): // Also catches DT_ARR_FIXED
        {
            n &= 0x0F;

            if ((mask & 0b10000) == 0b10000) // Bit 5 is 1 on array, 0 on map
            {
                return decode_array(b, n);
            }
            else
            {
                return decode_map(b, n);
            }
        }
        default:
        {
            PyErr_SetString(PyExc_ValueError, INVALID_MSG " (invalid fixlen type bits)");
            return NULL;
        }
        }
    }
    else
    {
        b->offset++;

        // Fetch the size of the bytes after the mask
        uint64_t n; // Mask correctly before using N
        memcpy(&n, b->base + b->offset, 8); // Copy 8 instead of 4 so that it also holds floats and large ints

        // Convert N back to host endianness
        n = BIG_64(n);

        // The length mode bits are stored in the lowest 2 bits
        unsigned int lenmode = mask & 0b11;

        switch ((mask >> 2) & 0b111)
        {
        case 0b110: // str
        {
            // Lenmode starts at 1 for str type, so decrement for macro use
            lenmode--;

            LENMODE_1BYTE
            else LENMODE_2BYTE
            else LENMODE_4BYTE
            else
            {
                PyErr_SetString(PyExc_ValueError, INVALID_MSG " (invalid length bits in str type)");
                return NULL;
            }

            PyObject *obj = PyUnicode_DecodeUTF8(b->base + b->offset, n, "strict");
            b->offset += n;
            return obj;
        }
        case 0b011: // uint
        {
            LENMODE_1BYTE
            else LENMODE_2BYTE
            else LENMODE_4BYTE
            // uint64_t takes up 8 bytes so no invalid data to mask away, as this is an 8-byte uint
            else b->offset += 8;

            return PyLong_FromUnsignedLongLong((unsigned long long)n);
        }
        case 0b100: // int
        {
            LENMODE_1BYTE
            else LENMODE_2BYTE
            else LENMODE_4BYTE
            else b->offset += 8;

            return PyLong_FromLongLong((long long)n);
        }
        case 0b111: // array & map
        {
            // Set lenmode to 1 or 2 manually as 2nd bit is used to specify map or array
            lenmode = (unsigned int)((mask & 1) == 1) + 1;

            LENMODE_2BYTE
            else LENMODE_4BYTE

            if ((mask & 0b10) == 0) // array
            {
                return decode_array(b, n);
            }
            else
            {
                return decode_map(b, n);
            }
        }
        case 0b000: // states
        {
            if (mask == DT_TRUE)
            {
                Py_RETURN_TRUE;
            }
            else if (mask == DT_FALSE)
            {
                Py_RETURN_FALSE;
            }
            else if (_LIKELY(mask == DT_NIL))
            {
                Py_RETURN_NONE;
            }
            else
            {
                PyErr_SetString(PyExc_ValueError, INVALID_MSG " (invalid state type)");
                return NULL;
            }

            PyObject *obj = PyBytes_FromStringAndSize(b->base + b->offset, n);
            b->offset += n;
            return obj;
        }
        case 0b010: // float
        {
            double num;

            if (mask == DT_FLOAT_BIT32)
            {
                #ifdef IS_BIG_ENDIAN
                n = n & 0xFFFFFFFF;
                #else
                n = (n >> 32) & 0xFFFFFFFF;
                #endif

                num = (double)((float)n); // Promote N to float first, the promote float to double
                b->offset += 4;
            }
            else
            {
                num = (double)n;
                b->offset += 8;
            }

            return PyFloat_FromDouble(num);
        }
        case 0b001: // bin & ext
        {
            LENMODE_1BYTE
            else LENMODE_2BYTE
            else LENMODE_4BYTE
            else
            {
                PyErr_SetString(PyExc_ValueError, INVALID_MSG " (invalid length bits in bin type, OR got currently unsupported ext type)");
                return NULL;
            }

            PyObject *obj = PyBytes_FromStringAndSize(b->base + b->offset, n);
            b->offset += n;
            return obj;
        }
        default:
        {
            PyErr_SetString(PyExc_ValueError, INVALID_MSG " (invalid varlen type bits)");
            return NULL;
        }
        }
    }

    // This shouldn't be reached
    PyErr_SetString(PyExc_ValueError, INVALID_MSG " (unknown error)");
    return NULL;
}

static PyObject *decode(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    if (_UNLIKELY(nargs) < 1)
    {
        PyErr_SetString(PyExc_ValueError, "Expected at least the 'encoded' argument");
        return NULL;
    }

    PyObject *encoded = args[0];

    if (_UNLIKELY(!PyBytes_Check(encoded)))
    {
        PyErr_Format(PyExc_ValueError, "Argument 'encoded' must be of type 'bytes', got '%s'", Py_TYPE(encoded)->tp_name);
        return NULL;
    }

    buffer_t b = {
        .base = PyBytes_AS_STRING(encoded),
        .allocated = PyBytes_GET_SIZE(encoded),
        .offset = 0,
    };

    return decode_bytes(&b);
}


////////////////////
//     MODULE     //
////////////////////

static void cleanup_module(PyObject *m)
{
    // Decref all common strings and free the buffer itself
    for (size_t i = 0; i < FIXASCII_COMMON_SLOTS; ++i)
    {
        Py_XDECREF(fixascii_common[i]);
    }

    free(fixascii_common);
}

static PyMethodDef CmsgpackMethods[] = {
    {"encode", (PyCFunction)encode, METH_FASTCALL, NULL},
    {"decode", (PyCFunction)decode, METH_FASTCALL, NULL},

    //{"validate", (PyCFunction)validate, METH_FASTCALL, NULL},

    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef cmsgpack = {
    PyModuleDef_HEAD_INIT,
    "cmsgpack",
    NULL,
    -1,
    CmsgpackMethods,
    NULL,
    NULL,
    NULL,
    (freefunc)cleanup_module,
};

PyMODINIT_FUNC PyInit_cmsgpack(void) {
    // Allocate the fixascii commons table, initialize with NULL using calloc
    fixascii_common = (PyObject **)calloc(FIXASCII_COMMON_SLOTS, sizeof(PyObject *));

    if (fixascii_common == NULL)
        return PyErr_NoMemory();

    PyObject *m = PyModule_Create(&cmsgpack);
    return m;
}

