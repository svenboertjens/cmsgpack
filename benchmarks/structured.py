import cmsgpack
import msgspec
import ormsgpack

cmsgpack_stream = cmsgpack.Stream()

msgspec_encoder = msgspec.msgpack.Encoder()
msgspec_decoder = msgspec.msgpack.Decoder()

modules = {
    "cmsgpack": {
        "encode": cmsgpack_stream.encode,
        "decode": cmsgpack_stream.decode
    },
    "msgspec": {
        "encode": msgspec_encoder.encode,
        "decode": msgspec_decoder.decode
    },
    "ormsgpack": {
        "encode": ormsgpack.packb,
        "decode": ormsgpack.unpackb
    },
}

RUNS = 100
ITERATIONS = 10000



import timeit

import random
import string

random.seed(0xA1B2C3D4)

def random_string(mmin, mmax):
    return ''.join(random.choices(string.ascii_letters + string.digits, k=random.randint(mmin, mmax)))

def generate_test_values():
    return {
        "user": {
            "id": random.randint(1, 1_000_000),
            "name": random_string(5, 12),
            "email": random_string(10, 15) + "@example.com",
            "verified": random.choice([True, False]),
            "preferences": {
                "theme": random.choice(["light", "dark"]),
                "notifications": {
                    "email": random.choice([True, False]),
                    "sms": random.choice([True, False]),
                    "push": random.choice([True, False])
                },
            },
        },
        "session": {
            "token": random_string(24, 32),
            "ip": ".".join(str(random.randint(0, 255)) for _ in range(4)),
            "created_at": random.randint(1600000000, 2000000000),
            "tags": [random_string(4, 8) for _ in range(5)],
        },
        "events": [
            {
                "type": random.choice(["click", "view", "purchase"]),
                "timestamp": random.randint(1600000000, 2000000000),
                "metadata": {
                    "value": random.uniform(0, 100),
                    "label": random_string(6, 12),
                }
            }
            for _ in range(10)
        ]
    }


for _ in range(RUNS):
    values = generate_test_values()

    for data in modules.values():
        enc = data["encode"]
        dec = data["decode"]

        data["times"] = [0, 0]

        for cat, v in values.items():
            encoded = enc(v)
    
            data["times"][0] += timeit.timeit(lambda: enc(v), number=ITERATIONS)
            data["times"][1] += timeit.timeit(lambda: dec(encoded), number=ITERATIONS)


print(f"## MessagePack serialization benchmark ##")
print(f"\n{RUNS} generated values, {ITERATIONS} iterations per value\n")

print("  Module      |  Encoding     |  Decoding")
print("--------------+---------------+--------------")

for modname, data in modules.items():
    times = data["times"]
    print(f"  {modname:11s} |  {(str(times[0])):.9s} s  |  {(str(times[1])):.9s} s")
