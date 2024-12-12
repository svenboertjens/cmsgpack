import cmsgpack as cm

class Cstr:
    def __init__(self, val):
        self.val = val


def enc_cstr(obj):
    return 1, obj.val.encode()

def dec_cstr(bytes):
    return Cstr(bytes.decode())


# Test regularly


enc = cm.ExtTypesEncode({
    Cstr: enc_cstr
})
dec = cm.ExtTypesDecode({
    1: dec_cstr
})


test_values = [
    Cstr("a" * 0xFF),
    Cstr("a" * 0x100),
    Cstr("a" * 0xFFFF),
    Cstr("a" * 0x10000),
    Cstr("a" * 0xFFFFFFF),
]

chars = "abcdefghijklmnopqrstuvwxyz"
for i in range(1, 27):
    test_values.append(Cstr(chars[:i]))


encoded = cm.encode(test_values, ext_types=enc)
decoded = cm.decode(encoded, ext_types=dec)

# Single encoded value for later tests
encoded_one = cm.encode(Cstr("abcde"), ext_types=enc)

for i, item in enumerate(decoded):
    if test_values[i].val != item.val:
        print(f"Failed on index {i}")


# Test if wrong ID is caught


wrong_dec = cm.ExtTypesDecode({
    2: dec_cstr
})

try:
    cm.decode(encoded_one, ext_types=wrong_dec)
except ValueError:
    pass
except e:
    print(e)


# Test if memoryview is received instead of bytes


def dec_cstr_memview(view):
    if not isinstance(view, memoryview):
        print("Didn't get memoryview object")
    
    return 0

memview_dec = cm.ExtTypesDecode({
    1: dec_cstr_memview
}, memoryview)

cm.decode(encoded_one, ext_types=memview_dec)

