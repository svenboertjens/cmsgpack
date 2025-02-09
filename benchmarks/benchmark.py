import cmsgpack
import msgspec
import ormsgpack

import timeit

values = {
    "strings":  ['a' * n for n in range(256)],
    "bytes":    [b'a' * n for n in range(256)],
    "integers": [n for n in range(1024)],
    "floats":   [float(n) for n in range(256)],
    "lists":    [1, [2, [3, [4, [5, [6, [7, [8, []]]]]]]]] * 5,
    "tuples":   (1, (2, (3, (4, (5, (6, (7, (8, ())))))))) * 5,
    "dicts":    [{'1': {'2': {'3': {'4': {'5': {'6': {'7': {'8': {}}}}}}}}}] * 5,
    "states":   [True, False, None] * 100,
}

ITERATIONS = 100_000

def bench(name, val, enc, dec):
    encoded = enc(val)
    
    enc_time = timeit.timeit(lambda: enc(val), number=ITERATIONS)
    dec_time = timeit.timeit(lambda: dec(encoded), number=ITERATIONS)
    
    print(f"  {name:9s} |  {(str(enc_time)):.9s} s  |  {(str(dec_time)):.9s} s")

def benchall(name, val):
    print(f"\n\n# Category '{name}':\n")
    print("  Module    |  Encoding     |  Decoding")
    print("------------+---------------+--------------")
    
    bench("Cmsgpack", val, cmsgpack.encode, cmsgpack.decode)
    bench("Msgspec", val, msgspec.msgpack.encode, msgspec.msgpack.decode)
    bench("Ormsgpack", val, ormsgpack.packb, ormsgpack.unpackb)


print(f"## MessagePack serialization benchmarking ##")
print(f"\n{ITERATIONS} iterations per category")


for name, val in values.items():
    benchall(name, val)

benchall("All combined", values)

