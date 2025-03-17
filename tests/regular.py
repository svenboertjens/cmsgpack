
# Test regular serialization

import cmsgpack as cm

from test_values import test_values
from test import Test


test = Test()

# Test all items from the test values
for v in test_values:
    if test.success(lambda: cm.decode(cm.encode(v))):
        test.equal(v, cm.decode(cm.encode(v)))

# Test the test values object itself
if test.success(lambda: cm.decode(cm.encode(test_values))):
    test.equal(test_values, cm.decode(cm.encode(test_values)))

# Test if cyclic references are caught
cyclic_ref = []
cyclic_ref.append(cyclic_ref)
test.exception(lambda: cm.encode(cyclic_ref), RecursionError)

# Test if unsupported types are caught
test.exception(lambda: cm.encode(2j + 3), TypeError)

# Test if non-buffer objects are caught
test.exception(lambda: cm.decode(123), BufferError)

# Test if invalid encoded data is caught
test.exception(lambda: cm.decode(b"\x00\x00"), ValueError)
test.exception(lambda: cm.decode(b""), ValueError)

# Test if invalid argument types are caught
test.exception(lambda: cm.encode(None, ext_types=123), TypeError)
test.exception(lambda: cm.decode(b" ", ext_types=123), TypeError)

# Test if invalid kwargs are caught
test.exception(lambda: cm.encode(None, invalid_kwarg=123), TypeError)
test.exception(lambda: cm.decode(b" ", invalid_kwarg=123), TypeError)

# Test if valid args are accepted
test.success(lambda: cm.encode(None,  ext_types=cm.ExtTypesEncode({1: (str, lambda: None)})))
test.success(lambda: cm.decode(b"\0", ext_types=cm.ExtTypesDecode({1: lambda: None})))


test.print()

