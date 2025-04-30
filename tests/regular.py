
# Test regular serialization

import cmsgpack as cm

from test_values import test_values, list_values, bytes_values
from test import Test


test = Test()

# Test all items from the test values
for v in test_values:
    if test.success(lambda: cm.decode(cm.encode(v))):
        test.equal(v, cm.decode(cm.encode(v)))

# Test the test values object itself
if test.success(lambda: cm.decode(cm.encode(test_values))):
    test.equal(test_values, cm.decode(cm.encode(test_values)))

# Test list values encoded as tuples
for item in list_values:
    item_tuple = tuple(item)

    if test.success(lambda: cm.decode(cm.encode(item_tuple))):
        test.equal(item, cm.decode(cm.encode(item_tuple)))

# Test list values encoded as bytearrays and memoryviews
for item in bytes_values:
    item_bytearray = bytearray(item)
    item_memoryview = memoryview(item)

    if test.success(lambda: cm.decode(cm.encode(item_bytearray))):
        test.equal(item, cm.decode(cm.encode(item_bytearray)))

    if test.success(lambda: cm.decode(cm.encode(item_memoryview))):
        test.equal(item, cm.decode(cm.encode(item_memoryview)))

# Test if string-keys-only is enforced when requested
test.exception(lambda: cm.encode({1: 2}, str_keys=True), TypeError)
test.exception(lambda: cm.decode(cm.encode({1: 2}), str_keys=True), TypeError)

# Test if integer overflows are caught
test.exception(lambda: cm.encode(2**64), OverflowError)
test.exception(lambda: cm.encode(-(2**63) - 1), OverflowError)

# Test NaN preservance
def test_nan():
    import math
    assert math.isnan(cm.decode(cm.encode(math.nan)))
test.success(test_nan)

# Test if cyclic references are caught
cyclic_ref = []
cyclic_ref.append(cyclic_ref)
test.exception(lambda: cm.encode(cyclic_ref), RecursionError)

# Test if unsupported types are caught
test.exception(lambda: cm.encode(2j + 3), TypeError)

# Test if decoding with a non-buffer fails
test.exception(lambda: cm.decode(123), TypeError)

# Test if invalid encoded data is caught
test.exception(lambda: cm.decode(b"\0\0"), ValueError)
test.exception(lambda: cm.decode(b""), ValueError)
test.exception(lambda: cm.decode(cm.encode("abcde")[0:1]), ValueError)

# Test if invalid argument types are caught
test.exception(lambda: cm.encode(None, extensions=123), TypeError)
test.exception(lambda: cm.decode(b" ", extensions=123), TypeError)

# Test if invalid kwargs are caught
test.exception(lambda: cm.encode(None, invalid_kwarg=123), TypeError)
test.exception(lambda: cm.decode(b" ", invalid_kwarg=123), TypeError)

# Test if valid args are accepted
test.success(lambda: cm.encode(None,  extensions=cm.Extensions()))
test.success(lambda: cm.decode(b"\0", extensions=cm.Extensions()))


test.print()

