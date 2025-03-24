# `cmsgpack` usage details

This file explains how to use the `cmsgpack` module in Python.

**Contents:**
- [Summary](#summary)
- [Serialization](#serialization)
- [Supported types](#supported-types)
- [Extension types](#extension-types)


## Summary

Here is a summary that briefly goes through everything that this serializer offers.

For straightforward and easy-to-use serialization, see [regular serialization](#regular-serialization). If you plan to pass optional arguments frequently, the [`Stream`](#stream) object is useful for both convenience and performance.

For file-based serialization, this library offers the [`FileStream`](#filestream) object. This object supports serialization that directly uses a file, meaning you don't have to manage file operations yourself.

For information on what types are supported and how, see the [supported types](#supported-types) section, or for unsupported types, the [extension types](#extension-types) section.


## Serialization

For serialization, `cmsgpack` offers the regular `encode`/`decode` functions, a `Stream` object that retains optional arguments for your convenience, and a `FileStream` object that can be used for directly reading/writing a file.

**Contents:**
- [Regular serialization](#regular-serialization)
	- [`encode`](#encode)
	- [`decode`](#decode)
- [`Stream`](#stream)
	- [`encode`](#streamencode)
	- [`decode`](#streamdecode)
- [`FileStream`](#filestream)
	- [`encode`](#filestreamencode)
	- [`decode`](#filestreamdecode)

### Regular serialization

For regular and straightforward serialization, the `encode` and `decode` functions are plenty. These functions support all optional features, but parsing them in each function call will add a little overhead, which is where the [`Stream`](#stream) object shines.

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

**Class variables:**
- `str_keys: bool`
- `extensions: Extensions`

The `Stream` object is useful when you need to pass optional arguments and want the best performance, or just for convenience. Instead of having to pass optional arguments to the `encode` and `decode` functions, you pass them once on class creation. These arguments can always be modified through the object's attributes.

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

The `FileStream` object is used for serialization with a file. This object manages everything from chunked reading to file offsets for you. And just like the `Stream` object, optional arguments for `encode` and `decode` are passed at object creation.

Writing data with this object will always append it to the end of the file. In case a write fails due to an OS error, the object always attempts to reverse any partially written data to avoid corruption. Otherwise, the error message will state the exact writing offset and length. Writing data is not chunked for performance reasons.

Reading data with this object starts from the reading offset, which is the start of the file by default. After reading/decoding data, the offset is automatically updated. Reading is always done in chunks.

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


## Supported types

This library supports all types defined by the MessagePack format. All encoded objects will be represented using the MessagePack type headers. This could mean loss of specific type information, like how a `list` and `tuple` both **have** to be encoded as an `ARRAY` type.

These types are supported "as-is", and decoded data will always output in these types:
- `str`
- `int`
- `float`
- `list`
- `dict`
- `bool`
- `NoneType`
- `bytes`

These are not the only supported types, but any other types *will lose their exact type info*. These are the other supported types and what they will be encoded as:
- `dict` subclasses, encoded as a regular `dict`
- `list` subclasses, encoded as a regular `list`
- `tuple` and `tuple` subclasses, encoded as a `list`
- `bytearray` & `memoryview`, encoded as `bytes`

Besides these types, MessagePack also offers "Extension Types". These types are identified using an ID instead, and require manually defined functions for encoding/decoding. More on those at the [Extension types](#extension-types) section below.


## Extension types

Extension types are used for serializing types that aren't supported by MessagePack. An extension type always goes paired with an ID between -128 and 127. This ID is used for identifying the type in encoded data, so the same ID has to be passed for encoding and decoding a type. Because these are *custom, unsupported* types, you're required to assign a function for encoding and decoding that type.

For clarity, the ID for a type has to be chosen by you, and it does not matter if you pick 1 or -17 or whatever, as long as it's within -128 and 127. If you assign the ID 1 to the type `complex`, the serializer stores that it's an extension type with ID 1 when you encode a `complex` type, and when you decode the data, it sees an extension type with ID 1 and calls the function assigned for decoding data with that ID.

This can be done using an `Extensions` object. You add your custom types to this object and pass it to `encode`/`decode` functions. Types can be registered for both encoding and decoding, or exclusively for either. The object can be initialized with types, and types can be dynamically added and removed freely after initialization.

`Extensions` objects handle types differently for encoding and decoding. When you encode a custom type, the type is used for looking up the ID and function for encoding that custom type. This means that multiple types are allowed to be stored under the same ID. When you decode data with an extension type in it, the stored ID is used for getting the function for decoding the data. In short, the custom type is used for identification during encoding, and the ID assigned to that custom type is used for identification during decoding.

For convenienve, `cmsgpack` also has a global `Extensions` object ready for use. This object can be accessed through `cmsgpack.extensions`. This `Extensions` object is automatically used in serialization unless a manually created `Extensions` object is passed, overriding the global object.

**Contents:**
- [The custom encoding/decoding functions](#the-custom-encodingdecoding-functions)
- [`Extensions`](#extensions)
	- [`add`](#extensionsadd)
	- [`add_encode`](#extensionsadd_encode)
	- [`add_decode`](#extensionsadd_decode)
	- [`remove`](#extensions.emove)
	- [`remove_encode`](#extensionsremove_encode)
	- [`remove_decode`](#extensionsremove_decode)
	- [`clear`](#extensionsclear)

### The custom encoding/decoding functions

The functions you need to provide for encoding and decoding a custom type should follow these function signatures:

```python
def encode_MyType(obj: MyType) -> Buffer: ...
def decode_MyType(data: bytes) -> any: ...
```

The encoding function will receive an object of the custom type, and is expected to return bytes-like object of the encoded object. `cmsgpack` will manage storing this encoded object.
The decoding function will receive a `bytes` object (or `memoryview` object, if enabled) that contains the encoded object. This would be the exact same object as the object that was returned by your encoding function. This function is free to return anything, no matter what type.

These functions are allowed to throw exceptions.

Here is an example on how this might be done for the `complex` type:

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

