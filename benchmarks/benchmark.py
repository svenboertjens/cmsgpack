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

RUNS = 100
ITERATIONS = 1000



import timeit

import random
import string

def generate_test_values():
    return {
        "strings": [
            ''.join(random.choices(string.ascii_letters + string.digits, k=random.randint(1, 256)))
            for _ in range(256)
        ],

        "bytes": [
            bytes(random.choices(range(256), k=random.randint(1, 256)))
            for _ in range(256)
        ],

        "integers": [
            random.choice([random.randint(-2**31, 2**31), random.randint(0, 1024), random.randint(2**32, 2**48)])
            for _ in range(1024)
        ],

        "floats": [
            float(i) for i in range(256)
        ],

        # Lists with varied nesting structures
        "lists": [
            random.sample(range(100), k=random.randint(5, 15)),
            [random.randint(1, 10), [random.randint(11, 20), [random.randint(21, 30)]]],
            [random.choices(range(50), k=random.randint(3, 8)) for _ in range(3)]
        ] * 5,

        "dicts": [
            {str(random.randint(1, 10)): {str(random.randint(11, 20)): {str(random.randint(21, 30)): {}}}}
            for _ in range(5)
        ] * 5,

        "states": [
            True, False, None
        ] * 500,
    }


categories = [
    "strings",
    "bytes",
    "integers",
    "floats",
    "lists",
    "dicts",
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


print(f"## MessagePack serialization benchmarking ##")
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

