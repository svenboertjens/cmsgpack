import cmsgpack
import msgspec
import ormsgpack

import timeit

import uuid
from datetime import datetime

# Test value
v = {
    "users": [
        {
            "id": str(uuid.uuid4()),
            "username": "johndoe",
            "email": "johndoe@example.com",
            "signup_date": datetime.now().isoformat(),
            "is_active": True,
            "preferences": {
                "theme": "dark",
                "notifications": {"email": True, "sms": False, "push": True},
            },
            "roles": ["user", "admin"],
            "tags": ["example", "test", "realworld"],
        },
        {
            "id": str(uuid.uuid4()),
            "username": "janedoe",
            "email": "janedoe@example.com",
            "signup_date": datetime.now().isoformat(),
            "is_active": False,
            "preferences": {
                "theme": "light",
                "notifications": {"email": False, "sms": True, "push": False},
            },
            "roles": ["user"],
            "tags": ["production", "sample"],
        },
    ],
    "settings": {
        "max_users": 1000,
        "enable_feature_x": True,
        "server_time": datetime.now().isoformat(),
        "limits": {"daily": 100, "monthly": 3000},
    },
    "events": [
        {
            "event_id": str(uuid.uuid4()),
            "name": "LoginAttempt",
            "timestamp": datetime.now().timestamp(),
            "details": {"ip": "192.168.1.1", "success": True},
        },
        {
            "event_id": str(uuid.uuid4()),
            "name": "PasswordChange",
            "timestamp": datetime.now().timestamp(),
            "details": {"ip": "192.168.1.2", "success": False, "reason": "InvalidToken"},
        },
    ],
    "stats": {
        "active_users": 153,
        "inactive_users": 47,
        "traffic": {
            "2024-01-01": 5000,
            "2024-01-02": 7500,
            "2024-01-03": 6200,
        },
    },
}

v = [n for n in range(1000)]

"""
On a small number of iterations, the benchmark times can appear
as a few seconds. This is due to the integer being converted to
a string and then shortened, which leaves away the `e-...` part.

"""

ITERATIONS = 100_000_0

def bench(name, enc, dec):
    enc_time = timeit.timeit(enc, number=ITERATIONS)
    dec_time = timeit.timeit(dec, number=ITERATIONS)
    
    print(f"| {name:10s} |  {(str(enc_time)):.9s} s |  {(str(dec_time)):.9s} s |")

b_cmsgpack = cmsgpack.encode(v)
b_msgspec = msgspec.msgpack.encode(v)
b_ormsgpack = ormsgpack.packb(v)

print(f"\n# MessagePack serialization benchmarks:\n ({ITERATIONS} iterations)\n")
print("#-- Module --#-- Encoding --#-- Decoding --#")

bench("Cmsgpack", lambda: cmsgpack.encode(v), lambda: cmsgpack.decode(b_cmsgpack))
bench("Msgspec", lambda: msgspec.msgpack.encode(v), lambda: msgspec.msgpack.decode(b_msgspec))
bench("Ormsgpack", lambda: ormsgpack.packb(v), lambda: ormsgpack.unpackb(b_ormsgpack))

print("#-+--------+-#-+----------+-#-+----------+-#")

