# `cmsgpack` usage details

This file explains how to use the `cmsgpack` module in Python.


**Contents:**
- [Installation](#installation)
- [Quick start](#quick-start)
	- [Encoding](#encoding-(simplified))
	- [Decoding](#decoding-(simplified))
- [Supported types](#supported-types)
- [Encoding](#encoding)
	- [`encode`](#encode)
	- [`Encoder`](#encoder)
- [Decoding](#decoding)
	- [`decode`](#decode)
	- [`Decoder`](#decoder)
- [Extension types](#extension-types)
	- [How ext types are managed](#how-extension-types-are-managed)
	- [Global extensions object](#the-global-extensions-object)
	- [Structuring the encoding/decoding functions](#structuring-the-encoding/decoding-functions)
	- [`Extensions`](#extensions)
- [Thread-safety](#thread-safety)
- [Questions and feedback](#questions-and-feedback)


## Introduction

`cmsgpack` is a [MessagePack](https://msgpack.org/) serializer for Python. For those who are unfamiliar with those terms, serialization means converting Python objects to bytes here. The format used for preserving the data is MessagePack.

But why use `cmsgpack` and not existing solutions like `msgpack`, or optimized solutions like `msgspec` and `ormgspack`? While these solutions are already great, `cmsgpack` aims to be **even better**. Apart from offering a simple-to-use API with rich features, `cmsgpack` is optimized to be efficient, fast, and lightweight. It minimizes memory allocations where possible through smart caching methods and adaptive buffer allocations.

If you are looking for schema-based serialization, `cmsgpack` does not support this. This module is made to be lightweight and optimized for regular serialization.

For performance benchmarks, you can use the [benchmark script](benchmarks/benchmark.py). This shows a comparison between the speed of `cmsgpack`, `ormsgpack`, and `msgspec`.


## Installation

The Python module is available via **pip**. To install it, you can use this command:

```shell
pip install cmsgpack
```

For a more manual approach, you can clone the repository and install it manually with this:

```shell
git clone https://github.com/svenboertjens/cmsgpack.git
cd cmsgpack
pip install .
```

These both install the `cmsgpack` module to your Python environment.

> [Note]
>
> The module is distributed as a "source distribution" (sdist) package, meaning it must be compiled during installation. This requires a C compiler. Recommended compilers are **GCC, Clang, and MSVC.**


## Quick start

Only need to know the basics of serialization? Here's a quick start!


### Encoding (simplified)

The function for encoding data is this:

```python
cmsgpack.encode(obj: any) -> bytes
```

*"Encode Python data into bytes."*

**Arguments**:
- `obj`: The object to encode. This can be any of the [supported types](#supported-types).

**Returns:** The encoded data in a `bytes` object.


### Decoding (simplified)

The function for decoding data is this:

```python
cmsgpack.decode(encoded: bytes) -> any
```

*"Decode any MessagePack-encoded data."*

**Arguments**:
- `encoded`: A bytes object that holds the encoded data.

**Returns:** The decoded Python object.


### Example

For reference, here is a simple example on how to use `encode` and `decode`.

```python
import cmsgpack

# This is the value we want to encode
obj = ["Hello,", "world!"]

# We can encode it using `encode`
encoded = cmsgpack.encode(obj)

# Your code stuff...
...

# We can decode it again using `decode`
decoded = cmsgpack.decode(encoded)

# Now, `decoded` is a copy of `obj`
assert obj == decoded # True
```


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

If these types don't include a type requirement for you, have a look at the [Extension types](#extension-types).


## Encoding

Encoding data to a `bytes` object.

### `encode`

```python
cmsgpack.encode(obj: any, /, extensions: Extensions=None) -> bytes
```

*"Encode Python data to bytes."*

**Arguments:**
- `obj`: The object to encode. This can be any of the [supported types](#supported-types).
- `extensions`: An [extension types](#extension-types) object for encoding custom objects.

**Returns:** The encoded data in a `bytes` object.


### `Encoder`

```python
cmsgpack.Encoder(file_name: str|None=None, extensions: Extensions=None, strict_keys: bool=False) -> Encoder
```

*"Class-based wrapper for `encode()` that retains optional arguments."*

**Arguments:**
- `file_name`: The path to the file to stream data to.
- `extensions`: An [extension types](#extension-types) object for encoding custom objects.

**Returns:** A new instance of the `Encoder` class.

> [Note]
>
> When the `file_name` argument is passed, the `Encoder` will be used for streaming to that file. `Encoder.encode` will not return the bytes object and instead directly write it to the file.


#### `Encoder.encode`

```python
cmsgpack.Encoder.encode(obj: any) -> bytes | None
```

*"Encode Python data to bytes using the class's stored arguments."*

**Arguments:**
- `obj`: The object to encode. This can be any of the [supported types](#supported-types).

**Returns:** The encoded data in a `bytes` object **if not streaming**, otherwise `None`.


## Decoding

Decoding binary data.


### `decode`

```python
cmsgpack.decode(encoded: Buffer, /, extensions: Extensions=None, strict_keys: bool=False) -> any:
```

*"Decode any MessagePack-encoded data."*

**Arguments:**
- `encoded`: A buffer object that holds the encoded data. Buffers are `bytes`, `bytearray`, or a custom type that supports the buffer protocol.
- `extensions`: An [extension types](#extension-types) object for encoding custom objects.

**Returns:** The decoded Python object.


### `Decoder`

```python
cmsgpack.Decoder(file_name: str|None=None, extensions: Extensions=None, strict_keys: bool=False) -> Decoder
```

*"Class-based wrapper for `decode()` that retains optional arguments."*

**Arguments:**
- `file_name`: The path to the file to read data from.
- `extensions`: An [extension types](#extension-types) object for decoding custom objects.

**Returns:** A new instance of the `Decoder` class.

> [Note]
>
> When the `file_name` argument is passed, the `Decoder` will be used for streaming from that file.


#### `Decoder.decode`

```python
cmsgpack.Decoder.decode(encoded: Buffer) -> any
```

*"Decode any MessagePack-encoded data using the class's stored arguments."*

**Arguments:**
- `encoded`: A buffer object that holds the encoded data. Buffers are `bytes`, `bytearray`, or a custom type that supports the buffer protocol. **This argument is required unless streaming is enabled.**

**Returns:** The decoded Python object.


## Extension types

Extension types (ext types) are used for serializing types not supported by default in MessagePack. To see which types are supported, see the [supported types](#supported-types). Extension types each have their own user-defined ID to track them.

It's important that the same ID is assigned to an extension type across all sessions. This includes sessions outside of `cmsgpack` or Python. The ID is used to identify the type, and when a different one is used, the serializer has no way to know what to do with the data.


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

This is an example of how it might look for a `complex` object:

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


## Thread-safety

`cmsgpack` is made to be thread-safe for Python versions that have the GIL removed. With the GIL, thread-safety is not required as only one operation can be performed at the same time. But without the GIL, `cmsgpack` makes sure to use thread-safe locks and other methods to prevent race-conditions and concurrency.

When the GIL is active, `cmsgpack` does not apply any explicit thread-safety measures because they are not required, and thread-safety measures add performance overhead. However, everything is still structured to be safe for sharing, meaning an object like the `Encoder` can be used for different data "concurrently" when the GIL is enabled.


## Questions and feedback

If you have any questions or feel like something is unclear or could be better, feel free to contact me by [mail](mailto:boertjens.sven@gmail.com) or create an issue on the [GitHub](https://github.com/svenboertjens/cmsgpack) page.