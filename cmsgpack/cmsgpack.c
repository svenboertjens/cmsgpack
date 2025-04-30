/* Licensed under the MIT License. */

#define PY_SSIZE_T_CLEAN

#include "masks.h"
#include "internals.h"

#include <Python.h>
#include <stdbool.h>

// Check if we need to consider thread-safety (for GIL-free Python)
#ifdef _need_threadsafe
    #include <stdatomic.h>
#endif


///////////////////
//   CONSTANTS   //
///////////////////

// Default file buffer size
#define FILEBUF_DEFAULTSIZE 8192 // 8 KB

// The number of slots to use in caches
#define STRING_CACHE_SLOTS 1024
#define INTEGER_CACHE_SLOTS (1024 + 128) // 1023 positive, 128 negative, and a zero
#define INTEGER_CACHE_NNEG 128 // Number of negative slots in the integer cache

// Minimum sizes for the 'extra' and 'item' adaptive allocation weights
#define EXTRA_ALLOC_MIN 64
#define ITEM_ALLOC_MIN 6

// Recursion limit
#define RECURSION_LIMIT 1000

// Immortal refcount value
#define IMMORTAL_REFCNT _Py_IMMORTAL_REFCNT

// Whether the Python version is 3.13+
#define PYVER13 (PY_VERSION_HEX >= 0x030D0000)

///////////////////////////
//  TYPEDEFS & FORWARDS  //
///////////////////////////

// Dict item for extensions
typedef struct {
    PyObject_HEAD
    char id;        // ID of the type
    PyObject *func; // The function used for encoding objects of the type set as the dict's key
} ext_dictitem_t;

// Extension data for during serialization
typedef struct {
    bool pass_memview; // Whether to pass memoryview objects instead of byte objects to decoding functions

    PyObject *dict;   // Dict object containing encoding data
    PyObject **funcs; // Functions array with decoding functions, indexed by ID casted to unsigned
} ext_data_t;

// Extensions object
typedef struct {
    PyObject_HEAD
    ext_data_t data;
    PyObject *funcs[256]; // The functions array, inlined to avoid having to allocate it
} extensions_t;

static PyTypeObject ExtDictItemObj;
static PyTypeObject ExtensionsObj;


// Keyarg struct for parsing keyword arguments
typedef struct {
    PyObject **dest;
    PyTypeObject *tp;
    PyObject *interned;
} keyarg_t;


// String cache struct
typedef struct {
    PyASCIIObject *slots[STRING_CACHE_SLOTS];
    uint8_t match_strength[STRING_CACHE_SLOTS];

    // One lock for each slot
    #ifdef _need_threadsafe
    atomic_flag locks[STRING_CACHE_SLOTS];
    #endif
} strcache_t;

// Integer cache struct
typedef struct {
    // No locks, these objects aren't modified after setup
    PyLongObject slots[INTEGER_CACHE_SLOTS];
} intcache_t;

// Module states
typedef struct {
    // Interned strings
    struct {
        PyObject *file_name;
        PyObject *types;
        PyObject *extensions;
        PyObject *allow_subclasses;
        PyObject *pass_memoryview;
        PyObject *str_keys;
        PyObject *reading_offset;
        PyObject *chunk_size;
    } interned;

    // Caches
    struct {
        intcache_t integers;
        strcache_t strings;
    } caches;

    // The global extensions object
    extensions_t extensions;
} mstates_t;


// File data
typedef struct {
    FILE *file; // File object being used
    char *name; // Name of the file
} filedata_t;

typedef struct {
    char *base;        // Base towards the buffer (for decoding from a file, this holds the file buffer)
    char *offset;      // Current writing offset in the buffer
    char *maxoffset;   // The max writing offset

    bool str_keys;     // Whether only string keys are allowed
    ext_data_t ext;    // Extensions data
    size_t recursion;  // Recursion depth to prevent cyclic references during encoding
    mstates_t *states; // The module states

    FILE *file;        // The file object in use, NULL if not using a file
    size_t fbuf_size;  // The actual size of the file buffer, for if it's updated
} buffer_t;


typedef struct {
    PyObject_HEAD

    bool str_keys;     // Whether to allow string keys
    PyObject *ext;     // The extensions object to use
    mstates_t *states; // The module states
} stream_t;

typedef struct {
    PyObject_HEAD

    #ifdef _need_threadsafe
    atomic_flag lock; // Object lock for multithreaded environments
    #endif

    bool str_keys;     // Whether to allow string keys
    PyObject *ext;     // The extensions object to use
    mstates_t *states; // The module states

    FILE *file;  // The file, opened in "a+b" mode
    size_t foff; // The file's reading offset

    char *fbuf;       // The file buffer for decoding
    size_t fbuf_size; // The size of the file buffer

    char *fname;      // The filename
} filestream_t;

static PyTypeObject StreamObj;
static PyTypeObject FileStreamObj;


static _always_inline mstates_t *get_mstates(PyObject *m);

static _always_inline bool encode_object_inline(buffer_t *b, PyObject *obj);
static PyObject *decode_bytes(buffer_t *b);

static _always_inline bool ensure_space(buffer_t *b, size_t required);
static _always_inline bool overread_check(buffer_t *b, size_t required);


static PyModuleDef cmsgpack;


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


////////////////////
//  ARGS PARSING  //
////////////////////

/* # How to use argument parsing tools
 * 
 * Positional-only arguments:
 * - Use `min_positional` to check if we got the minimum required number of arguments.
 * - Use `parse_positional` for each positional argument, with `nth` as the argument position.
 * 
 * Keyword arguments:
 * - Use the `keyarg_t` struct for declaring all keyword arguments. Declare this as a `keyarg_t[]` array.
 * - Define a keyarg in the keyargs array using `KEYARG()` and fill in the fields.
 * - Use `parse_keywords` for parsing keyword arguments, where:
 *       The `npos` argument should be 0 if there are no positional-only keywords, or else the number of positional-only keywords;
 *       The `nkey` argument should be the number of keyword arguments. For simplicity, use `NKEYARGS(keyarg_array)`.
 * 
 */

// Unicode object compare macros
#if PYVER13
    #define _unicode_equal(x, y) (PyUnicode_Compare(x, y) == 0)
#else
    #define _unicode_equal(x, y) _PyUnicode_EQ(x, y)
#endif

/* Declare a keyarg slot for in a keyargs array.
 * `dest` must be a pointer to the variable that will hold the argument's value (not set to NULL if argument wasn't passed),
 * `type` is the type that the argument's value should have (can be NULL),
 * `interned` is the interned string object of the argument's keyword name.
 */
#define KEYARG(_dest, _type, _interned) \
    {.dest = (PyObject **)_dest, .tp = (PyTypeObject *)_type, .interned = (PyObject *)_interned}

// Get the number of keyargs in a keyarg array, to pass as `nkey`
#define NKEYARGS(keyargs) \
    (sizeof(keyargs) / sizeof(keyargs[0]))

// Check if there's a minimum of MIN arguments
static _always_inline bool min_positional(Py_ssize_t nargs, Py_ssize_t min)
{
    if (nargs < min)
    {
        PyErr_Format(PyExc_TypeError, "Expected at least %zi positional arguments, but received only %zi", min, nargs);
        return false;
    }

    return true;
}

// Parse positional arguments
static _always_inline PyObject *parse_positional(PyObject **args, size_t nth, PyTypeObject *type, const char *argname)
{
    PyObject *obj = args[nth];

    // Check if we have a type to check for, and if so, check if it matches the object
    if (type && !Py_IS_TYPE(obj, type))
    {
        error_unexpected_argtype(argname, type->tp_name, Py_TYPE(obj)->tp_name);
        return NULL;
    }

    return obj;
}

// Parse a single keyword argument
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

// Parse keyword arguments
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
            error_unexpected_argtype(PyUnicode_AsUTF8(keyarg.interned), keyarg.tp->tp_name, Py_TYPE(obj)->tp_name);
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
    ext->data.pass_memview = pass_memview == Py_True;

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


////////////////////
//  WRITING DATA  //
////////////////////

// Get the current offset's byte and increment afterwards
#define INCBYTE ((b->offset++)[0])

// Write a header's typemask and its size based on the number of bytes the size should take up.
// NBYTES can be 0 for FIXSIZE, 1 for SMALL, 2 for MEDIUM, 4 for LARGE, and 8 for 64-bit int/uint cases
static _always_inline void write_mask(buffer_t *b, const unsigned char mask, const size_t size, const size_t nbytes)
{
    if (nbytes == 0) // FIXSIZE
    {
        INCBYTE = mask | size;
    }
    else if (nbytes == 1) // SMALL
    {
        const char head[2] = {
            mask,
            size
        };

        memcpy(b->offset, head, 2);
        b->offset += 2;
    }
    else if (nbytes == 2) // MEDIUM
    {
        const char head[3] = {
            mask,
            size >> 8,
            size
        };

        memcpy(b->offset, head, 3);
        b->offset += 3;
    }
    else if (nbytes == 4) // LARGE
    {
        const char head[5] = {
            mask,
            size >> 24,
            size >> 16,
            size >> 8,
            size,
        };

        memcpy(b->offset, head, 5);
        b->offset += 5;
    }
    else if (nbytes == 8)
    {
        const char head[9] = {
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

        memcpy(b->offset, head, 9);
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
    char *base;
    size_t size;
    py_str_data(obj, &base, &size);

    if (base == NULL)
        return false;

    if (!ensure_space(b, size + 5))
        return false;
    
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

static _always_inline bool write_binary(buffer_t *b, char *base, size_t size)
{
    if (!ensure_space(b, size + 5))
        return false;

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
    // No ensure_space, already done globally

    double num = PyFloat_AS_DOUBLE(obj);

    // Ensure big-endianness
    BIG_DOUBLE(num);

    INCBYTE = DT_FLOAT_BIT64;
    memcpy(b->offset, &num, 8);

    b->offset += 8;

    return true;
}

static _always_inline bool write_integer(buffer_t *b, PyObject *obj)
{
    // No ensure_space, already done globally

    // Cast the object to a LongObject for easy internals access
    PyLongObject *lobj = (PyLongObject *)obj;

    // Get the long_value fields for easy access
    digit *digits = lobj->long_value.ob_digit;
    uintptr_t tag = lobj->long_value.lv_tag;

    // Get the number of digits and whether the number is positive
    size_t ndigits = tag >> _PyLong_NON_SIZE_BITS;
    bool positive = (tag & 0b11) != 0b10; // Check if the number is positive (or zero)

    // Start the number off with the first digit
    uint64_t num = digits[0];

    // Iterate over the digits besides the first digit, using `ndigits` as the index to `digits`
    while (ndigits > 1)
    {
        // Decrement to get it as index, working from top to bottom
        --ndigits;

        uint64_t dig = digits[ndigits];

        // Calculate the shift amount for this digit
        int shift = PyLong_SHIFT * ndigits;

        // Shift the digit in place
        uint64_t shifted = dig << shift;

        // Check for overflow
        if ((shifted >> shift) != dig)
        {
            overflow_case: // Jump label for the extra 64-bit negative check

            PyErr_SetString(PyExc_OverflowError, "Integer values cannot exceed `2^64-1` or `-2^63` (must be within the 64-bit boundary)");
            return false;
        }

        // Add the shifted digit to the total number
        num |= shifted;
    }

    if (positive)
    {
        if (num <= LIMIT_UINT_FIXED)
        {
            write_mask(b, DT_UINT_FIXED, num, 0);
        }
        else if (num <= LIMIT_SMALL)
        {
            write_mask(b, DT_UINT_BIT8, num, 1);
        }
        else if (num <= LIMIT_MEDIUM)
        {
            write_mask(b, DT_UINT_BIT16, num, 2);
        }
        else if (num <= LIMIT_LARGE)
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
        // Get the number as signed and negative
        int64_t snum = -num;

        // Test if the limit was exceeded by checking if the negative number shows up as positive
        if (snum >= 0)
            goto overflow_case;

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
        else if (snum >= LIMIT_INT_BIT32)
        {
            write_mask(b, DT_INT_BIT32, snum, 4);
        }
        else
        {
            write_mask(b, DT_INT_BIT64, snum, 8);
        }
    }

    return true;
}

static _always_inline bool write_bool(buffer_t *b, PyObject *obj)
{
    // No ensure_space, already done globally

    INCBYTE = obj == Py_True ? DT_TRUE : DT_FALSE;
    return true;
}

static _always_inline bool write_nil(buffer_t *b, PyObject *obj)
{
    // No ensure_space, already done globally

    INCBYTE = DT_NIL;
    return true;
}

// Recursion check for container types
static _always_inline bool recursion_check(buffer_t *b)
{
    if (b->recursion > RECURSION_LIMIT)
    {
        PyErr_SetString(PyExc_RecursionError, "Exceeded the maximum recursion depth");
        return false;
    }

    return true;
}

static _always_inline bool write_array_header(buffer_t *b, size_t nitems)
{
    // No ensure_space, already done globally

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

static _always_inline bool write_list(buffer_t *b, PyObject *obj)
{
    // No ensure_space, already done globally

    b->recursion++;

    if (!recursion_check(b))
        return false;

    size_t nitems = PyList_GET_SIZE(obj);

    if (!write_array_header(b, nitems))
        return false;
    
    for (size_t i = 0; i < nitems; ++i)
    {
        PyObject *item = PyList_GET_ITEM(obj, i);
        
        if (!encode_object_inline(b, item))
            return false;
    }

    b->recursion--;

    return true;
}

static _always_inline bool write_tuple(buffer_t *b, PyObject *obj)
{
    // No ensure_space, already done globally

    b->recursion++;

    if (!recursion_check(b))
        return false;

    size_t nitems = PyTuple_GET_SIZE(obj);

    if (!write_array_header(b, nitems))
        return false;
    
    for (size_t i = 0; i < nitems; ++i)
    {
        PyObject *item = PyTuple_GET_ITEM(obj, i);
        
        if (!encode_object_inline(b, item))
            return false;
    }

    b->recursion--;

    return true;
}

static _always_inline bool write_dict(buffer_t *b, PyObject *obj)
{
    // No ensure_space, already done globally

    b->recursion++;

    if (!recursion_check(b))
        return false;

    const size_t npairs = PyDict_GET_SIZE(obj);

    // No ensure_space, already done globally
    
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

        if (!b->str_keys)
        {
            if (!encode_object_inline(b, key))
                return false;
        }
        else if (PyUnicode_CheckExact(key))
        {
            if (!write_string(b, obj))
                return false;
        }
        else
        {
            PyErr_Format(PyExc_TypeError, "Got a map key of type '%s' while only string keys were allowed", Py_TYPE(key)->tp_name);
            return false;
        }

        if (!encode_object_inline(b, val))
            return false;
    }

    b->recursion--;

    return true;
}

static _always_inline bool write_extension(buffer_t *b, PyObject *obj)
{
    // Attempt to encode the object as an extension type
    char id = 0;
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

    if (!ensure_space(b, 6 + size))
        return false;

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


/////////////////////
//  OBJECT CACHES  //
/////////////////////

static _always_inline uint32_t fnv1a_32(const char *data, size_t size)
{
    uint32_t hash = 0x811c9dc5;

    for (size_t i = 0; i < size; ++i)
    {
        hash ^= data[i];
        hash *= 0x01000193;
    }

    return hash;
}

// Create a string (<32 bytes) and attempt to get it from cache
static _always_inline PyObject *get_cached_str(buffer_t *b, size_t size)
{
    // Pointer to the string is at the current offset
    const char *ptr = b->offset;

    // Get the string cache
    strcache_t *cache = &b->states->caches.strings;

    // Calculate the hash of the string
    const size_t hash = fnv1a_32(ptr, size) & (STRING_CACHE_SLOTS - 1);

    // Lock the cache slot
    lock_flag(&cache->locks[hash]);

    // Load the match stored at the hash index
    PyASCIIObject *match = cache->slots[hash];

    if (match)
    {
        // Get the match's base and size
        const char *mbase = (const char *)(match + 1);
        const size_t msize = match->length;

        // Compare the match to the required string
        if (msize == size && memcmp_small(mbase, ptr, size))
        {
            // Get reference to the match object
            Py_INCREF(match);

            // Increase match strength as we matched again
            cache->match_strength[hash]++;

            // Release the lock
            unlock_flag(&cache->locks[hash]);

            return (PyObject *)match;
        }
    }

    // No common value, create a new one
    PyObject *obj = PyUnicode_DecodeUTF8(ptr, size, NULL);

    // If ASCII, it can be cached
    if (PyUnicode_IS_COMPACT_ASCII(obj))
    {
        // Decrease match strength as a match failed
        cache->match_strength[hash]--;

        // Replace the match if the strength reached zero
        if (cache->match_strength[hash] == 0)
        {
            // Lose reference to the match and keep reference to the new object
            Py_XDECREF(match);
            Py_INCREF(obj);

            // Assign the object to the cache and initialize match strength at 3
            cache->slots[hash] = (PyASCIIObject *)obj;
            cache->match_strength[hash] = 3;
        }
    }

    // Release the lock
    unlock_flag(&cache->locks[hash]);

    return obj;
}

// Get a cached integer, must be between -128 and 1023
static _always_inline PyObject *get_cached_int(buffer_t *b, int n)
{
    return (PyObject *)&b->states->caches.integers.slots[n + INTEGER_CACHE_NNEG];
}

/////////////////////////
//  CREATE CONTAINERS  //
/////////////////////////

static _always_inline PyObject *create_array(buffer_t *b, const size_t nitems)
{
    PyObject *list = PyList_New(nitems);

    if (!list)
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

static _always_inline PyObject *create_map(buffer_t *b, const size_t npairs)
{
    PyObject *dict = _PyDict_NewPresized(npairs);

    if (!dict)
        return PyErr_NoMemory();

    for (size_t i = 0; i < npairs; ++i)
    {
        PyObject *key;

        if (!overread_check(b, 1))
        {
            Py_DECREF(dict);
            return NULL;
        }

        // Special case for fixsize strings
        const unsigned char mask = b->offset[0];
        if ((mask & 0b11100000) == DT_STR_FIXED)
        {
            b->offset++;

            const size_t size = mask & 0b11111;

            key = get_cached_str(b, size);
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

            if (b->str_keys && !PyUnicode_CheckExact(key))
            {
                Py_DECREF(dict);
                Py_DECREF(key);

                PyErr_Format(PyExc_TypeError, "Got a map key of type '%s' while only string keys were allowed", Py_TYPE(key)->tp_name);
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
//    ENCODING    //
////////////////////

static bool encode_object(buffer_t *b, PyObject *obj, PyTypeObject *tp)
{
    if (tp == &PyList_Type || PyList_Check(obj))
    {
        return write_list(b, obj);
    }
    else if (tp == &PyDict_Type || PyDict_Check(obj))
    {
        return write_dict(b, obj);
    }
    else if (tp == &PyTuple_Type || PyTuple_Check(obj))
    {
        return write_tuple(b, obj);
    }
    else if (PyBytes_Check(obj))
    {
        return write_bytes(b, obj);
    }
    else if (PyByteArray_Check(obj))
    {
        return write_bytearray(b, obj);
    }
    else if (PyMemoryView_Check(obj))
    {
        return write_memoryview(b, obj);
    }
    else
    {
        return write_extension(b, obj);
    }
}

static _always_inline bool encode_object_inline(buffer_t *b, PyObject *obj)
{
    PyTypeObject *tp = Py_TYPE(obj);

    if (tp == &PyUnicode_Type)
    {
        return write_string(b, obj);
    }

    // A lot of cases below require 9 or less space, ensure globally for all of them here
    if (!ensure_space(b, 9))
        return false;

    if (tp == &PyLong_Type)
    {
        return write_integer(b, obj);
    }
    else if (tp == &PyFloat_Type)
    {
        return write_double(b, obj);
    }
    else if (tp == &PyBool_Type)
    {
        return write_bool(b, obj);
    }
    else if (obj == Py_None)
    {
        return write_nil(b, obj);
    }
    else
    {
        return encode_object(b, obj, tp);
    }
}


////////////////////
//    DECODING    //
////////////////////

// INCBYTE but type-casted to ensure we operate with a single byte
#define SIZEBYTE ((unsigned char)(b->offset++)[0])

// Masks away the top 3 bits of a mask for varlen cases, as they all have the same upper 3 bits
#define VARLEN_DT(mask) (mask & 0b11111)

// Decoding of fixsize objects
static _always_inline PyObject *decode_bytes_fixsize(buffer_t *b, unsigned char mask)
{
    // Check which type we got
    if ((mask & 0b11100000) == DT_STR_FIXED)
    {
        mask &= 0x1F;

        if (!overread_check(b, mask))
            return NULL;

        PyObject *obj = get_cached_str(b, mask);
        b->offset += mask;

        return obj;
    }
    else if ((mask & 0x80) == DT_UINT_FIXED) // uint only has upper bit set to 0
    {
        return get_cached_int(b, mask);
    }
    else if ((mask & 0b11100000) == DT_INT_FIXED)
    {
        int8_t num = mask & 0b11111;

        if ((num & 0b10000) == 0b10000)
            num |= ~0b11111;

        return get_cached_int(b, num);
    }
    else if ((mask & 0b11110000) == DT_ARR_FIXED) // Bit 5 is also set on ARR and MAP
    {
        return create_array(b, mask & 0x0F);
    }
    else if ((mask & 0b11110000) == DT_MAP_FIXED)
    {
        return create_map(b, mask & 0x0F);
    }
    else
    {
        return PyErr_Format(PyExc_ValueError, "Got an invalid header (0x%02X) while decoding data", mask);
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
        if (!overread_check(b, 4))
            return NULL;

        n |= SIZEBYTE << 24;
        n |= SIZEBYTE << 16;
    }
    case VARLEN_DT(DT_STR_MEDIUM):
    {
        if (!overread_check(b, 2))
            return NULL;

        n |= SIZEBYTE << 8;
    }
    case VARLEN_DT(DT_STR_SMALL):
    {
        if (!overread_check(b, 1))
            return NULL;

        n |= SIZEBYTE;

        if (!overread_check(b, n))
            return NULL;

        PyObject *obj = PyUnicode_DecodeUTF8(b->offset, n, NULL);
        b->offset += n;

        return obj;
    }

    case VARLEN_DT(DT_UINT_BIT8):
    {
        if (!overread_check(b, 1))
            return NULL;

        uint8_t num = SIZEBYTE;

        return get_cached_int(b, num);
    }
    case VARLEN_DT(DT_UINT_BIT16):
    {
        if (!overread_check(b, 2))
            return NULL;

        uint16_t num = SIZEBYTE << 8;
        num |= SIZEBYTE;

        if (num <= 1023)
            return get_cached_int(b, num);
        
        return PyLong_FromUnsignedLongLong(num);
    }
    case VARLEN_DT(DT_UINT_BIT32):
    {
        if (!overread_check(b, 4))
            return NULL;

        uint32_t num;
        memcpy(&num, b->offset, 4);
        b->offset += 4;

        num = BIG_32(num);
        
        return PyLong_FromUnsignedLongLong(num);
    }
    case VARLEN_DT(DT_UINT_BIT64):
    {
        if (!overread_check(b, 8))
            return NULL;

        uint64_t num;
        memcpy(&num, b->offset, 8);
        b->offset += 8;

        num = BIG_64(num);
        
        return PyLong_FromUnsignedLongLong(num);
    }

    case VARLEN_DT(DT_INT_BIT8):
    {
        if (!overread_check(b, 1))
            return NULL;

        int8_t num = SIZEBYTE;

        return get_cached_int(b, num);
    }
    case VARLEN_DT(DT_INT_BIT16):
    {
        if (!overread_check(b, 2))
            return NULL;

        int16_t num = (int16_t)SIZEBYTE << 8;
        num |= SIZEBYTE;
        
        return PyLong_FromLongLong(num);
    }
    case VARLEN_DT(DT_INT_BIT32):
    {
        if (!overread_check(b, 4))
            return NULL;

        int32_t num;
        memcpy(&num, b->offset, 4);
        b->offset += 4;

        num = BIG_32(num);
        
        return PyLong_FromLongLong(num);
    }
    case VARLEN_DT(DT_INT_BIT64):
    {
        if (!overread_check(b, 8))
            return NULL;

        int64_t num;
        memcpy(&num, b->offset, 8);
        b->offset += 8;

        num = BIG_64(num);

        return PyLong_FromLongLong(num);
    }

    case VARLEN_DT(DT_ARR_LARGE):
    {
        if (!overread_check(b, 4))
            return NULL;
        
        n |= SIZEBYTE << 24;
        n |= SIZEBYTE << 16;
    }
    case VARLEN_DT(DT_ARR_MEDIUM):
    {
        if (!overread_check(b, 2))
            return NULL;
        
        n |= SIZEBYTE << 8;
        n |= SIZEBYTE;

        return create_array(b, n);
    }

    case VARLEN_DT(DT_MAP_LARGE):
    {
        if (!overread_check(b, 4))
            return NULL;
        
        n |= SIZEBYTE << 24;
        n |= SIZEBYTE << 16;
    }
    case VARLEN_DT(DT_MAP_MEDIUM):
    {
        if (!overread_check(b, 2))
            return NULL;

        n |= SIZEBYTE << 8;
        n |= SIZEBYTE;

        return create_map(b, n);
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
        if (!overread_check(b, 4))
            return NULL;

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
        if (!overread_check(b, 8))
            return NULL;

        double num;
        memcpy(&num, b->offset, 8);
        b->offset += 8;

        BIG_DOUBLE(num);

        return PyFloat_FromDouble(num);
    }

    case VARLEN_DT(DT_BIN_LARGE):
    {
        if (!overread_check(b, 4))
            return NULL;

        n |= SIZEBYTE << 24;
        n |= SIZEBYTE << 16;
    }
    case VARLEN_DT(DT_BIN_MEDIUM):
    {
        if (!overread_check(b, 2))
            return NULL;

        n |= SIZEBYTE << 8;
    }
    case VARLEN_DT(DT_BIN_SMALL):
    {
        if (!overread_check(b, 1))
            return NULL;

        n |= SIZEBYTE;

        if (!overread_check(b, n))
            return NULL;

        PyObject *obj = PyBytes_FromStringAndSize(b->offset, n);
        b->offset += n;

        return obj;
    }

    case VARLEN_DT(DT_EXT_FIX1):
    {
        n = 1;
        goto ext_handling;
    }
    case VARLEN_DT(DT_EXT_FIX2):
    {
        n = 2;
        goto ext_handling;
    }
    case VARLEN_DT(DT_EXT_FIX4):
    {
        n = 4;
        goto ext_handling;
    }
    case VARLEN_DT(DT_EXT_FIX8):
    {
        n = 8;
        goto ext_handling;
    }
    case VARLEN_DT(DT_EXT_FIX16):
    {
        n = 16;
        goto ext_handling;
    }

    case VARLEN_DT(DT_EXT_LARGE):
    {
        if (!overread_check(b, 4))
            return NULL;
            
        n |= SIZEBYTE << 24;
        n |= SIZEBYTE << 16;
    }
    case VARLEN_DT(DT_EXT_MEDIUM):
    {
        if (!overread_check(b, 2))
            return NULL;

        n |= SIZEBYTE << 8;
    }
    case VARLEN_DT(DT_EXT_SMALL):
    {
        if (!overread_check(b, 1))
            return NULL;

        n |= SIZEBYTE;

        ext_handling: // Jump label for fixsize cases
        NULL; // No-op statement for pre-C23 standards (declarations not allowed after labels)

        if (!overread_check(b, 1 + n))
            return NULL;

        const char id = SIZEBYTE;

        PyObject *obj = attempt_decode_ext(b, b->offset, n, id);
        b->offset += n;

        if (obj == NULL)
        {
            if (!PyErr_Occurred())
                PyErr_Format(PyExc_ValueError, "Failed to match an extension type with ID %i", id);
            
            return NULL;
        }

        return obj;
    }

    default:
    {
        return PyErr_Format(PyExc_ValueError, "Got an invalid header (0x%02X) while decoding data", mask);
    }
    }
}

static PyObject *decode_bytes(buffer_t *b)
{
    // Check if it's safe to read the mask
    if (!overread_check(b, 1))
            return NULL;

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


///////////////////////////
//  ADAPTIVE ALLOCATION  //
///////////////////////////

// Use thread-local variables instead of atomic variables for thread-safety and
// to keep the averages more local, and to prevent counterintuitive scaling
_Thread_local size_t extra_avg = (EXTRA_ALLOC_MIN * 2);
_Thread_local size_t item_avg = (ITEM_ALLOC_MIN * 2);

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


/////////////////////
//  ENC/DEC START  //
/////////////////////

static _always_inline PyObject *encoding_write_file(buffer_t *b, filestream_t *fstream, size_t datasize)
{
    lock_flag(&fstream->lock);

    // Write the data to the file
    size_t written = fwrite(PyBytes_AS_STRING(b->base), 1, datasize, fstream->file);

    // Remove reference to the bytes object as we won't return it
    Py_DECREF(b->base);

    // Check if all data was written
    if (written != datasize)
    {
        // Get the errno
        const int err = errno;

        // Check if anything was written and otherwise attempt to truncate the file
        size_t start_offset = ftell(fstream->file) - written; // The offset before the written data
        if (written == 0 || _ftruncate(fstream->file, start_offset))
        {
            PyErr_Format(PyExc_OSError, "Attempted to write encoded data, but no data was written."
                "\n\tErrno %i: %s", err, strerror(err));
        }
        else
        {
            PyErr_Format(PyExc_OSError, "Attempted to write encoded data, but the write could not be completed and truncation failed. "
                "Incomplete data was written on position %zu, and %zu bytes were written."
                "\n\tErrno %i: %s", start_offset, written, err, strerror(err));
        }

        unlock_flag(&fstream->lock);

        return NULL;
    }

    unlock_flag(&fstream->lock);

    Py_RETURN_NONE;
}

static _always_inline PyObject *encoding_start(PyObject *obj, mstates_t *states, PyObject *ext, bool str_keys, filestream_t *fstream)
{
    buffer_t b;

    // Assign non-buffer fields (not filedata, that's not used for encoding)
    b.ext = ((extensions_t *)ext)->data;
    b.str_keys = str_keys;
    b.states = states;
    b.recursion = 0;

    // Estimate how much to allocate for the encoding buffer
    size_t buffersize = extra_avg;
    size_t nitems = 0;

    // Check if the object is a list/tuple/dict for adaptive allocation
    if (PyList_CheckExact(obj) || PyTuple_CheckExact(obj) || PyDict_CheckExact(obj))
    {
        nitems = Py_SIZE(obj);
        buffersize += item_avg * nitems;
    }
    
    // Allocate the buffer
    b.base = (char *)PyBytes_FromStringAndSize(NULL, buffersize);

    if (!b.base)
        return PyErr_NoMemory();
    
    // Set the offset and max offset
    b.offset = PyBytes_AS_STRING(b.base);
    b.maxoffset = b.offset + buffersize;

    // Attempt to encode the object
    if (!encode_object_inline(&b, obj))
    {
        Py_DECREF(b.base);
        return NULL;
    }
    
    // Calculate the size of the encoded data
    size_t datasize = (size_t)(b.offset - PyBytes_AS_STRING(b.base));
    Py_SET_SIZE(b.base, datasize);

    // Update the adaptive allocation
    update_adaptive_allocation(&b, nitems);

    // If not streaming, just return the object
    if (!fstream)
        return (PyObject *)b.base;

    // Otherwise, write the data to the file
    return encoding_write_file(&b, fstream, datasize);
}

// Start a decoding run
static _always_inline PyObject *decoding_start(PyObject *encoded, mstates_t *states, PyObject *ext, bool str_keys, filestream_t *fstream)
{
    buffer_t b;

    // Assign non-buffer fields
    b.ext = ((extensions_t *)ext)->data;
    b.str_keys = str_keys;
    b.states = states;

    // Simply decode and return if not file streaming
    if (!fstream)
    {
        // Set the file to NULL for overread checks
        b.file = NULL;

        // Get the buffer of the object
        Py_buffer buf;
        if (PyObject_GetBuffer(encoded, &buf, PyBUF_SIMPLE) < 0)
            return NULL;
        
        // Set the buffer fields (base not used for regular decoding)
        b.offset = buf.buf;
        b.maxoffset = buf.buf + buf.len;
        
        // Decode the data
        PyObject *result = decode_bytes(&b);

        // Release the buffer
        PyBuffer_Release(&buf);

        // Check if we reached the end of the buffer
        if (result != NULL && b.offset != b.maxoffset)
        {
            PyErr_SetString(PyExc_ValueError, "The encoded data pattern ended before the buffer ended");
            return NULL;
        }

        return result;
    }

    // This path is reached when file streaming

    // Assign the file-related fields (maxoffset is assigned based on how many bytes are read later)
    b.fbuf_size = fstream->fbuf_size;
    b.file = fstream->file;
    b.base = fstream->fbuf;
    b.offset = b.base;
    
    // Seek the file to the current offset
    fseek(b.file, fstream->foff, SEEK_SET);

    // Read data from the file into the buffer
    size_t read = fread(b.base, 1, b.fbuf_size, b.file);
    b.maxoffset = b.base + read; // Set the max buffer offset based on how much data we read

    // Decode the read data
    PyObject *result = decode_bytes(&b);

    // Calculate up to where we had to read from the file (up until the data of the next encoded data block)
    size_t end_offset = ftell(b.file);
    size_t buffer_unused = (size_t)(b.maxoffset - b.offset);
    size_t new_offset = end_offset - buffer_unused;
    fstream->foff = new_offset; // Update the reading offset

    // Update the file buffer address and size
    fstream->fbuf = b.base;
    fstream->fbuf_size = b.fbuf_size;

    unlock_flag(&fstream->lock);

    return result;
}

// Expand the encoding buffer when it doesn't have enough space
static bool encoding_expand_buffer(buffer_t *b, size_t required)
{
    // Scale the size by factor 1.5x
    const size_t allocsize = ((size_t)(b->offset - PyBytes_AS_STRING(b->base)) + required) * 1.5;

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

// Refresh the file buffer with new data while decoding
static bool decoding_refresh_fbuf(buffer_t *b, size_t required)
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
        PyErr_SetString(PyExc_EOFError, "Reached EOF before finishing the decoding run");
        return false;
    }

    // Update the offsets
    b->offset = b->base;
    b->maxoffset = b->base + unused + read;

    return true;
}

// Ensure enough space is in the encoding buffer
static _always_inline bool ensure_space(buffer_t *b, size_t required)
{
    if (b->offset + required >= b->maxoffset)
        return encoding_expand_buffer(b, required);
    
    return true;
}

// Check if we aren't overreading the buffer when reading REQUIRED bytes
static _always_inline bool overread_check(buffer_t *b, size_t required)
{
    // Check if the offset would exceed the max offset
    if (b->offset + required > b->maxoffset)
    {
        // If we have a file, we need to refresh the file buffer
        if (b->file)
        {
            return decoding_refresh_fbuf(b, required);
        }
        else
        {
            // Otherwise, we'll overread the data, so set an exception
            PyErr_SetString(PyExc_ValueError, "Received incomplete encoded data, the buffer ended before the encoded data pattern ended");
            return false;
        }
    }

    return true;
}


/////////////////////
//  BASIC ENC/DEC  //
/////////////////////

static PyObject *encode(PyObject *self, PyObject **args, Py_ssize_t nargs, PyObject *kwargs)
{
    const Py_ssize_t npositional = 1;
    if (!min_positional(nargs, npositional))
        return NULL;

    PyObject *obj = parse_positional(args, 0, NULL, "obj");

    if (!obj)
        return NULL;

    mstates_t *states = get_mstates(self);

    PyObject *str_keys = Py_False;
    PyObject *ext = (PyObject *)&states->extensions;

    keyarg_t keyargs[] = {
        KEYARG(&str_keys, &PyBool_Type, states->interned.str_keys),
        KEYARG(&ext, &ExtensionsObj, states->interned.extensions),
    };

    if (!parse_keywords(npositional, args, nargs, kwargs, keyargs, NKEYARGS(keyargs)))
        return NULL;

    return encoding_start(obj, states, ext, str_keys == Py_True, NULL);
}

static PyObject *decode(PyObject *self, PyObject **args, Py_ssize_t nargs, PyObject *kwargs)
{
    const Py_ssize_t npositional = 1;
    if (!min_positional(nargs, npositional))
        return NULL;

    PyObject *encoded = parse_positional(args, 0, NULL, "encoded");

    if (!encoded)
        return NULL;

    mstates_t *states = get_mstates(self);

    PyObject *str_keys = Py_False;
    PyObject *ext = (PyObject *)&states->extensions;

    keyarg_t keyargs[] = {
        KEYARG(&str_keys, &PyBool_Type, states->interned.str_keys),
        KEYARG(&ext, &ExtensionsObj, states->interned.extensions),
    };

    if (!parse_keywords(npositional, args, nargs, kwargs, keyargs, NKEYARGS(keyargs)))
        return NULL;

    return decoding_start(encoded, states, ext, str_keys == Py_True, NULL);
}

////////////////////
//  STREAM CLASS  //
////////////////////

static PyObject *Stream(PyObject *self, PyObject **args, Py_ssize_t nargs, PyObject *kwargs)
{
    mstates_t *states = get_mstates(self);

    PyObject *str_keys = Py_False;
    PyObject *ext = (PyObject *)&states->extensions;

    keyarg_t keyargs[] = {
        KEYARG(&str_keys, &PyBool_Type, states->interned.str_keys),
        KEYARG(&ext, &ExtensionsObj, states->interned.extensions),
    };

    if (!parse_keywords(0, args, nargs, kwargs, keyargs, NKEYARGS(keyargs)))
        return NULL;


    // Allocate the stream object based on if we got a file to use or not
    stream_t *stream = PyObject_New(stream_t, &StreamObj);

    if (!stream)
        return PyErr_NoMemory();

    // Set the object fields
    stream->ext = ext;
    stream->states = states;
    stream->str_keys = str_keys == Py_True;

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

static PyObject *stream_encode(stream_t *stream, PyObject *obj)
{
    return encoding_start(obj, stream->states, stream->ext, stream->str_keys, NULL);
}

static PyObject *stream_decode(stream_t *stream, PyObject *encoded)
{
    return decoding_start(encoded, stream->states, stream->ext, stream->str_keys, NULL);
}

static PyObject *stream_get_strkey(stream_t *stream, void *closure)
{
    return stream->str_keys == true ? Py_True : Py_False;
}

static PyObject *stream_get_extensions(stream_t *stream, void *closure)
{
    PyObject *ext = stream->ext;

    Py_INCREF(ext);
    return ext;
}

static PyObject *stream_set_strkey(stream_t *stream, PyObject *arg, void *closure)
{
    stream->str_keys = arg == Py_True;
    Py_RETURN_NONE;
}

static PyObject *stream_set_extensions(stream_t *stream, PyObject *arg, void *closure)
{
    if (Py_TYPE(arg) != &ExtensionsObj)
        return PyErr_Format(PyExc_TypeError, "Expected an object of type 'cmsgpack.Extensions', but got an object of type '%s'", Py_TYPE(arg)->tp_name);
    
    Py_DECREF(stream->ext);
    Py_INCREF(arg);

    stream->ext = arg;
    Py_RETURN_NONE;
}


/////////////////////////
//  FILE STREAM CLASS  //
/////////////////////////

static bool filestream_setup_fdata(filestream_t *stream, PyObject *filename, PyObject *reading_offset, PyObject *chunk_size)
{

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

    // Decide the file buffer's size
    stream->fbuf_size = FILEBUF_DEFAULTSIZE;
    if (chunk_size)
    {
        int overflow = 0;
        stream->fbuf_size = PyLong_AsLongLongAndOverflow(chunk_size, &overflow);

        if (overflow != 0)
        {
            free(stream->fname);

            PyErr_SetString(PyExc_ValueError, "The value of argument 'chunk_size' exceeded the 64-bit integer limit");
            return false;
        }
    }

    // Allocate the file buffer for decoding
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

    // Initialize the file offset
    stream->foff = 0;
    if (reading_offset)
    {
        int overflow = 0;
        stream->foff = PyLong_AsLongLongAndOverflow(reading_offset, &overflow);

        if (overflow != 0)
        {
            free(stream->fbuf);
            free(stream->fname);
            
            PyErr_SetString(PyExc_ValueError, "The value of argument 'reading_offset' exceeded the 64-bit integer limit");
            return false;
        }
    }

    // Initialize the file lock
    clear_flag(&stream->lock);

    return true;
}

static PyObject *FileStream(PyObject *self, PyObject **args, Py_ssize_t nargs, PyObject *kwargs)
{
    mstates_t *states = get_mstates(self);

    PyObject *reading_offset = NULL;
    PyObject *chunk_size = NULL;
    PyObject *filename = NULL;
    PyObject *str_keys = Py_False;
    PyObject *ext = (PyObject *)&states->extensions;

    keyarg_t keyargs[] = {
        KEYARG(&filename, &PyUnicode_Type, states->interned.file_name),
        KEYARG(&reading_offset, &PyLong_Type, states->interned.reading_offset),
        KEYARG(&chunk_size, &PyLong_Type, states->interned.chunk_size),
        KEYARG(&str_keys, &PyBool_Type, states->interned.str_keys),
        KEYARG(&ext, &ExtensionsObj, states->interned.extensions),
    };

    if (!parse_keywords(0, args, nargs, kwargs, keyargs, NKEYARGS(keyargs)))
        return NULL;

    // Check if we got the filename argument
    if (!filename)
    {
        PyErr_SetString(PyExc_TypeError, "Did not receive mandatory argument 'file_name'");
        return NULL;
    }


    filestream_t *stream = PyObject_New(filestream_t, &FileStreamObj);

    if (!stream)
        return PyErr_NoMemory();
    
    // Set the file data
    if (!filestream_setup_fdata(stream, filename, reading_offset, chunk_size))
    {
        PyObject_Del(stream);
        return NULL;
    }

    // Set the object fields
    stream->ext = ext;
    stream->states = states;
    stream->str_keys = str_keys == Py_True;

    // Keep a reference to the ext object
    Py_INCREF(ext);

    return (PyObject *)stream;
}

static void filestream_dealloc(filestream_t *stream)
{
    // Free the file buffer and file name, and close the file
    free(stream->fbuf);
    free(stream->fname);
    fclose(stream->file);

    // Remove reference to the ext object
    Py_DECREF(stream->ext);

    PyObject_Del(stream);
}

static PyObject *filestream_encode(filestream_t *stream, PyObject *obj)
{
    return encoding_start(obj, stream->states, stream->ext, stream->str_keys, stream);
}

static PyObject *filestream_decode(filestream_t *stream)
{
    return decoding_start(NULL, stream->states, stream->ext, stream->str_keys, stream);
}

static PyObject *filestream_get_readingoffset(filestream_t *stream, void *closure)
{
    PyObject *num = PyLong_FromLongLong(stream->foff);

    if (!num)
        return PyErr_NoMemory();
    
    return num;
}

static PyObject *filestream_get_chunksize(filestream_t *stream, void *closure)
{
    PyObject *num = PyLong_FromLongLong(stream->fbuf_size);

    if (!num)
        return PyErr_NoMemory();
    
    return num;
}

static PyObject *filestream_get_strkey(filestream_t *stream, void *closure)
{
    return stream->str_keys == true ? Py_True : Py_False;
}

static PyObject *filestream_get_extensions(filestream_t *stream, void *closure)
{
    PyObject *ext = stream->ext;

    Py_INCREF(ext);
    return ext;
}

static PyObject *filestream_set_readingoffset(filestream_t *stream, PyObject *arg, void *closure)
{
    if (Py_TYPE(arg) != &PyLong_Type)
        return PyErr_Format(PyExc_TypeError, "Expected an object of type 'int', but got an object of type '%s'", Py_TYPE(arg)->tp_name);
    
    int overflow = 0;
    long num = PyLong_AsLongAndOverflow(arg, &overflow);

    if (overflow)
    {
        PyErr_SetString(PyExc_ValueError, "Got an integer that exceeded the system word size");
        return NULL;
    }

    stream->foff = num;
    Py_RETURN_NONE;
}

static PyObject *filestream_set_chunksize(filestream_t *stream, PyObject *arg, void *closure)
{
    if (Py_TYPE(arg) != &PyLong_Type)
        return PyErr_Format(PyExc_TypeError, "Expected an object of type 'int', but got an object of type '%s'", Py_TYPE(arg)->tp_name);
    
    int overflow = 0;
    long num = PyLong_AsLongAndOverflow(arg, &overflow);

    if (overflow)
    {
        PyErr_SetString(PyExc_ValueError, "Got an integer that exceeded the system word size");
        return NULL;
    }

    char *newbuf = (char *)malloc(num);

    if (!newbuf)
        return PyErr_NoMemory();
    
    free(stream->fbuf);
    
    stream->fbuf = newbuf;
    stream->fbuf_size = num;

    Py_RETURN_NONE;
}

static PyObject *filestream_set_strkey(filestream_t *stream, PyObject *arg, void *closure)
{
    stream->str_keys = arg == Py_True;
    Py_RETURN_NONE;
}

static PyObject *filestream_set_extensions(filestream_t *stream, PyObject *arg, void *closure)
{
    if (Py_TYPE(arg) != &ExtensionsObj)
        return PyErr_Format(PyExc_TypeError, "Expected an object of type 'cmsgpack.Extensions', but got an object of type '%s'", Py_TYPE(arg)->tp_name);
    
    Py_DECREF(stream->ext);
    Py_INCREF(arg);

    stream->ext = arg;
    Py_RETURN_NONE;
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
    GET_ISTR(file_name)
    GET_ISTR(types)
    GET_ISTR(extensions)
    GET_ISTR(allow_subclasses)
    GET_ISTR(pass_memoryview)
    GET_ISTR(str_keys)
    GET_ISTR(reading_offset)
    GET_ISTR(chunk_size)

    /* CACHES */

    // NULL-initialize the string cache
    memset(s->caches.strings.slots, 0, sizeof(s->caches.strings.slots));

    // Initialize match strengths as 1 so that they'll immediately reach 0 on the first decrement
    memset(s->caches.strings.match_strength, 1, sizeof(s->caches.strings.match_strength));

    #ifdef _need_threadsafe

    // Initialize the string cache's locks to a clear state
    for (size_t i = 0; i < STRING_CACHE_SLOTS; ++i)
        clear_flag(&s->caches.strings.locks[i]);

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
    Py_SET_REFCNT(dummylong_pos, IMMORTAL_REFCNT);
    Py_SET_REFCNT(dummylong_neg, IMMORTAL_REFCNT);
    Py_SET_REFCNT(dummylong_zero, IMMORTAL_REFCNT);
    
    // First store the negative values
    for (size_t i = 0; i < INTEGER_CACHE_NNEG; ++i)
    {
        s->caches.integers.slots[i] = *dummylong_neg;

        digit val = INTEGER_CACHE_NNEG - i;

        // Negative values are stored as positive
        s->caches.integers.slots[i].long_value.ob_digit[0] = val;
    }

    // Store the zero object
    s->caches.integers.slots[INTEGER_CACHE_NNEG] = *dummylong_zero;
    
    // Then store positive values
    for (size_t i = 1; i < INTEGER_CACHE_SLOTS - INTEGER_CACHE_NNEG; ++i)
    {
        size_t idx = i + INTEGER_CACHE_NNEG; // Skip over negatives and zero

        s->caches.integers.slots[idx] = *dummylong_pos;

        // Set the integer value to the current index
        s->caches.integers.slots[idx].long_value.ob_digit[0] = i;
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
    Py_SET_REFCNT((PyObject *)&s->extensions, IMMORTAL_REFCNT);

    // Create the dict object for the encoding extension types
    s->extensions.data.dict = PyDict_New();

    if (!s->extensions.data.dict)
        return PyErr_NoMemory();

    // NULL-initialize the decode functions
    memset(s->extensions.funcs, 0, sizeof(s->extensions.funcs));
    s->extensions.data.funcs = s->extensions.funcs;

    // Set default values
    s->extensions.data.pass_memview = false;

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
        Py_XDECREF(s->caches.strings.slots[i]);
    
    
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

static PyGetSetDef StreamGetSet[] = {
    {"str_keys", (getter)stream_get_strkey, (setter)stream_set_strkey, NULL, NULL},
    {"extensions", (getter)stream_get_extensions, (setter)stream_set_extensions, NULL, NULL},

    {NULL}
};

static PyGetSetDef FileStreamGetSet[] = {
    {"reading_offset", (getter)filestream_get_readingoffset, (setter)filestream_set_readingoffset, NULL, NULL},
    {"chunk_size", (getter)filestream_get_chunksize, (setter)filestream_set_chunksize, NULL, NULL},
    {"str_keys", (getter)filestream_get_strkey, (setter)filestream_set_strkey, NULL, NULL},
    {"extensions", (getter)filestream_get_extensions, (setter)filestream_set_extensions, NULL, NULL},

    {NULL}
};

static PyGetSetDef ExtensionsGetSet[] = {
    {"pass_memoryview", (getter)extensions_get_passmemview, (setter)extensions_set_passmemview, NULL, NULL},

    {NULL}
};

static PyMethodDef CmsgpackMethods[] = {
    {"encode", (PyCFunction)encode, METH_FASTCALL | METH_KEYWORDS, NULL},
    {"decode", (PyCFunction)decode, METH_FASTCALL | METH_KEYWORDS, NULL},

    {"Stream", (PyCFunction)Stream, METH_FASTCALL | METH_KEYWORDS, NULL},
    {"FileStream", (PyCFunction)FileStream, METH_FASTCALL | METH_KEYWORDS, NULL},
    {"Extensions", (PyCFunction)Extensions, METH_FASTCALL | METH_KEYWORDS, NULL},

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
    .tp_getset = StreamGetSet,
    .tp_new = NULL,
};

static PyTypeObject FileStreamObj = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "cmsgpack.FileStream",
    .tp_basicsize = sizeof(filestream_t),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = FileStreamMethods,
    .tp_dealloc = (destructor)filestream_dealloc,
    .tp_getset = FileStreamGetSet,
    .tp_new = NULL,
};

static PyTypeObject ExtDictItemObj = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "cmsgpack.ExtDictItem",
    .tp_basicsize = sizeof(ext_dictitem_t),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_dealloc = (destructor)ExtDictItem_dealloc,
    .tp_new = NULL,
};

static PyTypeObject ExtensionsObj = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "cmsgpack.Extensions",
    .tp_basicsize = sizeof(extensions_t),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = ExtensionsMethods,
    .tp_dealloc = (destructor)extensions_dealloc,
    .tp_getset = ExtensionsGetSet,
    .tp_new = NULL,
};

static void cleanup(PyObject *module)
{
    cleanup_mstates(module);
}

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

    if (!m)
        return NULL;

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

