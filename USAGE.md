# `cmsgpack` usage details

This file explains how to use the `cmsgpack` module in Python.

!! THIS FILE IS NOT FINISHED AND MAY HAVE OUTDATED INFORMATION !!


## Introduction

`cmsgpack` is a [MessagePack](https://msgpack.org/) serializer for Python. For those who are unfamiliar with those terms, serialization means converting Python objects to bytes (in this case), which is done according to the MessagePack structure.

But why use `cmsgpack`, and not existing solutions like the default `msgpack` or the more optimized `msgspec` and `ormsgpack`? While these are already great solutions, `cmsgpack` aims to be *even better*. Apart from offering an easy API with unique but useful features, this library does not slack off at efficiency. In fact, `cmsgpack` is often **faster** than others! Not to mention the library stays lightweight, minimizing both the storage space of the library itself and the memory usage during runtime.


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


For your convenience, we also support the following types:


* `dict` subclasses (`class abc(dict)`):

Any object created as a subclass from `dict` is accepted.

* `list` subclasses (`class abc(list)`):

Any object created as a subclass from `list` is accepted.

* `tuple` objects:

The standard type `tuple` is supported and treated as a `list` object. This also includes objects that are a subclass of type `tuple`.

* `bytearray` & `memoryview` objects:

The standard types `bytearray` and `memoryview` are supported and treated as a `bytes` object.

> [Note]
>
> These types have to be converted into regular, MessagePack-supported types. This means that a `set` will be encoded as if it were a `list` object, for example.

If this still does not include a type you need, then maybe extension types will make you happy! Extension types are part of MessagePack by default, so they are compatible with other MessagePack parsers. These are explained in detail [here](#extension-types).


## Encoding

Encoding data to a `bytes` object.

### `encode`

```python
cmsgpack.encode(obj: any, /, ext_types: ExtTypesEncode=None, strict_keys: bool=False) -> bytes
```

*"Encode Python data to bytes."*

**Arguments:**
- `obj`: The object to encode. This can be any of the [supported types](#supported-types).
- `ext_types`: An [extension types](#extension-types) object for encoding custom objects.
- `strict_keys`: If `True`, only strings are allowed as map keys.

**Returns:** The encoded data in a `bytes` object.


### `Encoder`

```python
cmsgpack.Encoder(file_name: str|None=None, ext_types: ExtTypesEncode=None, strict_keys: bool=False) -> Encoder
```

*"Class-based wrapper for `encode()` that retains optional arguments."*

**Arguments:**
- `file_name`: The path to the file to stream data to.
- `ext_types`: An [extension types](#extension-types) object for encoding custom objects.
- `strict_keys`: If `True`, only strings are allowed as map keys.

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
cmsgpack.decode(encoded: Buffer, /, ext_types: ExtTypesDecode=None, strict_keys: bool=False) -> any:
```

*"Decode any MessagePack-encoded data."*

**Arguments:**
- `encoded`: A buffer object that holds the encoded data. Buffers are `bytes`, `bytearray`, or a custom type that supports the buffer protocol.
- `ext_types`: An [extension types](#extension-types) object for encoding custom objects.
- `strict_keys`: If `True`, only strings are allowed as map keys.

**Returns:** The decoded Python object.


### `Decoder`

```python
cmsgpack.Decoder(file_name: str|None=None, ext_types: ExtTypesDecode=None, strict_keys: bool=False) -> Decoder
```

*"Class-based wrapper for `decode()` that retains optional arguments."*

**Arguments:**
- `file_name`: The path to the file to read data from.
- `ext_types`: An [extension types](#extension-types) object for decoding custom objects.
- `strict_keys`: If `True`, only strings are allowed as map keys.

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

Extension types (ext types) are used for serializing unsupported types (types not listed [here](#supported-types)). Each extension type requires a unique identifier between -128 and 127, giving us room for 256 distinct types in total.

With `cmsgpack`, ext types are handled using the `ExtTypes` objects. Encoding and decoding use separate objects, `ExtTypesEncode` and `ExtTypesDecode`, respectively. These objects allow a separate function per ext type, giving us a more modular approach compared to the traditional `ext_hook` method, which uses a monolithic function that has to manually type check each object before converting it.

With `cmsgpack`'s approach, each function is dedicated to a single ext type, removing the need to type check each object as a function is *guaranteed* to get the correct type.

> [Note]
>
> It is important that you consistently assign **the same ID** to an ext type while encoding and decoding. Otherwise, encoded extension types will not match the correct decoding function.


## Questions

If you have any questions or feel like something is unclear or could be better, feel free to contact me by [mail](mailto:boertjens.sven@gmail.com) or create an issue on the [GitHub](https://github.com/svenboertjens/cmsgpack) page.