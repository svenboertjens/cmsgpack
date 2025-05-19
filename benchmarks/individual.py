import cmsgpack
import msgspec
import ormsgpack

modules = {
    "cmsgpack": {
        "encode": cmsgpack.encode,
        "decode": cmsgpack.decode
    },
    "msgspec": {
        "encode": msgspec.msgpack.encode,
        "decode": msgspec.msgpack.decode
    },
    "ormsgpack": {
        "encode": ormsgpack.packb,
        "decode": ormsgpack.unpackb
    },
}

RUNS = 10
ITERATIONS = 100



import timeit

import random
import string

random.seed(0xA1B2C3D4)

def random_string(mmin, mmax):
    return ''.join(random.choices(string.ascii_letters + string.digits, k=random.randint(mmin, mmax)))

def generate_test_values():
    return {
        "fixstr": [
            random_string(0, 31)
            for _ in range(2048)
        ],

        "str8": [
            random_string(32, 255)
            for _ in range(1024)
        ],

        "str16": [
            random_string(256, 0xFFF)
            for _ in range(256)
        ],

        "bin8": [
            bytes(random.choices(range(256), k=random.randint(0, 255)))
            for _ in range(2048)
        ],

        "bin16": [
            bytes(random.choices(range(256), k=random.randint(256, 0xFFF)))
            for _ in range(256)
        ],

        "fixint": [
            random.randint(-32, 128)
            for _ in range(4096)
        ],

        "int8": [
            x
            for _ in range(4096)
            for x in (random.randint(-0x7F, -32), random.randint(128, 0xFF))
        ],

        "int16": [
            x
            for _ in range(4096)
            for x in (random.randint(-0x7FFF, -0x80), random.randint(0x100, 0xFFFF))
        ],

        "int32": [
            x
            for _ in range(2048)
            for x in (random.randint(-0x7FFFFFFF, -0x8000), random.randint(0x10000, 0xFFFFFFFF))
        ],

        "int64": [
            x
            for _ in range(1024)
            for x in (random.randint(-0x7FFFFFFFFFFFFFFF, -0x80000000), random.randint(0x100000000, 0xFFFFFFFFFFFFFFFF))
        ],

        "float64": [
            random.uniform(-1e308, 1e308)
            for _ in range(4096)
        ],

        "fixarray": [
            [None] * 15
            for _ in range(1024)
        ],

        "array16": [
            [None] * random.randint(16, 0xFF)
            for _ in range(64)
        ],

        "fixmap": [
            {
                random_string(4, 31): None
                for _ in range(15)
            }
        ] * 256,

        "map16": [
            {
                random_string(4, 31): None
                for _ in range(16, 0xFF)
            }
        ] * 32,

        "states": [
            True, False, None
        ] * 4096,
    }


categories = [
    "fixstr",
    "str8",
    "str16",
    "bin8",
    "bin16",
    "fixint",
    "int8",
    "int16",
    "int32",
    "int64",
    "float64",
    "fixarray",
    "array16",
    "fixmap",
    "map16",
    "states"
]

for data in modules.values():
    data["sums"] = {cat: [0, 0] for cat in categories}
    data["total"] = [0, 0]


for _ in range(RUNS):
    values = generate_test_values()

    for data in modules.values():
        enc = data["encode"]
        dec = data["decode"]

        for cat, v in values.items():
            encoded = enc(v)
    
            data["sums"][cat][0] += timeit.timeit(lambda: enc(v), number=ITERATIONS)
            data["sums"][cat][1] += timeit.timeit(lambda: dec(encoded), number=ITERATIONS)


print(f"## MessagePack serialization benchmark ##")
print(f"\n{RUNS} runs, {ITERATIONS} iterations per category")

for cat in categories:
    print(f"\n\n# Category '{cat}':\n")
    print("  Module      |  Encoding     |  Decoding")
    print("--------------+---------------+--------------")

    for modname, data in modules.items():
        times = data["sums"][cat]

        data["total"][0] += times[0]
        data["total"][1] += times[1]

        print(f"  {modname:11s} |  {(str(times[0])):.9s} s  |  {(str(times[1])):.9s} s")


print(f"\n\n# Total times:\n")
print("  Module      |  Encoding     |  Decoding")
print("--------------+---------------+--------------")

for modname, data in modules.items():
    totals = data["total"]
    print(f"  {modname:11s} |  {(str(totals[0])):.9s} s  |  {(str(totals[1])):.9s} s")
