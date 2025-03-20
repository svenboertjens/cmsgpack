
# Test class-based serialization

import cmsgpack as cm

from test_values import test_values
from test import Test


FNAME = "classes_test.bin"

test = Test()


# Test if the classes can be created
created_enc = test.success(lambda: cm.Encoder())
created_dec = test.success(lambda: cm.Decoder())

# Early exit if they cannot be created at all
if not created_enc or not created_dec:
    test.print()
    exit()

# Classes to use for value tests
enc = cm.Encoder().encode
dec = cm.Decoder().decode

# Test the test values object itself
if test.success(lambda: dec(enc(test_values))):
    test.equal(test_values, dec(enc(test_values)))

# Test if unsupported types are caught properly
test.exception(lambda: enc(2j + 3), TypeError)

# Test if non-buffer objects are caught
test.exception(lambda: dec(123), BufferError)

# Test if redundant arguments are accepted
test.exception(lambda: enc(None,  123), TypeError)
test.exception(lambda: dec(b"\0", 123), TypeError)

# Test if invalid argument types are caught
test.exception(lambda: cm.Encoder(ext_types=123), TypeError)
test.exception(lambda: cm.Decoder(ext_types=123), TypeError)

test.exception(lambda: cm.Encoder(file_name=123), TypeError)
test.exception(lambda: cm.Decoder(file_name=123), TypeError)

# Test if invalid kwargs are caught
test.exception(lambda: cm.Encoder(invalid_kwarg=123), TypeError)
test.exception(lambda: cm.Decoder(invalid_kwarg=123), TypeError)

# Test if valid args are accepted
test.success(lambda: cm.Encoder(extensions=cm.Extensions(), file_name=FNAME))
test.success(lambda: cm.Decoder(extensions=cm.Extensions(), file_name=FNAME))


test.print()

try:
    import os
    os.remove(FNAME)

except:
    pass

