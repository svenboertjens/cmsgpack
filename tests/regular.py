import cmsgpack

test_values = [
    0, 1, 127, 255, 30000, 2**31-1, 2**48-1, 2**63-1,
    -1, -16, -128, -256, -30000, -(2**31), -(2**48), -(2**63),

    True, False, None,

    0.0, -0.0, 3.14159, -3.14159, 1.5e-45, -1.5e-45, 1.5e+308, -1.5e+308,

    "", "Hello, world!", "with\0null", "a" * 31, "a" * 32, "a" * 0xFF, "a" * 0xFFFF, "a" * 0xFFFFFF, "你好", "emoji 😊",

    b"", b"Hello, world!", b"with\0null", b"a" * 31, b"a" * 32, b"a" * 0xFF, b"a" * 0xFFFF, b"a" * 0xFFFFFF,

    [], [1, "2", 3.0, True, False, None, b"in", [], {}], ["a"] * 15, ["a"] * 0xFF, ["a"] * 0xFFFFF,

    {}, {"a": 1, "b": 2}, {"key": None, "nested": {"inner": True, "value": 3.14}},
    {"list": [1, 2, 3], "bools": [True, False]},
    
    [
        {"key": "val", "power": "rangers"},
        {"id": 2, "value": "item2", "nested": [None, 3.14, False]},
    ],
    {"this": {"is": {"a": {"nested": [1, "value"]}}}},
]

encoded = cmsgpack.encode(test_values)
decoded = cmsgpack.decode(encoded)

if (decoded == test_values):
    exit()

for i in range(len(decoded)):
    actual = test_values[i]
    created = decoded[i]
    
    if (actual != created):
        print(f"----\nShould be: ({type(actual).__name__})'{actual}'\n\nGot: ({type(created).__name__})'{created}'\n----\n")
    
