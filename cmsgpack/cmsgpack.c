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


////////////////////
//    TYPEDEFS    //
////////////////////

#include <Python.h>

// Check if the system is big-endian
#if Py_BIG_ENDIAN == 1
#define IS_BIG_ENDIAN
#endif

// Include these after defining IS_BIG_ENDIAN, as they depend on it
#include "masks.h"
#include "internals.h"

// Struct holding buffer data for encoding
typedef struct {
    PyObject *ob_base;  // Base of allocated PyBytes object
    char *base;        // Base of buffer to write to
    size_t offset;     // Writing offset relative to the buffer base
    size_t allocated;  // Space allocated for the buffer (excluding PyBytesObject size)
} encbuffer_t;

// Struct holding buffer data for decoding
typedef struct {
    char *base;
    size_t offset;
    size_t allocated;
} decbuffer_t;

static bool encode_object(PyObject *obj, encbuffer_t *b);
static PyObject *decode_bytes(decbuffer_t *b);
static void update_allocation_settings(const bool reallocd, const size_t offset, const size_t initial_allocated, const size_t nitems);

static size_t avg_alloc_size;
static size_t avg_item_size;

// Copied from cpython/Objects/bytesobject.c. Used for byteobject reallocation
#include <stddef.h>
#define PyBytes_SIZE (offsetof(PyBytesObject, ob_sval) + 1)


/////////////////////
//  ERROR METHODS  //
/////////////////////

static void error_unsupported_type(PyObject *obj)
{
    PyErr_Format(PyExc_ValueError, "Received unsupported type '%s'", Py_TYPE(obj)->tp_name);
}

static void error_incorrect_value(const char *msg)
{
    PyErr_SetString(PyExc_ValueError, msg);
}

static void error_no_memory(void)
{
    PyErr_NoMemory();
}


/////////////////////
//  PY TYPE CHECK  //
/////////////////////

static _always_inline bool py_is_str(PyObject *obj)
{
    return PyUnicode_CheckExact(obj);
}

static _always_inline bool py_is_bin(PyObject *obj)
{
    return PyBytes_CheckExact(obj);
}

static _always_inline bool py_is_int(PyObject *obj)
{
    return PyLong_CheckExact(obj);
}

static _always_inline bool py_is_float(PyObject *obj)
{
    return PyFloat_CheckExact(obj);
}

static _always_inline bool py_is_bool(PyObject *obj)
{
    return PyBool_Check(obj);
}

static _always_inline bool py_is_nil(PyObject *obj)
{
    return obj == Py_None;
}

static _always_inline bool py_is_arr(PyObject *obj)
{
    return PyList_CheckExact(obj);
}

static _always_inline bool py_is_map(PyObject *obj)
{
    return PyDict_CheckExact(obj);
}


/////////////////////////
//  EXPOSED FUNCTIONS  //
/////////////////////////

static PyObject *encode(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    if (_UNLIKELY(nargs < 1))
    {
        PyErr_SetString(PyExc_ValueError, "Expected at least the 'obj' argument");
        return NULL;
    }

    PyObject *obj = args[0];

    encbuffer_t b;

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

        // This creates a PyBytesObject with initialized internals. We will manually realloc this pointer
        b.ob_base = (void *)PyBytes_FromStringAndSize(NULL, initial_alloc);

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
    
    if (_UNLIKELY(encode_object(obj, &b) == false))
    {
        // Error should be set already
        PyObject_Free(b.ob_base);
        return NULL;
    }

    // Update allocation settings if we worked with a container object
    if (nitems != 0)
        update_allocation_settings(b.allocated != initial_alloc, b.offset, initial_alloc, nitems);

    // Correct the size of the bytes object and null-terminate before returning
    Py_SET_SIZE(b.ob_base, b.offset);
    b.base[b.offset] = 0;

    // Return the bytes object holding the data
    return (PyObject *)b.ob_base;
}

static PyObject *decode(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    if (_UNLIKELY(nargs) < 1)
    {
        PyErr_SetString(PyExc_ValueError, "Expected at least the 'encoded' argument");
        return NULL;
    }

    PyObject *encoded = args[0];

    Py_buffer buffer;
    if (PyObject_GetBuffer(encoded, &buffer, PyBUF_READ) != 0)
        return NULL;

    decbuffer_t b = {
        .base = buffer.buf,
        .allocated = buffer.len,
        .offset = 0,
    };

    if (b.allocated <= 0)
    {
        PyBuffer_Release(&buffer);
        PyErr_Format(PyExc_ValueError, "Expected the buffer to have a size of at least 1, got %zi", b.allocated);
        return NULL;
    }

    PyObject *result = decode_bytes(&b);

    PyBuffer_Release(&buffer);
    return result;
}


////////////////////
//  PY ITERATORS  //
////////////////////

static _always_inline bool iterate_over_arr(encbuffer_t *b, PyObject *obj, const size_t nitems)
{
    for (size_t i = 0; i < nitems; ++i)
    {
        if (_UNLIKELY(!encode_object(PyList_GET_ITEM(obj, i), b)))
            return false;
    }

    return true;
}

static _always_inline bool iterate_over_map(encbuffer_t *b, PyObject *obj, const size_t npairs)
{
    Py_ssize_t pos = 0;
    for (size_t i = 0; i < npairs; ++i)
    {
        PyObject *key;
        PyObject *val;

        PyDict_Next(obj, &pos, &key, &val);

        if (_UNLIKELY(
            !encode_object(key, b) ||
            !encode_object(val, b)
        )) return false;
    }

    return true;
}


/////////////////////
//  PY FETCH DATA  //
/////////////////////

static _always_inline void py_str_data(PyObject *obj, char **base, size_t *size)
{
    if (_LIKELY(PyUnicode_IS_COMPACT_ASCII(obj)))
    {
        *size = ((PyASCIIObject *)obj)->length;
    *base = (char *)(((PyASCIIObject *)obj) + 1);
    }
    else
    {
        *base = (char *)PyUnicode_AsUTF8AndSize(obj, (Py_ssize_t *)size);
    }
}

static _always_inline void py_bin_data(PyObject *obj, char **base, size_t *size)
{
    *base = PyBytes_AS_STRING(obj);
    *size = PyBytes_GET_SIZE(obj);
}

static _always_inline bool py_int_data(PyObject *obj, uint64_t *num, bool *neg)
{
    // Use custom integer extraction on Python 3.12+
    #if PY_VERSION_HEX >= 0x030c0000

    PyLongObject *lobj = (PyLongObject *)obj;

    const size_t digits = lobj->long_value.lv_tag >> _PyLong_NON_SIZE_BITS;
    uint64_t n = 0;

    if (digits >= 1)
    {
        n |= (uint64_t)lobj->long_value.ob_digit[0];
    }
    if (digits >= 2)
    {
        n |= (uint64_t)lobj->long_value.ob_digit[1] << PyLong_SHIFT;
    }
    if (digits >= 3)
    {
        const uint64_t dig3 = (uint64_t)lobj->long_value.ob_digit[2];

        const uint64_t dig3_overflow_val = (1ULL << (64 - (2 * PyLong_SHIFT))) - 1;
        const bool dig3_overflow = dig3 > dig3_overflow_val;

        if (_UNLIKELY(digits > 3 || dig3_overflow))
        {
            PyErr_SetString(PyExc_OverflowError, "integers cannot be more than 8 bytes");
            return false;
        }

        n |= dig3 << (2 * PyLong_SHIFT);
    }

    *neg = (lobj->long_value.lv_tag & _PyLong_SIGN_MASK) != 0;
    if (*neg)
        n = -n;
    
    *num = n;

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

static _always_inline void py_float_data(PyObject *obj, double *num)
{
    *num = PyFloat_AS_DOUBLE(obj);
    BIG_DOUBLE(*num);
    return;
}


///////////////////
//  BYTES TO PY  //
///////////////////

static _always_inline PyObject *anystr_to_py(const char *ptr, const size_t size)
{
    return PyUnicode_DecodeUTF8(ptr, size, "strict");
}

static _always_inline PyObject *anyfloat_to_py(const double num)
{
    return PyFloat_FromDouble(num);
}

static _always_inline PyObject *anybin_to_py(const char *ptr, const size_t size)
{
    return PyBytes_FromStringAndSize(ptr, size);
}

static _always_inline PyObject *true_to_py(void)
{
    Py_RETURN_TRUE;
}

static _always_inline PyObject *false_to_py(void)
{
    Py_RETURN_FALSE;
}

static _always_inline PyObject *nil_to_py(void)
{
    Py_RETURN_NONE;
}

/////////////////////
//  COMMON CACHES  //
/////////////////////

// The number of slots to use in the common value caches
#define COMMONCACHE_SLOTS 256

// FNV-1a hashing algorithm
static _always_inline uint32_t fnv1a(const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t hash = 0x811c9dc5;

    for (size_t i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= 0x01000193;
    }

    return hash;
}

static PyASCIIObject **fixascii_common;

static _always_inline PyObject *fixstr_to_py(const char *ptr, const size_t size)
{
    const size_t hash = (size_t)(fnv1a(ptr, size) % COMMONCACHE_SLOTS);
    PyASCIIObject *match = fixascii_common[hash];

    if (match != NULL)
    {
        const char *cbase = (const char *)(match + 1);
        const size_t csize = match->length;
        
        if (_LIKELY(csize == size && memcmp(cbase, ptr, size) == 0))
        {
            Py_INCREF(match);
            return (PyObject *)match;
        }
    }

    // No common value, create a new one
    PyObject *obj = PyUnicode_DecodeUTF8(ptr, size, "strict");

    // Add to commons table if ascii
    if (PyUnicode_IS_COMPACT_ASCII(obj))
    {
        Py_XDECREF(match);
        Py_INCREF(obj);
        fixascii_common[hash] = (PyASCIIObject *)obj;
    }

    return obj;
}

typedef struct {
    intptr_t val;
    PyObject *obj;
} smallint_common_t;
smallint_common_t *smallint_common;

static _always_inline PyObject *anyint_to_py(const uint64_t num, const bool is_uint)
{
    // Signed version of the number, for negative representations
    const int64_t snum = (int64_t)num;

    // Only use commons table on small numbers, otherwise only rarely useful
    if (num >= 0 ? num <= 8192 : snum >= -1024)
    {
        // Use the number for the hash directly
        const size_t hash = num % COMMONCACHE_SLOTS;
        smallint_common_t match = smallint_common[hash];

        if (match.obj != NULL)
        {
            if (_LIKELY(match.val == (intptr_t)snum))
            {
                Py_INCREF(match.obj);
                return match.obj;
            }

            Py_DECREF(match.obj);
        }

        // Cache miss, create new value and add it to the list
        PyObject *obj = PyLong_FromLong((long)num);
        Py_INCREF(obj);

        smallint_common[hash] = (smallint_common_t){
            .obj = obj,
            .val = (intptr_t)snum,
        };

        return obj;
    }

    if (is_uint)
        return PyLong_FromUnsignedLongLong((unsigned long long)num);
    
    return PyLong_FromLongLong((long long)num);
}

static bool setup_common_caches(void)
{
    fixascii_common = (PyASCIIObject **)calloc(COMMONCACHE_SLOTS, sizeof(PyASCIIObject *));
    if (fixascii_common == NULL)
    {
        return false;
    }
    
    smallint_common = (smallint_common_t *)calloc(COMMONCACHE_SLOTS, sizeof(smallint_common_t));
    if (smallint_common == NULL)
    {
        free(fixascii_common);
        return false;
    }

    return true;
}

static void cleanup_common_caches(void)
{
    for (size_t i = 0; i < COMMONCACHE_SLOTS; ++i)
    {
        Py_XDECREF(fixascii_common[i]);
        Py_XDECREF(smallint_common[i].obj);
    }

    free(fixascii_common);
    free(smallint_common);
}


//////////////////////////
//  BYTES TO CONTAINER  //
//////////////////////////

static _always_inline PyObject *anyarr_to_py(decbuffer_t *b, const size_t nitems)
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

static _always_inline PyObject *anymap_to_py(decbuffer_t *b, const size_t npairs)
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
            const char *ptr = b->base + b->offset;
            
            b->offset += size;

            key = fixstr_to_py(ptr, size);
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


////////////////////
//     MODULE     //
////////////////////

static void cleanup_module(PyObject *m)
{
    cleanup_common_caches();
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
    if (setup_common_caches() == false)
        return PyErr_NoMemory();

    PyObject *m = PyModule_Create(&cmsgpack);
    return m;
}


////////////////////
//    METADATA    //
////////////////////

#define INCBYTE (b->base + b->offset++)

static _always_inline void write_mask(encbuffer_t *b, const unsigned char mask, const size_t size, const size_t nbytes)
{
    if (nbytes == 0)
    {
        INCBYTE[0] = mask | (unsigned char)size;
    }
    else if (nbytes == 1)
    {
        const uint16_t mdata = BIG_16((size & 0xFF) | ((uint16_t)mask << 8));
        const size_t mdata_off = 0;
        memcpy(b->base + b->offset, (char *)(&mdata) + mdata_off, 2);
        b->offset += 2;
    }
    else if (nbytes == 2)
    {
        const uint32_t mdata = BIG_32((size & 0xFFFF) | ((uint32_t)mask << 16));
        const size_t mdata_off = 1;
        memcpy(b->base + b->offset, (char *)(&mdata) + mdata_off, 3);
        b->offset += 3;
    }
    else if (nbytes == 4)
    {
        const uint64_t mdata = BIG_64((uint64_t)(size & 0xFFFFFFFF) | ((uint64_t)mask << 32));
        const size_t mdata_off = 3;
        memcpy(b->base + b->offset, (char *)(&mdata) + mdata_off, 5);
        b->offset += 5;
    }
}

static _always_inline bool write_str_metadata(encbuffer_t *b, const size_t size)
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
    else if (_LIKELY(size <= STR_LARGE_MAXSIZE))
    {
        write_mask(b, DT_STR_LARGE, size, 4);
    }
    else
    {
        error_incorrect_value("String values can only hold up to 0xFFFFFFFF bytes in data");
        return false;
    }

    return true;
}

static _always_inline bool write_array_metadata(encbuffer_t *b, const size_t nitems)
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
        error_incorrect_value("Array values can only hold up to 0xFFFFFFFF items");
        return false;
    }

    return true;
}

static _always_inline bool write_map_metadata(encbuffer_t *b, const size_t npairs)
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
        error_incorrect_value("Map values can only hold up to 0xFFFFFFFF pairs");
        return false;
    }

    return true;
}

static _always_inline void write_int_metadata_and_data(encbuffer_t *b, const uint64_t num, const bool neg)
{
    if (!neg && num <= UINT_FIXED_MAXVAL)
    {
        write_mask(b, DT_UINT_FIXED, num, 0);
        return;
    }
    else if (neg && (int64_t)num >= INT_FIXED_MAXVAL)
    {
        write_mask(b, DT_INT_FIXED, num, 0);
        return;
    }

    // Convert negative numbers into positive ones, upscaled to match the uint max values
    const uint64_t pnum = !neg ? num : ((~num + 1) << 1) - 1;

    if (pnum <= UINT_BIT08_MAXVAL)
    {
        write_mask(b, !neg ? DT_UINT_BIT08 : DT_INT_BIT08, num, 1);
    }
    else if (pnum <= UINT_BIT16_MAXVAL)
    {
        write_mask(b, !neg ? DT_UINT_BIT16 : DT_INT_BIT16, num, 2);
    }
    else if (pnum <= UINT_BIT32_MAXVAL)
    {
        write_mask(b, !neg ? DT_UINT_BIT32 : DT_INT_BIT32, num, 4);
    }
    else
    {
        INCBYTE[0] = (!neg ? DT_UINT_BIT64 : DT_INT_BIT64);

        const uint64_t _num = BIG_64(num);
        memcpy(b->base + b->offset, &_num, 8);
        b->offset += 8;
    }
}

static _always_inline bool write_bin_metadata(encbuffer_t *b, const size_t size)
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
        error_incorrect_value("Binary values can only hold up to 0xFFFFFFFF bytes in data");
        return false;
    }

    return true;
}

static _always_inline bool write_ext_metadata(encbuffer_t *b, const size_t size, const unsigned char id)
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
        error_incorrect_value("Ext values can only hold up to 0xFFFFFFFF bytes in data");
        return false;
    }

    INCBYTE[0] = id;

    return true;
}


////////////////////
//   ALLOCATION   //
////////////////////

// Initial size to allocate for a buffer
#define INITIAL_ALLOC_SIZE 512

// Size to allocate up front for a buffer
static size_t avg_alloc_size = 128;
// Size to allocate per item in containers
static size_t avg_item_size = 12;

// Minimum values for the dynamic allocation values to prevent severe underallocation or underflow
#define ALLOC_SIZE_MIN 64
#define ITEM_SIZE_MIN 4

static void update_allocation_settings(const bool reallocd, const size_t offset, const size_t initial_allocated, const size_t nitems)
{
    if (reallocd)
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
static _always_inline bool buffer_ensure_space(encbuffer_t *b, size_t ensure_size)
{
    const size_t required_size = b->offset + ensure_size;
    if (_UNLIKELY(required_size >= b->allocated))
    {
        // Scale the buffer by 1.5 times the required size
        b->allocated = required_size * 1.5;

        void *new_ob = PyObject_Realloc(b->ob_base, PyBytes_SIZE + b->allocated);

        if (_UNLIKELY(new_ob == NULL))
        {
            error_no_memory();
            return false;
        }

        b->ob_base = new_ob;
        b->base = PyBytes_AS_STRING(new_ob);
    }

    return true;
}


/////////////////////
//  COMMONS CACHE  //
/////////////////////


////////////////////
//    ENCODING    //
////////////////////

bool encode_object(PyObject *obj, encbuffer_t *b)
{
    if (py_is_str(obj))
    {
        char *base;
        size_t size;

        py_str_data(obj, &base, &size);

        // Ensure there's enough space for metadata plus the string size
        if (_UNLIKELY(!buffer_ensure_space(b, 8 + size)))
            return false;
        
        write_str_metadata(b, size);
        
        memcpy(b->base + b->offset, base, size);
        b->offset += size;

        return true;
    }
    else if (py_is_bin(obj))
    {
        char *base;
        size_t size;

        py_bin_data(obj, &base, &size);

        if (_UNLIKELY(!buffer_ensure_space(b, 8 + size)))
            return false;
        
        write_bin_metadata(b, size);
        
        memcpy(b->base + b->offset, base, size);
        b->offset += size;

        return true;
    }

    if (_UNLIKELY(!buffer_ensure_space(b, 9)))
        return false;

    if (py_is_int(obj))
    {
        uint64_t num;
        bool neg;

        if (_UNLIKELY(!py_int_data(obj, &num, &neg)))
            return false;

        write_int_metadata_and_data(b, num, neg);
    }
    else if (py_is_float(obj))
    {
        double num;
        py_float_data(obj, &num);

        INCBYTE[0] = DT_FLOAT_BIT64;

        memcpy(b->base + b->offset, &num, 8);
        b->offset += 8;
    }
    else if (py_is_bool(obj))
    {
        // Write a typemask based on if the value is true or false
        INCBYTE[0] = obj == Py_True ? DT_TRUE : DT_FALSE;
    }
    else if (py_is_nil(obj))
    {
        INCBYTE[0] = DT_NIL;
    }
    else if (py_is_arr(obj))
    {
        // Get the number of items in the list
        const size_t nitems = PyList_GET_SIZE(obj);

        // Write metadata of the array
        write_array_metadata(b, nitems);

        return iterate_over_arr(b, obj, nitems);
    }
    else if (py_is_map(obj))
    {
        // Get the number of pairs in the dict
        const size_t npairs = PyDict_GET_SIZE(obj);

        write_map_metadata(b, npairs);

        return iterate_over_map(b, obj, npairs);
    }
    else
    {
        // Custom type logic will be placed here later

        error_unsupported_type(obj);
        return false;
    }

    return true;
}


////////////////////
//    DECODING    //
////////////////////

// Prefix error message for invalid encoded data
#define INVALID_MSG "Received invalid encoded data"

// Get a typemask for the fixlen switch case
#define FIXLEN_DT(mask) ((mask >> 5) & 0b111)
// Typemask for varlen switch case
#define VARLEN_DT(mask) ((mask >> 2) & 0b111)

// Extract macros for little-endian and big-endian systems, as data will be stored differently in N based on endianness
#ifdef IS_BIG_ENDIAN

#define EXTRACT1(n) n = n & 0xFF
#define EXTRACT2(n) n = n & 0xFFFF
#define EXTRACT4(n) n = n & 0xFFFFFFFF

#else

#define EXTRACT1(n) n = (n >> 56) & 0xFF
#define EXTRACT2(n) n = (n >> 48) & 0xFFFF
#define EXTRACT4(n) n = (n >> 32) & 0xFFFFFFFF

#endif

// Check if the buffer won't be overread
#define OVERREAD_CHECK(to_add) do { \
    if (_UNLIKELY(b->offset + to_add > b->allocated)) \
    { \
        error_incorrect_value(INVALID_MSG " (overread the encoded data)"); \
        return NULL; \
    } \
} while (0)

// Macros for adjusting N according to LENMODE
#define LENMODE_1BYTE if (lenmode == 0) { \
    EXTRACT1(n); \
    b->offset += 1; \
}
#define LENMODE_2BYTE if (lenmode == 1) { \
    EXTRACT2(n); \
    b->offset += 2; \
}
#define LENMODE_4BYTE if (lenmode == 2) { \
    EXTRACT4(n); \
    b->offset += 4; \
}

PyObject *decode_bytes(decbuffer_t *b)
{
    OVERREAD_CHECK(0);

    const unsigned char mask = (b->base + b->offset)[0];

    if ((mask & 0b11100000) != 0b11000000) // fixsize values
    {
        // Size inside of the fixed mask
        size_t n = (size_t)mask; // N has to be masked before use
        b->offset++;

        const unsigned char fixlen_mask = FIXLEN_DT(mask);
        switch (fixlen_mask)
        {
        case FIXLEN_DT(DT_STR_FIXED):
        {
            n &= 0x1F;

            OVERREAD_CHECK(n);

            const char *ptr = b->base + b->offset;
            b->offset += n;

            return fixstr_to_py(ptr, n);
        }
        case 0b000: // All possible combinations for fixuint as it uses just the upper 1 bit, and we're checking the upper 3
        case 0b001:
        case 0b010:
        case 0b011:
        {
            return anyint_to_py(n, true);
        }
        case FIXLEN_DT(DT_INT_FIXED):
        {
            long num = (long)n;

            num &= 0x1F;

            // Sign-extend the integer
            if (num & 0x10)
                num |= ~0x1F;

            return anyint_to_py(num, false);
        }
        case FIXLEN_DT(DT_MAP_FIXED): // Also catches DT_ARR_FIXED
        {
            n &= 0x0F;

            if ((mask & 0b10000) == 0b10000) // Bit 5 is 1 on array, 0 on map
            {
                return anyarr_to_py(b, n);
            }
            else
            {
                return anymap_to_py(b, n);
            }
        }
        default:
        {
            error_incorrect_value(INVALID_MSG " (invalid fixlen type bits)");
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

        const unsigned int varlen_mask = VARLEN_DT(mask);
        switch (varlen_mask)
        {
        case VARLEN_DT(DT_STR_SHORT):
        {
            // Lenmode starts at 1 for str type, so decrement for macro use
            lenmode--;

            LENMODE_1BYTE
            else LENMODE_2BYTE
            else LENMODE_4BYTE
            else
            {
                error_incorrect_value(INVALID_MSG " (invalid length bits in str type)");
                return NULL;
            }

            OVERREAD_CHECK(n);

            PyObject *obj = anystr_to_py(b->base + b->offset, n);
            b->offset += n;
            return obj;
        }
        case VARLEN_DT(DT_UINT_BIT08):
        case VARLEN_DT(DT_INT_BIT08):
        {
            const bool is_uint = varlen_mask == 0b011;

            if (lenmode == 0)
            {
                EXTRACT1(n);
                if (!is_uint && n & 0x80)
                    n |= ~0xFF;

                b->offset += 1;
            }
            else if (lenmode == 1)
            {
                EXTRACT2(n);
                if (!is_uint && n & 0x8000)
                    n |= ~0xFFFF;

                b->offset += 2;
            }
            else if (lenmode == 2)
            {
                EXTRACT4(n);
                if (!is_uint && n & 0x80000000)
                    n |= ~0xFFFFFFFFULL;

                b->offset += 4;
            }
            else
            {
                b->offset += 8;
            }

            return anyint_to_py(n, is_uint);
        }
        case VARLEN_DT(DT_ARR_MEDIUM): // Also matches maps
        {
            // Set lenmode to 1 or 2 manually as 2nd bit is used to specify map or array
            lenmode = (unsigned int)((mask & 1) == 1) + 1;

            LENMODE_2BYTE
            else LENMODE_4BYTE

            if ((mask & 0b10) == 0) // array
            {
                return anyarr_to_py(b, n);
            }
            else
            {
                return anymap_to_py(b, n);
            }
        }
        case VARLEN_DT(DT_NIL): // Matches all states
        {
            if (mask == DT_TRUE)
            {
                return true_to_py();
            }
            else if (mask == DT_FALSE)
            {
                return false_to_py();
            }
            else if (_LIKELY(mask == DT_NIL))
            {
                return nil_to_py();
            }
            else
            {
                error_incorrect_value(INVALID_MSG " (invalid state type)");
                return NULL;
            }
        }
        case VARLEN_DT(DT_FLOAT_BIT32):
        {
            double num;

            if (mask == DT_FLOAT_BIT32)
            {
                OVERREAD_CHECK(4);

                float _num;
                memcpy(&n, &_num, 4);
                num = (double)_num;

                b->offset += 4;
            }
            else
            {
                OVERREAD_CHECK(8);

                memcpy(&num, &n, 8);
                b->offset += 8;
            }

            return anyfloat_to_py(num);
        }
        case VARLEN_DT(DT_BIN_SHORT): // Also matches ext
        {
            LENMODE_1BYTE
            else LENMODE_2BYTE
            else LENMODE_4BYTE
            else
            {
                error_incorrect_value(INVALID_MSG " (invalid length bits in bin type, OR got currently unsupported ext type)");
                return NULL;
            }

            PyObject *obj = anybin_to_py(b->base + b->offset, n);
            b->offset += n;
            return obj;
        }
        default:
        {
            error_incorrect_value(INVALID_MSG " (invalid varlen type bits)");
            return NULL;
        }
        }
    }

    // This shouldn't be reached
    error_incorrect_value(INVALID_MSG " (unknown error)");
    return NULL;
}

