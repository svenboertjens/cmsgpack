
# Test Stream-based serialization

import cmsgpack as cm

from test_values import test_values
from test import Test


FNAME = "stream_test.bin"

test = Test()


# Early exit if we cannot create
if not test.success(lambda: cm.Stream()):
    test.print()
    exit()

# Classes to use for tests
stream = cm.Stream()
enc = stream.encode
dec = stream.decode

# Test the test values object
if test.success(lambda: dec(enc(test_values))):
    test.equal(test_values, dec(enc(test_values)))

# Test if string-keys-only is enforced when requested
test.exception(lambda: enc({1: 2}, str_keys=True), TypeError)
test.exception(lambda: dec(enc({1: 2}), str_keys=True), TypeError)

# Test if unsupported types are caught properly
test.exception(lambda: enc(2j + 3), TypeError)

# Test if non-buffer objects are caught
test.exception(lambda: dec(123), TypeError)

# Test if redundant arguments are accepted
test.exception(lambda: enc(None,  123), TypeError)
test.exception(lambda: dec(b"\0", 123), TypeError)

# Test if invalid argument types are caught
test.exception(lambda: cm.Stream(ext_types=123), TypeError)

# Test if invalid kwargs are caught
test.exception(lambda: cm.Stream(invalid_kwarg=123), TypeError)

# Test if valid args are accepted
test.success(lambda: cm.Stream(extensions=cm.Extensions()))


test.print()

