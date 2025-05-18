# `cmsgpack`


## Introduction

`cmsgpack` is an optimized MessagePack implementation for Python, offering high performance and a developer-friendly API.

But why use `cmsgpack`, and not the default `msgpack`? Well, `cmsgpack`...
- is optimized for **high-performance serialization** and uses heuristics-based allocation to avoid reallocations and memory waste where possible;
- supports native, chunked file streaming for efficient large file operations;
- provides unique features that make the API more intuitive and flexible.

If you're looking for something specific, see these:
- :rocket: **Quick Start**: For a quick look on how to use `cmsgpack`, see the [Quick Start](#quick-start).
- :wrench: **API & Usage**: For details on `cmsgpack`'s API and how to use it, see [USAGE](USAGE.md).
- :watch: **Benchmarks**: For benchmark comparisons, see [BENCHMARK](BENCHMARK.md).
- :information_source: **Compatibility**: For compatibility details, see [Compatibility](#compatibility)


## Quick Start

This section gives a basic example of how to install and use `cmsgpack`. See [USAGE](USAGE.md) for the complete documentation.

### Installation

`cmsgpack` can be installed using **pip**:

```shell
pip install cmsgpack
```


### Basic Serialization

To serialize data, use `cmsgpack.encode` and `cmsgpack.decode`:

```python
cmsgpack.encode(obj: any) -> bytes
cmsgpack.decode(encoded: bytes) -> any
```

An example of how these functions can be used:

```python
import cmsgpack

# Any value we want to encode
obj = "Hello, world!"

# Encode it using `cmsgpack.encode`:
encoded = cmsgpack.encode(obj)

# `encoded` now holds our encoded data as bytes

# Decode it again using `cmsgpack.decode`:
decoded = cmsgpack.decode(encoded)

# `decoded` now holds the original value, `obj`
assert obj == decoded # True
```

If you are unsure if a type is supported, check the [supported types](USAGE.md#supported-types) to see if it's mentioned there, or what to do otherwise.


## Compatibility

- **Compilers**: Supports all compilers compatible with C11. When building from source, ensure a compliant compiler is used.
- **Endianness**: Supports both big-endian and little-endian systems. Endianness is determined at compile-time; runtime changes are not supported.
- **Word size**: Tested on 64-bit systems; 32-bit systems are expected to work but are not officially tested.
- **Multi-Threading**: Internal thread safety is ensured for no-GIL Python builds. All class objects (`Stream`, `FileStream`, and `Extemsions`) must not be used concurrently across threads.


## Questions & Feedback

Have a question, issue, or idea? Feel free to [open an issue](https://github.com/svenboertjens/cmsgpack/issues) or [email me](mailto:boertjens.sven@gmail.com).


## License

This library is licensed under the MIT license. See [LICENSE](LICENSE).

