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


// Check if the system is big-endian
#if Py_BIG_ENDIAN == 1
#define IS_BIG_ENDIAN
#endif

// Include these after defining IS_BIG_ENDIAN, as they depend on it
#include "masks.h"
#include "internals.h"


///////////////////////////
//  TYPEDEFS & FORWARDS  //
///////////////////////////

// Number of slots in ext type hash tables
#define EXT_TABLE_SLOTS 256

// Keyval pair used in hash tables
typedef struct {
    PyTypeObject *key;
    PyObject *val;
} keyval_t;

// Hash table struct
typedef struct {
    uint8_t offsets[EXT_TABLE_SLOTS]; // Offset in the pairs for the hash value
    uint8_t lengths[EXT_TABLE_SLOTS]; // Number of pairs after the offset for the hash value
    keyval_t *pairs;                  // Pointer to a keyval pairs array
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
    PyObject *reads[256]; // Array of functions per ID, indexed based on the ID (256 IDs, range from -128 to 127. Normalize by casting the ID to unsigned)
} ext_types_decode_t;

static PyTypeObject ExtTypesEncodeObj;
static PyTypeObject ExtTypesDecodeObj;


typedef struct {
    intptr_t val;
    PyObject *obj;
} smallint_common_t;

// Module states
typedef struct {
    // Interned strings
    struct {
        PyObject *obj;
        PyObject *ext_types;
        PyObject *strict_keys;
        PyObject *file_name;
        PyObject *keep_open;
    } interned;

    // Common caches
    struct {
        PyASCIIObject **strings;
    } caches;

    // Encoding data
    struct {
        PyBytesObject *obj; // Reference to the last used object during encoding
        size_t        size; // Size of the last used object

        size_t item_avg;    // Average size to allocate per item
        size_t extra_avg;   // Average size to allocate extra
    } encoding;
} mstates_t;

static _always_inline mstates_t *get_mstates(PyObject *m);


// Struct holding buffer data for encoding
typedef struct {
    mstates_t *states; // Module states

    // The module states holds the object base and its size

    char *offset;      // Offset to write to
    char *maxoffset;   // Maximum offset value before reallocation is needed
    Py_ssize_t nitems; // Number of items within the "outer" object, used for adaptive allocation (-1 if not used at all)
    ext_types_encode_t *ext; // object for encoding ext types
    bool strict_keys;  // Whether map keys can only be strings
} encbuffer_t;

// Struct holding buffer data for decoding
typedef struct {
    mstates_t *states; // Module states
    char *base;        // Base of the buffer object that contains the data to decode
    size_t offset;     // Writing offset relative to BASE
    size_t allocated;  // Space allocated in the buffer object
    ext_types_decode_t *ext; // Object for decoding ext types
    bool strict_keys;  // Whether map keys can only be strings
    bool is_stream;    // Whether we are streaming, to determine what to do on overreads
} decbuffer_t;

static _always_inline size_t encbuffer_size(encbuffer_t *b);
static bool encbuffer_expand(encbuffer_t *b, size_t needed);


typedef struct {
    PyObject_HEAD
    encbuffer_t data;
} encoder_t;

typedef struct {
    PyObject_HEAD
    decbuffer_t data;
} decoder_t;

static PyTypeObject EncoderObj;
static PyTypeObject DecoderObj;

typedef struct {
    PyObject_HEAD
    char *name; // Name of the file
    FILE *file; // File object being used
} filedata_t;

typedef struct {
    filedata_t file;
    encbuffer_t data;
} streamenc_t;

typedef struct {
    filedata_t file;
    decbuffer_t data;
} streamdec_t;


// The offset of the `data` field on stream objects minus the PyObject size to simulate what encoder_encode and decoder_decode receive as encoder_t and decoder_t
#define STREAMOBJ_DATA_OFFSET (sizeof(filedata_t) - sizeof(PyObject))

static PyTypeObject StreamEncoderObj;
static PyTypeObject StreamDecoderObj;


static bool encode_object(PyObject *obj, encbuffer_t *b);
static PyObject *decode_bytes(decbuffer_t *b);
static bool decbuffer_refresh(streamdec_t *dec, const size_t requested);


// Copied from cpython/Objects/bytesobject.c, used for PyBytesObject reallocation
#include <stddef.h>
#define PyBytesObject_SIZE (offsetof(PyBytesObject, ob_sval) + 1)


static struct PyModuleDef cmsgpack;


/////////////////////
//  COMMON ERRORS  //
/////////////////////

// Error for when LIMIT_LARGE is exceeded
#define error_size_limit(name, size) (PyErr_Format(PyExc_ValueError, #name " values can only hold up to 4294967295 bytes (2^32-1, 4 bytes), got a size of %zu", size))

// Error for when we get an unexpected type at an argument
#define error_unexpected_type(argname, correct_tpname, received_tpname) (PyErr_Format(PyExc_TypeError, "Expected argument '%s' to be of type '%s', but got an object of type '%s'", argname, correct_tpname, received_tpname))

// Error for when a file couldn't be opened
#define error_cannot_open_file(filename, errno) (PyErr_Format(PyExc_OSError, "Unable to open file '%s', received errno %i: '%s'", filename, errno, strerror(errno)))


/////////////////////
//  PY FETCH DATA  //
/////////////////////

static _always_inline void py_str_data(PyObject *obj, char **base, size_t *size)
{
    if (PyUnicode_IS_COMPACT_ASCII(obj))
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

static PyObject *anyint_to_py(const uint64_t num, const bool is_uint)
{
    if (is_uint)
        return PyLong_FromUnsignedLongLong((unsigned long long)num);
    
    return PyLong_FromLongLong((long long)num);
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


////////////////////
//  ARGS PARSING  //
////////////////////

#if PY_VERSION_HEX >= 0x030d0000 // Python 3.13 and up
#define _unicode_equal(x, y) (PyUnicode_Compare(x, y) == 0)
#else
#define _unicode_equal(x, y) _PyUnicode_EQ(x, y)
#endif

typedef struct {
    PyObject **dest;
    PyTypeObject *tp;
    PyObject *interned;
} keyarg_t;

#define KEYARG(_dest, _type, _interned) \
    {.dest = (PyObject **)_dest, .tp = (PyTypeObject *)_type, .interned = (PyObject *)_interned}

#define min_positional(nargs, min) do { \
    if (_UNLIKELY(min > nargs)) \
    { \
        PyErr_Format(PyExc_TypeError, "Expected at least %zi positional arguments, but received only %zi", min, nargs); \
        return NULL; \
    } \
} while (0)

static _always_inline PyObject *parse_positional(PyObject **args, size_t nth, PyTypeObject *type, const char *argname)
{
    PyObject *obj = args[nth];

    if (type && _UNLIKELY(!Py_IS_TYPE(obj, type)))
    {
        error_unexpected_type(argname, type->tp_name, Py_TYPE(obj)->tp_name);
        return NULL;
    }

    return obj;
}

#define NULLCHECK(obj) \
    if (_UNLIKELY(!obj)) return NULL;

static _always_inline bool _parse_kwarg(PyObject *kwname, PyObject *val, Py_ssize_t nkey, keyarg_t *keyargs)
{
    for (Py_ssize_t i = 0; i < nkey; ++i)
    {
        keyarg_t dest = keyargs[i];

        if (dest.interned == kwname)
        {
            if (dest.tp != NULL && _UNLIKELY(!Py_IS_TYPE(val, dest.tp)))
            {
                error_unexpected_type((const char *)((PyASCIIObject *)dest.interned + 1), dest.tp->tp_name, Py_TYPE(val)->tp_name);
                return false;
            }

            *dest.dest = val;
            return true;
        }
    }

    for (Py_ssize_t i = 0; i < nkey; ++i)
    {
        keyarg_t dest = keyargs[i];

        if (_unicode_equal(dest.interned, kwname))
        {
            if (dest.tp != NULL && _UNLIKELY(!Py_IS_TYPE(*dest.dest, dest.tp)))
            {
                error_unexpected_type((const char *)((PyASCIIObject *)dest.interned + 1), dest.tp->tp_name, Py_TYPE(val)->tp_name);
                return false;
            }

            *dest.dest = val;
            return true;
        }
    }

    // This is reached on a keyword mismatch
    PyErr_Format(PyExc_TypeError, "Received an argument with unexpected keyword '%s'", PyUnicode_AsUTF8(kwname));
    return false;
}

static _always_inline bool parse_keywords(Py_ssize_t npos, PyObject **args, Py_ssize_t nargs, PyObject *kwargs, keyarg_t *keyargs, Py_ssize_t nkey)
{
    // Return early if nothing to parse
    if (nargs - npos <= 0 && kwargs == NULL)
        return true;
    
    // Skip over the positional arguments
    nargs -= npos;
    args += npos;

    if (_UNLIKELY(nargs > nkey))
    {
        PyErr_Format(PyExc_TypeError, "Expected at max %zi optional positional arguments, but received %zi", nkey, nargs);
        return false;
    }

    // Go over any remaining positional arguments for arguments not strictly positional
    for (Py_ssize_t i = 0; i < nargs; ++i)
    {
        keyarg_t keyarg = keyargs[i];
        PyObject *obj = args[i];

        if (keyarg.tp && _UNLIKELY(!Py_IS_TYPE(obj, keyarg.tp)))
        {
            // Use the interned string's value for the argument name
            error_unexpected_type(PyUnicode_AsUTF8(obj), keyarg.tp->tp_name, Py_TYPE(obj)->tp_name);
            return false;
        }

        *keyarg.dest = obj;
    }

    // Parse any keyword arguments if we received them
    if (kwargs != NULL)
    {
        // Skip the keyword arguments that were received as positional arguments
        keyargs += nargs;
        nkey -= nargs;

        const Py_ssize_t nkwargs = PyTuple_GET_SIZE(kwargs);

        for (Py_ssize_t i = 0; i < nkwargs; ++i)
        {
            if (_UNLIKELY(!_parse_kwarg(PyTuple_GET_ITEM(kwargs, i), args[nargs + i], nkey, keyargs)))
                return false;
        }
    }

    return true;
}

#define NKEYARGS(keyargs) \
    (sizeof(keyargs) / sizeof(keyargs[0]))


/////////////////////
//   EXT OBJECTS   //
/////////////////////

#define EXT_ENCODE_HASH(n) (((uintptr_t)(n) >> 8) % EXT_TABLE_SLOTS)

static PyObject *ExtTypesEncode(PyObject *self, PyObject *pairsdict)
{
    if (_UNLIKELY(!Py_IS_TYPE(pairsdict, &PyDict_Type)))
    {
        error_unexpected_type("pairs", PyDict_Type.tp_name, Py_TYPE(pairsdict)->tp_name);
        return NULL;
    }

    ext_types_encode_t *obj = PyObject_New(ext_types_encode_t, &ExtTypesEncodeObj);
    if (_UNLIKELY(obj == NULL))
        return PyErr_NoMemory();
    
    const size_t npairs = PyDict_GET_SIZE(pairsdict);
    const size_t array_size = npairs * sizeof(PyObject *);

    // Allocate a single buffer for both the keys and vals
    PyObject **keys_vals = (PyObject **)malloc(2 * array_size);
    if (_UNLIKELY(keys_vals == NULL))
        return PyErr_NoMemory();
    
    // Set the key array at the start of the shared buffer, and the val array directly on top of it
    PyObject **keys = keys_vals;
    PyObject **vals = (PyObject **)((char *)keys_vals + array_size);

    // Pairs buffer for the hash table
    keyval_t *pairs = (keyval_t *)PyObject_Malloc(npairs * sizeof(keyval_t));
    if (_UNLIKELY(pairs == NULL))
    {
        free(keys_vals);
        return PyErr_NoMemory();
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

        if (_UNLIKELY(!PyType_CheckExact(key) || !PyCallable_Check(val)))
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
    if (_UNLIKELY(obj == NULL))
        return PyErr_NoMemory();

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

        if (pair.key == tp)
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

// Attempt to convert any object to bytes using an ext object. Returns a tuple to be decremented after copying data from PTR, or NULL on failure
static PyObject *any_to_ext(encbuffer_t *b, PyObject *obj, int8_t *id, char **ptr, size_t *size)
{
    if (b->ext == NULL)
        return NULL;
    
    PyObject *func = ext_encode_pull(b->ext, Py_TYPE(obj));
    if (_UNLIKELY(func == NULL))
        return PyErr_Format(PyExc_ValueError, "Received unsupported type: '%s'", Py_TYPE(obj)->tp_name);
    
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
        return PyErr_NoMemory();

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

static _always_inline PyObject *fixstr_to_py(PyASCIIObject **commons, const char *ptr, const size_t size)
{
    const size_t hash = (size_t)(fnv1a(ptr, size) % COMMONCACHE_SLOTS);
    PyASCIIObject *match = commons[hash];

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
        commons[hash] = (PyASCIIObject *)obj;
    }

    return obj;
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

            key = fixstr_to_py(b->states->caches.strings, ptr, size);
        }
        else
        {
            key = decode_bytes(b);

            if (_UNLIKELY(key == NULL))
            {
                Py_DECREF(dict);
                return NULL;
            }

            if (b->strict_keys && _UNLIKELY(!PyUnicode_Check(key)))
            {
                Py_DECREF(dict);
                PyErr_Format(PyExc_KeyError, "Only string types are supported as map keys in strict mode, received a key of type '%s'", Py_TYPE(key)->tp_name);
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
//  WRITING DATA  //
////////////////////

#define ENSURE_SPACE(extra) do { \
    if (_UNLIKELY(b->offset + (extra) >= b->maxoffset)) \
    { \
        if (_UNLIKELY(encbuffer_expand(b, (extra)) == false)) \
            { return NULL; } \
    } \
} while (0)

// Get the current offset's byte and increment afterwards
#define INCBYTE ((b->offset++)[0])

// Write a header's typemask and its size based on the number of bytes the size should take up
static _always_inline void write_mask(encbuffer_t *b, const unsigned char mask, const size_t size, const size_t nbytes)
{
    if (nbytes == 0) // FIXSIZE
    {
        INCBYTE = mask | size;
    }
    else if (nbytes == 1) // SMALL
    {
        b->offset[0] = mask;
        b->offset[1] = size;
        b->offset += 2;
    }
    else if (nbytes == 2) // MEDIUM
    {
        b->offset[0] = mask;
        b->offset[1] = size >> 8;
        b->offset[2] = size;
        b->offset += 3;
    }
    else if (nbytes == 4) // LARGE
    {
        b->offset[0] = mask;
        b->offset[1] = size >> 24;
        b->offset[2] = size >> 16;
        b->offset[3] = size >> 8;
        b->offset[4] = size;
        b->offset += 5;
    }
}

static _always_inline bool write_str(encbuffer_t *b, PyObject *obj)
{
    size_t size;
    char *base;
    py_str_data(obj, &base, &size);

    if (_UNLIKELY(base == NULL))
        return false;

    if (size <= LIMIT_STR_FIXED)
    {
        // Python allocates more than 32 bytes for unicode objects, so copy in chunks for efficiency

        // Ensure space of 33, which is 1 for the mask and 32 for the 2 chunks case
        ENSURE_SPACE(33);

        write_mask(b, DT_STR_FIXED, size, 0);

        // Always copy the first chunk
        memcpy(b->offset, base, 16);

        // Check if we also need to copy a second chunk
        if (size > 16)
            memcpy(b->offset + 16, base + 16, 16);

        b->offset += size;

        return true;
    }

    ENSURE_SPACE(size + 5);

    if (size <= LIMIT_SMALL)
    {
        write_mask(b, DT_STR_SMALL, size, 1);
    }
    else if (size <= LIMIT_MEDIUM)
    {
        write_mask(b, DT_STR_MEDIUM, size, 2);
    }
    else if (_LIKELY(size <= LIMIT_LARGE))
    {
        write_mask(b, DT_STR_LARGE, size, 4);
    }
    else
    {
        error_size_limit(String, size);
        return false;
    }

    memcpy(b->offset, base, size);
    b->offset += size;

    return true;
}

static _always_inline bool write_bin(encbuffer_t *b, PyObject *obj)
{
    char *base;
    size_t size;
    py_bin_data(obj, &base, &size);

    ENSURE_SPACE(5 + size);

    if (size <= LIMIT_SMALL)
    {
        write_mask(b, DT_BIN_SMALL, size, 1);
    }
    else if (size <= LIMIT_MEDIUM)
    {
        write_mask(b, DT_BIN_MEDIUM, size, 2);
    }
    else if (_LIKELY(size <= LIMIT_LARGE))
    {
        write_mask(b, DT_BIN_LARGE, size, 4);
    }
    else
    {
        error_size_limit(Binary, size);
        return false;
    }

    memcpy(b->offset, base, size);
    b->offset += size;

    return true;
}

static _always_inline bool write_arr(encbuffer_t *b, PyObject *obj)
{
    const size_t nitems = PyList_GET_SIZE(obj);

    ENSURE_SPACE(5);

    if (nitems <= LIMIT_ARR_FIXED)
    {
        write_mask(b, DT_ARR_FIXED, nitems, 0);
    }
    else if (nitems <= LIMIT_MEDIUM)
    {
        write_mask(b, DT_ARR_MEDIUM, nitems, 2);
    }
    else if (_LIKELY(nitems <= LIMIT_LARGE))
    {
        write_mask(b, DT_ARR_LARGE, nitems, 4);
    }
    else
    {
        error_size_limit(Array, nitems);
        return false;
    }

    for (size_t i = 0; i < nitems; ++i)
    {
        if (_UNLIKELY(!encode_object(PyList_GET_ITEM(obj, i), b)))
            return false;
    }

    return true;
}

static _always_inline bool write_map(encbuffer_t *b, PyObject *obj)
{
    const size_t npairs = PyDict_GET_SIZE(obj);

    ENSURE_SPACE(5);
    
    if (npairs <= LIMIT_MAP_FIXED)
    {
        write_mask(b, DT_MAP_FIXED, npairs, 0);
    }
    else if (npairs <= LIMIT_MEDIUM)
    {
        write_mask(b, DT_MAP_MEDIUM, npairs, 2);
    }
    else if (_LIKELY(npairs <= LIMIT_LARGE))
    {
        write_mask(b, DT_MAP_LARGE, npairs, 4);
    }
    else
    {
        error_size_limit(Map, npairs);
        return false;
    }

    Py_ssize_t pos = 0;
    for (size_t i = 0; i < npairs; ++i)
    {
        PyObject *key;
        PyObject *val;

        PyDict_Next(obj, &pos, &key, &val);

        if (b->strict_keys && _UNLIKELY(!PyUnicode_Check(key)))
        {
            PyErr_Format(PyExc_KeyError, "Only string types are supported as map keys in strict mode, received a key of type '%s'", Py_TYPE(key)->tp_name);
            return false;
        }

        if (_UNLIKELY(
            !encode_object(key, b) ||
            !encode_object(val, b)
        )) return false;
    }

    return true;
}

static bool write_int(encbuffer_t *b, PyObject *obj)
{
    ENSURE_SPACE(9);

    uint64_t num;
    bool neg;

    // Use manual integer extraction on Python 3.12+
    #if PY_VERSION_HEX >= 0x030c0000

    PyLongObject *lobj = (PyLongObject *)obj;

    neg = (lobj->long_value.lv_tag & _PyLong_SIGN_MASK) != 0;
    num = lobj->long_value.ob_digit[0];

    if (_UNLIKELY(!_PyLong_IsCompact(lobj)))
    {
        num |= (uint64_t)lobj->long_value.ob_digit[1] << PyLong_SHIFT;

        const size_t digits = lobj->long_value.lv_tag >> _PyLong_NON_SIZE_BITS;
        if (digits > 2)
        {
            const uint64_t dig3 = (uint64_t)lobj->long_value.ob_digit[2];

            const uint64_t dig3_overflow_val = (1ULL << (64 - (2 * PyLong_SHIFT))) - 1;
            const bool dig3_overflow = dig3 > dig3_overflow_val;

            if (_UNLIKELY(digits > 3 || dig3_overflow))
                goto int_size_exceeded;

            num |= dig3 << (2 * PyLong_SHIFT);
        }
    }

    #else

    int overflow = 0;
    num = (uint64_t)PyLong_AsLongLongAndOverflow(obj, &overflow);
    neg = (int64_t)num < 0;

    if (_UNLIKELY(overflow != 0))
        goto int_size_exceeded;

    #endif

    if (!neg)
    {
        if (num <= LIMIT_UINT_FIXED)
        {
            INCBYTE = num;
        }
        else if (num <= LIMIT_UINT_BIT8)
        {
            write_mask(b, DT_UINT_BIT8, num, 1);
        }
        else if (num <= LIMIT_UINT_BIT16)
        {
            write_mask(b, DT_UINT_BIT16, num, 2);
        }
        else if (num <= LIMIT_UINT_BIT32)
        {
            write_mask(b, DT_UINT_BIT32, num, 4);
        }
        else
        {
            INCBYTE = DT_UINT_BIT64;

            const uint64_t _num = BIG_64(num);
            memcpy(b->offset, &_num, 8);
            b->offset += 8;
        }
    }
    else
    {
        int64_t _num = -((int64_t)num);

        if (_num >= LIMIT_INT_FIXED)
        {
            write_mask(b, DT_INT_FIXED, _num & 0b11111, 0);
        }
        else if (_num >= LIMIT_INT_BIT8)
        {
            write_mask(b, DT_INT_BIT8, _num, 1);
        }
        else if (_num >= LIMIT_INT_BIT16)
        {
            write_mask(b, DT_INT_BIT16, _num, 2);
        }
        else if (_num >= LIMIT_INT_BIT32)
        {
            write_mask(b, DT_INT_BIT32, _num, 4);
        }
        else
        {
            INCBYTE = DT_INT_BIT64;

            _num = BIG_64(_num);
            memcpy(b->offset, &_num, 8);
            b->offset += 8;
        }
    }

    return true;


    int_size_exceeded:

    PyErr_SetString(PyExc_OverflowError, "Integer values cannot exceed " LIMIT_UINT_STR " (2^64-1) or " LIMIT_INT_STR " (-2^63)");
    return false;
}

static _always_inline bool write_float(encbuffer_t *b, PyObject *obj)
{
    ENSURE_SPACE(9);

    double num;
    py_float_data(obj, &num);

    char buf[9];

    buf[0] = DT_FLOAT_BIT64;
    memcpy(buf + 1, &num, 8);

    memcpy(b->offset, buf, 9);
    b->offset += 9;

    return true;
}

static _always_inline bool write_bool(encbuffer_t *b, PyObject *obj)
{
    ENSURE_SPACE(1);
    INCBYTE = obj == Py_True ? DT_TRUE : DT_FALSE;
    return true;
}

static _always_inline bool write_nil(encbuffer_t *b)
{
    ENSURE_SPACE(1);
    INCBYTE = DT_NIL;
    return true;
}

#define STR_LITERAL(name, s)            \
    char name[sizeof(s)];               \
    sprintf(name, "%s", s);

static _always_inline bool write_ext(encbuffer_t *b, PyObject *obj)
{
    int8_t id;
    char *ptr;
    size_t size;

    PyObject *result = any_to_ext(b, obj, &id, &ptr, &size);

    if (result == NULL)
    {
        // Error might not be set, so default to unsupported type error
        if (!PyErr_Occurred())
            PyErr_Format(PyExc_ValueError, "Received unsupported type: '%s'", Py_TYPE(obj)->tp_name);
        
        return false;
    }

    if (id == 17)
    {
        STR_LITERAL(test123, "Hello, world! This is a test string.");
        printf(test123);
    }

    ENSURE_SPACE(6 + size);

    // If the size is a base of 2 and not larger than 16, it can be represented with a fixsize mask
    const bool is_baseof2 = (size & (size - 1)) == 0;
    if (size <= 16 && is_baseof2)
    {
        const unsigned char fixmask = (
            size ==  8 ? DT_EXT_FIX8  :
            size == 16 ? DT_EXT_FIX16 :
            size ==  4 ? DT_EXT_FIX4  :
            size ==  2 ? DT_EXT_FIX2  :
                         DT_EXT_FIX1
        );

        INCBYTE = fixmask;
    }
    else if (size <= LIMIT_SMALL)
    {
        write_mask(b, DT_EXT_SMALL, size, 1);
    }
    else if (size <= LIMIT_MEDIUM)
    {
        write_mask(b, DT_EXT_MEDIUM, size, 2);
    }
    else if (_LIKELY(size <= LIMIT_LARGE))
    {
        write_mask(b, DT_EXT_LARGE, size, 4);
    }
    else
    {
        error_size_limit(Ext, size);
        return false;
    }

    INCBYTE = id;

    memcpy(b->offset, ptr, size);
    b->offset += size;

    Py_DECREF(result);

    return true;
}


////////////////////
//    ENCODING    //
////////////////////

static bool encode_object(PyObject *obj, encbuffer_t *b)
{
    PyTypeObject *tp = Py_TYPE(obj);

    if (tp == &PyUnicode_Type)
    {
        return write_str(b, obj);
    }
    else if (tp == &PyLong_Type)
    {
        return write_int(b, obj);
    }
    else if (tp == &PyFloat_Type)
    {
        return write_float(b, obj);
    }
    else if (tp == &PyBool_Type)
    {
        return write_bool(b, obj);
    }
    else if (tp == &PyList_Type)
    {
        return write_arr(b, obj);
    }
    else if (tp == &PyDict_Type)
    {
        return write_map(b, obj);
    }
    else if (tp == Py_TYPE(Py_None)) // No explicit PyNone_Type available
    {
        return write_nil(b);
    }
    else if (tp == &PyBytes_Type)
    {
        return write_bin(b, obj);
    }
    else
    {
        return write_ext(b, obj);
    }
}


////////////////////
//    DECODING    //
////////////////////

// Prefix error message for invalid encoded data
#define INVALID_MSG "Received invalid encoded data"

// Typemask for varlen switch case
#define VARLEN_DT(mask) (mask & 0b11111)

// INCBYTE but masked to ensure we operate with a single byte
#define SIZEBYTE ((b->base + b->offset++)[0] & 0xFF)

// Check if the buffer won't be overread
#define OVERREAD_CHECK(to_add) do { \
    if (_UNLIKELY(b->offset + to_add > b->allocated)) \
    { \
        if (_LIKELY(b->is_stream)) \
        { \
            NULLCHECK(decbuffer_refresh((streamdec_t *)b, to_add)); \
        } \
        else \
        { \
            PyErr_SetString(PyExc_ValueError, INVALID_MSG " (overread the encoded data)"); \
            return NULL; \
        } \
    } \
} while (0)

static PyObject *decode_bytes(decbuffer_t *b)
{
    OVERREAD_CHECK(1);

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

            return fixstr_to_py(b->states->caches.strings, ptr, n);
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
        else if ((mask & 0b11110000) == DT_ARR_FIXED) // Bit 5 is also set on ARR and MAP
        {
            return anyarr_to_py(b, n & 0x0F);
        }
        else if ((mask & 0b11110000) == DT_MAP_FIXED)
        {
            return anymap_to_py(b, n & 0x0F);
        }
        else
        {
            PyErr_SetString(PyExc_ValueError, INVALID_MSG " (invalid header)");
            return NULL;
        }
    }
    else
    {
        b->offset++;

        // Global variable to hold the size across multiple cases
        uint64_t n = 0;

        const unsigned int varlen_mask = VARLEN_DT(mask);
        switch (varlen_mask)
        {
        case VARLEN_DT(DT_STR_LARGE):
        {
            n |= SIZEBYTE << 24;
            n |= SIZEBYTE << 16;
        }
        case VARLEN_DT(DT_STR_MEDIUM):
        {
            n |= SIZEBYTE << 8;
        }
        case VARLEN_DT(DT_STR_SMALL):
        {
            n |= SIZEBYTE;

            OVERREAD_CHECK(n);

            PyObject *obj = anystr_to_py(b->base + b->offset, n);
            b->offset += n;
            return obj;
        }

        case VARLEN_DT(DT_UINT_BIT8):
        {
            OVERREAD_CHECK(1);
            n = SIZEBYTE;
            
            return anyint_to_py(n, true);
        }
        case VARLEN_DT(DT_UINT_BIT16):
        {
            OVERREAD_CHECK(2);
            n |= SIZEBYTE << 8;
            n |= SIZEBYTE;
            
            return anyint_to_py(n, true);
        }
        case VARLEN_DT(DT_UINT_BIT32):
        {
            OVERREAD_CHECK(4);
            n |= SIZEBYTE << 24;
            n |= SIZEBYTE << 16;
            n |= SIZEBYTE << 8;
            n |= SIZEBYTE;
            
            return anyint_to_py(n, true);
        }
        case VARLEN_DT(DT_UINT_BIT64):
        {
            OVERREAD_CHECK(8);

            memcpy(&n, b->base + b->offset, 8);
            b->offset += 8;

            n = BIG_64(n);

            return anyint_to_py(n, true);
        }

        case VARLEN_DT(DT_INT_BIT8):
        {
            OVERREAD_CHECK(1);
            n |= SIZEBYTE;

            // Sign-extend
            if (_LIKELY(n & 0x80))
                n |= ~0xFF;
            
            return anyint_to_py(n, false);
        }
        case VARLEN_DT(DT_INT_BIT16):
        {
            OVERREAD_CHECK(2);
            n |= SIZEBYTE << 8;
            n |= SIZEBYTE;

            if (_LIKELY(n & 0x8000))
                n |= ~0xFFFF;
            
            return anyint_to_py(n, false);
        }
        case VARLEN_DT(DT_INT_BIT32):
        {
            OVERREAD_CHECK(4);
            n |= SIZEBYTE << 24;
            n |= SIZEBYTE << 16;
            n |= SIZEBYTE << 8;
            n |= SIZEBYTE;

            if (_LIKELY(n & 0x80000000))
                n |= ~0xFFFFFFFF;
            
            return anyint_to_py(n, false);
        }
        case VARLEN_DT(DT_INT_BIT64):
        {
            OVERREAD_CHECK(8);

            memcpy(&n, b->base + b->offset, 8);
            b->offset += 8;
            n = BIG_64(n);

            return anyint_to_py(n, false);
        }

        case VARLEN_DT(DT_ARR_LARGE):
        {
            OVERREAD_CHECK(4);
            n |= SIZEBYTE << 24;
            n |= SIZEBYTE << 16;
        }
        case VARLEN_DT(DT_ARR_MEDIUM):
        {
            OVERREAD_CHECK(2);
            n |= SIZEBYTE << 8;
            n |= SIZEBYTE;

            return anyarr_to_py(b, n);
        }

        case VARLEN_DT(DT_MAP_LARGE):
        {
            OVERREAD_CHECK(4);
            n |= SIZEBYTE << 24;
            n |= SIZEBYTE << 16;
        }
        case VARLEN_DT(DT_MAP_MEDIUM):
        {
            OVERREAD_CHECK(2);
            n |= SIZEBYTE << 8;
            n |= SIZEBYTE;

            return anymap_to_py(b, n);
        }

        case VARLEN_DT(DT_NIL):
        {
            return nil_to_py();
        }
        case VARLEN_DT(DT_TRUE):
        {
            return true_to_py();
        }
        case VARLEN_DT(DT_FALSE):
        {
            return false_to_py();
        }

        case VARLEN_DT(DT_FLOAT_BIT32):
        case VARLEN_DT(DT_FLOAT_BIT64):
        {
            double num;

            if (mask == DT_FLOAT_BIT64)
            {
                OVERREAD_CHECK(8);

                memcpy(&num, b->base + b->offset, 8);
                b->offset += 8;
            }
            else
            {
                OVERREAD_CHECK(4);

                float _num;
                memcpy(&_num, b->base + b->offset, 4);
                num = (double)_num;

                b->offset += 4;
            }

            BIG_DOUBLE(num);

            return anyfloat_to_py(num);
        }

        case VARLEN_DT(DT_BIN_LARGE):
        {
            OVERREAD_CHECK(4);
            n |= SIZEBYTE << 24;
            n |= SIZEBYTE << 16;
        }
        case VARLEN_DT(DT_BIN_MEDIUM):
        {
            OVERREAD_CHECK(2);
            n |= SIZEBYTE << 8;
        }
        case VARLEN_DT(DT_BIN_SMALL):
        {
            OVERREAD_CHECK(1);
            n |= SIZEBYTE;

            PyObject *obj = anybin_to_py(b->base + b->offset, n);
            b->offset += n;
            return obj;
        }

        case VARLEN_DT(DT_EXT_FIX1):
        {
            OVERREAD_CHECK(1); // For the ID
            n = 1;

            goto ext_handling;
        }
        case VARLEN_DT(DT_EXT_FIX2):
        {
            OVERREAD_CHECK(1);
            n = 2;
            
            goto ext_handling;
        }
        case VARLEN_DT(DT_EXT_FIX4):
        {
            OVERREAD_CHECK(1);
            n = 4;
            
            goto ext_handling;
        }
        case VARLEN_DT(DT_EXT_FIX8):
        {
            OVERREAD_CHECK(1);
            n = 8;
            
            goto ext_handling;
        }
        case VARLEN_DT(DT_EXT_FIX16):
        {
            OVERREAD_CHECK(1);
            n = 16;
            
            goto ext_handling;
        }

        case VARLEN_DT(DT_EXT_LARGE):
        {
            OVERREAD_CHECK(5); // 4 size bytes + 1 ID byte
            n |= SIZEBYTE << 24;
            n |= SIZEBYTE << 16;
        }
        case VARLEN_DT(DT_EXT_MEDIUM):
        {
            OVERREAD_CHECK(3);
            n |= SIZEBYTE << 8;
        }
        case VARLEN_DT(DT_EXT_SMALL):
        {
            OVERREAD_CHECK(2);
            n |= SIZEBYTE;

            ext_handling: // For fixsize cases
            NULL; // No-op statement for pre-C23 standards (declarations not allowed after labels)

            const int8_t id = SIZEBYTE;

            OVERREAD_CHECK(n);

            PyObject *obj = ext_to_any(b, id, n);

            if (obj == NULL)
            {
                if (!PyErr_Occurred())
                    PyErr_Format(PyExc_ValueError, INVALID_MSG " (failed to match an ext type with ID %i)", id);
                
                return NULL;
            }

            return obj;
        }

        default:
        {
            PyErr_SetString(PyExc_ValueError, INVALID_MSG " (invalid header)");
            return NULL;
        }
        }
    }
}


///////////////////////////
//  ADAPTIVE ALLOCATION  //
///////////////////////////

// Default size to allocate for encode buffers
#define ENCBUFFER_DEFAULTSIZE 256

// Prepare the encoding buffer struct
static _always_inline bool encbuffer_prepare(encbuffer_t *b, mstates_t *states, PyObject *obj)
{
    // Attempt to allocate appropriate memory for the buffer based on OBJ's type
    PyTypeObject *tp = Py_TYPE(obj);
    size_t new_size = 0;

    if (_LIKELY(tp == &PyList_Type || tp == &PyDict_Type))
    {
        // Get the number of items in the container
        b->nitems = Py_SIZE(obj);
        
        // Add allocation size based on the number of items
        new_size += b->nitems * states->encoding.item_avg;
    }
    else
    {
        // NITEMS is 0 with non-container objects
        b->nitems = 0;

        // Use just the average alloc size
        new_size = states->encoding.extra_avg;
    }

    // Check if the size estimated to be required doesn't exceed the size of the existing buffer, and re-use that if possible
    if (Py_REFCNT(states->encoding.obj) == 1 && states->encoding.size >= new_size)
    {
        // NITEMS is -1 when we can't update allocation data
        b->nitems = -1;
    }
    else
    {
        // Release reference to the last object
        Py_DECREF(states->encoding.obj);

        // Set the new size
        states->encoding.size = new_size;

        // Create the new buffer object
        states->encoding.obj = (PyBytesObject *)PyBytes_FromStringAndSize(NULL, states->encoding.size);

        if (_UNLIKELY(states->encoding.obj == NULL))
        {
            PyErr_NoMemory();
            return false;
        }
    }

    // Set up the offset and max offset based on the buffer
    b->offset = states->encoding.obj->ob_sval;
    b->maxoffset = states->encoding.obj->ob_sval + states->encoding.size;

    // Assign the states to the struct
    b->states = states;

    return true;
}

// Get the size of an encbuffer object
static _always_inline size_t encbuffer_size(encbuffer_t *b)
{
    // Calculate the size by subtracting the base address from the offset address
    return b->offset - b->states->encoding.obj->ob_sval;
}

#define EXTRA_ALLOC_MIN 64
#define ITEM_ALLOC_MIN 6

static void update_adaptive_allocation(encbuffer_t *b)
{
    const size_t allocated = b->states->encoding.size;
    const size_t needed = encbuffer_size(b);
    const Py_ssize_t nitems = b->nitems;

    // Early return if NITEMS is -1, meaning we re-used a buffer and cannot update
    if (nitems == -1)
        return;

    // Take the average between how much we needed and the currently set value
    const size_t extra_avg_val = (b->states->encoding.extra_avg + needed) / 2;

    // Set the size to how much we used plus a bit of what was allocated extra
    // to avoid under-allocating in the next round
    b->states->encoding.extra_avg = extra_avg_val + ((allocated - needed) / 4);

    // Enforce a minimum size for safety. Check if the threshold is crossed by subtracting
    // it from the value, casting to a signed value, and checking if we're below 0 (overflow happened)
    if (_UNLIKELY((Py_ssize_t)(b->states->encoding.extra_avg - EXTRA_ALLOC_MIN) < 0))
        b->states->encoding.extra_avg = EXTRA_ALLOC_MIN;
    
    // Return if NITEMS is 0 to avoid division by zero
    if (b->nitems == 0)
        return;

    // We use multiplication by reciprocal instead of division for speed below

    // Size allocated per item and required per item
    const size_t used_per_item = allocated / nitems;
    const size_t needed_per_item = needed / nitems;

    // Take the average between the currently set per-item value and that of this round
    const size_t item_avg_val = (b->states->encoding.item_avg + needed_per_item) / 2;

    // Set the new size to the average, and add a bit based on what was allocated extra
    b->states->encoding.item_avg = item_avg_val + ((used_per_item - needed_per_item) / 2);

    // Enforce a value threshold as we did with the `extra_avg` value
    if (_UNLIKELY((Py_ssize_t)(b->states->encoding.item_avg - ITEM_ALLOC_MIN) < 0))
        b->states->encoding.item_avg = ITEM_ALLOC_MIN;

    return;
}

// Prepares the bytes object to give the user and return it
static _always_inline PyObject *encbuffer_return(encbuffer_t *b)
{
    PyObject *obj = (PyObject *)b->states->encoding.obj;

    // Incref to keep a reference
    Py_INCREF(obj);

    // Set the object's size
    Py_SET_SIZE(obj, encbuffer_size(b));

    // Update the adaptive allocation
    update_adaptive_allocation(b);

    // Return the object
    return obj;
}

static bool encbuffer_expand(encbuffer_t *b, size_t extra)
{
    // Scale the size by factor 1.5x
    const size_t new_size = (encbuffer_size(b) + extra) * 1.5;

    // Reallocate the object (also allocate space for the bytes object itself)
    PyBytesObject *reallocd = PyObject_Realloc(b->states->encoding.obj, new_size + PyBytesObject_SIZE);

    if (_UNLIKELY(reallocd == NULL))
    {
        PyErr_NoMemory();
        return false;
    }

    // Update the offsets
    b->offset = reallocd->ob_sval + encbuffer_size(b);
    b->maxoffset = reallocd->ob_sval + b->states->encoding.size;

    // Update the mod states
    b->states->encoding.obj = reallocd;
    b->states->encoding.size = new_size;

    return true;
}


///////////////////////
//  ENC/DEC HANDLES  //
///////////////////////

static PyObject *encode(PyObject *self, PyObject **args, Py_ssize_t nargs, PyObject *kwargs)
{
    const Py_ssize_t npos = 1;
    min_positional(nargs, npos);

    PyObject *obj = parse_positional(args, 0, NULL, "obj");
    NULLCHECK(obj);

    mstates_t *states = get_mstates(self);
    encbuffer_t b;

    b.ext = NULL;
    PyObject *strict_keys = NULL;

    keyarg_t keyargs[] = {
        KEYARG(&b.ext, &ExtTypesEncodeObj, states->interned.ext_types),
        KEYARG(&strict_keys, &PyBool_Type, states->interned.strict_keys)
    };

    if (_UNLIKELY(!parse_keywords(npos, args, nargs, kwargs, keyargs, NKEYARGS(keyargs))))
        return NULL;

    b.strict_keys = strict_keys == Py_True;


    // Prepare the encode buffer
    NULLCHECK(encbuffer_prepare(&b, get_mstates(self), obj));

    // Encode the object
    NULLCHECK(encode_object(obj, &b));
    
    return encbuffer_return(&b);
}

static PyObject *decode(PyObject *self, PyObject **args, Py_ssize_t nargs, PyObject *kwargs)
{
    const Py_ssize_t npos = 1;
    min_positional(nargs, npos);

    PyObject *encoded = parse_positional(args, 0, NULL, "encoded");
    NULLCHECK(encoded);

    decbuffer_t b;

    b.ext = NULL;
    PyObject *strict_keys = NULL;

    mstates_t *states = get_mstates(self);

    keyarg_t keyargs[] = {
        KEYARG(&b.ext, &ExtTypesDecodeObj, states->interned.ext_types),
        KEYARG(&strict_keys, &PyBool_Type, states->interned.strict_keys),
    };

    if (_UNLIKELY(!parse_keywords(npos, args, nargs, kwargs, keyargs, NKEYARGS(keyargs))))
        return NULL;
    
    b.strict_keys = strict_keys == Py_True;


    Py_buffer buffer;
    if (PyObject_GetBuffer(encoded, &buffer, PyBUF_SIMPLE) != 0)
        return NULL;
    
    b.base = buffer.buf;
    b.allocated = buffer.len;
    b.offset = 0;
    b.is_stream = false;
    b.states = states;

    PyObject *result = decode_bytes(&b);
    PyBuffer_Release(&buffer);

    if (_UNLIKELY(result != NULL && b.offset != b.allocated))
    {
        PyErr_SetString(PyExc_ValueError, INVALID_MSG " (encoded message ended early)");
        return NULL;
    }

    return result;
}

static PyObject *Encoder(PyObject *self, PyObject **args, Py_ssize_t nargs, PyObject *kwargs)
{
    PyObject *ext = NULL;
    PyObject *strict_keys = NULL;

    mstates_t *states = get_mstates(self);

    keyarg_t keyargs[] = {
        KEYARG(&ext, &ExtTypesEncodeObj, states->interned.ext_types),
        KEYARG(&strict_keys, &PyBool_Type, states->interned.strict_keys),
    };

    NULLCHECK(parse_keywords(0, args, nargs, kwargs, keyargs, NKEYARGS(keyargs)))

    encoder_t *enc = PyObject_New(encoder_t, &EncoderObj);
    if (enc == NULL)
        return PyErr_NoMemory();

    enc->data.ext = (ext_types_encode_t *)ext;
    enc->data.strict_keys = strict_keys == Py_True;

    return (PyObject *)enc;
}

static PyObject *Decoder(PyObject *self, PyObject **args, Py_ssize_t nargs, PyObject *kwargs)
{
    decoder_t *dec = PyObject_New(decoder_t, &DecoderObj);
    if (dec == NULL)
        return PyErr_NoMemory();

    dec->data.ext = NULL;
    PyObject *strict_keys = NULL;

    mstates_t *states = get_mstates(self);

    keyarg_t keyargs[] = {
        KEYARG(&dec->data.ext, &ExtTypesDecodeObj, states->interned.ext_types),
        KEYARG(&strict_keys, &PyBool_Type, states->interned.strict_keys),
    };

    if (_UNLIKELY(!parse_keywords(0, args, nargs, kwargs, keyargs, NKEYARGS(keyargs))))
    {
        Py_DECREF(dec);
        return NULL;
    }

    dec->data.strict_keys = strict_keys == Py_True;
    dec->data.is_stream = false;

    return (PyObject *)dec;
}

static PyObject *encoder_encode(encoder_t *enc, PyObject *obj)
{
    // Prepare the encode buffer
    NULLCHECK(encbuffer_prepare(&enc->data, get_mstates(NULL), obj));

    // Encode the object
    NULLCHECK(encode_object(obj, &enc->data) == false);
    
    return encbuffer_return(&enc->data);
}

static PyObject *decoder_decode(decoder_t *dec, PyObject *encoded)
{
    Py_buffer buffer;
    if (PyObject_GetBuffer(encoded, &buffer, PyBUF_SIMPLE) != 0)
        return NULL;
    
    dec->data.base = buffer.buf;
    dec->data.allocated = buffer.len;
    dec->data.offset = 0;
    dec->data.states = get_mstates(NULL);

    PyObject *result = decode_bytes(&dec->data);
    PyBuffer_Release(&buffer);

    if (_UNLIKELY(result != NULL && dec->data.offset != dec->data.allocated))
    {
        PyErr_SetString(PyExc_ValueError, INVALID_MSG " (encoded message ended early)");
        return NULL;
    }

    return result;
}


static void streamenc_dealloc(streamenc_t *enc)
{
    PyObject_FREE(enc->file.name);
    PyObject_Del(enc);
}

static void streamdec_dealloc(streamdec_t *dec)
{
    free(dec->data.base);
    PyObject_FREE(dec->file.name);
    PyObject_Del(dec);
}

static PyObject *StreamEncoder(PyObject *self, PyObject **args, Py_ssize_t nargs, PyObject *kwargs)
{
    PyObject *filename = NULL;
    PyObject *ext = NULL;
    PyObject *strict_keys = NULL;

    mstates_t *states = get_mstates(self);

    keyarg_t keyargs[] = {
        KEYARG(&filename, &PyUnicode_Type, states->interned.file_name),
        KEYARG(&ext, &ExtTypesEncodeObj, states->interned.ext_types),
        KEYARG(&strict_keys, &PyBool_Type, states->interned.strict_keys),
    };

    NULLCHECK(parse_keywords(0, args, nargs, kwargs, keyargs, NKEYARGS(keyargs)))

    if (_UNLIKELY(filename == NULL))
    {
        PyErr_SetString(PyExc_TypeError, "Expected at least the 'file_name' argument");
        return NULL;
    }

    streamenc_t *enc = PyObject_New(streamenc_t, &StreamEncoderObj);
    if (enc == NULL)
        return PyErr_NoMemory();

    size_t strsize;
    char *str;
    py_str_data(filename, &str, &strsize);
    
    enc->file.name = PyObject_Malloc(strsize);
    if (_UNLIKELY(enc->file.name == NULL))
    {
        Py_DECREF(enc);
        return NULL;
    }

    enc->file.file = fopen(enc->file.name, "ab");
    if (_UNLIKELY(enc->file.file == NULL))
    {
        const int err = errno;
        error_cannot_open_file(enc->file.name, err);
        return NULL;
    }

    // Disable buffering, we already write in chunks
    setvbuf(enc->file.file, NULL, _IONBF, 0);

    enc->data.ext = (ext_types_encode_t *)ext;
    enc->data.strict_keys = strict_keys == Py_True;

    return (PyObject *)enc;
}

static PyObject *StreamDecoder(PyObject *self, PyObject **args, Py_ssize_t nargs, PyObject *kwargs)
{
    PyObject *filename = NULL;
    PyObject *ext = NULL;
    PyObject *strict_keys = NULL;

    mstates_t *states = get_mstates(self);

    keyarg_t keyargs[] = {
        KEYARG(&filename, &PyUnicode_Type, states->interned.file_name),
        KEYARG(&ext, &ExtTypesDecodeObj, states->interned.ext_types),
        KEYARG(&strict_keys, &PyBool_Type, states->interned.strict_keys),
    };

    NULLCHECK(parse_keywords(0, args, nargs, kwargs, keyargs, NKEYARGS(keyargs)))

    if (_UNLIKELY(filename == NULL))
    {
        PyErr_SetString(PyExc_TypeError, "Expected at least the 'file_name' argument");
        return NULL;
    }

    streamdec_t *dec = PyObject_New(streamdec_t, &StreamDecoderObj);
    if (dec == NULL)
        return PyErr_NoMemory();

    // Pre-allocate a buffer for the decoder
    dec->data.allocated = 4096;
    dec->data.base = (char *)malloc(dec->data.allocated);

    if (_UNLIKELY(dec->data.base == NULL))
    {
        PyObject_Del(dec); // No DECREF, filename is not allocated yet
        return PyErr_NoMemory();
    }

    size_t strsize;
    char *str;
    py_str_data(filename, &str, &strsize);
    
    dec->file.name = PyObject_Malloc(strsize);
    if (_UNLIKELY(dec->file.name == NULL))
    {
        Py_DECREF(dec);
        return NULL;
    }

    dec->file.file = fopen(dec->file.name, "rb");
    if (_UNLIKELY(dec->file.file == NULL))
    {
        const int err = errno;
        error_cannot_open_file(dec->file.name, err);
        return NULL;
    }

    // Disable buffering, we already chunk by writing after an entire call
    setvbuf(dec->file.file, NULL, _IONBF, 0);

    dec->data.ext = (ext_types_decode_t *)ext;
    dec->data.strict_keys = strict_keys == Py_True;
    dec->data.is_stream = true;

    return (PyObject *)dec;
}

static PyObject *streamenc_encode(streamenc_t *enc, PyObject *obj)
{
    PyBytesObject *result = (PyBytesObject *)encoder_encode((encoder_t *)((char *)enc + STREAMOBJ_DATA_OFFSET), obj);
    NULLCHECK(result);

    const size_t size = PyBytes_GET_SIZE(result);

    // Check if all data is written to the file, otherwise truncate up until data that was written incompletely
    size_t written;
    if (_UNLIKELY(encbuffer_size(&enc->data) != (written = fwrite(result->ob_sval, 1, size, enc->file.file))))
    {
        // Attempt to truncate the file
        if (_UNLIKELY(_ftruncate_and_close(enc->file.file, ftell(enc->file.file) - written, enc->file.name) != 0))
        {
            enc->file.file = NULL;

            const int err = errno;
            PyErr_Format(PyExc_OSError, "While handling the above exception, truncating the file back to its old size failed, leaving partially written or corrupt data in file '%s'. Received errno %i: %s", enc->file.name, err, strerror(err));
            return NULL;
        }
    }

    return (PyObject *)result;
}

static PyObject *streamdec_decode(streamdec_t *dec)
{
    dec->data.states = get_mstates(NULL);

    if (dec->file.file == NULL)
    {
        dec->file.file = fopen(dec->file.name, "rb");

        if (_UNLIKELY(dec->file.file == NULL))
        {
            const int err = errno;
            error_cannot_open_file(dec->file.name, err);
            return NULL;
        }

        // Disable buffering, we use a custom buffer
        setvbuf(dec->file.file, NULL, _IONBF, 0);
    }
    
    // Don't refresh the buffer yet, we can continue from the buffer offset as the buffer contains part of the data we need now (remaining from the last call, or set up on StreamDecoder creation)

    PyObject *result = decode_bytes(&dec->data);

    if (_UNLIKELY(result != NULL && dec->data.offset != dec->data.allocated))
    {
        PyErr_SetString(PyExc_ValueError, INVALID_MSG " (encoded message ended early)");
        return NULL;
    }

    return result;
}

static PyObject *streamobj_setfile(filedata_t *f, PyObject *obj)
{
    if (_UNLIKELY(!PyUnicode_CheckExact(obj)))
    {
        error_unexpected_type("file_name", PyUnicode_Type.tp_name, Py_TYPE(obj)->tp_name);
        return NULL;
    }

    PyObject_FREE(f->name);

    size_t size;
    char *str;
    py_str_data(obj, &str, &size);

    f->name = (char *)PyObject_Malloc(size + 1);

    if (_UNLIKELY(f->name == NULL))
        return PyErr_NoMemory();
    
    memcpy(f->name, str, size);
    f->name[size] = 0;

    Py_RETURN_NONE;
}

static bool decbuffer_refresh(streamdec_t *dec, const size_t requested)
{
    // Check if the requested size fits in the buffer
    if (_UNLIKELY(requested > dec->data.allocated))
    {
        // Allocate a new chunk that matches the needs of the request
        const size_t new_size = requested * 1.5;
        char *tmp = (char *)malloc(new_size);

        if (tmp == NULL)
        {
            PyErr_NoMemory();
            return false;
        }

        free(dec->data.base);

        dec->data.base = tmp;
        dec->data.allocated = new_size;
    }

    // How much we have to read based on what was requested
    const size_t to_read = dec->data.allocated - requested;

    // How much we have to preserve from the buffer
    const size_t to_preserve = dec->data.allocated - dec->data.offset;

    // Move the data to preserve to the start of the buffer
    memmove(dec->data.base, dec->data.base + dec->data.offset, to_preserve);

    // Read the new data into the buffer, above the preserved data.
    // Update ALLOCATED with the bytes read, ensuring a partial read will limit what can be read from the buffer
    if (_UNLIKELY((dec->data.allocated = fread(dec->data.base + to_preserve, 1, to_read, dec->file.file)) < requested))
    {
        PyErr_Format(PyExc_EOFError, "Reached the end of file '%s' before finishing a decoding process", dec->file.file);
        return false;
    }

    return true;
}


//////////////////
//    STATES    //
//////////////////

// Get interned string of NAME
#define GET_ISTR(name) if ((s->interned.name = PyUnicode_InternFromString(#name)) == NULL) { return false; }

static bool setup_mstates(mstates_t *s)
{
    // Create interned strings
    GET_ISTR(obj)
    GET_ISTR(ext_types)
    GET_ISTR(strict_keys)
    GET_ISTR(file_name)
    GET_ISTR(keep_open)

    // Allocate caches
    s->caches.strings = (PyASCIIObject **)calloc(COMMONCACHE_SLOTS, sizeof(PyASCIIObject *));
    if (s->caches.strings == NULL)
    {
        return false;
    }

    // Create initial bytes object for re-use
    s->encoding.size = ENCBUFFER_DEFAULTSIZE;
    s->encoding.obj = (PyBytesObject *)PyBytes_FromStringAndSize(NULL, s->encoding.size);

    if (s->encoding.obj == NULL)
    {
        PyErr_NoMemory();
        return false;
    }

    // Initialize the average sizes
    s->encoding.extra_avg = EXTRA_ALLOC_MIN;
    s->encoding.item_avg = ITEM_ALLOC_MIN;

    return true;
}

static void cleanup_mstates(mstates_t *s)
{
    // Clean up the cache
    for (size_t i = 0; i < COMMONCACHE_SLOTS; ++i)
    {
        Py_XDECREF(s->caches.strings[i]);
    }
    free(s->caches.strings);

    // Remove reference to last encode object
    Py_DECREF(s->encoding.obj);
}

// Get the global states of the module
static _always_inline mstates_t *get_mstates(PyObject *m)
{
    if (m)
        return (mstates_t *)PyModule_GetState(m);
    
    return (mstates_t *)PyModule_GetState(PyState_FindModule(&cmsgpack));
}


/////////////////////
//   METHOD DEFS   //
/////////////////////

static void cleanup_module(PyObject *m);

static PyMethodDef EncoderMethods[] = {
    {"encode", (PyCFunction)encoder_encode, METH_O, NULL},
};

static PyMethodDef DecoderMethods[] = {
    {"decode", (PyCFunction)decoder_decode, METH_O, NULL},
};

static PyMethodDef StreamEncoderMethods[] = {
    {"encode", (PyCFunction)streamenc_encode, METH_O, NULL},
    {"set_file", (PyCFunction)streamobj_setfile, METH_O, NULL},
};

static PyMethodDef StreamDecoderMethods[] = {
    {"decode", (PyCFunction)streamdec_decode, METH_NOARGS, NULL},
    {"set_file", (PyCFunction)streamobj_setfile, METH_O, NULL},
};

static PyMethodDef CmsgpackMethods[] = {
    {"encode", (PyCFunction)encode, METH_FASTCALL | METH_KEYWORDS, NULL},
    {"decode", (PyCFunction)decode, METH_FASTCALL | METH_KEYWORDS, NULL},

    {"ExtTypesEncode", (PyCFunction)ExtTypesEncode, METH_O, NULL},
    {"ExtTypesDecode", (PyCFunction)ExtTypesDecode, METH_FASTCALL, NULL},

    {"Encoder", (PyCFunction)Encoder, METH_FASTCALL | METH_KEYWORDS, NULL},
    {"Decoder", (PyCFunction)Decoder, METH_FASTCALL | METH_KEYWORDS, NULL},

    {"StreamEncoder", (PyCFunction)StreamEncoder, METH_FASTCALL | METH_KEYWORDS, NULL},
    {"StreamDecoder", (PyCFunction)StreamDecoder, METH_FASTCALL | METH_KEYWORDS, NULL},

    {NULL, NULL, 0, NULL}
};


///////////////////
//    OBJECTS    //
///////////////////

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

static PyTypeObject EncoderObj = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "Encoder",
    .tp_basicsize = sizeof(encoder_t),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = EncoderMethods,
};

static PyTypeObject DecoderObj = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "Decoder",
    .tp_basicsize = sizeof(decoder_t),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = DecoderMethods,
};

static PyTypeObject StreamEncoderObj = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "StreamEncoder",
    .tp_basicsize = sizeof(streamenc_t),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = StreamEncoderMethods,
    .tp_dealloc = (destructor)streamenc_dealloc,
};

static PyTypeObject StreamDecoderObj = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "StreamDecoder",
    .tp_basicsize = sizeof(streamdec_t),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = StreamDecoderMethods,
    .tp_dealloc = (destructor)streamdec_dealloc,
};

static struct PyModuleDef cmsgpack = {
    PyModuleDef_HEAD_INIT,
    .m_name = "cmsgpack",
    .m_doc = "Fast MessagePack serializer",
    .m_methods = CmsgpackMethods,
    .m_size = sizeof(mstates_t),
    .m_free = (freefunc)cleanup_module,
};


//////////////////////
//  MODULE METHODS  //
//////////////////////

static void cleanup_module(PyObject *m)
{
    cleanup_mstates(get_mstates(m));
}

#define PYTYPE_READY(type) \
    if (PyType_Ready(&type)) return NULL;

PyMODINIT_FUNC PyInit_cmsgpack(void) {
    PyObject *m = PyState_FindModule(&cmsgpack);
    if (m != NULL)
    {
        Py_INCREF(m);
        return m;
    }
    
    PYTYPE_READY(EncoderObj);
    PYTYPE_READY(DecoderObj);

    PYTYPE_READY(StreamEncoderObj);
    PYTYPE_READY(StreamDecoderObj);

    PYTYPE_READY(ExtTypesEncodeObj);
    PYTYPE_READY(ExtTypesDecodeObj);

    m = PyModule_Create(&cmsgpack);
    NULLCHECK(m);
    
    if (!setup_mstates((mstates_t *)PyModule_GetState(m)))
    {
        Py_DECREF(m);
        return NULL;
    }

    return m;
}

