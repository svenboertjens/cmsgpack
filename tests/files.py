
# Test file-based serialization

import cmsgpack as cm

from test_values import test_values
from test import Test

import os


FNAME = "files_test.bin"

test = Test()


# Test if the stream object can be created
if not test.success(lambda: cm.Stream(file_name=FNAME)):
    test.print()
    exit()

# The object to use for streaming
stream = cm.Stream(file_name=FNAME)
enc = stream.encode
dec = stream.decode

# Test if we can create another instance towards the same file
test.success(lambda: cm.Stream(file_name=FNAME))

# Test if data can be written and read normally
wrote = test.success(lambda: enc(123))
read  = test.success(lambda: dec())

# Early exit if simple reading/writing fails
if not wrote or not read:
    try:
        import os
        os.remove(FNAME)
    except:
        pass

    test.print()
    exit()

# Test if written data can be read properly
enc(123)
if not test.equal(123, dec()):
    # Early exit if the read+decoded data isn't equal
    test.print()
    exit()

# Test if the same data will be read if we open a separate object
enc(456) # Encode value on top of the `123` to see if we don't get `456`
dec()    # The decode call ensures the internal offset is updated for further tests
if test.success(lambda: cm.Stream(file_name=FNAME).decode()):
    test.equal(123, cm.Stream(file_name=FNAME).decode())

# Test all items from the test values
for v in test_values:
    if test.success(lambda: enc(v)) and test.success(lambda: dec()):
        enc(v)
        test.equal(v, dec())

# Test the test values object itself
if test.success(lambda: enc(test_values)) and test.success(lambda: dec()):
    enc(test_values)
    test.equal(test_values, dec())


test.print()

os.remove(FNAME)

