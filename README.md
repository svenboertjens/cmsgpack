# `cmsgpack`

**Contents:**
- [Introduction](#introduction)
- [Installation](#installation)
- [Quick start](#quick-start)
- [GIL-free Python](#gil-free-python)
- [Questions/feedback](#questions-and-feedback)
- [License](#license)

## Introduction

`cmsgpack` is a [MessagePack](https://msgpack.org/) serializer for Python. For those who are unfamiliar with those terms, serialization means converting Python objects to bytes here. The format used for preserving the data is MessagePack.

But why use `cmsgpack`, and not an existing solution like `msgpack`, or even an optimized solution like `msgspec` or `ormsgpack`? While these options are already great, `cmsgpack` aims to be even better by offering a **development-friendly API** with many useful features, **extremely fast serialization**, and **optimized memory usage**.

`cmsgpack` uses smart caching and adaptive allocation strategies to reduce allocations where possible, which is beneficial for performance, but also for long-running processes to slow down the fragmentation of memory.

For performance benchmarks, see the [benchmark script](benchmarks/benchmark.py), which shows a serialization speed comparison between `cmsgpack`, `ormsgpack`, and `msgspec`.


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

Need to know just the basics? Here's a quick start guide! For more detailed and comprehensive documentation, see the [usage](USAGE.md) file.

For encoding our Python objects, we use the function `encode`, and for decoding data we use the function `decode`. These are the (simplified) function signatures of these functions:

```python
cmsgpack.encode(obj: any) -> bytes
cmsgpack.decode(encoded: bytes) -> any
```

The argument `obj` is our Python object. It can be any of the supported types. The argument `encoded` is a bytes object of encoded data. Here, "encoded data" means any serialized data in MessagePack format, like what we receive from the `encode` function.

Here is a basic example on how to use these functions:

```python
import cmsgpack

# This is the value we want to encode
obj = ["Hello", "world!"]

# We can encode it using `encode`
encoded = cmsgpack.encode(obj)

# Your code...
...

# We can decode it again using `decode`
decoded = cmsgpack.decode(encoded)

# Now, `decoded` is a copy of `obj`
assert obj == decoded # True
```

The basic supported types are:
- `str`
- `int`
- `float`
- `bool`
- `NoneType`
- `list`
- `dict`

For more information on "non-basic" support types, consult the [supported types](USAGE.md#supported-types) section from the usage file.


## GIL-free Python

This module is compatible with the GIL-free variant of Python. Concurrency concerns are addressed and prevented through atomic locks. These locks are only used when the GIL is removed, as they have a (very small) effect on performance. This only applies to areas where locks are necessary and not just "to be sure", such as on internal caches or in class objects.

Please do note that classes of type `Stream` and `FileStream` have internal locks for safety, and these classes should not be used across threads. When an object is in use, it is locked, stopping all other threads from using it. So for multi-threaded cases with the GIL removed, please use a separate class instance per thread.


## Questions and feedback

If you have any questions or feel like something is unclear or could be better, feel free to contact me by [mail](mailto:boertjens.sven@gmail.com) or create an issue on the [GitHub](https://github.com/svenboertjens/cmsgpack) page.


# License

This library is licensed under the [MIT license]. See the [license file](LICENSE).