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

// Check if the system is big-endian
#if Py_BIG_ENDIAN == 1
#define IS_BIG_ENDIAN
#endif

// Include these after defining IS_BIG_ENDIAN, as they depend on it
#include "masks.h"
#include "internals.h"


// Number of slots in ext type hash tables
#define EXT_TABLE_SLOTS 256

typedef struct {
    PyTypeObject *key;
    PyObject *val;
} keyval_t;

typedef struct {
    uint8_t offsets[EXT_TABLE_SLOTS];
    uint8_t lengths[EXT_TABLE_SLOTS];
    keyval_t *pairs;
} hashtable_t;

// Usertypes encode object struct
typedef struct {
    PyObject_HEAD
    hashtable_t table;
} ext_types_encode_t;

// Usertypes decode object struct
typedef struct {
    PyObject_HEAD
    PyTypeObject *argtype;
    PyObject *reads[EXT_TABLE_SLOTS];
} ext_types_decode_t;

static PyTypeObject ExtTypesEncodeObj;
static PyTypeObject ExtTypesDecodeObj;


// Struct holding buffer data for encoding
typedef struct {
    PyObject *ob_base; // Base of allocated PyBytes object
    char *base;        // Base of buffer to write to
    size_t offset;     // Writing offset relative to the buffer base
    size_t allocated;  // Space allocated for the buffer (excluding PyBytesObject size)
    ext_types_encode_t *ext; // object for encoding ext types
} encbuffer_t;

// Struct holding buffer data for decoding
typedef struct {
    char *base;        // Base of the buffer object that contains the data to decode
    size_t offset;     // Writing offset relative to the base
    size_t allocated;  // Space allocated in the buffer object
    ext_types_decode_t *ext; // Object for decoding ext types
} decbuffer_t;


static bool encode_object(PyObject *obj, encbuffer_t *b);
static PyObject *decode_bytes(decbuffer_t *b);
static void update_allocation_settings(const bool reallocd, const size_t offset, const size_t initial_allocated, const size_t nitems);

static size_t avg_alloc_size;
static size_t avg_item_size;

// Copied from cpython/Objects/bytesobject.c. Used for byteobject reallocation
#include <stddef.h>
#define PyBytesObject_SIZE (offsetof(PyBytesObject, ob_sval) + 1)


/////////////////////
//  ERROR METHODS  //
/////////////////////

static void error_unsupported_type(PyObject *obj)
{
    PyErr_Format(PyExc_TypeError, "Received unsupported type '%s'", Py_TYPE(obj)->tp_name);
}

static void error_incorrect_value(const char *msg)
{
    PyErr_SetString(PyExc_ValueError, msg);
}

static void error_no_memory(void)
{
    PyErr_NoMemory();
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
            PyErr_SetString(PyExc_OverflowError, "Integers cannot be more than 8 bytes");
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
        PyErr_SetString(PyExc_OverflowError, "Integers cannot be more than 8 bytes");
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


//////////////////
//    STATES    //
//////////////////

// Global states
typedef struct {
    PyObject *ext_types;
    PyObject *arg_type;
} gstates;

// Get interned string of NAME
#define GET_ISTR(name) if ((s->name = PyUnicode_InternFromString(#name)) == NULL) { return false; }

static bool setup_gstates(gstates *s)
{
    GET_ISTR(ext_types)
    GET_ISTR(arg_type)

    return true;
}

// Get the global states of the module
static _always_inline gstates *get_gstates(PyObject *m)
{
    return (gstates *)PyModule_GetState(m);
}


////////////////////
//  KWARGS PARSE  //
////////////////////

#if PY_VERSION_HEX >= 0x030d0000 // Python 3.13 and up
#define _unicode_equal(x, y) (PyUnicode_Compare(x, y) == 0)
#else
#define _unicode_equal(x, y) _PyUnicode_EQ(x, y)
#endif

static _always_inline PyObject *get_kwarg(PyObject *const *args, PyObject *kwargs, PyObject *key)
{
    const size_t nkwargs = PyTuple_GET_SIZE(kwargs);

    // Attempt match with interned strings
    for (size_t i = 0; i < nkwargs; ++i)
    {
        if (PyTuple_GET_ITEM(kwargs, i) == key)
            return args[i];
    }

    // Fallback to comparing strings
    for (size_t i = 0; i < nkwargs; ++i)
    {
        if (_unicode_equal(PyTuple_GET_ITEM(kwargs, i), key))
            return args[i];
    }

    return NULL;
}

#define PARSE_KWARGS_START \
    gstates *s = get_gstates(self); \
    size_t nkwargs = PyTuple_GET_SIZE(kwargs);

// Get kwarg NAME assigned to DEST, replaced by DEFVAL if not found. If TYPE != NULL, check if DEST is TYPE
#define PARSE_KWARG(dest, name, type, defval) do { \
    if (((dest) = get_kwarg(args + nargs, kwargs, s->name)) != NULL) \
    { \
        if (type != NULL) \
        { \
            if (_UNLIKELY(!Py_IS_TYPE(dest, type))) \
            { \
                PyErr_Format(PyExc_TypeError, "Argument '" #name "' should be of type '%s', got '%s'", (type)->tp_name, Py_TYPE(dest)->tp_name); \
                return NULL; \
            } \
        } \
        if (--nkwargs == 0) \
            goto kwargs_parsing_end; \
    } \
    else \
    { \
        dest = defval; \
    } \
} while (0)

#define PARSE_KWARGS_END \
    kwargs_parsing_end: \
    if (nkwargs > 0) \
    { \
        PyErr_SetString(PyExc_TypeError, "Received extra positional arguments"); \
        return NULL; \
    }


/////////////////////
//   EXT OBJECTS   //
/////////////////////

#define EXT_ENCODE_HASH(n) (((uintptr_t)(n) >> 8) % EXT_TABLE_SLOTS)

static PyObject *ExtTypesEncode(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    if (_UNLIKELY(nargs != 1))
    {
        PyErr_SetString(PyExc_TypeError, "Expected just the 'pairs' argument");
        return NULL;
    }

    PyObject *pairsdict = args[0];

    ext_types_encode_t *obj = PyObject_New(ext_types_encode_t, &ExtTypesEncodeObj);
    if (obj == NULL)
    {
        error_no_memory();
        return NULL;
    }
    
    const size_t npairs = PyDict_GET_SIZE(pairsdict);
    const size_t array_size = npairs * sizeof(PyObject *);

    // Allocate a single buffer for both the keys and vals
    PyObject **keys_vals = (PyObject **)malloc(2 * array_size);
    if (keys_vals == NULL)
    {
        error_no_memory();
        return NULL;
    }
    
    // Set the key array at the start of the shared buffer, and the val array directly on top of it
    PyObject **keys = keys_vals;
    PyObject **vals = (PyObject **)((char *)keys_vals + array_size);

    // Pairs buffer for the hash table
    keyval_t *pairs = (keyval_t *)PyObject_Malloc(npairs * sizeof(keyval_t));
    if (pairs == NULL)
    {
        free(keys_vals);
        error_no_memory();
        return NULL;
    }

    // Set the pairs array in the table
    obj->table.pairs = pairs;
    
    // Collect all keys and vals in their respective arrays and type check them
    Py_ssize_t pos = 0;
    for (size_t i = 0; i < npairs; ++i)
    {
        PyObject *key;
        PyObject *val;

        PyDict_Next(pairsdict, &pos, &key, &val);

        if (!PyType_CheckExact(key) || !PyCallable_Check(val))
        {
            free(keys_vals);
            free(pairs);

            PyErr_Format(PyExc_TypeError, "Expected keys of type 'type' and values of type 'callable', got a key of type '%s' with a value of type '%s'", Py_TYPE(key)->tp_name, Py_TYPE(val)->tp_name);
            return NULL;
        }

        keys[i] = key;
        vals[i] = val;
    }

    // Calculate the length required per hash of each pair
    uint8_t lengths[EXT_TABLE_SLOTS] = {0};
    for (size_t i = 0; i < npairs; ++i)
    {
        const size_t hash = EXT_ENCODE_HASH(keys[i]);
        lengths[hash]++;
    }

    // Copy the lengths into the table
    memcpy(obj->table.lengths, lengths, sizeof(lengths));

    // Initialize the first offset as zero, calculate the others
    obj->table.offsets[0] = 0;
    for (size_t i = 1; i < EXT_TABLE_SLOTS; ++i)
    {
        // Set the offset of the current index based on the offset and length of the previous hash
        obj->table.offsets[i] = obj->table.offsets[i - 1] + obj->table.lengths[i - 1];
    }

    for (size_t i = 0; i < npairs; ++i)
    {
        PyObject *key = keys[i];
        PyObject *val = vals[i];

        const size_t hash = EXT_ENCODE_HASH(key);
        const size_t length = --lengths[hash]; // Decrement to place the next item under this hash one spot lower
        const size_t offset = obj->table.offsets[hash] + length;

        pairs[offset].key = (PyTypeObject *)key;
        pairs[offset].val = val;

        // Incref to ensure a reference to the objects
        Py_INCREF(key);
        Py_INCREF(val);
    }

    free(keys_vals);

    return (PyObject *)obj;
}

static PyObject *ExtTypesDecode(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    if (_UNLIKELY(nargs < 0))
    {
        PyErr_SetString(PyExc_TypeError, "Expected at least the positional 'pairs' argument");
        return NULL;
    }
    if (_UNLIKELY(nargs > 2))
    {
        PyErr_Format(PyExc_TypeError, "Expected 2 positional arguments at max, got %zi", nargs);
        return NULL;
    }

    PyObject *pairsdict = args[0];
    PyTypeObject *argtype = &PyBytes_Type;

    if (nargs == 2)
    {
        argtype = (PyTypeObject *)args[1];

        if (_UNLIKELY(argtype != &PyBytes_Type && argtype != &PyMemoryView_Type))
        {
            if (!PyType_CheckExact(argtype))
            {
                PyErr_Format(PyExc_TypeError, "Argument 'arg_type' should be of type 'type', got '%s'", Py_TYPE(argtype)->tp_name);
            }
            else
            {
                PyErr_Format(PyExc_ValueError, "Argument 'arg_type' should be either 'bytes' or 'memoryview', got '%s'", argtype->tp_name);
            }

            return NULL;
        }
    }

    ext_types_decode_t *obj = PyObject_New(ext_types_decode_t, &ExtTypesDecodeObj);
    if (obj == NULL)
    {
        error_no_memory();
        return NULL;
    }

    // Zero-initialize the reads field to set unused slots to NULL
    memset(obj->reads, 0, sizeof(obj->reads));
    
    obj->argtype = argtype;
    
    PyObject *key, *val;
    Py_ssize_t pos = 0;
    while (PyDict_Next(pairsdict, &pos, &key, &val))
    {
        if (_UNLIKELY(!PyLong_CheckExact(key) || !PyCallable_Check(val)))
        {
            Py_DECREF(obj);
            PyErr_Format(PyExc_TypeError, "Expected keys of type 'int' and values of type 'callable', got a key of type '%s' with a value of type '%s'", Py_TYPE(key)->tp_name, Py_TYPE(val)->tp_name);
            return NULL;
        }

        const long id = PyLong_AS_LONG(key);

        if (_UNLIKELY(id < -128 || id > 127))
        {
            Py_DECREF(obj);
            PyErr_Format(PyExc_ValueError, "IDs are only allowed to range between -128 and 127, got %i", id);
            return NULL;
        }

        // Index is the ID plus 128, to range from 0 to 255 for indexing
        const uint8_t idx = (uint8_t)id;

        // Set the reads function to the correct ID
        obj->reads[idx] = val;

        Py_INCREF(val);
    }

    return (PyObject *)obj;
}

static void ext_types_encode_dealloc(ext_types_encode_t *obj)
{
    // Calculate the number of pairs in the table
    size_t num_pairs = 0;
    for (size_t i = 0; i < EXT_TABLE_SLOTS; ++i)
    {
        num_pairs += obj->table.lengths[i];
    }

    keyval_t *pairs = obj->table.pairs;

    // Decref the PyObjects in the pairs
    for (size_t i = 0; i < num_pairs; ++i)
    {
        Py_XDECREF(pairs[i].key);
        Py_XDECREF(pairs[i].val);
    }

    PyObject_Del(obj);
}

static void ext_types_decode_dealloc(ext_types_decode_t *obj)
{
    // Decref all objects stored in the read slots
    for (size_t i = 0; i < EXT_TABLE_SLOTS; ++i)
    {
        Py_XDECREF(obj->reads[i]);
    }

    PyObject_Del(obj);
}

static PyObject *ext_encode_pull(ext_types_encode_t *obj, PyTypeObject *tp)
{
    const size_t hash = EXT_ENCODE_HASH(tp);

    hashtable_t tbl = obj->table;
    keyval_t *pairs = tbl.pairs;


    const size_t offset = tbl.offsets[hash]; // Offset to index on
    const size_t length = tbl.lengths[hash]; // Number of entries for this hash value

    for (size_t i = 0; i < length; ++i)
    {
        keyval_t pair = pairs[offset + i];

        if (_LIKELY(pair.key == tp))
            return pair.val;
    }

    return NULL;
}

static PyObject *ext_decode_pull(ext_types_decode_t *obj, const int8_t id)
{
    const uint8_t idx = (uint8_t)id;

    // Return the reads object located on the index. Will be NULL if it doesn't exist
    return obj->reads[idx];
}

// Attempt to convert any object to bytes using an ext object. Returns a tuple to be decremented after copying data from PTR, NULL on failure
static PyObject *any_to_ext(encbuffer_t *b, PyObject *obj, int8_t *id, char **ptr, size_t *size)
{
    if (b->ext == NULL)
        return NULL;
    
    PyObject *func = ext_encode_pull(b->ext, Py_TYPE(obj));
    if (_UNLIKELY(func == NULL))
    {
        error_unsupported_type(obj);
        return NULL;
    }
    
    PyObject *result = PyObject_CallOneArg(func, obj);
    if (_UNLIKELY(result == NULL))
        return NULL;
    
    if (_UNLIKELY(!PyTuple_CheckExact(result)))
    {
        Py_DECREF(result);
        PyErr_Format(PyExc_TypeError, "Expected to receive a tuple from an ext type encode function, got return argument of type '%s'", Py_TYPE(result)->tp_name);
        return NULL;
    }

    if (_UNLIKELY(PyTuple_GET_SIZE(result) != 2))
    {
        Py_DECREF(result);
        PyErr_Format(PyExc_ValueError, "Expected the tuple from an ext type encode function to hold exactly 2 items, got %zi items", PyTuple_GET_SIZE(result));
        return NULL;
    }

    PyObject *id_obj = PyTuple_GET_ITEM(result, 0);
    PyObject *bytes_obj = PyTuple_GET_ITEM(result, 1);

    if (_UNLIKELY(!PyLong_CheckExact(id_obj) || !PyBytes_CheckExact(bytes_obj)))
    {
        Py_DECREF(result);
        PyErr_Format(PyExc_ValueError, "Expected the tuple from an ext type object to hold an int on index 0 and bytes on index 1, got a '%s' on index 0 and '%s' on index 1", Py_TYPE(id_obj)->tp_name, Py_TYPE(bytes_obj)->tp_name);
        return NULL;
    }

    const long _id = PyLong_AS_LONG(id_obj);

    if (_UNLIKELY(_id < -128 || _id > 127))
    {
        Py_DECREF(result);
        PyErr_Format(PyExc_ValueError, "Expected the ID for an ext type to range between -128 to 127, got %i", _id);
        return NULL;
    }

    *id = (int8_t)_id;
    *size = PyBytes_GET_SIZE(bytes_obj);
    *ptr = PyBytes_AS_STRING(bytes_obj);

    if (_UNLIKELY(*size == 0))
    {
        Py_DECREF(result);
        PyErr_SetString(PyExc_ValueError, "Ext types do not support zero-length data");
        return NULL;
    }

    return result;
}

static PyObject *ext_to_any(decbuffer_t *b, const int8_t id, const size_t size)
{
    if (b->ext == NULL)
        return NULL;

    PyObject *func = ext_decode_pull(b->ext, id);

    if (_UNLIKELY(func == NULL))
    {
        PyErr_Format(PyExc_ValueError, "Could not match an ext function for decoding on id %i", id);
        return NULL;
    }

    // Either create bytes or a memoryview object based on what the user wants
    PyObject *arg;
    if (b->ext->argtype == &PyMemoryView_Type)
    {
        arg = PyMemoryView_FromMemory(b->base + b->offset, (Py_ssize_t)size, PyBUF_READ);
    }
    else
    {
        arg = PyBytes_FromStringAndSize(b->base + b->offset, (Py_ssize_t)size);
    }
    
    if (arg == NULL)
    {
        error_no_memory();
        return NULL;
    }

    b->offset += size;

    // Call the function, passing the memoryview to it, and return its result
    PyObject *result = PyObject_CallOneArg(func, arg);

    Py_DECREF(arg);
    return result;
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

static void cleanup_common_caches(const bool used)
{
    if (used)
    {
        for (size_t i = 0; i < COMMONCACHE_SLOTS; ++i)
        {
            Py_XDECREF(fixascii_common[i]);
            Py_XDECREF(smallint_common[i].obj);
        }
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
        write_mask(b, neg ? DT_INT_BIT08 : DT_UINT_BIT08, num, 1);
    }
    else if (pnum <= UINT_BIT16_MAXVAL)
    {
        write_mask(b, neg ? DT_INT_BIT16 : DT_UINT_BIT16, num, 2);
    }
    else if (pnum <= UINT_BIT32_MAXVAL)
    {
        write_mask(b, neg ? DT_INT_BIT32 : DT_UINT_BIT32, num, 4);
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

static _always_inline bool write_ext_metadata(encbuffer_t *b, const size_t size, const int8_t id)
{
    const bool is_baseof2 = (size & (size - 1)) == 0;

    if (size <= 16 && is_baseof2)
    {
        const unsigned char fixmask = (
            size == 16 ? DT_EXT_FIX16 :
            DT_EXT_FIX1 | (31 - LEADING_ZEROES_32(size))
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
    else if (_LIKELY(size <= EXT_LARGE_MAXSIZE))
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
static bool buffer_ensure_space(encbuffer_t *b, size_t ensure_size)
{
    // Scale the buffer by 1.5 times the required size
    b->allocated = (b->offset + ensure_size) * 1.5;

    void *new_ob = PyObject_Realloc(b->ob_base, b->allocated + PyBytesObject_SIZE);

    if (_UNLIKELY(new_ob == NULL))
    {
        error_no_memory();
        return false;
    }

    b->ob_base = new_ob;
    b->base = PyBytes_AS_STRING(new_ob);
    
    return true;
}


////////////////////
//    ENCODING    //
////////////////////

#define ENSURE_SPACE(extra) do { \
    if (_UNLIKELY(b->offset + extra >= b->allocated)) \
    { \
        if (buffer_ensure_space(b, extra) == false) \
            { return NULL; } \
    } \
} while (0)

bool encode_object(PyObject *obj, encbuffer_t *b)
{
    PyTypeObject *tp = Py_TYPE(obj);

    if (tp == &PyUnicode_Type)
    {
        char *base;
        size_t size;

        py_str_data(obj, &base, &size);

        ENSURE_SPACE(5 + size);
        
        write_str_metadata(b, size);
        
        memcpy(b->base + b->offset, base, size);
        b->offset += size;

        return true;
    }
    else if (tp == &PyBytes_Type)
    {
        char *base;
        size_t size;

        py_bin_data(obj, &base, &size);

        ENSURE_SPACE(5 + size);
        
        write_bin_metadata(b, size);
        
        memcpy(b->base + b->offset, base, size);
        b->offset += size;

        return true;
    }

    ENSURE_SPACE(9);

    if (tp == &PyLong_Type)
    {
        uint64_t num;
        bool neg;

        if (_UNLIKELY(!py_int_data(obj, &num, &neg)))
            return false;

        write_int_metadata_and_data(b, num, neg);
    }
    else if (tp == &PyFloat_Type)
    {
        double num;
        py_float_data(obj, &num);

        INCBYTE[0] = DT_FLOAT_BIT64;

        memcpy(b->base + b->offset, &num, 8);
        b->offset += 8;
    }
    else if (tp == &PyBool_Type)
    {
        // Write a typemask based on if the value is true or false
        INCBYTE[0] = obj == Py_True ? DT_TRUE : DT_FALSE;
    }
    else if (tp == Py_TYPE(Py_None)) // No explicit PyNone_Type available
    {
        INCBYTE[0] = DT_NIL;
    }
    else if (tp == &PyList_Type)
    {
        // Get the number of items in the list
        const size_t nitems = PyList_GET_SIZE(obj);

        // Write metadata of the array
        write_array_metadata(b, nitems);

        return iterate_over_arr(b, obj, nitems);
    }
    else if (tp == &PyDict_Type)
    {
        // Get the number of pairs in the dict
        const size_t npairs = PyDict_GET_SIZE(obj);

        write_map_metadata(b, npairs);

        return iterate_over_map(b, obj, npairs);
    }
    else
    {
        int8_t id;
        char *ptr;
        size_t size;

        PyObject *result = any_to_ext(b, obj, &id, &ptr, &size);

        if (result == NULL)
        {
            // Error might not be set, so default to unsupported type error
            if (!PyErr_Occurred())
                error_unsupported_type(obj);
            
            return false;
        }

        ENSURE_SPACE(6 + size);

        write_ext_metadata(b, size, id);

        memcpy(b->base + b->offset, ptr, size);
        b->offset += size;

        Py_DECREF(result);
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

        const unsigned char fixmask = mask & 0b11100000;
        if (fixmask == DT_STR_FIXED)
        {
            n &= 0x1F;

            OVERREAD_CHECK(n);

            const char *ptr = b->base + b->offset;
            b->offset += n;

            return fixstr_to_py(ptr, n);
        }
        else if ((mask & 0x80) == 0) // uint only has upper bit set to 0
        {
            return anyint_to_py(n, true);
        }
        else if (fixmask == DT_INT_FIXED)
        {
            long num = (long)n;

            num &= 0x1F;

            // Sign-extend the integer
            if (num & 0x10)
                num |= ~0x1F;

            return anyint_to_py(num, false);
        }
        else if (fixmask == (DT_ARR_FIXED & 0b11100000)) // Catches both ARR and MAP
        {
            n &= 0x0F;

            if ((mask & 0b10000) == 0b10000) // Bit 5 is 1 on array, 0 on map
            {
                return anyarr_to_py(b, n & 0x0F);
            }
            else
            {
                return anymap_to_py(b, n & 0x0F);
            }
        }
        else
        {
            error_incorrect_value(INVALID_MSG " (invalid fixlen type bits)");
            return NULL;
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

        /* NOTE: CONVERT TO IF-CHAIN TO SEE IF PERFORMANCE IMPROVES CONSISTENTLY */

        const unsigned int varlen_mask = VARLEN_DT(mask);
        switch (varlen_mask)
        {
        case VARLEN_DT(DT_STR_SHORT): // Also matches DT_EXT_FIX16
        {
            if (_UNLIKELY(mask == DT_EXT_FIX16))
                goto ext_types_fix16;
            
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
        case VARLEN_DT(DT_FLOAT_BIT32): // Also matched DT_EXT_MEDIUM and DT_EXT_LARGE
        {
            if (_UNLIKELY(mask == DT_EXT_MEDIUM))
                goto ext_types_varlen_medium;
            if (_UNLIKELY(mask == DT_EXT_LARGE))
                goto ext_types_varlen_large;

            double num;

            if (mask == DT_FLOAT_BIT32)
            {
                OVERREAD_CHECK(4);

                float _num;
                memcpy(&_num, &n, 4);
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
        case VARLEN_DT(DT_BIN_SHORT): // Also matches DT_EXT_SHORT
        {
            if (_UNLIKELY(mask == DT_EXT_SHORT))
                goto ext_types_varlen_short;
            
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
        case VARLEN_DT(DT_EXT_FIX1):
        {
            n = 1ULL << lenmode;

            // Break to go to the global ext type handling path
            break;
        }
        default:
        {
            error_incorrect_value(INVALID_MSG " (invalid varlen type bits)");
            return NULL;
        }
        }

        // Special path for varlen ext types, as these are all matched in other types

        if (0)
        {
            ext_types_varlen_short:
            lenmode = 0;
            LENMODE_1BYTE
        }
        if (0)
        {
            ext_types_varlen_medium:
            lenmode = 1;
            LENMODE_2BYTE
        }
        if (0)
        {
            ext_types_varlen_large:
            lenmode = 2;
            LENMODE_4BYTE
        }
        if (0)
        {
            ext_types_fix16:
            n = 16;
        }

        const int8_t id = INCBYTE[0];

        OVERREAD_CHECK(n);

        PyObject *obj = ext_to_any(b, id, n);

        if (obj == NULL)
        {
            if (!PyErr_Occurred())
                error_incorrect_value(INVALID_MSG " (failed to match an ext type)");
            
            return NULL;
        }

        return obj;
    }
}


///////////////////////
//  ENC/DEC HANDLES  //
///////////////////////

static PyObject *encode(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwargs)
{
    if (_UNLIKELY(nargs != 1))
    {
        if (nargs > 1)
        {
            PyErr_SetString(PyExc_TypeError, "Only the 'obj' argument is allowed to be positional");
        }
        else
        {
            PyErr_SetString(PyExc_TypeError, "Expected the positional 'obj' argument");
        }
        
        return NULL;
    }

    PyObject *obj = args[0];
    encbuffer_t b;

    if (kwargs != NULL)
    {
        PARSE_KWARGS_START
        
        PARSE_KWARG(b.ext, ext_types, &ExtTypesEncodeObj, NULL);

        PARSE_KWARGS_END
    }
    else
    {
        b.ext = NULL;
    }

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
        b.ob_base = (PyObject *)PyBytes_FromStringAndSize(NULL, initial_alloc);

        if (b.ob_base == NULL)
        {
            error_no_memory();
            return NULL;
        }

        // Set the other data of the buffer struct
        b.base = PyBytes_AS_STRING(b.ob_base);
        b.allocated = initial_alloc;
        b.offset = 0;
    }
    else
    {
        // Create an object with 64 bytes. Will be adjusted if insufficient dynamically, but is enough for a lot of values
        b.ob_base = (PyObject *)PyBytes_FromStringAndSize(NULL, 64);
        b.base = PyBytes_AS_STRING(b.ob_base);
        b.allocated = 64;
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
    return b.ob_base;
}

static PyObject *decode(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwargs)
{
    if (_UNLIKELY(nargs != 1))
    {
        if (nargs > 1)
        {
            PyErr_SetString(PyExc_ValueError, "Only the 'encoded' argument is allowed to be positional");
        }
        else
        {
            PyErr_SetString(PyExc_ValueError, "Expected the positional 'encoded' argument");
        }
        
        return NULL;
    }

    PyObject *encoded = args[0];

    decbuffer_t b;

    b.ext = NULL;

    if (kwargs != NULL)
    {
        PARSE_KWARGS_START
        
        PARSE_KWARG(b.ext, ext_types, &ExtTypesDecodeObj, NULL);

        PARSE_KWARGS_END
    }

    Py_buffer buffer;
    if (PyObject_GetBuffer(encoded, &buffer, PyBUF_READ) != 0)
        return NULL;
    
    b.base = buffer.buf;
    b.allocated = buffer.len;
    b.offset = 0;

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
//     MODULE     //
////////////////////

static void cleanup_module(PyObject *m)
{
    cleanup_common_caches(true);
}

static PyMethodDef CmsgpackMethods[] = {
    {"encode", (PyCFunction)encode, METH_FASTCALL | METH_KEYWORDS, NULL},
    {"decode", (PyCFunction)decode, METH_FASTCALL | METH_KEYWORDS, NULL},

    {"ExtTypesEncode", (PyCFunction)ExtTypesEncode, METH_FASTCALL, NULL},
    {"ExtTypesDecode", (PyCFunction)ExtTypesDecode, METH_FASTCALL, NULL},

    //{"validate", (PyCFunction)validate, METH_FASTCALL, NULL},

    {NULL, NULL, 0, NULL}
};

static PyTypeObject ExtTypesEncodeObj = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "ExtTypesEncode",
    .tp_basicsize = sizeof(ext_types_encode_t),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_dealloc = (destructor)ext_types_encode_dealloc,
};

static PyTypeObject ExtTypesDecodeObj = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "ExtTypesDecode",
    .tp_basicsize = sizeof(ext_types_decode_t),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_dealloc = (destructor)ext_types_decode_dealloc,
};

static struct PyModuleDef cmsgpack = {
    PyModuleDef_HEAD_INIT,
    "cmsgpack",
    "fast msgpack-format serializer",
    sizeof(gstates),
    CmsgpackMethods,
    NULL,
    NULL,
    NULL,
    (freefunc)cleanup_module,
};

PyMODINIT_FUNC PyInit_cmsgpack(void) {
    PyObject *_m = PyState_FindModule(&cmsgpack);
    if (_m != NULL)
    {
        Py_INCREF(_m);
        return _m;
    }

    if (PyType_Ready(&ExtTypesEncodeObj))
        return NULL;
    if (PyType_Ready(&ExtTypesDecodeObj))
        return NULL;

    if (setup_common_caches() == false)
    {
        error_no_memory();
        return NULL;
    }
    

    PyObject *m = PyModule_Create(&cmsgpack);
    
    if (setup_gstates((gstates *)PyModule_GetState(m)) == false)
    {
        Py_DECREF(m);
        return NULL;
    }

    return m;
}

