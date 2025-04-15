# `cmsgpack`

**Contents:**
- [Introduction](#introduction)
- [Installation](#installation)
- [Quick start](#quick-start)
- [GIL-free Python](#gil-free-python)
- [Questions/feedback](#questions-and-feedback)
- [License](#license)

## Introduction

`cmsgpack` is a serializer for Python. It supports and uses the [MessagePack](https://msgpack.org/) format, as the name may have given away already.

But why use `cmsgpack`, and not an existing solution like `msgpack`, or even an optimized solution like `msgspec` or `ormsgpack`? While these options are already great, `cmsgpack` aims to be even better by offering a **development-friendly API** with many useful features, **extremely fast serialization**, and **optimized memory usage**.

`cmsgpack` uses smart caching techniques and adaptive allocation to reduce the number of memory allocations, which is beneficial for not only performance, but also for memory fragmentation, which can make performance suffer in the long run.

For performance benchmarks, see the [benchmark script](benchmarks/benchmark.py) for a benchmark comparison between `cmsgpack`, `ormsgpack`, and `msgspec`.


## Installation

The Python module is available via **pip**. To install it, you can use this command:

```shell
pip install cmsgpack
```

If you want to manually install it from the source, you can use **git**. Do note that this method might install a version with unfinished or unstable changes.

```shell
git clone https://github.com/svenboertjens/cmsgpack.git
cd cmsgpack
pip install .
```

> [Note]
>
> The module is distributed as a "source distribution" (sdist) package, meaning it must be compiled during installation. This requires a C compiler (with C11 support). Recommended compilers are **GCC** and **Clang**. **MSVC is not supported** due to its lack of standards compliance.


## Quick start

Need just the basics? Here's a quick start guide! For the complete guide, see the [usage](USAGE.md) file.

For encoding our Python objects, we use the function `encode`, and for decoding data we use the function `decode`. These are the (simplified) function signatures of these functions:

For encoding Python objects, we use the function `encode`. This function accepts any of the [supported types](USAGE.md#supported-types) and returns a `bytes` object with the encoded data.
For decoding any MessagePack-encoded data (like what `encode` returns), we use the function `decode`. This function accepts the encoded data and returns the decoded Python object.

These are the simplified function signatures of the functions:

```python
cmsgpack.encode(obj: any) -> bytes
cmsgpack.decode(encoded: bytes) -> any
```

For `encode`, the argument `obj` is the Python object to encode.
For `decode`, the argument `encoded` is the object holding the encoded data.

Here is a basic example on how these functions are used:

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


## GIL-free Python

This module is compatible with the GIL-free Python version. Any concurrency hazards are addressed with thread-local variables and atomic locks, to avoid race conditions or corruption. As these measures have a small performance cost, they are only enabled when `Py_NOGIL` is declared by Python, saving the performance cost when it's not needed.

If you plan to use the `FileStream` class in a GIL-free Python build, it is recommended to use a separate instance per thread. This object is locked internally when in use, so any other threads attempting to use it will be halted indefinitely.


## Questions and feedback

If you have any questions or feedback, feel free to contact me by [mail](mailto:boertjens.sven@gmail.com) or create an issue on the [GitHub](https://github.com/svenboertjens/cmsgpack) page.


# License

This library is licensed under the MIT license. See the [license file](LICENSE).

