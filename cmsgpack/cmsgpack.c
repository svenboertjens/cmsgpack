/* Licensed under the MIT License. */

#define PY_SSIZE_T_CLEAN

#include "masks.h"
#include "internals.h"
#include "hashtable.h"

#include <Python.h>
#include <stdbool.h>

#ifdef _need_threadsafe
#include <stdatomic.h>
#endif


// Python versions
#define PYVER12 (PY_VERSION_HEX >= 0x030C0000)
#define PYVER13 (PY_VERSION_HEX >= 0x030D0000)


///////////////////
//   CONSTANTS   //
///////////////////

// Number of slots in ext type hash tables
#define EXT_TABLE_SLOTS 256

// Default (regular) buffer size
#define BUFFER_DEFAULTSIZE 256

// The number of slots to use in caches
#define STRING_CACHE_SLOTS 1024
#define INTEGER_CACHE_SLOTS (1024 + 128) // 1024 unsigned, 128 signed

// Minimum sizes for the 'extra' and 'item' adaptive allocation weights
#define EXTRA_ALLOC_MIN 64
#define ITEM_ALLOC_MIN 6

// Recursion limit
#define RECURSION_LIMIT 1000


///////////////////////////
//  TYPEDEFS & FORWARDS  //
///////////////////////////

// Usertypes encode object struct
typedef struct {
    PyObject_HEAD
    table_t *table;
} ext_types_encode_t;

// Usertypes decode object struct
typedef struct {
    PyObject_HEAD
    PyTypeObject *argtype;
    PyObject *reads[256]; // Array of functions per ID, indexed based on the ID (256 IDs, range from -128 to 127. Normalize by casting the ID to unsigned)
} ext_types_decode_t;

static PyTypeObject ExtTypesEncodeObj;
static PyTypeObject ExtTypesDecodeObj;


// String cache struct with a lock per object
typedef struct {
    PyASCIIObject *strings[STRING_CACHE_SLOTS];
    uint8_t match_strength[STRING_CACHE_SLOTS];

    #ifdef _need_threadsafe
    atomic_flag locks[STRING_CACHE_SLOTS];
    #endif
} strcache_t;

// Module states
typedef struct {
    // Interned strings
    struct {
        PyObject *obj;
        PyObject *ext_types;
        PyObject *file_name;
    } interned;

    // Caches
    struct {
        PyLongObject integers[INTEGER_CACHE_SLOTS];
        strcache_t strings;
    } caches;
} mstates_t;


// File data
typedef struct {
    FILE *file;  // File object being used
    char name[]; // Name of the file
} filedata_t;

// Configurations for buffer objects
typedef struct {
    mstates_t *states; // The module state
    filedata_t *fdata; // File data pointer, NULL if not used
    void *ext;         // Extension types, NULL if not used
} bufconfig_t;

// Struct holding buffer data for encoding
typedef struct {
    char *base;         // Base of the buffer object (base of PyBytesObject in encoding)
    char *offset;       // Offset to write to
    char *maxoffset;    // Maximum offset value before reallocation is needed

    size_t recursion;   // Recursion depth against circular references

    bufconfig_t config; // Configurations
} buffer_t;

// For encoder/decoder objects
typedef struct {
    PyObject_HEAD
    bufconfig_t config;
} hookobj_t;

static PyTypeObject EncoderObj;
static PyTypeObject DecoderObj;


static _always_inline mstates_t *get_mstates(PyObject *m);

static bool encode_object(buffer_t *b, PyObject *obj);
static PyObject *decode_bytes(buffer_t *b);

static bool encbuffer_expand(buffer_t *b, size_t needed);
static bool decoder_streaming_refresh(buffer_t *b, const size_t requested);


static struct PyModuleDef cmsgpack;


/////////////////////
//  COMMON ERRORS  //
/////////////////////

// Error for when LIMIT_LARGE is exceeded
#define error_size_limit(name, size) (PyErr_Format(PyExc_ValueError, #name " values can only hold up to 4294967295 bytes (2^32-1, 4 bytes), got a size of %zu", size))

// Error for when we get an unexpected tyoe
#define error_unexpected_type(expected, received) (PyErr_Format(PyExc_TypeError, "Expected an object of type '%s', but got an object of type '%s'", expected, received))

// Error for when we get an unexpected type at an argument
#define error_unexpected_argtype(argname, expected, received) (PyErr_Format(PyExc_TypeError, "Expected argument '%s' to be of type '%s', but got an object of type '%s'", argname, expected, received))

// Error for when we get an unexpected header
#define error_unexpected_header(expected_tpname, received_header) (PyErr_Format(PyExc_TypeError, "Expected a header of type '%s', but got header byte '0x%02X'", expected_tpname, received_header))

// Error for when a file couldn't be opened
#define error_cannot_open_file(filename, err) (PyErr_Format(PyExc_OSError, "Unable to open file '%s', received errno %i: '%s'", filename, err, strerror(err)))


///////////////////
//  BYTES TO PY  //
///////////////////

#ifdef _need_threadsafe

// Acquire a lock
static _always_inline void cache_acquire_lock(strcache_t *cache, size_t idx)
{
    atomic_flag *lock = &cache->locks[idx];
    while (atomic_flag_test_and_set_explicit(lock, memory_order_acquire)) { }
}

// Release a lock
static inline void cache_release_lock(strcache_t *cache, size_t idx)
{
    atomic_flag *lock = &cache->locks[idx];
    atomic_flag_clear_explicit(lock, memory_order_release);
}

#else

// Dummies
#define cache_acquire_lock(cache, idx) (NULL)
#define cache_release_lock(cache, idx) (NULL)

#endif

static _always_inline uint32_t fnv1a_32(const char *data, size_t size)
{
    uint32_t hash = 0x811c9dc5;

    for (size_t i = 0; i < size; i++)
    {
        hash ^= data[i];
        hash *= 0x01000193;
    }

    return hash;
}

// Version for small strings to be cached
static _always_inline PyObject *fixstr_to_py(char *ptr, size_t size, strcache_t *cache)
{
    // Calculate the hash of the string
    const size_t hash = fnv1a_32(ptr, size) & (STRING_CACHE_SLOTS - 1);

    // Wait on the lock of the hash
    cache_acquire_lock(cache, hash);

    // Load the match stored at the hash index
    PyASCIIObject *match = cache->strings[hash];

    if (match)
    {
        const char *cbase = (const char *)(match + 1);
        const size_t csize = match->length;

        if (csize == size && memcmp_small(cbase, ptr, size))
        {
            Py_INCREF(match);

            cache->match_strength[hash]++;

            // Release the lock
            cache_release_lock(cache, hash);

            return (PyObject *)match;
        }
    }

    // No common value, create a new one
    PyObject *obj = PyUnicode_DecodeUTF8(ptr, size, NULL);

    // If ASCII, it can be cached
    if (PyUnicode_IS_COMPACT_ASCII(obj))
    {
        cache->match_strength[hash]--;

        // Replace the match if the strength reached zero
        if (cache->match_strength[hash] == 0)
        {
            Py_XDECREF(match);
            Py_INCREF(obj);

            cache->strings[hash] = (PyASCIIObject *)obj;
            cache->match_strength[hash] = 3;
        }
    }

    // Release the lock
    cache_release_lock(cache, hash);

    return obj;
}


////////////////////
//  ARGS PARSING  //
////////////////////

#if PYVER13 // Python 3.13 and up
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
    if (nargs < min) \
    { \
        PyErr_Format(PyExc_TypeError, "Expected at least %zi positional arguments, but received only %zi", min, nargs); \
        return NULL; \
    } \
} while (0)

#define fixed_positional(nargs, required) do { \
    if (nargs != required) \
    { \
        PyErr_Format(PyExc_TypeError, "Expected exactly %zi positional arguments, but received %zi", required, nargs); \
        return NULL; \
    } \
} while (0)

static _always_inline PyObject *parse_positional(PyObject **args, size_t nth, PyTypeObject *type, const char *argname)
{
    PyObject *obj = args[nth];

    if (type && !Py_IS_TYPE(obj, type))
    {
        error_unexpected_argtype(argname, type->tp_name, Py_TYPE(obj)->tp_name);
        return NULL;
    }

    return obj;
}

#define NULLCHECK(obj) \
    if (!obj) return NULL;

static _always_inline bool _parse_kwarg(PyObject *kwname, PyObject *val, Py_ssize_t nkey, keyarg_t *keyargs)
{
    for (Py_ssize_t i = 0; i < nkey; ++i)
    {
        keyarg_t dest = keyargs[i];

        if (dest.interned == kwname)
        {
            if (dest.tp != NULL && !Py_IS_TYPE(val, dest.tp))
            {
                error_unexpected_argtype((const char *)((PyASCIIObject *)dest.interned + 1), dest.tp->tp_name, Py_TYPE(val)->tp_name);
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
            if (dest.tp != NULL && !Py_IS_TYPE(*dest.dest, dest.tp))
            {
                error_unexpected_argtype((const char *)((PyASCIIObject *)dest.interned + 1), dest.tp->tp_name, Py_TYPE(val)->tp_name);
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

    if (nargs > nkey)
    {
        PyErr_Format(PyExc_TypeError, "Expected at max %zi positional arguments, but received %zi", nkey, nargs);
        return false;
    }

    // Go over any remaining positional arguments for arguments not strictly positional
    for (Py_ssize_t i = 0; i < nargs; ++i)
    {
        keyarg_t keyarg = keyargs[i];
        PyObject *obj = args[i];

        if (keyarg.tp && !Py_IS_TYPE(obj, keyarg.tp))
        {
            // Use the interned string's value for the argument name
            error_unexpected_argtype(PyUnicode_AsUTF8(obj), keyarg.tp->tp_name, Py_TYPE(obj)->tp_name);
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
            if (!_parse_kwarg(PyTuple_GET_ITEM(kwargs, i), args[nargs + i], nkey, keyargs))
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
    if (!Py_IS_TYPE(pairsdict, &PyDict_Type))
    {
        error_unexpected_argtype("pairs", PyDict_Type.tp_name, Py_TYPE(pairsdict)->tp_name);
        return NULL;
    }

    ext_types_encode_t *obj = PyObject_New(ext_types_encode_t, &ExtTypesEncodeObj);
    if (obj == NULL)
        return PyErr_NoMemory();
    
    const size_t npairs = PyDict_GET_SIZE(pairsdict);

    // Pairs buffer for the hash table
    pair_t *pairs = (pair_t *)PyObject_Malloc(npairs * sizeof(pair_t));
    if (pairs == NULL)
    {
        PyObject_Del(obj);

        PyErr_NoMemory();
        return NULL;
    }
    
    // Collect all keys and vals in their respective arrays and type check them
    Py_ssize_t pos = 0;
    for (size_t i = 0; i < npairs; ++i)
    {
        PyObject *key;
        PyObject *val;

        PyDict_Next(pairsdict, &pos, &key, &val);

        if (!PyLong_CheckExact(key) || !PyTuple_CheckExact(val))
        {
            PyObject_Del(obj);
            PyObject_Free(pairs);

            PyErr_Format(PyExc_TypeError, "Expected keys of type 'int' and values of type 'tuple', got a key of type '%s' with a value of type '%s'", Py_TYPE(key)->tp_name, Py_TYPE(val)->tp_name);
            return NULL;
        }

        if (PyTuple_GET_SIZE(val) != 2)
        {
            PyObject_Del(obj);
            PyObject_Free(pairs);

            PyErr_Format(PyExc_TypeError, "Expected tuples with 2 items each, got a tuple with %zi items", PyTuple_GET_SIZE(val));
            return NULL;
        }

        PyObject *type = PyTuple_GET_ITEM(val, 0);
        PyObject *callable = PyTuple_GET_ITEM(val, 1);

        if (!PyType_CheckExact(type) || !PyCallable_Check(callable))
        {
            PyObject_Del(obj);
            PyObject_Free(pairs);

            PyErr_Format(PyExc_TypeError, "Expected tuple items of type 'type' and 'callable' respectively, got '%s' and '%s'", Py_TYPE(type)->tp_name, Py_TYPE(callable)->tp_name);
            return NULL;
        }

        long id = PyLong_AS_LONG(key);

        if ((int8_t)id != id)
        {
            PyObject_Del(obj);
            PyObject_Free(pairs);
            
            PyErr_Format(PyExc_ValueError, "Expected an ID between -128 and 127, got %li", id);
            return NULL;
        }

        Py_INCREF(type);
        Py_INCREF(callable);

        pairs[i].key = type;
        pairs[i].val = callable;
        pairs[i].extra = (void *)id;
    }

    obj->table = hashtable_create(pairs, npairs, false);

    PyObject_Free(pairs);

    if (obj->table == NULL)
    {
        PyObject_Del(obj);

        PyErr_NoMemory();
        return NULL;
    }

    return (PyObject *)obj;
}

static PyObject *ExtTypesDecode(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    if (nargs < 0)
    {
        PyErr_SetString(PyExc_TypeError, "Expected at least the positional 'pairs' argument");
        return NULL;
    }
    if (nargs > 2)
    {
        PyErr_Format(PyExc_TypeError, "Expected 2 positional arguments at max, got %zi", nargs);
        return NULL;
    }

    PyObject *pairsdict = args[0];
    PyTypeObject *argtype = &PyBytes_Type;

    if (nargs == 2)
    {
        argtype = (PyTypeObject *)args[1];

        if (argtype != &PyBytes_Type && argtype != &PyMemoryView_Type)
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
        return PyErr_NoMemory();

    // Zero-initialize the reads field to set unused slots to NULL
    memset(obj->reads, 0, sizeof(obj->reads));
    
    obj->argtype = argtype;
    
    PyObject *key, *val;
    Py_ssize_t pos = 0;
    while (PyDict_Next(pairsdict, &pos, &key, &val))
    {
        if (!PyLong_CheckExact(key) || !PyCallable_Check(val))
        {
            Py_DECREF(obj);
            PyErr_Format(PyExc_TypeError, "Expected keys of type 'int' and values of type 'callable', got a key of type '%s' with a value of type '%s'", Py_TYPE(key)->tp_name, Py_TYPE(val)->tp_name);
            return NULL;
        }

        const long id = PyLong_AS_LONG(key);

        if (id < -128 || id > 127)
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
    size_t npairs;
    pair_t *pairs = hashtable_get_pairs(obj->table, &npairs);

    for (size_t i = 0; i < npairs; ++i)
    {
        Py_DECREF(pairs[i].key);
        Py_DECREF(pairs[i].val);
    }

    hashtable_destroy(obj->table);

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

static PyObject *ext_decode_pull(ext_types_decode_t *obj, const int8_t id)
{
    const uint8_t idx = (uint8_t)id;

    // Return the reads object located on the index. Will be NULL if it doesn't exist
    return obj->reads[idx];
}

// Attempt to convert any object to bytes using an ext object. Returns a tuple to be decremented after copying data from PTR, or NULL on failure
static PyObject *any_to_ext(buffer_t *b, PyObject *obj, int8_t *id, char **ptr, size_t *size)
{
    // Return NULL if we don't have ext types
    if (b->config.ext == NULL)
        return NULL;
    
    // Attempt to find the encoding function assigned to this type
    pair_t *pair = hashtable_pull(((ext_types_encode_t *)b->config.ext)->table, Py_TYPE(obj));
    if (pair == NULL)
    {
        // Attempt to match the parent type
        pair = hashtable_pull(((ext_types_encode_t *)b->config.ext)->table, Py_TYPE(obj)->tp_base);

        // No match against parent type either
        if (pair == NULL)
            return PyErr_Format(PyExc_ValueError, "Received unsupported type: '%s'", Py_TYPE(obj)->tp_name);
    }
    
    // Call the function and get what it returns
    PyObject *result = PyObject_CallOneArg((PyObject *)pair->val, obj);
    NULLCHECK(result);
    
    if (!PyBytes_CheckExact(result))
    {
        Py_DECREF(result);

        PyErr_Format(PyExc_TypeError, "Expected to receive a bytes object from an ext type encode function, got return argument of type '%s'", Py_TYPE(result)->tp_name);
        return NULL;
    }

    *id = (int8_t)((uintptr_t)pair->extra);
    *size = PyBytes_GET_SIZE(result);
    *ptr = PyBytes_AS_STRING(result);

    if (*size == 0)
    {
        Py_DECREF(result);
        PyErr_SetString(PyExc_ValueError, "Ext types do not support zero-length data");
        return NULL;
    }

    return result;
}

static PyObject *ext_to_any(buffer_t *b, const int8_t id, const size_t size)
{
    if (b->config.ext == NULL)
        return NULL;

    PyObject *func = ext_decode_pull(b->config.ext, id);

    if (func == NULL)
    {
        PyErr_Format(PyExc_ValueError, "Could not match an ext function for decoding on id %i", id);
        return NULL;
    }

    // Either create bytes or a memoryview object based on what the user wants
    PyObject *arg;
    if (((ext_types_decode_t *)b->config.ext)->argtype == &PyMemoryView_Type)
    {
        arg = PyMemoryView_FromMemory(b->offset, (Py_ssize_t)size, PyBUF_READ);
    }
    else
    {
        arg = PyBytes_FromStringAndSize(b->offset, (Py_ssize_t)size);
    }
    
    if (arg == NULL)
        return PyErr_NoMemory();

    b->offset += size;

    // Call the function, passing the memoryview to it, and return its result
    PyObject *result = PyObject_CallOneArg(func, arg);

    Py_DECREF(arg);
    return result;
}


//////////////////////////
//  BYTES TO CONTAINER  //
//////////////////////////

static _always_inline PyObject *arr_to_py(buffer_t *b, const size_t nitems)
{
    PyObject *list = PyList_New(nitems);

    if (list == NULL)
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

static _always_inline PyObject *map_to_py(buffer_t *b, const size_t npairs)
{
    PyObject *dict = _PyDict_NewPresized(npairs);

    if (dict == NULL)
        return PyErr_NoMemory();

    for (size_t i = 0; i < npairs; ++i)
    {
        PyObject *key;

        // Special case for fixsize strings
        const unsigned char mask = b->offset[0];
        if ((mask & 0b11100000) == DT_STR_FIXED)
        {
            b->offset++;

            const size_t size = mask & 0b11111;

            key = fixstr_to_py(b->offset, size, &b->config.states->caches.strings);
            b->offset += size;
        }
        else
        {
            key = decode_bytes(b);

            if (key == NULL)
            {
                Py_DECREF(dict);
                return NULL;
            }
        }

        PyObject *val = decode_bytes(b);

        if (val == NULL)
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
    if (b->offset + (extra) >= b->maxoffset) \
        NULLCHECK(encbuffer_expand(b, (extra))); \
} while (0)

// Get the current offset's byte and increment afterwards
#define INCBYTE ((b->offset++)[0])

// INCBYTE but masked to ensure we operate with a single byte
#define SIZEBYTE ((unsigned char)(b->offset++)[0])

// Typemask for fixsize and varlen switch case
#define FIXSIZE_DT(mask) (mask & 0b11100000)
#define VARLEN_DT(mask) (mask & 0b11111)

// Prefix error message for invalid encoded data
#define INVALID_MSG "Received invalid encoded data"

// Check if the buffer won't be overread
#define OVERREAD_CHECK(to_add) do { \
    if (b->offset + to_add > b->maxoffset) \
    { \
        if (b->config.fdata != NULL) \
        { \
            NULLCHECK(decoder_streaming_refresh(b, to_add)); \
        } \
        else \
        { \
            PyErr_SetString(PyExc_ValueError, INVALID_MSG " (overread the encoded data)"); \
            return NULL; \
        } \
    } \
} while (0)

// Write a header's typemask and its size based on the number of bytes the size should take up
static _always_inline void write_mask(buffer_t *b, const unsigned char mask, const size_t size, const size_t nbytes)
{
    if (nbytes == 0) // FIXSIZE
    {
        INCBYTE = mask | size;
    }
    else if (nbytes == 1) // SMALL
    {
        char _buf[2] = {
            mask,
            size
        };

        memcpy(b->offset, _buf, 2);
        b->offset += 2;
    }
    else if (nbytes == 2) // MEDIUM
    {
        char _buf[3] = {
            mask,
            size >> 8,
            size
        };

        memcpy(b->offset, _buf, 3);
        b->offset += 3;
    }
    else if (nbytes == 4) // LARGE
    {
        char _buf[5] = {
            mask,
            size >> 24,
            size >> 16,
            size >> 8,
            size,
        };

        memcpy(b->offset, _buf, 5);
        b->offset += 5;
    }
    else if (nbytes == 8) // For specific cases
    {
        char _buf[9] = {
            mask,
            size >> 56,
            size >> 48,
            size >> 40,
            size >> 32,
            size >> 24,
            size >> 16,
            size >> 8,
            size
        };

        memcpy(b->offset, _buf, 9);
        b->offset += 9;
    }
}

// Separate from `write_str` for re-use in other places, for getting string data
static _always_inline void py_str_data(PyObject *obj, char **base, size_t *size)
{
    // Check if the object is compact ASCII, use quick path for that
    if (PyUnicode_IS_COMPACT_ASCII(obj))
    {
        *size = ((PyASCIIObject *)obj)->length;
        *base = (char *)(((PyASCIIObject *)obj) + 1);
    }
    else
    {
        // Otherwise simply get data through this
        *base = (char *)PyUnicode_AsUTF8AndSize(obj, (Py_ssize_t *)size);

        if (base == NULL)
            PyErr_SetString(PyExc_BufferError, "Unable to get the internal buffer of a string");
    }
}

// Write string without the data fetching part
static _always_inline bool _write_string(buffer_t *b, char *base, size_t size)
{
    ENSURE_SPACE(size + 5);

    if (size <= LIMIT_STR_FIXED)
    {
        write_mask(b, DT_STR_FIXED, size, 0);
    }
    else if (size <= LIMIT_SMALL)
    {
        write_mask(b, DT_STR_SMALL, size, 1);
    }
    else if (size <= LIMIT_MEDIUM)
    {
        write_mask(b, DT_STR_MEDIUM, size, 2);
    }
    else if (size <= LIMIT_LARGE)
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

static _always_inline bool write_string(buffer_t *b, PyObject *obj)
{
    // Fast path for common cases
    if (PyUnicode_IS_COMPACT_ASCII(obj) && ((PyASCIIObject *)obj)->length <= (Py_ssize_t)LIMIT_STR_FIXED)
    {
        char *base = (char *)(((PyASCIIObject *)obj) + 1);
        size_t size = ((PyASCIIObject *)obj)->length;

        ENSURE_SPACE(1 + LIMIT_STR_FIXED);

        write_mask(b, DT_STR_FIXED, size, 0);

        memcpy(b->offset, base, size);
        b->offset += size;

        return true;
    }

    char *base;
    size_t size;
    py_str_data(obj, &base, &size);

    if (base == NULL)
        return false;

    return _write_string(b, base, size);
}

// Separate for writing binary
static _always_inline bool write_binary(buffer_t *b, char *base, size_t size)
{
    ENSURE_SPACE(5 + size);

    if (size <= LIMIT_SMALL)
    {
        write_mask(b, DT_BIN_SMALL, size, 1);
    }
    else if (size <= LIMIT_MEDIUM)
    {
        write_mask(b, DT_BIN_MEDIUM, size, 2);
    }
    else if (size <= LIMIT_LARGE)
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

static _always_inline bool write_bytes(buffer_t *b, PyObject *obj)
{
    char *base = PyBytes_AS_STRING(obj);
    size_t size = PyBytes_GET_SIZE(obj);

    return write_binary(b, base, size);
}

static _always_inline bool write_bytearray(buffer_t *b, PyObject *obj)
{
    char *base = PyByteArray_AS_STRING(obj);
    size_t size = PyByteArray_GET_SIZE(obj);

    return write_binary(b, base, size);
}

static _always_inline bool write_memoryview(buffer_t *b, PyObject *obj)
{
    PyMemoryViewObject *ob = (PyMemoryViewObject *)obj;

    char *base = ob->view.buf;
    size_t size = ob->view.len;

    return write_binary(b, base, size);
}

static _always_inline bool write_double(buffer_t *b, PyObject *obj)
{
    double num = PyFloat_AS_DOUBLE(obj);

    ENSURE_SPACE(9);

    // Ensure big-endianness
    BIG_DOUBLE(num);

    INCBYTE = DT_FLOAT_BIT64;
    memcpy(b->offset, &num, 8);

    b->offset += 8;

    return true;
}

static _always_inline bool write_integer(buffer_t *b, PyObject *obj)
{
    // Ensure 9 bytes of space, max size of integer + header
    ENSURE_SPACE(9);

    // Cast the object to a long value to access internals
    PyLongObject *lobj = (PyLongObject *)obj;

    #if PYVER12

    uint64_t num = lobj->long_value.ob_digit[0];

    const uintptr_t tag = lobj->long_value.lv_tag & 0b11010; // Bit 4/5 store size, bit 2 stores signedness (0 is positive)
    if ((tag == 0b01000 || tag == 0b00000)) // Positive, 1 digit, or zero
    {
        if (num <= LIMIT_UINT_FIXED)
        {
            write_mask(b, DT_UINT_FIXED, num, 0);
        }
        else if (num <= LIMIT_UINT_BIT8)
        {
            write_mask(b, DT_UINT_BIT8, num, 1);
        }
        else if (num <= LIMIT_UINT_BIT16)
        {
            write_mask(b, DT_UINT_BIT16, num, 2);
        }
        else
        {
            write_mask(b, DT_UINT_BIT32, num, 4);
        }
    }
    else if (tag == 0b01010) // Negative, 1 digit
    {
        int64_t snum = -((int64_t)num);

        if (snum >= LIMIT_INT_FIXED)
        {
            write_mask(b, DT_INT_FIXED, snum, 0);
        }
        else if (snum >= LIMIT_INT_BIT8)
        {
            write_mask(b, DT_INT_BIT8, snum, 1);
        }
        else if (snum >= LIMIT_INT_BIT16)
        {
            write_mask(b, DT_INT_BIT16, snum, 2);
        }
        else
        {
            write_mask(b, DT_INT_BIT32, snum, 4);
        }
    }
    else // More than 1 digit
    {
        bool positive = (tag & _PyLong_SIGN_MASK) == 0;

        num |= (uint64_t)(lobj->long_value.ob_digit[1]) << PyLong_SHIFT;

        if ((tag & 0b11000) == 0b11000) // 3 digits
        {
            uint64_t shift = (PyLong_SHIFT * 2);
            uint64_t digit3 = lobj->long_value.ob_digit[2];

            uint64_t shifted = digit3 << shift;

            // Shift one less if negative to account for signed going up to 63 bits instead of 64
            if (!positive)
            {
                shift--;
                digit3 <<= 1;
            }
            
            if (shifted >> shift != digit3)
            {
                PyErr_SetString(PyExc_OverflowError, "Integer values cannot exceed `-2^63` or `2^64-1`");
                return false;
            }

            num |= shifted;
        }

        if (positive)
        {
            if (num <= LIMIT_UINT_BIT32)
            {
                write_mask(b, DT_UINT_BIT32, num, 4);
            }
            else
            {
                write_mask(b, DT_UINT_BIT64, num, 8);
            }
        }
        else
        {
            if ((int64_t)(-num) >= LIMIT_INT_BIT32)
            {
                write_mask(b, DT_INT_BIT32, -num, 4);
            }
            else
            {
                write_mask(b, DT_INT_BIT64, -num, 8);
            }
        }
    }

    #else

    // Number of digits (negative value if the long is negative)
    long _ndigits = (long)Py_SIZE(lobj);

    // Convert the digits to its absolute value
    long ndigits = Py_ABS(ndigits);

    // Set the negative flag based on if the number of digits changed (went from negative to positive)
    bool neg = ndigits != _ndigits;

    // Set the pointer to the digits array
    digit *digits = lobj->ob_digit;

    // Set the first digit of the value
    uint64_t num = (uint64_t)digits[0];

    if (!_PyLong_IsCompact(lobj))
    {
        uint64_t last;
        while (--ndigits >= 1)
        {
            last = num;

            num <<= PyLong_SHIFT;
            num |= digits[ndigits];

            if ((num >> PyLong_SHIFT) != last)
                return false;
        }

        // Check if the value exceeds the size limit when negative
        if (neg && num > (1ull << 63))
            return false;
    }

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
            write_mask(b, DT_UINT_BIT64, num, 8);
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
            write_mask(b, DT_INT_BIT64, _num, 8);
        }
    }

    #endif

    return true;
}

static _always_inline bool write_bool(buffer_t *b, PyObject *obj)
{
    ENSURE_SPACE(1);
    INCBYTE = obj == Py_True ? DT_TRUE : DT_FALSE;
    return true;
}

static _always_inline bool write_nil(buffer_t *b, PyObject *obj)
{
    ENSURE_SPACE(1);
    INCBYTE = DT_NIL;
    return true;
}

static _always_inline bool write_array_header(buffer_t *b, size_t nitems)
{
    ENSURE_SPACE(5);

    if (nitems <= LIMIT_ARR_FIXED)
    {
        write_mask(b, DT_ARR_FIXED, nitems, 0);
    }
    else if (nitems <= LIMIT_MEDIUM)
    {
        write_mask(b, DT_ARR_MEDIUM, nitems, 2);
    }
    else if (nitems <= LIMIT_LARGE)
    {
        write_mask(b, DT_ARR_LARGE, nitems, 4);
    }
    else
    {
        error_size_limit(Array, nitems);
        return false;
    }

    return true;
}

// Used for writing array types
static bool write_array(buffer_t *b, PyObject **items, size_t nitems)
{
    if (++(b->recursion) > RECURSION_LIMIT)
    {
        PyErr_SetString(PyExc_RecursionError, "Exceeded the maximum recursion depth");
        return false;
    }

    NULLCHECK(write_array_header(b, nitems));

    for (size_t i = 0; i < nitems; ++i)
    {
        if (!encode_object(b, items[i]))
            return false;
    }

    b->recursion--;

    return true;
}

static _always_inline bool write_list(buffer_t *b, PyObject *obj)
{
    PyObject **items = ((PyListObject *)obj)->ob_item;
    size_t nitems = PyList_GET_SIZE(obj);

    return write_array(b, items, nitems);
}

static _always_inline bool write_tuple(buffer_t *b, PyObject *obj)
{
    PyObject **items = ((PyTupleObject *)obj)->ob_item;
    size_t nitems = PyTuple_GET_SIZE(obj);

    return write_array(b, items, nitems);
}

static _always_inline bool write_map_header(buffer_t *b, size_t npairs)
{
    ENSURE_SPACE(5);
    
    if (npairs <= LIMIT_MAP_FIXED)
    {
        write_mask(b, DT_MAP_FIXED, npairs, 0);
    }
    else if (npairs <= LIMIT_MEDIUM)
    {
        write_mask(b, DT_MAP_MEDIUM, npairs, 2);
    }
    else if (npairs <= LIMIT_LARGE)
    {
        write_mask(b, DT_MAP_LARGE, npairs, 4);
    }
    else
    {
        error_size_limit(Map, npairs);
        return false;
    }

    return true;
}

static _always_inline bool write_dict(buffer_t *b, PyObject *obj)
{
    if (++(b->recursion) > RECURSION_LIMIT)
    {
        PyErr_SetString(PyExc_RecursionError, "Exceeded the maximum recursion depth");
        return false;
    }

    const size_t npairs = PyDict_GET_SIZE(obj);

    NULLCHECK(write_map_header(b, npairs));

    Py_ssize_t pos = 0;
    for (size_t i = 0; i < npairs; ++i)
    {
        PyObject *key;
        PyObject *val;
        PyDict_Next(obj, &pos, &key, &val);

        if ((PyUnicode_CheckExact(key)))
        {
            NULLCHECK(write_string(b, key));
        }
        else
        {
            NULLCHECK(encode_object(b, key));
        }

        NULLCHECK(encode_object(b, val));
    }

    b->recursion--;

    return true;
}

static _always_inline bool write_extension(buffer_t *b, PyObject *obj)
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
    else if (size <= LIMIT_LARGE)
    {
        write_mask(b, DT_EXT_LARGE, size, 4);
    }
    else
    {
        Py_DECREF(result);

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

static bool encode_object(buffer_t *b, PyObject *obj)
{
    PyTypeObject *tp = Py_TYPE(obj);

    // All standard types
    if (tp == &PyUnicode_Type)
    {
        return write_string(b, obj);
    }
    else if (tp == &PyLong_Type)
    {
        return write_integer(b, obj);
    }
    else if (tp == &PyFloat_Type)
    {
        return write_double(b, obj);
    }
    else if (PyDict_Check(obj)) // Same as with lists and tuples
    {
        return write_dict(b, obj);
    }
    else if (tp == &PyBool_Type)
    {
        return write_bool(b, obj);
    }
    else if (PyList_Check(obj)) // No exact check to support subclasses
    {
        return write_list(b, obj);
    }
    else if (PyTuple_Check(obj)) // Same as with lists, no exact check to support subclasses
    {
        return write_tuple(b, obj);
    }
    else if (tp == Py_TYPE(Py_None)) // No explicit PyNone_Type available
    {
        return write_nil(b, obj);
    }
    else if (tp == &PyBytes_Type)
    {
        return write_bytes(b, obj);
    }
    else if (tp == &PyByteArray_Type)
    {
        return write_bytearray(b, obj);
    }
    else if (tp == &PyMemoryView_Type)
    {
        return write_memoryview(b, obj);
    }
    else
    {
        return write_extension(b, obj);
    }
}


////////////////////
//    DECODING    //
////////////////////

// Decoding of fixsize objects
static _always_inline PyObject *decode_bytes_fixsize(buffer_t *b, unsigned char mask)
{
    // Check which fixmask we got
    const unsigned char fixmask = FIXSIZE_DT(mask);
    if (fixmask == DT_STR_FIXED)
    {
        mask &= 0x1F;
        OVERREAD_CHECK(mask);

        PyObject *obj = fixstr_to_py(b->offset, mask, &b->config.states->caches.strings);
        b->offset += mask;

        return obj;
    }
    else if ((mask & 0x80) == DT_UINT_FIXED) // uint only has upper bit set to 0
    {
        return (PyObject *)&b->config.states->caches.integers[mask + 128];
    }
    else if (fixmask == DT_INT_FIXED)
    {
        int8_t num = mask & 0b11111;

        if ((num & 0b10000) == 0b10000)
            num |= ~0b11111;

        return (PyObject *)&b->config.states->caches.integers[num + 128];
    }
    else if ((mask & 0b11110000) == DT_ARR_FIXED) // Bit 5 is also set on ARR and MAP
    {
        return arr_to_py(b, mask & 0x0F);
    }
    else if ((mask & 0b11110000) == DT_MAP_FIXED)
    {
        return map_to_py(b, mask & 0x0F);
    }
    else
    {
        PyErr_SetString(PyExc_ValueError, INVALID_MSG " (invalid header)");
        return NULL;
    }
}

// Decoding of varlen objects
static _always_inline PyObject *decode_bytes_varlen(buffer_t *b, const unsigned char mask)
{
    // Global variable to hold the size across multiple cases
    uint64_t n = 0;

    // Switch over the varlen mask
    const unsigned int varlen_mask = VARLEN_DT(mask);
    switch (varlen_mask)
    {
    case VARLEN_DT(DT_STR_LARGE):
    {
        OVERREAD_CHECK(4);
        n |= SIZEBYTE << 24;
        n |= SIZEBYTE << 16;
    }
    case VARLEN_DT(DT_STR_MEDIUM):
    {
        OVERREAD_CHECK(2);
        n |= SIZEBYTE << 8;
    }
    case VARLEN_DT(DT_STR_SMALL):
    {
        OVERREAD_CHECK(1);
        n |= SIZEBYTE;

        OVERREAD_CHECK(n);

        PyObject *obj = PyUnicode_DecodeUTF8(b->offset, n, NULL);
        b->offset += n;

        return obj;
    }

    case VARLEN_DT(DT_UINT_BIT8):
    {
        OVERREAD_CHECK(1);

        uint8_t num = SIZEBYTE;

        return (PyObject *)&b->config.states->caches.integers[num + 128];
    }
    case VARLEN_DT(DT_UINT_BIT16):
    {
        OVERREAD_CHECK(2);

        uint16_t num = SIZEBYTE << 8;
        num |= SIZEBYTE;

        if (num <= 1023)
            return (PyObject *)&b->config.states->caches.integers[num + 128];
        
        return PyLong_FromUnsignedLongLong(num);
    }
    case VARLEN_DT(DT_UINT_BIT32):
    {
        OVERREAD_CHECK(4);

        uint32_t num;
        memcpy(&num, b->offset, 4);
        b->offset += 4;

        num = BIG_32(num);
        
        return PyLong_FromUnsignedLongLong(num);
    }
    case VARLEN_DT(DT_UINT_BIT64):
    {
        OVERREAD_CHECK(8);

        uint64_t num;
        memcpy(&num, b->offset, 8);
        b->offset += 8;

        num = BIG_64(num);
        
        return PyLong_FromUnsignedLongLong(num);
    }

    case VARLEN_DT(DT_INT_BIT8):
    {
        OVERREAD_CHECK(1);

        int8_t num = SIZEBYTE;

        return (PyObject *)&b->config.states->caches.integers[num + 128];
    }
    case VARLEN_DT(DT_INT_BIT16):
    {
        OVERREAD_CHECK(2);

        int16_t num = (int16_t)SIZEBYTE << 8;
        num |= SIZEBYTE;
        
        return PyLong_FromLongLong(num);
    }
    case VARLEN_DT(DT_INT_BIT32):
    {
        OVERREAD_CHECK(4);

        int32_t num;
        memcpy(&num, b->offset, 4);
        b->offset += 4;

        num = BIG_32(num);
        
        return PyLong_FromLongLong(num);
    }
    case VARLEN_DT(DT_INT_BIT64):
    {
        OVERREAD_CHECK(8);

        int64_t num;
        memcpy(&num, b->offset, 8);
        b->offset += 8;

        num = BIG_64(num);

        return PyLong_FromLongLong(num);
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

        return arr_to_py(b, n);
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

        return map_to_py(b, n);
    }

    case VARLEN_DT(DT_NIL):
    {
        return Py_None;
    }
    case VARLEN_DT(DT_TRUE):
    {
        return Py_True;
    }
    case VARLEN_DT(DT_FALSE):
    {
        return Py_False;
    }

    case VARLEN_DT(DT_FLOAT_BIT32):
    {
        float num_float;
        memcpy(&num_float, b->offset, 4);
        b->offset += 4;
        
        // Use type promotion to convert to double
        double num = (double)num_float;

        BIG_DOUBLE(num);

        return PyFloat_FromDouble(num);
    }
    case VARLEN_DT(DT_FLOAT_BIT64):
    {
        double num;
        memcpy(&num, b->offset, 8);
        b->offset += 8;

        BIG_DOUBLE(num);

        return PyFloat_FromDouble(num);
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

        OVERREAD_CHECK(n);

        PyObject *obj = PyBytes_FromStringAndSize(b->offset, n);
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

static PyObject *decode_bytes(buffer_t *b)
{
    // Check if it's safe to read the mask
    OVERREAD_CHECK(1);

    // The mask is stored on the next byte
    const unsigned char mask = SIZEBYTE;

    // Only fixsize values don't have `110` set on the upper 3 mask bits
    if ((mask & 0b11100000) != 0b11000000)
    {
        return decode_bytes_fixsize(b, mask);
    }
    else
    {
        // Otherwise, it's a varlen object
        return decode_bytes_varlen(b, mask);
    }
}


//////////////////////
//  ENC/DEC BUFFER  //
//////////////////////

// Use thread-local variables instead of atomic variables to keep
// the averages more local, and to prevent counterintuitive scaling
_Thread_local size_t extra_avg = (EXTRA_ALLOC_MIN * 2);
_Thread_local size_t item_avg = (ITEM_ALLOC_MIN * 2);

// Prepare the encoding buffer struct
static _always_inline bool encbuffer_prepare(buffer_t *b, PyObject *obj, size_t *nitems)
{
    // Initialize the new size with the extra average and set NITEMS to 0 in advance
    size_t allocsize = extra_avg;
    *nitems = 0;

    // Attempt to allocate appropriate memory for the buffer based on OBJ's type
    PyTypeObject *tp = Py_TYPE(obj);
    if (tp == &PyList_Type || tp == &PyDict_Type)
    {
        // Get the number of items in the container
        *nitems = Py_SIZE(obj);
        
        // Add allocation size based on the number of items
        allocsize += *nitems * item_avg;
    }

    // Create the new buffer object
    b->base = (char *)PyBytes_FromStringAndSize(NULL, allocsize);

    if (b->base == NULL)
    {
        // Attempt to create an object with the default buffer size
        b->base = (char *)PyBytes_FromStringAndSize(NULL, BUFFER_DEFAULTSIZE);

        if (b->base == NULL)
        {
            PyErr_NoMemory();
            return false;
        }
    }

    // Set up the offset and max offset based on the buffer
    b->offset = PyBytes_AS_STRING(b->base);
    b->maxoffset = PyBytes_AS_STRING(b->base) + allocsize;

    return true;
}

static _always_inline size_t biased_average(size_t curr, size_t new)
{
    const size_t curr_doubled = curr * 2;

    // Safety against growing too quickly by limiting growth size to a factor of 2
    if (curr_doubled < new)
        return curr_doubled;
    
    // Return a biased average leaning more towards the current value
    return (curr_doubled + new) / 3;
}

// Safely set a minimum value in a way that accounts for overflow cases
#define _SAFE_MIN(val, min) \
    (Py_ssize_t)val < min ? min : val

static _always_inline void update_adaptive_allocation(buffer_t *b, size_t nitems)
{
    const size_t needed = (size_t)(b->offset - PyBytes_AS_STRING(b->base));

    // Take the average between how much we needed and the currently set value
    extra_avg = biased_average(extra_avg, needed);

    // Enforce the minimum value of EXTRA_ALLOC_MIN
    extra_avg = _SAFE_MIN(extra_avg, EXTRA_ALLOC_MIN);
    
    // Return if NITEMS is 0 to avoid division by zero
    if (nitems == 0)
        return;

    // Size allocated per item and required per item
    const size_t needed_per_item = needed * pow((double)nitems, -1.0); // Multiply by reciprocal, faster than division

    // Take the average between the currently set per-item value and that of this round
    item_avg = biased_average(item_avg, needed_per_item);

    // Ensure a minimum value of ITEM_ALLOC_MIN
    item_avg = _SAFE_MIN(item_avg, ITEM_ALLOC_MIN);

    return;
}

// Start encoding with an encbuffer
static PyObject *encbuffer_start(bufconfig_t config, PyObject *obj)
{
    size_t nitems;
    buffer_t b;

    b.config = config;
    b.recursion = 0;

    // Prepare the encode buffer
    NULLCHECK(encbuffer_prepare(&b, obj, &nitems));

    // Encode the object
    NULLCHECK(encode_object(&b, obj));

    // Set the object's size
    Py_SET_SIZE(b.base, (size_t)(b.offset - PyBytes_AS_STRING(b.base)));

    // Update the adaptive allocation
    update_adaptive_allocation(&b, nitems);

    // Return the object
    return (PyObject *)b.base;
}

// Expand the encoding buffer
static bool encbuffer_expand(buffer_t *b, size_t extra)
{
    // Scale the size by factor 1.5x
    const size_t allocsize = ((size_t)(b->offset - PyBytes_AS_STRING(b->base)) + extra) * 1.5;

    // Reallocate the object (also allocate space for the bytes object itself)
    char *reallocd = PyObject_Realloc(b->base, allocsize + sizeof(PyBytesObject));

    if (reallocd == NULL)
    {
        PyErr_NoMemory();
        return false;
    }

    // Update the offsets
    b->offset = PyBytes_AS_STRING(reallocd) + (size_t)(b->offset - PyBytes_AS_STRING(b->base));
    b->maxoffset = PyBytes_AS_STRING(reallocd) + allocsize;
    b->base = reallocd;

    return true;
}


// Start decoding with a decbuffer (not used with streaming)
static _always_inline PyObject *decbuffer_start(bufconfig_t config, PyObject *encoded)
{
    buffer_t b;

    // Get a buffer of the encoded data buffer
    Py_buffer buffer;
    if (PyObject_GetBuffer(encoded, &buffer, PyBUF_SIMPLE) != 0)
    {
        PyErr_SetString(PyExc_BufferError, "Unable to open a buffer of the received encoded data.");
        return NULL;
    }
    
    b.base = buffer.buf;
    b.offset = b.base;
    b.maxoffset = b.base + buffer.len;

    b.config = config;

    PyObject *result = decode_bytes(&b);

    PyBuffer_Release(&buffer);

    // Check if we reached the end of the buffer
    if (result != NULL && b.offset != b.maxoffset)
    {
        PyErr_SetString(PyExc_ValueError, INVALID_MSG " (encoded message ended early)");
        return NULL;
    }

    return result;
}


/////////////////////
//  BASIC ENC/DEC  //
/////////////////////

static PyObject *encode(PyObject *self, PyObject **args, Py_ssize_t nargs, PyObject *kwargs)
{
    const Py_ssize_t npos = 1;
    min_positional(nargs, npos);

    PyObject *obj = parse_positional(args, 0, NULL, "obj");
    NULLCHECK(obj);

    bufconfig_t config;

    // Default config values
    config.states = get_mstates(self);
    config.fdata = NULL;
    config.ext = NULL;

    keyarg_t keyargs[] = {
        KEYARG(&config.ext, &ExtTypesEncodeObj, config.states->interned.ext_types),
    };

    if (!parse_keywords(npos, args, nargs, kwargs, keyargs, NKEYARGS(keyargs)))
        return NULL;

    return encbuffer_start(config, obj);
}

static PyObject *decode(PyObject *self, PyObject **args, Py_ssize_t nargs, PyObject *kwargs)
{
    const Py_ssize_t npos = 1;
    min_positional(nargs, npos);

    PyObject *encoded = parse_positional(args, 0, NULL, "encoded");
    NULLCHECK(encoded);

    bufconfig_t config;

    // Default config values
    config.states = get_mstates(self);
    config.fdata = NULL;
    config.ext = NULL;

    keyarg_t keyargs[] = {
        KEYARG(&config.ext, &ExtTypesDecodeObj, config.states->interned.ext_types),
    };

    if (!parse_keywords(npos, args, nargs, kwargs, keyargs, NKEYARGS(keyargs)))
        return NULL;

    return decbuffer_start(config, encoded);
}


///////////////////////
//  ENC/DEC OBJECTS  //
///////////////////////

static PyObject *Encoder(PyObject *self, PyObject **args, Py_ssize_t nargs, PyObject *kwargs)
{
    PyObject *filename = NULL;
    PyObject *ext = NULL;

    mstates_t *states = get_mstates(self);

    keyarg_t keyargs[] = {
        KEYARG(&filename, &PyUnicode_Type, states->interned.file_name),
        KEYARG(&ext, &ExtTypesEncodeObj, states->interned.ext_types),
    };

    NULLCHECK(parse_keywords(0, args, nargs, kwargs, keyargs, NKEYARGS(keyargs)))


    // Create the Encoder
    hookobj_t *enc = PyObject_New(hookobj_t, &EncoderObj);
    if (enc == NULL)
        return PyErr_NoMemory();
    
    // Set the buffer configs
    enc->config.ext = ext;
    enc->config.fdata = NULL;
    enc->config.states = get_mstates(self);

    // Check if we got a filename
    if (filename != NULL)
    {
        // Get the string data of the filename
        size_t strsize;
        char *str;
        py_str_data(filename, &str, &strsize);

        if (str == NULL)
        {
            PyObject_Del(enc);
            return NULL;
        }

        // Allocate the fdata object with space for the string (+1 for null terminator)
        enc->config.fdata = (filedata_t *)PyObject_Malloc(sizeof(filedata_t) + strsize + 1);

        if (enc->config.fdata == NULL)
        {
            PyObject_Del(enc);

            PyErr_NoMemory();
            return NULL;
        }

        // Copy the string name into the fdata object and null-terminate it
        memcpy(enc->config.fdata->name, str, strsize);
        enc->config.fdata->name[strsize] = 0;

        // Open the file in advance
        enc->config.fdata->file = fopen(enc->config.fdata->name, "ab");
        if (enc->config.fdata->file == NULL)
        {
            const int err = errno;
            error_cannot_open_file(enc->config.fdata->name, err);
            return NULL;
        }

        // Disable buffering because we already write in chunks
        setvbuf(enc->config.fdata->file, NULL, _IONBF, 0);
    }

    // Keep reference to the ext object
    Py_XINCREF(ext);

    return (PyObject *)enc;
}

static PyObject *Decoder(PyObject *self, PyObject **args, Py_ssize_t nargs, PyObject *kwargs)
{
    PyObject *filename = NULL;
    PyObject *ext = NULL;

    mstates_t *states = get_mstates(self);

    keyarg_t keyargs[] = {
        KEYARG(&filename, &PyUnicode_Type, states->interned.file_name),
        KEYARG(&ext, &ExtTypesDecodeObj, states->interned.ext_types),
    };

    NULLCHECK(parse_keywords(0, args, nargs, kwargs, keyargs, NKEYARGS(keyargs)))


    // Create the Decoder
    hookobj_t *dec = PyObject_New(hookobj_t, &DecoderObj);
    if (dec == NULL)
        return PyErr_NoMemory();
    
    // Set buffer configurations
    dec->config.ext = ext;
    dec->config.fdata = NULL;
    dec->config.states = get_mstates(self);

    // Check if we received a filename
    if (filename != NULL)
    {
        // Get the string data of the filename
        size_t strsize;
        char *str;
        py_str_data(filename, &str, &strsize);

        if (str == NULL)
        {
            PyObject_Del(dec);
            return NULL;
        }

        // Allocate a fdata object with space for the string
        dec->config.fdata = (filedata_t *)PyObject_Malloc(sizeof(filedata_t) + strsize + 1);

        if (dec->config.fdata == NULL)
        {
            PyObject_Del(dec);

            PyErr_NoMemory();
            return NULL;
        }
        
        // Copy the name into the name buffer and null-terminate
        memcpy(dec->config.fdata->name, str, strsize);
        dec->config.fdata->name[strsize] = 0;

        // Open the file in advance
        dec->config.fdata->file = fopen(dec->config.fdata->name, "rb");

        if (dec->config.fdata->file == NULL)
        {
            Py_DECREF(dec);

            const int err = errno;
            return error_cannot_open_file(dec->config.fdata->name, err);
        }

        // Disable buffering
        setvbuf(dec->config.fdata->file, NULL, _IONBF, 0);
    }

    Py_XINCREF(ext);

    return (PyObject *)dec;
}

static PyObject *encoder_encode(hookobj_t *enc, PyObject *obj)
{
    // Check if the file is closed when streaming
    if (enc->config.fdata != NULL && enc->config.fdata->file == NULL)
    {
        enc->config.fdata->file = fopen(enc->config.fdata->name, "ab");

        if (enc->config.fdata->file == NULL)
        {
            const int err = errno;
            return error_cannot_open_file(enc->config.fdata->name, err);
        }
    }

    PyBytesObject *result = (PyBytesObject *)encbuffer_start(enc->config, obj);
    NULLCHECK(result);

    // Simply return the result if we're not streaming
    if (enc->config.fdata == NULL)
        return (PyObject *)result;
    
    // Otherwise stream the data to a file

    const size_t size = PyBytes_GET_SIZE(result);

    // Check if all data is written to the file, otherwise truncate up until data that was written incompletely
    size_t written;
    if (size != (written = fwrite(PyBytes_AS_STRING(result), 1, size, enc->config.fdata->file)))
    {
        PyErr_Format(PyExc_OSError, "Failed to write encoded data to file '%s'. A partial read might have occurred, which is attempted to be fixed.", enc->config.fdata->name);

        // Attempt to truncate the file
        if (_ftruncate_and_close(enc->config.fdata->file, ftell(enc->config.fdata->file) - written, enc->config.fdata->name) != 0)
        {
            // Set file to NULL to prevent invalid pointer errors, and trigger re-opening on a new write
            enc->config.fdata->file = NULL;

            const int err = errno;
            PyErr_Print(); // Print previous error
            PyErr_Format(PyExc_OSError, "Truncating the file back to its old size failed, leaving partially written or corrupt data in file '%s'. Received errno %i: %s", enc->config.fdata->name, err, strerror(err));
        }

        return NULL;
    }

    Py_RETURN_NONE;
}

// Check if we read nothing, throw appropriate error if so
static _always_inline bool nowrite_check(buffer_t *b, const size_t read)
{
    if (read == 0)
    {
        // Check if it's an EOF or something different
        if (feof(b->config.fdata->file))
        {
            PyErr_Format(PyExc_EOFError, "Unable to read data from file '%s', reached End Of File (EOF)", b->config.fdata->name);
        }
        else
        {
            const int err = errno;
            PyErr_Format(PyExc_OSError, "Unable to read data from file '%s', received errno %i: '%s'", b->config.fdata->name, err, strerror(err));
        }

        return false;
    }

    return true;
}

#define DECBUFFER_DEFAULTSIZE 4096

static PyObject *decoder_decode(hookobj_t *dec, PyObject **args, Py_ssize_t nargs)
{
    // Separate case for no streaming
    if (dec->config.fdata == NULL)
    {
        // Parse arguments if not streaming
        fixed_positional(nargs, 1);

        PyObject *encoded = parse_positional(args, 0, NULL, "encoded");
        NULLCHECK(encoded);

        return decbuffer_start(dec->config, encoded);
    }

    // Manually set up the buffer object
    buffer_t b;
    
    // Assign the config data
    b.config = dec->config;

    // Set up the buffer
    b.base = (char *)malloc(DECBUFFER_DEFAULTSIZE);

    if (b.base == NULL)
        return PyErr_NoMemory();

    b.offset = b.base;
    b.maxoffset = b.base + DECBUFFER_DEFAULTSIZE;

    // Keep track of the file offset
    //const size_t start_offset = ftell(b.config.fdata->file);

    // Read data from the file
    const size_t read = fread(b.base, 1, DECBUFFER_DEFAULTSIZE, b.config.fdata->file);

    NULLCHECK(nowrite_check(&b, read));

    // Set the max offset to how far we read
    b.maxoffset = b.base + read;

    // Decode the data
    PyObject *result = decode_bytes(&b);

    // Calculate up to where we used file data
    const size_t end_offset = ftell(b.config.fdata->file) - (b.maxoffset - b.offset);

    // Set the file offset at the end of the data we needed, so that it starts at the new data
    fseek(b.config.fdata->file, end_offset, SEEK_SET);

    // Free the internal buffer
    free(b.base);

    return result;
}

// Refresh the streaming buffer
static bool decoder_streaming_refresh(buffer_t *b, const size_t requested)
{
    // How much we have to reread from the file
    const size_t reread = (size_t)(b->maxoffset - b->offset);

    // Set the file cursor back to where we have to reread from
    fseek(b->config.fdata->file, -reread, SEEK_CUR);

    // Check if the requested size is larger than the entire buffer
    if (requested > (size_t)(b->maxoffset - b->base))
    {
        // Allocate a new object to fit the requested size
        const size_t newsize = requested * 1.2;

        char *newptr = (char *)realloc(b->base, newsize);

        if (newptr == NULL)
        {
            PyErr_NoMemory();
            return false;
        }

        // Update the max offset for the reading part
        b->maxoffset = newptr + newsize;
        b->base = newptr;
    }

    // Read new data into the buffer
    const size_t to_read = b->maxoffset - b->base;
    const size_t read = fread(b->base, 1, to_read, b->config.fdata->file);

    NULLCHECK(nowrite_check(b, read));

    // Update the offsets
    b->offset = b->base;
    b->maxoffset = b->base + read;

    return true;
}

static void encoder_dealloc(hookobj_t *enc)
{
    Py_XDECREF(enc->config.ext);

    if (enc->config.fdata != NULL)
    {
        // Check if a file is open
        if (enc->config.fdata->file)
                fclose(enc->config.fdata->file);
        
        // Free the fdata object itself
        PyObject_Free(enc->config.fdata);
    }

    PyObject_Del(enc);
}

static void decoder_dealloc(hookobj_t *dec)
{
    Py_XDECREF(dec->config.ext);

    if (dec->config.fdata != NULL)
    {
        // Check if a file is open
        if (dec->config.fdata->file)
                fclose(dec->config.fdata->file);
        
        // Free the fdata object itself
        PyObject_Free(dec->config.fdata);
    }

    PyObject_Del(dec);
}


//////////////////
//    STATES    //
//////////////////

// Convenience function for getting the module states
static _always_inline mstates_t *get_mstates(PyObject *m)
{
    return (mstates_t *)PyModule_GetState(m);
}

// Create an interned string of NAME
#define GET_ISTR(name) \
    if ((s->interned.name = PyUnicode_InternFromString(#name)) == NULL) return false;

// Set up the module states
static bool setup_mstates(PyObject *m)
{
    mstates_t *s = get_mstates(m);

    // Create interned strings
    GET_ISTR(obj)
    GET_ISTR(ext_types)
    GET_ISTR(file_name)


    // NULL-initialize the string cache
    memset(s->caches.strings.strings, 0, sizeof(s->caches.strings.strings));

    // Initialize match strengths as 1 so that they'll immediately reach 0 on the first decrement
    memset(s->caches.strings.match_strength, 1, sizeof(s->caches.strings.match_strength));

    #ifdef _need_threadsafe

    // Initialize the string cache's locks to a clear state
    for (size_t i = 0; i < STRING_CACHE_SLOTS; ++i)
        atomic_flag_clear(&s->caches.strings.locks[i]);

    #endif


    // Dummy objects to copy into the cache
    PyLongObject *dummylong_pos = (PyLongObject *)PyLong_FromLong(1);
    PyLongObject *dummylong_neg = (PyLongObject *)PyLong_FromLong(-1);

    // Temporarily make them immortal
    Py_ssize_t dummylong_pos_refcnt = Py_REFCNT(dummylong_pos);
    Py_ssize_t dummylong_neg_refcnt = Py_REFCNT(dummylong_neg);
    Py_SET_REFCNT(dummylong_pos, _Py_IMMORTAL_REFCNT);
    Py_SET_REFCNT(dummylong_neg, _Py_IMMORTAL_REFCNT);
    
    // First store the negative values
    for (size_t i = 0; i < 128; ++i)
    {
        s->caches.integers[i] = *dummylong_neg;

        digit val = 128 - i;

        // Negative values are stored as positive
        #if PYVER12
        s->caches.integers[i].long_value.ob_digit[0] = val;
        #else
        s->caches.integers[i].ob_digit[0] = val;
        #endif
    }
    
    for (size_t i = 0; i < 1024; ++i)
    {
        size_t idx = i + 128;

        s->caches.integers[idx] = *dummylong_pos;

        // Set the integer value to the current index
        #if PYVER12
        s->caches.integers[idx].long_value.ob_digit[0] = i;
        #else
        s->caches.integers[idx].ob_digit[0] = i;
        #endif
    }

    // Restore the dummy refcount
    Py_SET_REFCNT(dummylong_pos, dummylong_pos_refcnt);
    Py_SET_REFCNT(dummylong_neg, dummylong_neg_refcnt);

    Py_DECREF(dummylong_pos);
    Py_DECREF(dummylong_neg);

    return true;
}

// Clean up the module states
static void cleanup_mstates(PyObject *m)
{
    mstates_t *s = get_mstates(m);

    // Remove references from the string cache
    for (size_t i = 0; i < STRING_CACHE_SLOTS; ++i)
        Py_XDECREF(s->caches.strings.strings[i]);
    
}


/////////////////////
//   METHOD DEFS   //
/////////////////////

static PyMethodDef EncoderMethods[] = {
    {"encode", (PyCFunction)encoder_encode, METH_O, NULL},
};

static PyMethodDef DecoderMethods[] = {
    {"decode", (PyCFunction)decoder_decode, METH_FASTCALL, NULL},
};

static PyMethodDef CmsgpackMethods[] = {
    {"encode", (PyCFunction)encode, METH_FASTCALL | METH_KEYWORDS, NULL},
    {"decode", (PyCFunction)decode, METH_FASTCALL | METH_KEYWORDS, NULL},

    {"ExtTypesEncode", (PyCFunction)ExtTypesEncode, METH_O, NULL},
    {"ExtTypesDecode", (PyCFunction)ExtTypesDecode, METH_FASTCALL, NULL},

    {"Encoder", (PyCFunction)Encoder, METH_FASTCALL | METH_KEYWORDS, NULL},
    {"Decoder", (PyCFunction)Decoder, METH_FASTCALL | METH_KEYWORDS, NULL},

    {NULL, NULL, 0, NULL}
};


///////////////////
//    OBJECTS    //
///////////////////

static PyTypeObject ExtTypesEncodeObj = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "cmsgpack.ExtTypesEncode",
    .tp_basicsize = sizeof(ext_types_encode_t),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_dealloc = (destructor)ext_types_encode_dealloc,
};

static PyTypeObject ExtTypesDecodeObj = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "cmsgpack.ExtTypesDecode",
    .tp_basicsize = sizeof(ext_types_decode_t),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_dealloc = (destructor)ext_types_decode_dealloc,
};

static PyTypeObject EncoderObj = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "cmsgpack.Encoder",
    .tp_basicsize = sizeof(hookobj_t),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = EncoderMethods,
    .tp_dealloc = (destructor)encoder_dealloc,
};

static PyTypeObject DecoderObj = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "cmsgpack.Decoder",
    .tp_basicsize = sizeof(hookobj_t),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = DecoderMethods,
    .tp_dealloc = (destructor)decoder_dealloc,
};

static void cleanup(PyObject *module);

static struct PyModuleDef cmsgpack = {
    PyModuleDef_HEAD_INIT,
    .m_name = "cmsgpack",
    .m_doc = "Fast MessagePack serializer",
    .m_methods = CmsgpackMethods,
    .m_size = sizeof(mstates_t),
    .m_free = (freefunc)cleanup,
};


//////////////////////
//  MODULE METHODS  //
//////////////////////

static void cleanup(PyObject *module)
{
    cleanup_mstates(module);
}

#define PYTYPE_READY(type) \
    if (PyType_Ready(&type) != 0) return NULL;

PyMODINIT_FUNC PyInit_cmsgpack(void)
{
    // See if the module is already in the state
    PyObject *m = PyState_FindModule(&cmsgpack);
    if (m != NULL)
    {
        Py_INCREF(m);
        return m;
    }
    
    // Prepare custom types
    PYTYPE_READY(EncoderObj);
    PYTYPE_READY(DecoderObj);

    PYTYPE_READY(ExtTypesEncodeObj);
    PYTYPE_READY(ExtTypesDecodeObj);

    // Create main module
    m = PyModule_Create(&cmsgpack);
    NULLCHECK(m);

    // Add the module to the state
    if (PyState_AddModule(m, &cmsgpack) != 0) {
        Py_DECREF(m);
        return NULL;
    }
    
    // Setup the module states
    if (!setup_mstates(m))
    {
        Py_DECREF(m);
        return NULL;
    }

    return m;
}

