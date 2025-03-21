/* Licensed under the MIT License. */

#define PY_SSIZE_T_CLEAN

#include "masks.h"
#include "internals.h"

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

// Default buffer size
#define BUFFER_DEFAULTSIZE 256

// Default file buffer size
#define FILEBUF_DEFAULTSIZE 4096

// The number of slots to use in caches
#define STRING_CACHE_SLOTS 1024
#define INTEGER_CACHE_SLOTS (1024 + 128) // 1023 positive, 128 negative, and a zero object
#define INTEGER_CACHE_NNEG 128 // Number of negative slots in the integer cache

// Minimum sizes for the 'extra' and 'item' adaptive allocation weights
#define EXTRA_ALLOC_MIN 64
#define ITEM_ALLOC_MIN 6

// Recursion limit
#define RECURSION_LIMIT 1000

// The default values for extensions objects
#define EXTENSIONS_PASSMEMVIEW_DEFAULTVAL false


///////////////////////////
//  TYPEDEFS & FORWARDS  //
///////////////////////////

typedef struct {
    PyObject_HEAD
    char id;        // ID of the type
    PyObject *func; // The function used for encoding objects of the type set as the dict's key
} ext_dictitem_t;

typedef struct {
    bool pass_memview; // Whether to pass memoryview objects instead of byte objects to decoding functions

    PyObject *dict;   // Dict object containing encoding data
    PyObject **funcs; // Functions array with decoding functions, indexed by ID casted to unsigned
} ext_data_t;

typedef struct {
    PyObject_HEAD
    ext_data_t data;
    PyObject *funcs[256]; // The functions array, inlined to avoid having to allocate it
} extensions_t;

static PyTypeObject ExtDictItemObj;
static PyTypeObject ExtensionsObj;


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
        PyObject *file_name;
        PyObject *types;
        PyObject *extensions;
        PyObject *allow_subclasses;
        PyObject *pass_memoryview;
    } interned;

    // Caches
    struct {
        PyLongObject integers[INTEGER_CACHE_SLOTS];
        strcache_t strings;
    } caches;

    // Global extensions object
    extensions_t extensions;
} mstates_t;


// File data
typedef struct {
    FILE *file;  // File object being used
    char *name; // Name of the file
} filedata_t;

typedef struct {
    char *base;      // Base towards the buffer (for decoding from a file, this holds the file buffer)
    char *offset;    // Current writing offset in the buffer
    char *maxoffset; // The max writing offset

    size_t recursion;  // Recursion depth to prevent cyclic references during encoding

    ext_data_t ext;    // Extensions data
    mstates_t *states; // The module states

    FILE *file;        // The file object in use, NULL if not using a file
    size_t fbuf_size;  // The actual size of the file buffer, for if it's updated
} buffer_t;


typedef struct {
    PyObject_HEAD

    
    extensions_t *ext; // The extensions object to use
    mstates_t *states; // The module states
} stream_t;

typedef struct {
    PyObject_HEAD

    // Fields from `stream_t` for interoperability during creation
    extensions_t *ext;
    mstates_t *states;

    #ifdef _need_threadsafe
    atomic_flag flock; // File lock for multithreaded environments
    #endif
    FILE *file;  // The file, opened in "a+b" mode
    size_t foff; // The file's reading offset

    char *fbuf;       // The file buffer for decoding
    size_t fbuf_size; // The size of the file buffer

    char *fname; // The filename
} filestream_t;

static PyTypeObject StreamObj;
static PyTypeObject FileStreamObj;


static _always_inline mstates_t *get_mstates(PyObject *m);

static bool encode_object(buffer_t *b, PyObject *obj);
static PyObject *decode_bytes(buffer_t *b);

static bool encbuffer_expand(buffer_t *b, size_t needed);
static bool filestream_refresh_fbuf(buffer_t *b, size_t required);


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

static bool extensions_add_encode_internal(extensions_t *ext, char id, PyObject *type, PyObject *encfunc)
{
    // Create a dict item object
    ext_dictitem_t *item = PyObject_New(ext_dictitem_t, &ExtDictItemObj);

    if (!item)
        return PyErr_NoMemory();
    
    Py_INCREF(encfunc);

    item->func = encfunc;
    item->id = id;

    int status = PyDict_SetItem(ext->data.dict, type, (PyObject *)item);

    Py_DECREF(item);

    return status >= 0;
}

static bool extensions_add_decode_internal(extensions_t *ext, char id, PyObject *decfunc)
{
    unsigned char idx = (unsigned char)id;

    Py_XDECREF(ext->funcs[idx]);
    Py_INCREF(decfunc);

    ext->funcs[idx] = decfunc;

    return true;
}

static PyObject *extensions_add(extensions_t *ext, PyObject **args, Py_ssize_t nargs)
{
    if (nargs != 4)
        return PyErr_Format(PyExc_TypeError, "Expected exactly 4 arguments, but got %zi", nargs);
    
    PyObject *longobj = args[0];
    PyObject *type = args[1];
    PyObject *encfunc = args[2];
    PyObject *decfunc = args[3];

    if (!PyLong_CheckExact(longobj))
        return error_unexpected_argtype("id", "int", Py_TYPE(longobj)->tp_name);

    if (!PyType_Check(type))
        return PyErr_Format(PyExc_TypeError, "Expected argument 'type' to be a type object, but got an object of type '%s'", Py_TYPE(type)->tp_name);

    if (!PyCallable_Check(encfunc))
        return PyErr_Format(PyExc_TypeError, "Expected argument 'encfunc' to be a callable object, but got an object of type '%s'", Py_TYPE(encfunc)->tp_name);

    if (!PyCallable_Check(decfunc))
        return PyErr_Format(PyExc_TypeError, "Expected argument 'decfunc' to be a callable object, but got an object of type '%s'", Py_TYPE(decfunc)->tp_name);
    
    long long_id = PyLong_AS_LONG(longobj);

    if (long_id < -128 || long_id > 127)
        return PyErr_Format(PyExc_ValueError, "Expected the ID to be between -128 and 127, but got an ID of %zi", long_id);
    
    char id = (char)long_id;

    if (!extensions_add_encode_internal(ext, id, type, encfunc) ||
        !extensions_add_decode_internal(ext, id, decfunc))
        return NULL;
    
    Py_RETURN_NONE;
}

static PyObject *extensions_add_encode(extensions_t *ext, PyObject **args, Py_ssize_t nargs)
{
    if (nargs != 3)
        return PyErr_Format(PyExc_TypeError, "Expected exactly 3 arguments, but got %zi", nargs);
    
    PyObject *longobj = args[0];
    PyObject *type = args[1];
    PyObject *encfunc = args[2];

    if (!PyLong_CheckExact(longobj))
        return error_unexpected_argtype("id", "int", Py_TYPE(longobj)->tp_name);

    if (!PyType_Check(type))
        return PyErr_Format(PyExc_TypeError, "Expected argument 'type' to be a type object, but got an object of type '%s'", Py_TYPE(type)->tp_name);

    if (!PyCallable_Check(encfunc))
        return PyErr_Format(PyExc_TypeError, "Expected argument 'encfunc' to be a callable object, but got an object of type '%s'", Py_TYPE(encfunc)->tp_name);
    
    long long_id = PyLong_AS_LONG(longobj);

    if (long_id < -128 || long_id > 127)
        return PyErr_Format(PyExc_ValueError, "Expected the ID to be between -128 and 127, but got an ID of %zi", long_id);
    
    char id = (char)long_id;

    if (!extensions_add_encode_internal(ext, id, type, encfunc))
        return NULL;
    
    Py_RETURN_NONE;
}

static PyObject *extensions_add_decode(extensions_t *ext, PyObject **args, Py_ssize_t nargs)
{
    if (nargs != 2)
        return PyErr_Format(PyExc_TypeError, "Expected exactly 2 arguments, but got %zi", nargs);
    
    PyObject *longobj = args[0];
    PyObject *decfunc = args[1];

    if (!PyLong_CheckExact(longobj))
        return error_unexpected_argtype("id", "int", Py_TYPE(longobj)->tp_name);

    if (!PyCallable_Check(decfunc))
        return PyErr_Format(PyExc_TypeError, "Expected argument 'decfunc' to be a callable object, but got an object of type '%s'", Py_TYPE(decfunc)->tp_name);
    
    long long_id = PyLong_AS_LONG(longobj);

    if (long_id < -128 || long_id > 127)
        return PyErr_Format(PyExc_ValueError, "Expected the ID to be between -128 and 127, but got an ID of %zi", long_id);
    
    char id = (char)long_id;

    if (!extensions_add_decode_internal(ext, id, decfunc))
        return NULL;
    
    Py_RETURN_NONE;
}

static PyObject *extensions_remove(extensions_t *ext, PyObject **args, Py_ssize_t nargs)
{
    if (nargs != 2)
        return PyErr_Format(PyExc_TypeError, "Expected exactly 2 arguments, but got %zi", nargs);
    
    PyObject *longobj = args[0];
    PyObject *type = args[1];

    if (!PyLong_CheckExact(longobj))
        return error_unexpected_argtype("id", "int", Py_TYPE(longobj)->tp_name);

    if (!PyType_Check(type))
        return PyErr_Format(PyExc_TypeError, "Expected argument 'type' to be a type object, but got an object of type '%s'", Py_TYPE(type)->tp_name);
    
    long long_id = PyLong_AS_LONG(longobj);

    if (long_id < -128 || long_id > 127)
        return PyErr_Format(PyExc_ValueError, "Expected the ID to be between -128 and 127, but got an ID of %zi", long_id);
    
    char id = (char)long_id;

    // Remove the encode item
    if (PyDict_DelItem(ext->data.dict, type) < 0)
        PyErr_Clear(); // Silence the error if it doesn't exist, this function supports attempting to delete non-existent entries

    // Remove the decode function
    Py_XDECREF(ext->funcs[(unsigned char)id]);
    ext->funcs[(unsigned char)id] = NULL;
    
    Py_RETURN_NONE;
}

static PyObject *extensions_remove_encode(extensions_t *ext, PyObject *type)
{
    if (!PyType_Check(type))
        return PyErr_Format(PyExc_TypeError, "Expected argument 'type' to be a type object, but got an object of type '%s'", Py_TYPE(type)->tp_name);
    
    if (PyDict_DelItem(ext->data.dict, type) < 0)
        PyErr_Clear();
    
    Py_RETURN_NONE;
}

static PyObject *extensions_remove_decode(extensions_t *ext, PyObject *longobj)
{
    if (!PyLong_CheckExact(longobj))
        return error_unexpected_argtype("id", "int", Py_TYPE(longobj)->tp_name);
    
    long long_id = PyLong_AS_LONG(longobj);

    if (long_id < -128 || long_id > 127)
        return PyErr_Format(PyExc_ValueError, "Expected the ID to be between -128 and 127, but got an ID of %zi", long_id);
    
    char id = (char)long_id;

    Py_XDECREF(ext->funcs[(unsigned char)id]);
    ext->funcs[(unsigned char)id] = NULL;
    
    Py_RETURN_NONE;
}

static PyObject *extensions_clear(extensions_t *ext)
{
    // Decref the dict and all registered functions
    Py_DECREF(ext->data.dict);
    
    for (size_t i = 0; i < 256; ++i)
    {
        Py_XDECREF(ext->funcs[i]);
        ext->funcs[i] = NULL;
    }
    
    // Create a new dict object
    ext->data.dict = PyDict_New();

    if (!ext->data.dict)
        return PyErr_NoMemory();
    
    Py_RETURN_NONE;
}

// Create an ExtTypesEncode object
static PyObject *Extensions(PyObject *self, PyObject **args, Py_ssize_t nargs, PyObject *kwargs)
{
    mstates_t *states = get_mstates(self);

    PyObject *dict = NULL;
    PyObject *allow_subclasses = NULL;
    PyObject *pass_memview = NULL;

    keyarg_t keyargs[] = {
        KEYARG(&dict, &PyDict_Type, states->interned.types),
        KEYARG(&allow_subclasses, &PyBool_Type, states->interned.allow_subclasses),
        KEYARG(&pass_memview, &PyBool_Type, states->interned.pass_memoryview),
    };

    if (!parse_keywords(0, args, nargs, kwargs, keyargs, NKEYARGS(keyargs)))
        return NULL;


    // Create the ext object itself
    extensions_t *ext = PyObject_New(extensions_t, &ExtensionsObj);

    if (!ext)
        return PyErr_NoMemory();

    // Set the true/false values
    ext->data.pass_memview = pass_memview ? pass_memview == Py_True : EXTENSIONS_PASSMEMVIEW_DEFAULTVAL;

    // NULL-initialize the decoding functions array and set its pointer
    memset(ext->funcs, 0, sizeof(ext->funcs));
    ext->data.funcs = ext->funcs;

    // Create the dict holding the data
    ext->data.dict = PyDict_New();

    if (!ext->data.dict)
    {
        PyObject_Del(ext);
        return PyErr_NoMemory();
    }

    // If we received a dict, parse it
    if (dict && dict != Py_None)
    {
        Py_ssize_t pos = 0;
        PyObject *key, *val;
        while (PyDict_Next(dict, &pos, &key, &val))
        {
            if (!PyTuple_CheckExact(val))
            {
                Py_DECREF(ext);
                return PyErr_Format(PyExc_TypeError, "Expected dict values to be objects of type 'tuple', but got an object of type '%s'", Py_TYPE(val)->tp_name);
            }

            if (PyTuple_GET_SIZE(val) != 3)
            {
                Py_DECREF(ext);
                return PyErr_Format(PyExc_ValueError, "Expected dict values to be tuples with 3 items, but got one with %zi items", PyTuple_GET_SIZE(val));
            }

            PyObject *longobj = key;
            PyObject *type = PyTuple_GET_ITEM(val, 0);
            PyObject *encfunc = PyTuple_GET_ITEM(val, 1);
            PyObject *decfunc = PyTuple_GET_ITEM(val, 2);

            if (!PyLong_CheckExact(longobj))
            {
                Py_DECREF(ext);
                return PyErr_Format(PyExc_TypeError, "Expected dict keys to be of type 'int', but got an object of type '%s'", Py_TYPE(longobj)->tp_name);
            }

            long long_id = PyLong_AS_LONG(longobj);

            if (long_id < -128 || long_id > 127)
            {
                Py_DECREF(ext);
                return PyErr_Format(PyExc_ValueError, "Expected type IDs to be between -128 and 127, but got an ID of %zi", long_id);
            }

            char id = (char)long_id;

            if (!PyType_CheckExact(type) || !PyCallable_Check(encfunc) || !PyCallable_Check(decfunc))
            {
                Py_DECREF(ext);
                return PyErr_Format(PyExc_TypeError, "Expected dict tuples to hold a type object, a callable, and another callable (in respective order),"
                    "but got items of type '%s', '%s', and '%s'", Py_TYPE(type)->tp_name, Py_TYPE(encfunc)->tp_name, Py_TYPE(decfunc)->tp_name);
            }

            if (!extensions_add_encode_internal(ext, id, type, encfunc) ||
                !extensions_add_decode_internal(ext, id, decfunc))
            {
                Py_DECREF(ext);
                return NULL;
            }
        }
    }

    return (PyObject *)ext;
}

static void ExtDictItem_dealloc(ext_dictitem_t *item)
{
    Py_DECREF(item->func);
    PyObject_Del(item);
}

static void extensions_dealloc(extensions_t *ext)
{
    // Decref the dict and all registered functions
    Py_DECREF(ext->data.dict);
    
    for (size_t i = 0; i < 256; ++i)
        Py_XDECREF(ext->funcs[i]);

    PyObject_Del(ext);
}

static PyObject *extensions_get_passmemview(extensions_t *ext, void *closure)
{
    return ext->data.pass_memview == true ? Py_True : Py_False;
}

static PyObject *extensions_set_passmemview(extensions_t *ext, PyObject *arg, void *closure)
{
    ext->data.pass_memview = arg == Py_True;
    Py_RETURN_NONE;
}

// Attempt to encode an object as an ext type. Returns the object from the type's encode function
static _always_inline PyObject *attempt_encode_ext(buffer_t *b, PyObject *obj, char *id)
{
    // We're guaranteed to have an ExtTypesEncode object due to the global one

    // Attempt to get an item for all subclasses of OBJ, up until Type
    ext_dictitem_t *item = NULL;
    PyTypeObject *type = Py_TYPE(obj);
    while (type != &PyType_Type)
    {
        item = (ext_dictitem_t *)PyDict_GetItem(b->ext.dict, (PyObject *)type);

        if (item)
            break;
        
        type = Py_TYPE(type);
    }

    // If we didn't find an item, the object is of an invalid type
    if (!item)
        return PyErr_Format(PyExc_TypeError, "Received unsupported type '%s'"
            "\n\tHint: Did you mean to add this type to the Extension Types?", Py_TYPE(obj)->tp_name);
    
    *id = item->id;
    
    // Call the function for encoding data and return the result
    return PyObject_CallOneArg(item->func, obj);
}

// Attempt to decode an ext type object. Returns the object from the type's decode function
static _always_inline PyObject *attempt_decode_ext(buffer_t *b, char *buf, size_t size, char id)
{
    // We're guaranteed to have an ExtTypesDecode object due to the global one

    // See if there's a function for this ID
    PyObject *func = b->ext.funcs[(unsigned char)id];

    if (!func)
        return PyErr_Format(PyExc_TypeError, "Found an extension type with ID %i, but no function was registered for this ID"
            "\n\tHint: Did you forget to add a decoding function to the extension types, or is there a mismatch between IDs?", id);
    
    // Decide if we should return a bytes or memoryview object
    PyObject *bufobj;

    if (b->ext.pass_memview)
    {
        bufobj = PyMemoryView_FromMemory(buf, (Py_ssize_t)size, PyBUF_READ);
    }
    else
    {
        bufobj = PyBytes_FromStringAndSize(buf, (Py_ssize_t)size);
    }

    if (!bufobj)
        return PyErr_NoMemory();
    
    // Call the decode function
    PyObject *result = PyObject_CallOneArg(func, bufobj);

    Py_DECREF(bufobj);
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

            key = fixstr_to_py(b->offset, size, &b->states->caches.strings);
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

// Used for writing array types
static bool write_array(buffer_t *b, PyObject **items, size_t nitems)
{
    if (++(b->recursion) > RECURSION_LIMIT)
    {
        PyErr_SetString(PyExc_RecursionError, "Exceeded the maximum recursion depth");
        return false;
    }

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

static _always_inline bool write_dict(buffer_t *b, PyObject *obj)
{
    if (++(b->recursion) > RECURSION_LIMIT)
    {
        PyErr_SetString(PyExc_RecursionError, "Exceeded the maximum recursion depth");
        return false;
    }

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
    else if (npairs <= LIMIT_LARGE)
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
    // Attempt to encode the object as an extension type
    char id;
    PyObject *result = attempt_encode_ext(b, obj, &id);

    if (!result)
        return false;
    
    Py_buffer buf;
    if (PyObject_GetBuffer(result, &buf, PyBUF_SIMPLE) < 0)
    {
        PyErr_Format(PyExc_TypeError, "Expected to receive a bytes-like object from extension encode functions, but got an object of type '%s'", Py_TYPE(result)->tp_name);
        Py_DECREF(result);
        return false;
    }

    size_t size = (size_t)buf.len;

    ENSURE_SPACE(6 + size);

    // If the size is a base of 2 and not larger than 16, it can be represented with a fixsize mask
    const bool is_baseof2 = size != 0 && (size & (size - 1)) == 0;
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

    memcpy(b->offset, buf.buf, size);
    b->offset += size;

    PyBuffer_Release(&buf);
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
        if (b->file) \
        { \
            NULLCHECK(filestream_refresh_fbuf(b, to_add)); \
        } \
        else \
        { \
            PyErr_SetString(PyExc_ValueError, INVALID_MSG " (overread the encoded data)"); \
            return NULL; \
        } \
    } \
} while (0)

// Decoding of fixsize objects
static _always_inline PyObject *decode_bytes_fixsize(buffer_t *b, unsigned char mask)
{
    // Check which fixmask we got
    const unsigned char fixmask = FIXSIZE_DT(mask);
    if (fixmask == DT_STR_FIXED)
    {
        mask &= 0x1F;
        OVERREAD_CHECK(mask);

        PyObject *obj = fixstr_to_py(b->offset, mask, &b->states->caches.strings);
        b->offset += mask;

        return obj;
    }
    else if ((mask & 0x80) == DT_UINT_FIXED) // uint only has upper bit set to 0
    {
        return (PyObject *)&b->states->caches.integers[mask + INTEGER_CACHE_NNEG];
    }
    else if (fixmask == DT_INT_FIXED)
    {
        int8_t num = mask & 0b11111;

        if ((num & 0b10000) == 0b10000)
            num |= ~0b11111;

        return (PyObject *)&b->states->caches.integers[num + INTEGER_CACHE_NNEG];
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

        return (PyObject *)&b->states->caches.integers[num + INTEGER_CACHE_NNEG];
    }
    case VARLEN_DT(DT_UINT_BIT16):
    {
        OVERREAD_CHECK(2);

        uint16_t num = SIZEBYTE << 8;
        num |= SIZEBYTE;

        if (num <= 1023)
            return (PyObject *)&b->states->caches.integers[num + INTEGER_CACHE_NNEG];
        
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

        return (PyObject *)&b->states->caches.integers[num + 128];
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

        const char id = SIZEBYTE;

        OVERREAD_CHECK(n);

        PyObject *obj = attempt_decode_ext(b, b->offset, n, id);
        b->offset += n;

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
static _always_inline PyObject *encbuffer_start(PyObject *obj, ext_data_t ext, mstates_t *states, FILE *file)
{
    size_t nitems;
    buffer_t b;

    // Assign all standard fields
    b.ext = ext;
    b.states = states;
    b.file = file;
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
static _always_inline PyObject *decbuffer_start(PyObject *encoded, ext_data_t ext, mstates_t *states)
{
    buffer_t b;

    // Assign the standard data
    b.ext = ext;
    b.states = states;
    b.file = NULL; // File data doesn't go through here, so always NULL

    // Get a buffer of the encoded data buffer
    Py_buffer buf;
    if (PyObject_GetBuffer(encoded, &buf, PyBUF_SIMPLE) != 0)
    {
        PyErr_SetString(PyExc_BufferError, "Unable to open a buffer of the received encoded data.");
        return NULL;
    }
    
    b.offset = buf.buf;
    b.maxoffset = buf.buf + buf.len;

    PyObject *result = decode_bytes(&b);

    PyBuffer_Release(&buf);

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
    const Py_ssize_t npositional = 1;
    min_positional(nargs, npositional);

    PyObject *obj = parse_positional(args, 0, NULL, "obj");
    NULLCHECK(obj);

    mstates_t *states = get_mstates(self);
    extensions_t *ext = &states->extensions;

    keyarg_t keyargs[] = {
        KEYARG(&ext, &ExtensionsObj, states->interned.extensions),
    };

    if (!parse_keywords(npositional, args, nargs, kwargs, keyargs, NKEYARGS(keyargs)))
        return NULL;

    return encbuffer_start(obj, ext->data, states, NULL);
}

static PyObject *decode(PyObject *self, PyObject **args, Py_ssize_t nargs, PyObject *kwargs)
{
    const Py_ssize_t npositional = 1;
    min_positional(nargs, npositional);

    PyObject *encoded = parse_positional(args, 0, NULL, "encoded");
    NULLCHECK(encoded);

    mstates_t *states = get_mstates(self);
    extensions_t *ext = &states->extensions;

    keyarg_t keyargs[] = {
        KEYARG(&ext, &ExtensionsObj, states->interned.extensions),
    };

    if (!parse_keywords(npositional, args, nargs, kwargs, keyargs, NKEYARGS(keyargs)))
        return NULL;

    return decbuffer_start(encoded, ext->data, states);
}


///////////////////////
//  ENC/DEC OBJECTS  //
///////////////////////

// Initialize the file data of a stream object
static bool stream_initialize_fdata(stream_t *_stream, PyObject *filename)
{
    // Nothing to do if we don't have a filename, not an error case
    if (!filename)
        return true;
    
    filestream_t *stream = (filestream_t *)_stream;

    // Get the filename data if we got a filename object
    size_t fname_size;
    const char *fname = PyUnicode_AsUTF8AndSize(filename, (Py_ssize_t *)&fname_size);

    // Allocate the filename buffer
    stream->fname = (char *)malloc(fname_size + 1);

    if (!stream->fname)
        return PyErr_NoMemory();

    // Copy the filename into the object and NULL-terminate it
    memcpy(stream->fname, fname, fname_size);
    stream->fname[fname_size] = 0;

    // Allocate the file buffer for decoding
    stream->fbuf_size = FILEBUF_DEFAULTSIZE;
    stream->fbuf = (char *)malloc(stream->fbuf_size);

    if (!stream->fbuf)
    {
        free(stream->fname);
        return PyErr_NoMemory();
    }

    // Attempt to open the file
    stream->file = fopen(stream->fname, "a+b");

    if (!stream->file)
    {
        free(stream->fbuf);
        free(stream->fname);

        const int err = errno;
        return error_cannot_open_file(fname, err);
    }

    // Disable file buffering as we already read/write in a chunk-like manner
    setbuf(stream->file, NULL);

    #ifdef _need_threadsafe
    // Initialize the file lock
    atomic_flag_clear(&stream->flock);
    #endif

    return true;
}

static PyObject *Stream(PyObject *self, PyObject **args, Py_ssize_t nargs, PyObject *kwargs)
{
    mstates_t *states = get_mstates(self);

    PyObject *filename = NULL;
    extensions_t *ext = &states->extensions;

    keyarg_t keyargs[] = {
        KEYARG(&filename, &PyUnicode_Type, states->interned.file_name),
        KEYARG(&ext, &ExtensionsObj, states->interned.extensions),
    };

    NULLCHECK(parse_keywords(0, args, nargs, kwargs, keyargs, NKEYARGS(keyargs)))


    // Allocate the stream object based on if we got a file to use or not
    stream_t *stream;

    if (filename)
    {
        stream = PyObject_New(stream_t, &FileStreamObj);
    }
    else
    {
        stream = PyObject_New(stream_t, &StreamObj);
    }

    if (!stream)
        return PyErr_NoMemory();

    // Set the object fields
    stream->ext = ext;
    stream->states = states;

    // Set the file data if required
    if (!stream_initialize_fdata(stream, filename))
    {
        PyObject_Del(stream);
        return NULL;
    }

    // Keep a reference to the ext object
    Py_INCREF(ext);

    return (PyObject *)stream;
}

static void stream_dealloc(stream_t *stream)
{
    // Remove reference to the ext object
    Py_DECREF(stream->ext);
    PyObject_Del(stream);
}

static void filestream_dealloc(filestream_t *stream)
{
    // Check if we were using a file
    if (stream->file)
    {
        // Free the file buffer and file name, and close the file
        free(stream->fbuf);
        free(stream->fname);
        fclose(stream->file);
    }

    // Remove reference to the ext object
    Py_DECREF(stream->ext);

    PyObject_Del(stream);
}

static PyObject *stream_encode(stream_t *stream, PyObject *obj)
{
    return encbuffer_start(obj, stream->ext->data, stream->states, NULL);
}

static PyObject *stream_decode(stream_t *stream, PyObject *encoded)
{
    return decbuffer_start(encoded, stream->ext->data, stream->states);
}

#ifdef _need_threadsafe

// TODO
#define filestream_lock() (NULL)
#define filestream_unlock() (NULL)

#else

#define filestream_lock() (NULL)
#define filestream_unlock() (NULL)

#endif

static PyObject *filestream_encode(filestream_t *stream, PyObject *obj)
{
    // Encode the data and get the result
    PyObject *encoded = encbuffer_start(obj, stream->ext->data, stream->states, NULL);

    if (!encoded)
        return NULL;

    filestream_lock();

    // Get the buffer data
    size_t size = PyBytes_GET_SIZE(encoded);
    char *data = PyBytes_AS_STRING(encoded);

    // Write the encoded data to the file
    size_t written = fwrite(data, 1, size, stream->file);

    // Ensure we wrote all data
    if (written != size)
    {
        // Lose reference to the encoded object and set it to NULL to return NULL later
        Py_DECREF(encoded);
        encoded = NULL;

        // Get the errno
        const int err = errno;

        // Check if anything was written
        if (written == 0)
        {
            PyErr_Format(PyExc_OSError, "Attempted to write encoded data to file '%s', but no data was written. Errno %i: %s", stream->fname, err, strerror(err));
        }
        else
        {
            // Attempt to reverse partial writes
            // TODO
        }
    }

    filestream_unlock();

    return encoded;
}

static PyObject *filestream_decode(filestream_t *stream)
{
    // Set up the buffer object manually to have it use the file buffer
    buffer_t b;

    b.fbuf_size = stream->fbuf_size;
    b.base = stream->fbuf;
    b.offset = b.base;

    b.file = stream->file;
    b.ext = stream->ext->data;
    b.states = stream->states;

    filestream_lock();

    // Seek the file to the current offset
    fseek(b.file, stream->foff, SEEK_SET);

    // Read data from the file into the buffer
    size_t read = fread(b.base, 1, b.fbuf_size, b.file);

    // Set the max buffer offset based on how much data we read
    b.maxoffset = b.base + read;

    // Decode the data and get the result
    PyObject *result = decode_bytes(&b);

    // Calculate up to where we had to read from the file (up until the data of the next encoded data block)
    size_t end_offset = ftell(b.file);
    size_t buffer_unused = (size_t)(b.maxoffset - b.offset);
    stream->foff = end_offset - buffer_unused; // Update the reading offset

    // Update the file buffer size
    stream->fbuf_size = b.fbuf_size;

    filestream_unlock();

    return result;
}

// Refresh the file buffer's contents
static bool filestream_refresh_fbuf(buffer_t *b, size_t required)
{
    // Calculate how much from the buffer is unused
    size_t unused = (size_t)(b->maxoffset - b->offset);

    // Move the unused data to the start of the buffer
    memmove(b->base, b->offset, unused);

    // Check if the required size exceeds the buffer size
    if (required > b->fbuf_size)
    {
        size_t newsize = required * 1.2;
        char *fbuf = (char *)realloc(b->base, newsize);

        if (!fbuf)
            return PyErr_NoMemory();
        
        b->base = fbuf;
        b->fbuf_size = newsize;
    }

    // Read new data into the buffer above the unused data
    size_t read = fread(b->base + unused, 1, b->fbuf_size - unused, b->file);

    // Check if we have less data than required, meaning we reached EOF
    if (read + unused < required)
    {
        PyErr_SetString(PyExc_EOFError, "Reached EOF while decoding from a file before reaching end of data");
        return false;
    }

    // Update the offsets
    b->offset = b->base;
    b->maxoffset = b->base + unused + read;

    return true;
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

    /* INTERNED STRINGS */

    // Create interned strings
    GET_ISTR(obj)
    GET_ISTR(file_name)
    GET_ISTR(types)
    GET_ISTR(extensions)
    GET_ISTR(allow_subclasses)
    GET_ISTR(pass_memoryview)

    /* CACHES */

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
    PyLongObject *dummylong_zero = (PyLongObject *)PyLong_FromLong(0);

    if (!dummylong_pos || !dummylong_neg || !dummylong_zero)
        return PyErr_NoMemory();

    // Temporarily make them immortal
    Py_ssize_t dummylong_pos_refcnt = Py_REFCNT(dummylong_pos);
    Py_ssize_t dummylong_neg_refcnt = Py_REFCNT(dummylong_neg);
    Py_ssize_t dummylong_zero_refcnt = Py_REFCNT(dummylong_zero);
    Py_SET_REFCNT(dummylong_pos, _Py_IMMORTAL_REFCNT);
    Py_SET_REFCNT(dummylong_neg, _Py_IMMORTAL_REFCNT);
    Py_SET_REFCNT(dummylong_zero, _Py_IMMORTAL_REFCNT);
    
    // First store the negative values
    for (size_t i = 0; i < INTEGER_CACHE_NNEG; ++i)
    {
        s->caches.integers[i] = *dummylong_neg;

        digit val = INTEGER_CACHE_NNEG - i;

        // Negative values are stored as positive
        #if PYVER12
        s->caches.integers[i].long_value.ob_digit[0] = val;
        #else
        s->caches.integers[i].ob_digit[0] = val;
        #endif
    }

    // Store the zero object
    s->caches.integers[INTEGER_CACHE_NNEG] = *dummylong_zero;
    
    // Then store positive values
    for (size_t i = 1; i < INTEGER_CACHE_SLOTS - INTEGER_CACHE_NNEG; ++i)
    {
        size_t idx = i + INTEGER_CACHE_NNEG; // Skip over negatives and zero

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
    Py_SET_REFCNT(dummylong_zero, dummylong_zero_refcnt);

    Py_DECREF(dummylong_pos);
    Py_DECREF(dummylong_neg);
    Py_DECREF(dummylong_zero);

    /* GLOBAL EXTENSION OBJECTS */

    // Set the ext object type and make them immortal
    Py_SET_TYPE((PyObject *)&s->extensions, &ExtensionsObj);
    Py_SET_REFCNT((PyObject *)&s->extensions, _Py_IMMORTAL_REFCNT);

    // Create the dict object for the encoding extension types
    s->extensions.data.dict = PyDict_New();

    if (!s->extensions.data.dict)
        return PyErr_NoMemory();

    // NULL-initialize the decode functions
    memset(s->extensions.funcs, 0, sizeof(s->extensions.funcs));
    s->extensions.data.funcs = s->extensions.funcs;

    // Set default values
    s->extensions.data.pass_memview = EXTENSIONS_PASSMEMVIEW_DEFAULTVAL;

    // Add the global ext object to the module
    if (PyModule_AddObjectRef(m, "extensions", (PyObject *)&s->extensions) < 0)
        return false;

    return true;
}

// Clean up the module states
static void cleanup_mstates(PyObject *m)
{
    mstates_t *s = get_mstates(m);

    // Remove references from the string cache
    for (size_t i = 0; i < STRING_CACHE_SLOTS; ++i)
        Py_XDECREF(s->caches.strings.strings[i]);
    
    
    // Lose references to the global ext objects' dict and functions
    Py_DECREF(s->extensions.data.dict);

    for (size_t i = 0; i < 256; ++i)
        Py_XDECREF(s->extensions.funcs[i]);
    
}


/////////////////////
//   METHOD DEFS   //
/////////////////////

static PyMethodDef StreamMethods[] = {
    {"encode", (PyCFunction)stream_encode, METH_O, NULL},
    {"decode", (PyCFunction)stream_decode, METH_O, NULL},

    {NULL}
};

static PyMethodDef FileStreamMethods[] = {
    {"encode", (PyCFunction)filestream_encode, METH_O, NULL},
    {"decode", (PyCFunction)filestream_decode, METH_NOARGS, NULL},

    {NULL}
};

static PyMethodDef ExtensionsMethods[] = {
    {"add", (PyCFunction)extensions_add, METH_FASTCALL, NULL},
    {"add_encode", (PyCFunction)extensions_add_encode, METH_FASTCALL, NULL},
    {"add_decode", (PyCFunction)extensions_add_decode, METH_FASTCALL, NULL},

    {"remove", (PyCFunction)extensions_remove, METH_FASTCALL, NULL},
    {"remove_encode", (PyCFunction)extensions_remove_encode, METH_O, NULL},
    {"remove_decode", (PyCFunction)extensions_remove_decode, METH_O, NULL},

    {"clear", (PyCFunction)extensions_clear, METH_NOARGS, NULL},

    {NULL}
};

static PyGetSetDef ExtensionsGetSet[] = {
    {"pass_memoryview", (getter)extensions_get_passmemview, (setter)extensions_set_passmemview, NULL, NULL},

    {NULL}
};

static PyMethodDef CmsgpackMethods[] = {
    {"encode", (PyCFunction)encode, METH_FASTCALL | METH_KEYWORDS, NULL},
    {"decode", (PyCFunction)decode, METH_FASTCALL | METH_KEYWORDS, NULL},

    {"Extensions", (PyCFunction)Extensions, METH_FASTCALL | METH_KEYWORDS, NULL},

    {"Stream", (PyCFunction)Stream, METH_FASTCALL | METH_KEYWORDS, NULL},

    {NULL}
};


///////////////////
//    OBJECTS    //
///////////////////

static PyTypeObject StreamObj = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "cmsgpack.Stream",
    .tp_basicsize = sizeof(stream_t),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = StreamMethods,
    .tp_dealloc = (destructor)stream_dealloc,
};

static PyTypeObject FileStreamObj = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "cmsgpack.FileStream",
    .tp_basicsize = sizeof(filestream_t),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = FileStreamMethods,
    .tp_dealloc = (destructor)filestream_dealloc,
};

static PyTypeObject ExtDictItemObj = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "cmsgpack.ExtDictItem",
    .tp_basicsize = sizeof(ext_dictitem_t),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_dealloc = (destructor)ExtDictItem_dealloc,
};

static PyTypeObject ExtensionsObj = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "cmsgpack.Extensions",
    .tp_basicsize = sizeof(extensions_t),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = ExtensionsMethods,
    .tp_dealloc = (destructor)extensions_dealloc,
    .tp_getset = ExtensionsGetSet,
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
    PYTYPE_READY(StreamObj);
    PYTYPE_READY(FileStreamObj);

    PYTYPE_READY(ExtDictItemObj);
    PYTYPE_READY(ExtensionsObj);

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

