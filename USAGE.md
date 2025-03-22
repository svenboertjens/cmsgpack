# `cmsgpack` usage details

This file explains how to use the `cmsgpack` module in Python.

**Contents:**
- [Installation](#installation)
- [Supported types](#supported-types)
- [Encoding](#encoding)
- [Decoding](#decoding)
- [Extension types](#extension-types)


## Supported types

The following types are supported as-is:
- `str`
- `int`
- `float`
- `bytes`
- `bool`
- `NoneType`
- `list`
- `dict`

These types are defined in the MessagePack format and can be serialized as normal.

For convenience, `cmsgpack` also supports the following types:
- `dict` subclasses;
- `list` subclasses;
- `tuple` objects & subclasses, encoded as a `list` type;
- `bytearray` & `memoryview` objects & subclasses, encoded as a `bytes` type;

If this doesn't include a type you want to use, have a look at the [Extension types](#extension-types).


## Serialization

For serialization, `cmsgpack` offers the regular `encode`/`decode` functions, a `Stream` object that retains optional arguments for your convenience, and a `FileStream` object that can be used for directly reading/writing a file.

**Contents:**
- [`encode`](#encode)
- [`decode`](#decode)
- [`Stream`](#stream)
	- [`encode`](#streamencode)
	- [`decode`](#streamdecode)
- [`FileStream`](#filestream)
	- [`encode`](#filestreamencode)
	- [`decode`](#filestreamdecode)

### `encode`

```python
cmsgpack.encode(obj: any, /, str_keys: bool=False, extensions: Extensions=None) -> bytes
```

*"Encode Python data to bytes."*

**Arguments:**
- `obj`: The object to encode. This can be any of the [supported types](#supported-types).
- `str_keys`: If true, dictionaries are only allowed to use keys of type `str`.
- `extensions`: An [extension types](#extension-types) object for encoding custom objects. If not given, the global extensions object is used.

**Returns:** The encoded data as a `bytes` object.

### `decode`

```python
cmsgpack.decode(encoded: Buffer, /, str_keys: bool=False, extensions: Extensions=None) -> any:
```

*"Decode any MessagePack-encoded data."*

**Arguments:**
- `encoded`: A buffer object that holds the encoded data. Can be any object that supports the buffer protocol.
- `str_keys`: If true, dictionaries are only allowed to use keys of type `str`.
- `extensions`: An [extension types](#extension-types) object for decoding custom objects. If not given, the global extensions object is used.

**Returns:** The decoded Python object.

### `Stream`

```python
cmsgpack.Stream(str_keys: bool=False, extensions: Extensions=None) -> Stream
```

*"Wrapper for `encode`/`decode` that retains optional arguments."*

**Arguments:**
- `str_keys`: If true, dictionaries are only allowed to use keys of type `str`.
- `extensions`: An [extension types](#extension-types) object for decoding custom objects. If not given, the global extensions object is used.

**Returns:** A new instance of the `Stream` class.

**Class variables:**
- `str_keys: bool`
- `extensions: Extensions`

The `Stream` class can be used for incremental serialization. The optional arguments that you'd normally pass to `encode`/`decode` are instead passed once when you create an instance of the class, and the class retains them and automatically uses them when you use the class's `encode`/`decode` functions. Using the `Stream` object is also slightly more efficient when you plan on passing keyword arguments.

#### `Stream.encode`

```python
cmsgpack.Stream.encode(obj: any, /) -> bytes
```

*"Encode Python data to bytes."*

**Arguments:**
- `obj`: The object to encode. This can be any of the [supported types](#supported-types).

**Returns:** The encoded data as a `bytes` object.

#### `Stream.decode`

```python
cmsgpack.Stream.decode(encoded: Buffer, /) -> any
```

*"Decode any MessagePack-encoded data."*

**Arguments:**
- `encoded`: A buffer object that holds the encoded data. Can be any object that supports the buffer protocol.

**Returns:** The decoded Python object.

### `FileStream`

```python
cmsgpack.FileStream(file_name: str, reading_offset: int=0, chunk_size: int=16384, str_keys: bool=False, extensions: Extensions=None) -> FileStream
```

*"Wrapper for `encode`/`decode` that retains optional arguments and reads/writes a file directly."*

**Arguments:**
- `file_name`: The path towards the file to use for reading and writing.
- `reading_offset`: The reading offset to start at in the file.
- `chunk_size`: The chunk size of the file buffer for reading. A larger size can be used if the size of the data is large to minimize direct disk reads. A smaller size can be used if the size of the data is small to minimize memory usage.
- `str_keys`: If true, dictionaries are only allowed to use keys of type `str`.
- `extensions`: An [extension types](#extension-types) object for decoding custom objects. If not given, the global extensions object is used.

**Returns:** A new instance of the `FileStream` class.

**Class variables:**
- `reading_offset: int`
- `chunk_size: int`
- `str_keys: bool`
- `extensions: Extensions`

The `FileStream` object can be used for serialization to/from a file. Just like the `Stream` object, optional arguments are retained. Reading is done from an offset within the file, which defaults to the start of the file, offset 0. Writing always appends to the end of the file.

In the unlikely event of a failed write due to an OS error, this library always attempts to truncate the file to remove any partially written data to avoid data corruption. Otherwise, the error message will always provide the exact writing offset and length. File truncation currently only works for Windows and POSIX-compliant systems.

#### `FileStream.encode`

```python
cmsgpack.FileStream.encode(obj: any, /) -> NoReturn
```

*"Encode Python data and write it to the file."*

**Arguments:**
- `obj`: The object to encode. This can be any of the [supported types](#supported-types).

**Returns:** This function does not return the encoded data, as this is written to the file. `None` is returned instead.

#### `FileStream.decode`

```python
cmsgpack.FileStream.decode() -> any
```

*"Decode MessagePack-encoded data read from the file."*

**Arguments:** This function does not take any arguments. The encoded data is read from the file instead.

**Returns:** The decoded Python object.


## Extension types

Extension types (ext types) are used for serializing types not supported by default in MessagePack. To see which types are supported, see the [supported types](#supported-types). Extension types each have their own user-defined ID to track them.

It's important that the same ID is assigned to an extension type across all sessions. This includes sessions outside of `cmsgpack` or Python. The ID is used to identify the type, and when a different one is used, the serializer has no way to know what to do with the data.

**Contents:**
- [How extension types are managed](#how-extension-types-are-managed)
- [The global extensions object](#the-global-extensions-object)
- [Structuring the encoding/decoding functions](#structuring-the-encodingdecoding-functions)
- [`Extensions`](#extensions)
	- [`add`](#extensionsadd)
	- [`add_encode`](#extensionsadd_encode)
	- [`add_decode`](#extensionsadd_decode)
	- [`remove`](#extensions.emove)
	- [`remove_encode`](#extensionsremove_encode)
	- [`remove_decode`](#extensionsremove_decode)
	- [`clear`](#extensionsclear)

### How extension types are managed

An ext type has to be identified with an ID, which can be any number between -128 and 127. These types can be registered in an extension object for use in serialization with `cmsgpack`. Besides the ID, a function for encoding and decoding is needed for the ID. The encoding function is invoked when an object of the ext type is found, and the decoding function is invoked when an ext type with the specified ID is encountered.

Ext types are registered through `Extension` objects. These objects can be created manually using `cmsgpack.Extensions()`, or the global extensions object `cmsgpack.extensions` can be used for simplicity.

Extension objects handle both the encoding and decoding part, but they are managed differently. The encoding part is centered around the extension type (the type you want to support through extensions), and when an unsupported object is encountered, `cmsgpack` checks if the used extensions object supports the object's type. The decoding part is centered around the ext type's ID, the type itself is not used. When `cmsgpack` encounters an ext type, it checks if a function for decoding is registered under the ID of the ext type and invokes it.

This separation allows us to assign the same ID to different types, if required for your use case. And decoding is not limited to one type, but instead invokes the decoding function, which is allowed to return anything.

> [Note]
> 
> The separation between encoding and decoding makes the remove functions of extension objects appear a bit strange. The reason `cmsgpack` requires the extension type when removing the encoding part, and the ID when removing a decoding part, is because look-ups for encoding are done using the type, and look-ups for decoding are done using the ID.

### The global extensions object

The global extensions object, `cmsgpack.extensions`, is already set up and ready for use. This
object can be used just like a manually created extensions object, for example: `cmsgpack.extensions.add(...)` to add a type. The default values when creating an extensions object are also applied to this object.

The global object does not have to be passed to functions explicitly, this is automatically managed internally. When you want to use a manual extensions object, you can pass it to the function, and it will override the global object.

### Structuring the encoding/decoding functions

The encoding function will receive an object that is guaranteed to be of the type under which the function was registered, and is expected to return a bytes-like object of the encoded object.

The decoding function will receive a `bytes` object (or `memoryview` object, if enabled) containing the data previously encoded by the type's encoding function. This function is allowed to return anything, no type checking is performed on the return value.

Both functions are allowed to throw exceptions. These will be handled properly, and the serialization process will be stopped so that the exception can be thrown.

Here is an example of how it might look for a `complex` object:

```python
# The function for encoding a `complex` object
def encode_complex(obj: complex) -> bytes:
	# Convert the complex to a string and encode it to bytes
	return str(obj).encode()

# The function for decoding a `complex` object
def decode_complex(data: bytes) -> complex:
	# Decode the encoded data and convert it to a `complex` object
	return complex(data.decode())

# The ID we want to use for `complex` object
EXT_ID_COMPLEX = 1

# Register the type to the global extensions object
cmsgpack.extensions.add(EXT_ID_COMPLEX, complex, encode_complex, decode_complex)
```

### `Extensions`

```python
cmsgpack.Extensions(types: dict | None=None, pass_memoryview: bool=False) -> self
```

*"Class for using MessagePack extension types during serialization."*

**Arguments:**
- `types`: A dictionary used for initializing the class with extension types. This is structured as `{ID: (type, encfunc, decfunc)}`, where `ID` is the ID to assign to the type, `type` is the type belonging to the ID, and `encfunc`/`decfunc` are the functions used for encoding and decoding, respectively.
- `pass_memoryview`: Whether decoding functions should receive a `memoryview` object instead of a `bytes` object. Using a `memoryview` object is better for performance, but may add more complexity to handling the data.

**Returns:** A new instance of the `Extensions` class.

**Class variables:**
- `pass_memoryview: bool`

An example on how to structure the `types` dict:

```python
# Assuming we have declared:
#    `EXT_ID`  as the ID to use for this type;
#    `my_type` as the extension type;
#    `encfunc` for the encoding function;
#    `decfunc` for the decoding function;

# Create the types dict using our data
types = {
	EXT_ID: (
		my_type,
		encfunc,
		decfunc
	)
}

# Create the extensions object using the types dict
ext = cmsgpack.Extensions(types)
```

> [Note]
> 
> If multiple types need to be encoded under the same ID, they should be added explicitly through an add function.

#### `Extensions.add`

```python
cmsgpack.Extensions.add(id: int, type: type, encfunc: Callable, decfunc: Callable, /) -> None
```

*"Add an extension type for encoding and decoding."*

**Arguments:**
- `id`: The ID to assign to the extension type.
- `type`: The extension type.
- `encfunc`: The function to call for encoding an object of type `type`.
- `decfunc`: The function to call for decoding an ext type with ID `id`.

**Returns**: `None`.

#### `Extensions.add_encode`

```python
cmsgpack.Extensions.add_encode(id: int, type: type, encfunc: Callable, /) -> None
```

*"Add an extension type for just encoding."*

**Arguments:**
- `id`: The Id to assign to the extension type.
- `type`: The extension type.
- `encfunc`: The function to call for encoding an object of type `type`.

**Returns:** `None`.

#### `Extensions.add_decode`

```python
cmsgpack.Extensions.add_decode(id: int, decfunc: Callable, /) -> None
```

*"Add an extension type for just decoding."*

**Arguments:**
- `id`: The Id to assign to the extension type.
- `decfunc`: The function to call for decoding an ext type with ID `id`.

**Returns:** `None`.

#### `Extensions.remove`

```python
cmsgpack.Extensions.remove(id: int, type: type) -> None
```

*"Remove the encoding and decoding entry for the given ID and type."*

**Arguments:**
- `id`: The ID to remove the decode function from.
- `type`: The type to remove the encode function from.

**Returns:** `None`.

> [Note]
> 
> The remove functions from `Extensions` objects don't throw an exception when the type/ID is not present.

#### `Extensions.remove_encode`

```python
cmsgpack.Extensions.remove_encode(type: type) -> None
```

*"Remove just the encoding entry for the given type."*

**Arguments:**
- `type`: The type to remove the encode function from.

**Returns:** `None`.

#### `Extensions.remove_decode`

```python
cmsgpack.Extensions.remove_decode(id: int) -> None
```

*"Remove just the decoding entry for the given ID."*

**Arguments:**
- `id`: The ID to remove the decode function from.

**Returns:** `None`.

#### `Extensions.clear`

```python
cmsgpack.Extensions.clear() -> None
```

*"Clears the entire extensions object."*

**Arguments:**
- No arguments.

**Returns:** `None`.