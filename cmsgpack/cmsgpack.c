// Licensed under the MIT License.

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

// Default buffer size
#define BUFFER_DEFAULTSIZE 256

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

    // Encoding data
    struct {
        size_t item_avg;    // Average size to allocate per item
        size_t extra_avg;   // Average size to allocate extra
    } encoding;
} mstates_t;

static _always_inline mstates_t *get_mstates(PyObject *m);


// Struct holding buffer data for encoding
typedef struct {
    PyBytesObject *obj; // The Bytes object with the buffer
    size_t allocated;   // How much was allocated to the buffer

    char *offset;      // Offset to write to
    char *maxoffset;   // Maximum offset value before reallocation is needed
    Py_ssize_t nitems; // Number of items within the "outer" object, used for adaptive allocation (-1 if not used)
    ext_types_encode_t *ext; // object for encoding ext types
    bool strict_keys;  // Whether map keys can only be strings
} encbuffer_t;

// Struct holding buffer data for decoding
typedef struct {
    char *base;        // Base of the buffer object that contains the data to decode
    char *offset;      // Offset writing adress
    char *maxoffset;   // Maximum offset address
    ext_types_decode_t *ext; // Object for decoding ext types
    bool strict_keys;  // Whether map keys can only be strings
    bool is_stream;    // Whether we are streaming, to determine what to do on overreads
} decbuffer_t;

static _always_inline size_t encbuffer_reloffset(encbuffer_t *b);
static bool encbuffer_expand(encbuffer_t *b, size_t needed);


typedef struct {
    char *name; // Name of the file
    FILE *file; // File object being used
} filedata_t;

typedef struct {
    PyObject_HEAD
    encbuffer_t data;  // Encode buffer to retain arguments
    filedata_t  file;  // Optional file data for streaming
    mstates_t *states; // Pointer to the module states for easy access
} encoder_t;

typedef struct {
    PyObject_HEAD
    decbuffer_t data;
    filedata_t  file;
    mstates_t *states;

    size_t buffer_size; // The actual size of the buffer
} decoder_t;

static PyTypeObject EncoderObj;
static PyTypeObject DecoderObj;

static bool encode_object(encbuffer_t *b, PyObject *obj);
static PyObject *decode_bytes(decbuffer_t *b);
static bool decoder_streaming_refresh(decoder_t *dec, const size_t requested);


static struct PyModuleDef cmsgpack;


/////////////////////
//  COMMON ERRORS  //
/////////////////////

// Error for when LIMIT_LARGE is exceeded
#define error_size_limit(name, size) (PyErr_Format(PyExc_ValueError, #name " values can only hold up to 4294967295 bytes (2^32-1, 4 bytes), got a size of %zu", size))

// Error for when we get an unexpected type at an argument
#define error_unexpected_type(argname, correct_tpname, received_tpname) (PyErr_Format(PyExc_TypeError, "Expected argument '%s' to be of type '%s', but got an object of type '%s'", argname, correct_tpname, received_tpname))

// Error for when a file couldn't be opened
#define error_cannot_open_file(filename, err) (PyErr_Format(PyExc_OSError, "Unable to open file '%s', received errno %i: '%s'", filename, err, strerror(err)))


///////////////////
//  BYTES TO PY  //
///////////////////

// FNV-1a hashing algorithm
static _always_inline uint32_t fnv1a(const char *in, size_t size)
{
    uint32_t hash = 0x811C9DC5;

    for (size_t i = 0; i < size; ++i)
        hash = (hash ^ in[i]) * 0x01000193;

    return hash;
}

// The number of slots to use in the common value caches
static size_t common_string_slots = 1024;
static PyASCIIObject **common_strings;

static _always_inline PyObject *fixstr_to_py(char *ptr, size_t size)
{
    const size_t hash = fnv1a(ptr, size) & (common_string_slots - 1);
    PyASCIIObject *match = common_strings[hash];

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
    PyObject *obj = PyUnicode_DecodeUTF8(ptr, size, NULL);

    // Add to commons table if ascii
    if (PyUnicode_IS_COMPACT_ASCII(obj))
    {
        Py_XDECREF(match);
        Py_INCREF(obj);
        common_strings[hash] = (PyASCIIObject *)obj;
    }

    return obj;
}

static _always_inline PyObject *str_to_py(char *ptr, size_t size)
{
    return PyUnicode_DecodeUTF8(ptr, size, NULL);;
}

static size_t common_long_slots = 4096;
static PyLongObject *common_longs;

static PyObject *int_to_py(uint64_t num, bool is_uint)
{
    if (_LIKELY(num < common_long_slots && is_uint))
    {
        PyLongObject *obj = &common_longs[num];

        Py_INCREF(obj);
        return (PyObject *)obj;
    }

    // Most cases can use the LongLong function, prefer that path
    if (_LIKELY(num < 0x8000000000000000 || !is_uint))
        return PyLong_FromLongLong((long long)num);
    
    // This is reached by unsigned integers that'd be interpreted as signed by PyLong_FromLongLong
    return PyLong_FromUnsignedLongLong((unsigned long long)num);
}

static _always_inline PyObject *float_to_py(double num)
{
    // Manually create a float object, Python's approach is slower
    PyFloatObject *obj = PyObject_New(PyFloatObject, &PyFloat_Type);

    // Convert the double to native endianness
    BIG_DOUBLE(num);

    // Set the double to the object
    obj->ob_fval = num;

    return (PyObject *)obj;
}

static _always_inline PyObject *bin_to_py(char *ptr, size_t size)
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
    if (_UNLIKELY(nargs < min)) \
    { \
        PyErr_Format(PyExc_TypeError, "Expected at least %zi positional arguments, but received only %zi", min, nargs); \
        return NULL; \
    } \
} while (0)

#define fixed_positional(nargs, required) do { \
    if (_UNLIKELY(nargs != required)) \
    { \
        PyErr_Format(PyExc_TypeError, "Expected exactly %zi positional arguments, but received %zi", required, nargs); \
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
    // Return NULL if we don't have ext types
    if (b->ext == NULL)
        return NULL;
    
    // Attempt to find the encoding function assigned to this type
    PyObject *func = ext_encode_pull(b->ext, Py_TYPE(obj));
    if (_UNLIKELY(func == NULL))
    {
        // Attempt to match the parent type
        func = ext_encode_pull(b->ext, Py_TYPE(obj)->tp_base);

        // No match against parent type either
        if (_UNLIKELY(func == NULL))
            return PyErr_Format(PyExc_ValueError, "Received unsupported type: '%s'", Py_TYPE(obj)->tp_name);
    }
    
    // Call the function and get what it returns
    PyObject *result = PyObject_CallOneArg(func, obj);
    NULLCHECK(result);
    
    // We should receive a tuple of 2 items, holding the ID and the buffer
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

    // The ID should be on index 0, and the bytes on index 1
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

            key = fixstr_to_py(b->offset, size);
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
        NULLCHECK(encbuffer_expand(b, (extra))); \
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
        char buf[2] = {
            mask,
            size
        };

        memcpy(b->offset, buf, 2);
        b->offset += 2;
    }
    else if (nbytes == 2) // MEDIUM
    {
        char buf[3] = {
            mask,
            size >> 8,
            size
        };

        memcpy(b->offset, buf, 3);
        b->offset += 3;
    }
    else if (nbytes == 4) // LARGE
    {
        char buf[5] = {
            mask,
            size >> 24,
            size >> 16,
            size >> 8,
            size,
        };

        memcpy(b->offset, buf, 5);
        b->offset += 5;
    }
    else if (nbytes == 8) // For specific cases
    {
        char buf[9] = {
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

        memcpy(b->offset, buf, 9);
        b->offset += 9;
    }
}

static _always_inline bool write_string(encbuffer_t *b, char *base, size_t size)
{
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

static _always_inline bool write_binary(encbuffer_t *b, char *base, size_t size)
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

static bool write_array(encbuffer_t *b, PyObject **items, size_t nitems)
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
        if (_UNLIKELY(!encode_object(b, items[i])))
            return false;
    }

    return true;
}

static _always_inline bool write_map(encbuffer_t *b, size_t npairs)
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
    else if (_LIKELY(npairs <= LIMIT_LARGE))
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

static _always_inline bool write_integer(encbuffer_t *b, uint64_t num, bool neg)
{
    ENSURE_SPACE(9);

    if (_LIKELY(!neg))
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

    return true;
}

static _always_inline bool write_double(encbuffer_t *b, double num)
{
    ENSURE_SPACE(9);

    // Ensure big-endianness
    BIG_DOUBLE(num);

    INCBYTE = DT_FLOAT_BIT64;
    memcpy(b->offset, &num, 8);

    b->offset += 8;

    return true;
}

static _always_inline bool write_boolean(encbuffer_t *b, bool istrue)
{
    ENSURE_SPACE(1);
    INCBYTE = istrue ? DT_TRUE : DT_FALSE;
    return true;
}

static _always_inline bool write_nil(encbuffer_t *b)
{
    ENSURE_SPACE(1);
    INCBYTE = DT_NIL;
    return true;
}

static _always_inline bool write_extension(encbuffer_t *b, PyObject *obj)
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


///////////////////
//  PY TO WRITE  //
///////////////////

// Function separate from `write_py_str` for re-use in other places
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
    }
}

static _always_inline bool write_py_str(encbuffer_t *b, PyObject *obj)
{
    char *base;
    size_t size;
    py_str_data(obj, &base, &size);

    return write_string(b, base, size);
}

static _always_inline bool write_py_bytes(encbuffer_t *b, PyObject *obj)
{
    char *base = PyBytes_AS_STRING(obj);
    size_t size = PyBytes_GET_SIZE(obj);

    return write_binary(b, base, size);
}

static _always_inline bool write_py_bytearray(encbuffer_t *b, PyObject *obj)
{
    char *base = PyByteArray_AS_STRING(obj);
    size_t size = PyByteArray_GET_SIZE(obj);

    return write_binary(b, base, size);
}

static _always_inline bool write_py_memoryview(encbuffer_t *b, PyObject *obj)
{
    PyMemoryViewObject *ob = (PyMemoryViewObject *)obj;

    char *base = ob->view.buf;
    size_t size = ob->view.len;

    return write_binary(b, base, size);
}

static _always_inline bool write_py_float(encbuffer_t *b, PyObject *obj)
{
    double num = PyFloat_AS_DOUBLE(obj);

    write_double(b, num);

    return true;
}

static _always_inline bool extract_py_long(PyObject *obj, uint64_t *num, bool *neg)
{
    // Cast the object to a long value to access internals
    PyLongObject *lobj = (PyLongObject *)obj;

    // Number of digits and pointer to digits array
    long ndigits;
    digit *digits;
    
    // Manual extraction based on the Python version
    #if PY_VERSION_HEX >= 0x030C0000

    // Extract the negative flag
    *neg = (lobj->long_value.lv_tag & _PyLong_SIGN_MASK) != 0;

    // Extract the number of digits
    ndigits = lobj->long_value.lv_tag >> _PyLong_NON_SIZE_BITS;
    
    // Set the pointer to the digits array
    digits = lobj->long_value.ob_digit;

    #else

    // Number of digits (negative value if the long is negative)
    long _ndigits = (long)Py_SIZE(lobj);

    // Convert the digits to its absolute value
    unsigned long mask = _ndigits >> (sizeof(long) * 8 - 1);
    ndigits = (_ndigits + mask) ^ mask;

    // Set the negative flag based on if the number of digits changed (went from negative to positive)
    *neg = ndigits != _ndigits;

    // Set the pointer to the digits array
    digits = lobj->ob_digit;

    #endif

    // Set the first digit of the value
    *num = (uint64_t)digits[0];

    if (!_PyLong_IsCompact(lobj))
    {
        uint64_t last;
        while (--ndigits >= 1)
        {
            last = *num;

            *num <<= PyLong_SHIFT;
            *num |= digits[ndigits];

            if ((*num >> PyLong_SHIFT) != last)
                return false;
        }

        // Check if the value exceeds the size limit when negative
        if (_UNLIKELY(neg && *num > (1ull << 63)))
            return false;
    }

    return true;
}

static _always_inline bool write_py_int(encbuffer_t *b, PyObject *obj)
{
    uint64_t num;
    bool neg;
    if (_UNLIKELY(!extract_py_long(obj, &num, &neg)))
    {
        PyErr_SetString(PyExc_OverflowError, "Integer values cannot exceed " LIMIT_UINT_STR " (2^64-1) or " LIMIT_INT_STR " (-2^63)");
        return false;
    }

    return write_integer(b, num, neg);
}

static _always_inline bool write_py_bool(encbuffer_t *b, PyObject *obj)
{
    return write_boolean(b, obj == Py_True);
}

static _always_inline bool write_py_none(encbuffer_t *b, PyObject *obj)
{
    return write_nil(b);
}

static _always_inline bool write_py_list(encbuffer_t *b, PyObject *obj)
{
    PyObject **items = ((PyListObject *)obj)->ob_item;
    size_t nitems = PyList_GET_SIZE(obj);

    return write_array(b, items, nitems);
}

static _always_inline bool write_py_tuple(encbuffer_t *b, PyObject *obj)
{
    PyObject **items = ((PyTupleObject *)obj)->ob_item;
    size_t nitems = PyTuple_GET_SIZE(obj);

    return write_array(b, items, nitems);
}

static _always_inline bool write_py_dict(encbuffer_t *b, PyObject *obj)
{
    const size_t npairs = PyDict_GET_SIZE(obj);

    NULLCHECK(write_map(b, npairs));

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
            !encode_object(b, key) ||
            !encode_object(b, val)
        )) return false;
    }

    return true;
}

static _always_inline bool write_py_ext(encbuffer_t *b, PyObject *obj)
{
    return write_extension(b, obj);
}


////////////////////
//    ENCODING    //
////////////////////

static bool encode_object(encbuffer_t *b, PyObject *obj)
{
    PyTypeObject *tp = Py_TYPE(obj);

    // All standard types
    if (tp == &PyUnicode_Type)
    {
        return write_py_str(b, obj);
    }
    else if (tp == &PyLong_Type)
    {
        return write_py_int(b, obj);
    }
    else if (tp == &PyFloat_Type)
    {
        return write_py_float(b, obj);
    }
    else if (tp == &PyBool_Type)
    {
        return write_py_bool(b, obj);
    }
    else if (PyList_Check(obj)) // No exact check to support subclasses
    {
        return write_py_list(b, obj);
    }
    if (PyTuple_Check(obj)) // Same as with lists, no exact check
    {
        return write_py_tuple(b, obj);
    }
    else if (PyDict_Check(obj)) // Same as with lists and tuples
    {
        return write_py_dict(b, obj);
    }
    else if (tp == Py_TYPE(Py_None)) // No explicit PyNone_Type available
    {
        return write_py_none(b, obj);
    }
    else if (tp == &PyBytes_Type)
    {
        return write_py_bytes(b, obj);
    }
    else if (tp == &PyByteArray_Type)
    {
        return write_py_bytearray(b, obj);
    }
    else if (tp == &PyMemoryView_Type)
    {
        return write_py_memoryview(b, obj);
    }
    else
    {
        return write_py_ext(b, obj);
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
#define SIZEBYTE ((unsigned char)(b->offset++)[0])

// Check if the buffer won't be overread
#define OVERREAD_CHECK(to_add) do { \
    if (_UNLIKELY(b->offset + to_add > b->maxoffset)) \
    { \
        if (_LIKELY(b->is_stream)) \
        { \
            NULLCHECK(decoder_streaming_refresh((decoder_t *)b, to_add)); \
        } \
        else \
        { \
            PyErr_SetString(PyExc_ValueError, INVALID_MSG " (overread the encoded data)"); \
            return NULL; \
        } \
    } \
} while (0)

// Decoding of fixsize objects
static _always_inline PyObject *decode_bytes_fixsize(decbuffer_t *b, const unsigned char mask)
{
    // Size inside of the fixed mask
    size_t n = (size_t)mask; // N has to be masked before use

    // Check which fixmask we got
    const unsigned char fixmask = mask & 0b11100000;
    if (fixmask == DT_STR_FIXED)
    {
        n &= 0x1F;
        OVERREAD_CHECK(n);

        PyObject *obj = fixstr_to_py(b->offset, n);
        b->offset += n;
        return obj;
    }
    else if ((mask & 0x80) == 0) // uint only has upper bit set to 0
    {
        return int_to_py(n, true);
    }
    else if (fixmask == DT_INT_FIXED)
    {
        long num = (long)n;

        num &= 0x1F;

        // Sign-extend the integer
        if (num & 0x10)
            num |= ~0x1F;

        return int_to_py(num, false);
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

// Decoding of varlen objects
static _always_inline PyObject *decode_bytes_varlen(decbuffer_t *b, const unsigned char mask)
{
    // Global variable to hold the size across multiple cases
    uint64_t n = 0;

    // Switch over the varlen mask
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

        PyObject *obj = str_to_py(b->offset, n);
        b->offset += n;
        return obj;
    }

    case VARLEN_DT(DT_UINT_BIT8):
    {
        OVERREAD_CHECK(1);
        n = SIZEBYTE;
        
        return int_to_py(n, true);
    }
    case VARLEN_DT(DT_UINT_BIT16):
    {
        OVERREAD_CHECK(2);
        n |= SIZEBYTE << 8;
        n |= SIZEBYTE;
        
        return int_to_py(n, true);
    }
    case VARLEN_DT(DT_UINT_BIT32):
    {
        OVERREAD_CHECK(4);
        n |= SIZEBYTE << 24;
        n |= SIZEBYTE << 16;
        n |= SIZEBYTE <<  8;
        n |= SIZEBYTE;
        
        return int_to_py(n, true);
    }
    case VARLEN_DT(DT_UINT_BIT64):
    {
        OVERREAD_CHECK(8);

        memcpy(&n, b->offset, 8);
        b->offset += 8;

        n = BIG_64(n);

        return int_to_py(n, true);
    }

    case VARLEN_DT(DT_INT_BIT8):
    {
        OVERREAD_CHECK(1);
        n |= SIZEBYTE;

        // Sign-extend
        if (_LIKELY(n & 0x80))
            n |= ~0xFF;
        
        return int_to_py(n, false);
    }
    case VARLEN_DT(DT_INT_BIT16):
    {
        OVERREAD_CHECK(2);
        n |= SIZEBYTE << 8;
        n |= SIZEBYTE;

        if (_LIKELY(n & 0x8000))
            n |= ~0xFFFF;
        
        return int_to_py(n, false);
    }
    case VARLEN_DT(DT_INT_BIT32):
    {
        OVERREAD_CHECK(4);
        n |= SIZEBYTE << 24;
        n |= SIZEBYTE << 16;
        n |= SIZEBYTE <<  8;
        n |= SIZEBYTE;

        if (_LIKELY(n & 0x80000000))
            n |= ~0xFFFFFFFF;
        
        return int_to_py(n, false);
    }
    case VARLEN_DT(DT_INT_BIT64):
    {
        OVERREAD_CHECK(8);

        memcpy(&n, b->offset, 8);
        b->offset += 8;
        n = BIG_64(n);

        return int_to_py(n, false);
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

        if (_LIKELY(mask == DT_FLOAT_BIT64))
        {
            OVERREAD_CHECK(8);

            memcpy(&num, b->offset, 8);
            b->offset += 8;
        }
        else
        {
            OVERREAD_CHECK(4);

            float _num;
            memcpy(&_num, b->offset, 4);
            num = (double)_num;

            b->offset += 4;
        }

        return float_to_py(num);
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

        PyObject *obj = bin_to_py(b->offset, n);
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

static PyObject *decode_bytes(decbuffer_t *b)
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

// Prepare the encoding buffer struct
static _always_inline bool encbuffer_prepare(encbuffer_t *b, PyObject *obj, mstates_t *states)
{
    // Initialize the new size with the extra average and set NITEMS to 0 in advance
    size_t allocsize = states->encoding.extra_avg;
    b->nitems = 0;

    // Attempt to allocate appropriate memory for the buffer based on OBJ's type
    PyTypeObject *tp = Py_TYPE(obj);
    if (_LIKELY(tp == &PyList_Type || tp == &PyDict_Type))
    {
        // Get the number of items in the container
        b->nitems = Py_SIZE(obj);
        
        // Add allocation size based on the number of items
        allocsize += b->nitems * states->encoding.item_avg;
    }

    // Set the new size
    b->allocated = allocsize;

    // Create the new buffer object
    b->obj = (PyBytesObject *)PyBytes_FromStringAndSize(NULL, b->allocated);

    if (_UNLIKELY(b->obj == NULL))
    {
        // Attempt to create an object with the default buffer size
        b->obj = (PyBytesObject *)PyBytes_FromStringAndSize(NULL, BUFFER_DEFAULTSIZE);

        if (_UNLIKELY(b->obj == NULL))
        {
            PyErr_NoMemory();
            return false;
        }
    }

    // Set up the offset and max offset based on the buffer
    b->offset = PyBytes_AS_STRING(b->obj);
    b->maxoffset = PyBytes_AS_STRING(b->obj) + b->allocated;

    return true;
}

#define EXTRA_ALLOC_MIN 64
#define ITEM_ALLOC_MIN 6

static _always_inline size_t biased_average(size_t curr, size_t new)
{
    const size_t curr_doubled = curr * 2;

    // Safety against growing too quickly by limiting growth size to a factor of 2
    if (_UNLIKELY(curr_doubled < new))
        return curr_doubled;
    
    // Return a biased average leaning more towards the current value
    return (curr_doubled + new) / 3;
}

static _always_inline void update_adaptive_allocation(encbuffer_t *b, mstates_t *states)
{
    const size_t needed = encbuffer_reloffset(b);
    const Py_ssize_t nitems = b->nitems;

    // Take the average between how much we needed and the currently set value
    states->encoding.extra_avg = biased_average(states->encoding.extra_avg, needed);

    // Enforce a minimum size for safety. Check if the threshold is crossed by subtracting
    // it from the value, casting to a signed value, and checking if we're below 0 (overflow happened)
    if (_UNLIKELY((Py_ssize_t)(states->encoding.extra_avg - EXTRA_ALLOC_MIN) < 0))
        states->encoding.extra_avg = EXTRA_ALLOC_MIN;
    
    // Return if NITEMS is 0 to avoid division by zero
    if (b->nitems == 0)
        return;

    // Size allocated per item and required per item
    const size_t needed_per_item = needed * pow((double)nitems, -1.0); // Multiply by reciprocal, faster than division

    // Take the average between the currently set per-item value and that of this round
    states->encoding.item_avg = biased_average(states->encoding.item_avg, needed_per_item);

    // Enforce a value threshold as we did with the `extra_avg` value
    if (_UNLIKELY((Py_ssize_t)(states->encoding.item_avg - ITEM_ALLOC_MIN) < 0))
       states->encoding.item_avg = ITEM_ALLOC_MIN;

    return;
}

// Start encoding with an encbuffer
static PyObject *encbuffer_start(encbuffer_t *b, PyObject *obj, mstates_t *states)
{
    // Prepare the encode buffer
    NULLCHECK(encbuffer_prepare(b, obj, states));

    // Encode the object
    NULLCHECK(encode_object(b, obj));

    // Set the object's size
    Py_SET_SIZE(b->obj, encbuffer_reloffset(b));

    // Update the adaptive allocation
    update_adaptive_allocation(b, states);

    // Return the object
    return (PyObject *)b->obj;
}

// Expand the encoding buffer
static bool encbuffer_expand(encbuffer_t *b, size_t extra)
{
    // Scale the size by factor 1.5x
    const size_t allocsize = (encbuffer_reloffset(b) + extra) * 1.5;

    // Reallocate the object (also allocate space for the bytes object itself)
    PyBytesObject *reallocd = PyObject_Realloc(b->obj, allocsize + sizeof(PyBytesObject));

    if (_UNLIKELY(reallocd == NULL))
    {
        PyErr_NoMemory();
        return false;
    }

    // Update the offsets
    b->offset = PyBytes_AS_STRING(reallocd) + encbuffer_reloffset(b);
    b->maxoffset = PyBytes_AS_STRING(reallocd) + allocsize;

    // Update the mod states
    b->obj = reallocd;
    b->allocated = allocsize;

    return true;
}


// Start decoding with a decbuffer (not used with streaming)
static _always_inline PyObject *decbuffer_start(decbuffer_t *b, PyObject *encoded)
{
    // Get a buffer of the encoded data buffer
    Py_buffer buffer;
    if (PyObject_GetBuffer(encoded, &buffer, PyBUF_SIMPLE) != 0)
    {
        PyErr_SetString(PyExc_BufferError, "Unable to open a buffer of the received encoded data.");
        return NULL;
    }
    
    b->base = buffer.buf;
    b->offset = b->base;
    b->maxoffset = b->base + buffer.len;

    PyObject *result = decode_bytes(b);

    PyBuffer_Release(&buffer);

    // Check if we reached the end of the buffer
    if (_UNLIKELY(result != NULL && b->offset != b->maxoffset && 0))
    {
        PyErr_SetString(PyExc_ValueError, INVALID_MSG " (encoded message ended early)");
        return NULL;
    }

    return result;
}

// Get the relative offset of an encbuffer
static _always_inline size_t encbuffer_reloffset(encbuffer_t *b)
{
    return b->offset - PyBytes_AS_STRING(b->obj);
}

// Get the relative offset of a decbuffer
static _always_inline size_t decbuffer_reloffset(decbuffer_t *b)
{
    return (size_t)(b->offset - b->base);
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

    return encbuffer_start(&b, obj, states);
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
    b.is_stream = false;

    return decbuffer_start(&b, encoded);
}


///////////////////////
//  ENC/DEC OBJECTS  //
///////////////////////

static PyObject *Encoder(PyObject *self, PyObject **args, Py_ssize_t nargs, PyObject *kwargs)
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


    // Create the Encoder
    encoder_t *enc = PyObject_New(encoder_t, &EncoderObj);
    if (enc == NULL)
        return PyErr_NoMemory();
    

    // See if we got a filename object, assign it if so
    if (filename != NULL)
    {
        size_t strsize;
        char *str;
        py_str_data(filename, &str, &strsize);
        
        enc->file.name = PyObject_Malloc(strsize + 1);
        if (_UNLIKELY(enc->file.name == NULL))
        {
            Py_DECREF(enc);
            return NULL;
        }

        memcpy(enc->file.name, str, strsize);
        enc->file.name[strsize] = 0;

        enc->file.file = fopen(enc->file.name, "ab");
        if (_UNLIKELY(enc->file.file == NULL))
        {
            const int err = errno;
            error_cannot_open_file(enc->file.name, err);
            return NULL;
        }

        // Disable buffering because we already write in chunks
        setvbuf(enc->file.file, NULL, _IONBF, 0);
    }
    else
    {
        // Otherwise set the file and name to NULL
        enc->file.file = NULL;
        enc->file.name = NULL;
    }

    enc->data.ext = (ext_types_encode_t *)ext;
    enc->data.strict_keys = strict_keys == Py_True;
    enc->states = states;

    return (PyObject *)enc;
}

static PyObject *Decoder(PyObject *self, PyObject **args, Py_ssize_t nargs, PyObject *kwargs)
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


    // Create the Decoder
    decoder_t *dec = PyObject_New(decoder_t, &DecoderObj);
    if (dec == NULL)
        return PyErr_NoMemory();
    
    // Check if we received a filename for streaming
    if (filename != NULL)
    {
        // Set the buffer size
        dec->buffer_size = BUFFER_DEFAULTSIZE;

        // Pre-allocate a buffer for the decoder
        dec->data.base = (char *)malloc(dec->buffer_size);

        if (_UNLIKELY(dec->data.base == NULL))
        {
            PyObject_Del(dec); // No DECREF, filename is not allocated yet
            return PyErr_NoMemory();
        }

        // Get the data of the file name
        size_t strsize;
        char *str;
        py_str_data(filename, &str, &strsize);
        
        // Allocate space for the file name
        dec->file.name = PyObject_Malloc(strsize + 1);
        if (_UNLIKELY(dec->file.name == NULL))
        {
            Py_DECREF(dec);
            return NULL;
        }

        // Copy the name into the name buffer and null-terminate
        memcpy(dec->file.name, str, strsize);
        dec->file.name[strsize] = 0;

        // Set the file offsets
        dec->data.maxoffset = dec->data.base + dec->buffer_size;
        dec->data.offset = dec->data.base;

        // Open the file
        dec->file.file = fopen(dec->file.name, "rb");

        if (_UNLIKELY(dec->file.file == NULL))
        {
            Py_DECREF(dec);

            const int err = errno;
            return error_cannot_open_file(dec->file.name, err);
        }

        // Disable buffering
        setvbuf(dec->file.file, NULL, _IONBF, 0);
    }
    else
    {
        // Otherwise set the file stuff to NULL
        dec->file.file = NULL;
        dec->file.name = NULL;
    }

    dec->data.ext = (ext_types_decode_t *)ext;
    dec->data.strict_keys = strict_keys == Py_True;
    dec->data.is_stream = true;
    dec->states = states;

    return (PyObject *)dec;
}

static PyObject *encoder_encode(encoder_t *enc, PyObject *obj)
{
    // Check if the file is closed when streaming
    if (_UNLIKELY(enc->file.name && enc->file.file == NULL))
    {
        enc->file.file = fopen(enc->file.name, "ab");

        if (_UNLIKELY(enc->file.file == NULL))
        {
            const int err = errno;
            return error_cannot_open_file(enc->file.name, err);
        }
    }

    PyBytesObject *result = (PyBytesObject *)encbuffer_start(&enc->data, obj, enc->states);
    NULLCHECK(result);

    // Return the result if we're not streaming
    if (enc->file.name == NULL)
        return (PyObject *)result;
    
    // Otherwise stream the data to a file
    
    // Decref the result as we won't return it.
    // We hold an extra reference, so this won't free the buffer
    Py_DECREF(result);

    const size_t size = PyBytes_GET_SIZE(result);

    // Check if all data is written to the file, otherwise truncate up until data that was written incompletely
    size_t written;
    if (_UNLIKELY(size != (written = fwrite(PyBytes_AS_STRING(result), 1, size, enc->file.file))))
    {
        PyErr_Format(PyExc_OSError, "Failed to write encoded data to file '%s'. A partial read might have occurred, which is attempted to be fixed.", enc->file.name);

        // Attempt to truncate the file
        if (_UNLIKELY(_ftruncate_and_close(enc->file.file, ftell(enc->file.file) - written, enc->file.name) != 0))
        {
            // Set file to NULL to prevent invalid pointer errors, and trigger re-opening on a new write
            enc->file.file = NULL;

            const int err = errno;
            PyErr_Print(); // Print previous error
            PyErr_Format(PyExc_OSError, "Truncating the file back to its old size failed, leaving partially written or corrupt data in file '%s'. Received errno %i: %s", enc->file.name, err, strerror(err));
        }

        return NULL;
    }

    Py_RETURN_NONE;
}

// Check if we read nothing, throw appropriate error if so
static _always_inline bool nowrite_check(decoder_t *dec, const size_t read)
{
    if (_UNLIKELY(read == 0))
    {
        // Check if it's an EOF or something different
        if (feof(dec->file.file))
        {
            PyErr_Format(PyExc_EOFError, "Unable to read data from file '%s', reached End Of File (EOF)", dec->file.name);
        }
        else
        {
            const int err = errno;
            PyErr_Format(PyExc_OSError, "Unable to read data from file '%s', received errno %i: '%s'", dec->file.name, err, strerror(err));
        }

        // Don't close file, that happens when the object is destroyed

        return false;
    }

    return true;
}

static PyObject *decoder_decode(decoder_t *dec, PyObject **args, Py_ssize_t nargs)
{
    // Separate case for no streaming
    if (dec->file.name == NULL)
    {
        // Parse arguments if not streaming
        fixed_positional(nargs, 1);

        PyObject *encoded = parse_positional(args, 0, NULL, "encoded");
        NULLCHECK(encoded);

        return decbuffer_start(&dec->data, encoded);
    }

    // Move the offset to the start of the buffer
    dec->data.offset = dec->data.base;

    // Read data from the file
    const size_t read = fread(dec->data.base, 1, dec->buffer_size, dec->file.file);

    NULLCHECK(nowrite_check(dec, read));

    // Set the max offset to how far we read
    dec->data.maxoffset = dec->data.base + read;

    // Decode the data
    PyObject *result = decode_bytes(&dec->data);

    return result;
}

// Refresh the streaming buffer
static bool decoder_streaming_refresh(decoder_t *dec, const size_t requested)
{
    // Check if the requested size is larger than the entire buffer
    if (_UNLIKELY(requested > dec->buffer_size))
    {
        // Allocate a new object to fit the requested size
        const size_t newsize = requested * 1.2;

        char *newptr = (char *)malloc(newsize);

        if (_UNLIKELY(newptr == NULL))
        {
            PyErr_NoMemory();
            return false;
        }

        // Update the actual buffer size
        dec->buffer_size = newsize;

        // Update the file offsets
        dec->data.offset = newptr + decbuffer_reloffset(&dec->data);
        dec->data.maxoffset = newptr + newsize;

        // Update the base pointer
        dec->data.base = newptr;
    }

    // Read extra data into the buffer
    const size_t to_read = dec->buffer_size;
    const size_t read = fread(dec->data.base, 1, to_read, dec->file.file);

    NULLCHECK(nowrite_check(dec, read));

    // Update the offsets
    dec->data.offset = dec->data.base;
    dec->data.maxoffset = dec->data.base + read;

    return true;
}

static void encoder_dealloc(encoder_t *enc)
{
    // Free the filename
    PyObject_FREE(enc->file.name); // Accepts NULL in case of no streaming

    // Check if a file is open
    if (enc->file.file)
            fclose(enc->file.file);

    PyObject_Del(enc);
}

static void decoder_dealloc(decoder_t *dec)
{
    // BASE is an invalid pointer when not streaming
    if (dec->file.name)
    {
        free(dec->data.base);
        PyObject_FREE(dec->file.name);

        // Also close the file
        fclose(dec->file.file);
    }

    PyObject_Del(dec);
}


//////////////////
//    STATES    //
//////////////////

// Get interned string of NAME
#define GET_ISTR(name) if ((s->interned.name = PyUnicode_InternFromString(#name)) == NULL) return false;

static bool setup_mstates(mstates_t *s)
{
    // Create interned strings
    GET_ISTR(obj)
    GET_ISTR(ext_types)
    GET_ISTR(strict_keys)
    GET_ISTR(file_name)
    GET_ISTR(keep_open)

    // Initialize the average sizes
    s->encoding.extra_avg = EXTRA_ALLOC_MIN;
    s->encoding.item_avg = ITEM_ALLOC_MIN;

    return true;
}

// Get the global states of the module
static _always_inline mstates_t *get_mstates(PyObject *m)
{
    return (mstates_t *)PyModule_GetState(m);
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
    .tp_dealloc = (destructor)encoder_dealloc,
};

static PyTypeObject DecoderObj = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "Decoder",
    .tp_basicsize = sizeof(decoder_t),
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
    free(common_strings);
    free(common_longs);
}

#define PYTYPE_READY(type) \
    if (PyType_Ready(&type)) return NULL;

PyMODINIT_FUNC PyInit_cmsgpack(void) {
    // See if we can find the module in the state already
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
    if (!setup_mstates((mstates_t *)PyModule_GetState(m)))
    {
        Py_DECREF(m);
        return NULL;
    }

    common_strings = (PyASCIIObject **)calloc(sizeof(PyASCIIObject *), common_string_slots);

    if (common_strings == NULL)
        return NULL;
    
    common_longs = (PyLongObject *)malloc(sizeof(PyLongObject) * common_long_slots);

    if (common_longs == NULL)
        return NULL;
    
    for (size_t i = 0; i < common_long_slots; ++i)
    {
        PyLongObject *obj = &common_longs[i];
        
        Py_SET_TYPE(obj, &PyLong_Type);
        Py_SET_REFCNT(obj, 1);

        obj->long_value.ob_digit[0] = i;
        obj->long_value.lv_tag = 0b1100;
    }

    return m;
}

