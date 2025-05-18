# `cmsgpack` usage details

This file explains how to use the `cmsgpack` module in Python.

**Contents:**
- [Summary](#summary)
- [Installation](#installation)
- [Serialization](#serialization)
- [Supported Types](#supported-types)
- [Extension Types](#extension-types)


## Summary

A brief summary of all sections and where to find what.

For straightforward serialization, see [Regular Serialization](#regular-serialization). If you plan to pass optional arguments frequently, or when managing multiple data streams, the [`Stream`](#stream) class is useful both for convenience and performance.

For file-based serialization, use the [`FileStream`](#filestream) class. This directly streams data to/from files.

For information on what types are supported, see the [Supported Types](#supported-types) section, or for unsupported types, the [Extension Types](#extension-types) section.


## Installation

`cmsgpack` is available through **pip**. For a regular installations, you can directly use:

```shell
pip install cmsgpack
```

If you want to explicitly install from source, use pip's `--no-binary` option:

```shell
pip install --no-binary cmsgpack cmsgpack
```

If no wheel is available for your specific platform or when explicitly building from source, a C compiler with C11 support is required.


## Serialization

For serialization, `cmsgpack` offers the regular `encode`/`decode` functions, a `Stream` object that retains optional arguments, and a `FileStream` object that can be used for directly reading/writing a file.

**Contents:**
- [Regular Serialization](#regular-serialization)
	- [`encode`](#encode)
	- [`decode`](#decode)
- [`Stream`](#stream)
	- [`encode`](#streamencode)
	- [`decode`](#streamdecode)
- [`FileStream`](#filestream)
	- [`encode`](#filestreamencode)
	- [`decode`](#filestreamdecode)

### Regular Serialization

For basic serialization, use the `encode` and `decode` functions. When frequently passing keyword arguments, using the [`Stream`](#stream) object is recommended for easier keyword management and a small performance advantage.

#### `encode`

```python
cmsgpack.encode(obj: any, /, str_keys: bool=False, extensions: Extensions=None) -> bytes
```

*"Encode Python data to bytes."*

**Arguments:**
- `obj`: The object to encode. This can be any of the [supported types](#supported-types).
- `str_keys`: If true, dictionaries are only allowed to use keys of type `str`.
- `extensions`: An [extension types](#extension-types) object for encoding custom objects. If not given, the global extensions object is used.

**Returns:** The encoded data as a `bytes` object.

#### `decode`

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

**Class attributes:**
- `str_keys: bool`
- `extensions: Extensions`


The `Stream` object is useful when keyword arguments are used. These arguments have to be passed only once on the object's creation, and can always be modified through the object's attributes.

`Stream` objects are also recommended when handling multiple data streams, as each object tracks its own heuristics for more predictable buffer allocations. So a separate object per data stream is recommended for optimal performance.

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

**Class attributes:**
- `reading_offset: int`
- `chunk_size: int`
- `str_keys: bool`
- `extensions: Extensions`


The `FileStream` object is used for direct serialization with files. It internally manages file offsetting and processes data in chunks when decoding. Just like with `Stream`, keyword arguments are retained and can be modified at any time, except for the file name.

Writing data with this object will always append it to the end of the file. Reading starts at the reading offset, which is the start of the file by default. This offset is incremented automatically and will be positioned directly after the previously read data.

When a write operation fails, an attempt to truncate the file back to the position before the write is done. File truncation is supported on Windows and POSIX-compliant systems. If the truncate fails or isn't supported on the system, an error is thrown with details of the exact file position and how many bytes were written before the write failed.

#### `FileStream.encode`

```python
cmsgpack.FileStream.encode(obj: any, /) -> `None`
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


## Supported Types

`cmsgpack` supports all types defined in the MessagePack spec. Some Python types are supported, but may lose exact type information when serialized.

These types are supported "as-is", and decoded data will always output in these types:
- `str`
- `int`
- `float`
- `list`
- `dict`
- `bool`
- `NoneType`
- `bytes`

And these are the supported types that lose exact type information (and what they will be serialized as):
- `dict` subclasses, encoded as a regular `dict`
- `list` subclasses, encoded as a regular `list`
- `bytes` subclasses, encoded as a regular `bytes`
- `tuple` and `tuple` subclasses, encoded as a `list`
- `bytearray` and `memoryview` (and subclasses of those), encoded as `bytes`

Besides these types, MessagePack also offers extension types, used for serializing non-standard or custom types. This is further explained in the [Extension Types](#extension-types) section.


## Extension Types

Extension types are used for serializing types not supported by MessagePack, or not indirectly supported by `cmsgpack` itself. An extension type has to be identified using an ID, which is a number between -128 and 127. This is done using the `Extensions` object.

Each extension type requires its own functions for encoding and decoding, registered to the `Extensions` object alongside the type and ID. Encoding is centered around the registered type, and all objects encountered of said type are passed to the encoding function. Decoding is centered around the registered ID, and all extension data with said ID is passed to the decoding function.

The separation between using the type when encoding and the ID when decoding gives flexibility in how we register extension types. Everything registered through `add_encode` will not affect any decoding behavior, likewise for `add_decode` not affecting any encoding behavior.

A global `Extensions` object exists by default, accessible through `cmsgpack.extensions`. This object is used by default, when no object was passed to the `extensions` optional argument. This object can be freely used for extension type management in place of manually creating an object.

**Contents:**
- [The custom encoding/decoding functions](#the-custom-encodingdecoding-functions)
- [`Extensions`](#extensions)
	- [`add`](#extensionsadd)
	- [`add_encode`](#extensionsadd_encode)
	- [`add_decode`](#extensionsadd_decode)
	- [`remove`](#extensionsremove)
	- [`remove_encode`](#extensionsremove_encode)
	- [`remove_decode`](#extensionsremove_decode)
	- [`clear`](#extensionsclear)

### The custom encoding/decoding functions

The functions for serializing a custom type require the following signatures:

```python
def encode_MyType(obj: MyType) -> Buffer: ...
def decode_MyType(data: bytes) -> Any: ...
```

The encode function will always receive an object of the registered type, and is expected to return a bytes-like object of the serialized object. This data will be written to the buffer.

The decode function will receive the data that was previously returned by the encode function during serialization. This will be in a `bytes` object by default, or a `memoryview` object when optionally enabled. The decode function is allowed to return any type, regardless of what type was registered for encoding with this ID.

Both functions are allowed to raise exceptions.

An example on how this can be used for the `complex` type:

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

**Class attributes:**
- `pass_memoryview: bool`

An example on how to structure the `types` dict:

```python
# Assuming we have:
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
- `id`: The ID to assign to the extension type.
- `type`: The extension type.
- `encfunc`: The function to call for encoding an object of type `type`.

**Returns:** `None`.

#### `Extensions.add_decode`

```python
cmsgpack.Extensions.add_decode(id: int, decfunc: Callable, /) -> None
```

*"Add an extension type for just decoding."*

**Arguments:**
- `id`: The ID to assign to the extension type.
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

