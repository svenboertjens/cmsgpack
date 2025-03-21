
# Test extension type serialization

import cmsgpack as cm

from test import Test


# Functions for custom type encoding/decoding

def encode_complex(obj: complex):
    return str(obj).encode()

def decode_complex(b: bytes):
    if not isinstance(b, bytes):
        raise TypeError("Did not receive a bytes object!")

    return complex(b.decode())

class MyClass:
    obj: str
    def __init__(self, obj):
        self.obj = obj
    
    def __eq__(self, other):
        return isinstance(other, MyClass) and other.obj == self.obj

def encode_myclass(obj: MyClass):
    return obj.obj.encode()

def decode_myclass(b: bytes):
    return MyClass(b.decode())


ID_COMPLEX = 1
ID_MYCLASS = 2

ext_types = {
    ID_COMPLEX: (complex, encode_complex, decode_complex),
    ID_MYCLASS: (MyClass, encode_myclass, decode_myclass)
}


test = Test()


# Early exit if the object couldn't be created
if not test.success(lambda: cm.Extensions(ext_types)):
    test.print()
    exit()

# The ext object to use
ext = cm.Extensions(ext_types)

# Test if invalid types are caught
test.exception(lambda: cm.Extensions(types=123), TypeError)
test.exception(lambda: cm.Extensions(allow_subclasses=123), TypeError)
test.exception(lambda: cm.Extensions(pass_memoryview=123), TypeError)

test.exception(lambda: ext.add(None, str, lambda: None, lambda: None), TypeError)
test.exception(lambda: ext.add(1, 123, lambda: None, lambda: None), TypeError)
test.exception(lambda: ext.add(1, str, 123, lambda: None), TypeError)
test.exception(lambda: ext.add(1, str, lambda: None, 123), TypeError)

test.exception(lambda: ext.add_encode(None, str, lambda: None), TypeError)
test.exception(lambda: ext.add_encode(1, 123, lambda: None), TypeError)
test.exception(lambda: ext.add_encode(1, str, 123), TypeError)

test.exception(lambda: ext.add_decode(None, lambda: None), TypeError)
test.exception(lambda: ext.add_decode(1, 123), TypeError)

# Test if invalid IDs are caught
test.exception(lambda: cm.Extensions({1234: (str, lambda: None, lambda: None)}), ValueError)
test.exception(lambda: ext.add(1234, str, lambda: None, lambda: None), ValueError)
test.exception(lambda: ext.add_encode(1234, str, lambda: None), ValueError)
test.exception(lambda: ext.add_decode(1234, lambda: None), ValueError)

# Test if an incorrect number of arguments is caught
test.exception(lambda: ext.add(), TypeError)
test.exception(lambda: ext.add(1, 2, 3, 4, 5), TypeError)
test.exception(lambda: ext.add_encode(), TypeError)
test.exception(lambda: ext.add_encode(1, 2, 3, 4, 5), TypeError)
test.exception(lambda: ext.add_decode(), TypeError)
test.exception(lambda: ext.add_decode(1, 2, 3, 4, 5), TypeError)

# Test if invalid kwargs are caught
test.exception(lambda: cm.Extensions(false_kwarg=123), TypeError)

# Test if the objects can be encoded and decoded
test_value = [
    2j + 3, 3j + 2,
    [MyClass("a" * i) for i in range(17)]
]

if test.success(lambda: cm.decode(cm.encode(test_value, extensions=ext), extensions=ext)):
    test.equal(test_value, cm.decode(cm.encode(test_value, extensions=ext), extensions=ext))

# Create a new ext object, but add encoding and decoding through the functions
ext = cm.Extensions()

# Add Complex through ADD and MyClass through ADD_ENCODE/ADD_DECODE
success_add = test.success(lambda: ext.add(ID_COMPLEX, complex, encode_complex, decode_complex))
success_add = test.success(lambda: ext.add_encode(ID_MYCLASS, MyClass, encode_myclass)) and success_add
success_add = test.success(lambda: ext.add_decode(ID_MYCLASS, decode_myclass)) and success_add

# Only test the new ext object if we added stuff successfully
if success_add:
    if test.success(lambda: cm.decode(cm.encode(test_value, extensions=ext), extensions=ext)):
        test.equal(test_value, cm.decode(cm.encode(test_value, extensions=ext), extensions=ext))

# Test if we can add ext data to the global ext object
if not test.success(lambda: cm.extensions.add(ID_COMPLEX, complex, encode_complex, decode_complex)):
    test.print()
    exit()

# Test if it can encode and decode a complex number using the global ext objects
test.success(lambda: cm.decode(cm.encode(2j + 3)))

# Test if it fails correctly on MyClass, which we didn't add
test.exception(lambda: cm.encode(MyClass("abc")), TypeError)

# Test if an encoding function returning a non-buffer is caught
cm.extensions.add_encode(ID_COMPLEX, complex, lambda x: 123)
test.exception(lambda: cm.encode(2j + 3), TypeError)

# Test if entries can be removed
cm.extensions.remove(ID_COMPLEX, complex)
if test.exception(lambda: cm.encode(2j + 3), TypeError) or not test.exception(lambda: cm.decode(cm.encode(2j + 3, extensions=ext)), TypeError):
    # Test if ID mismatches are caught
    cm.extensions.add_encode(ID_COMPLEX, complex, encode_complex)
    cm.extensions.add_decode(ID_COMPLEX + 1, decode_complex)
    test.exception(lambda: cm.decode(cm.encode(2j + 3)), TypeError)

    # Test if entries can be removed separately as well
    cm.extensions.remove(ID_COMPLEX + 1, complex)
    cm.extensions.add(ID_COMPLEX, complex, encode_complex, decode_complex)
    cm.extensions.remove_encode(complex)
    cm.extensions.remove_decode(ID_COMPLEX)

    test.exception(lambda: cm.encode(2j + 3), TypeError)
    test.exception(lambda: cm.decode(cm.encode(2j + 3, extensions=ext)), TypeError)

# Test if the object can be cleared
cm.extensions.add(ID_COMPLEX, complex, encode_complex, decode_complex)
cm.extensions.clear()
test.exception(lambda: cm.encode(2j + 3), TypeError)
test.exception(lambda: cm.decode(cm.encode(2j + 3, extensions=ext)), TypeError)

# Test if the object handles `pass_memview` properly
ext = cm.Extensions({ID_COMPLEX: (complex, encode_complex, decode_complex)}, pass_memoryview=True)
test.exception(lambda: cm.decode(cm.encode(2j + 3, extensions=ext), extensions=ext), TypeError)


test.print()

