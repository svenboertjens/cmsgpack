
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
test.exception(lambda: cm.decode(123), TypeError)

# Test if invalid encoded data is caught
test.exception(lambda: cm.decode(b"\0\0"), ValueError)
test.exception(lambda: cm.decode(b""), ValueError)

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

