
# Test FileStream-based serialization

import cmsgpack as cm

from test_values import test_values
from test import Test

import os


FNAME = "files_test.bin"

test = Test()


# Test if the stream object can be created
if not test.success(lambda: cm.FileStream(FNAME)):
    test.print()
    exit()

# Test if invalid argument types are caught
test.exception(lambda: cm.FileStream(file_name=123), TypeError)
test.exception(lambda: cm.FileStream(reading_offset=123), TypeError)
test.exception(lambda: cm.FileStream(chunk_size=123), TypeError)
test.exception(lambda: cm.FileStream(extensions=123), TypeError)
test.exception(lambda: cm.FileStream(123), TypeError)
test.exception(lambda: cm.FileStream("", None), TypeError)
test.exception(lambda: cm.FileStream("", 1, None), TypeError)
test.exception(lambda: cm.FileStream("", 1, 2, None), TypeError)

# Test if invalid kwargs are caught
test.exception(lambda: cm.FileStream(invalid_kwarg=123), TypeError)

# Test if no filename given is caught
test.exception(lambda: cm.FileStream(), TypeError)

# Test if valid args are accepted
test.success(lambda: cm.FileStream(FNAME, 1, 2, extensions=cm.Extensions()))

# The object to use for streaming
stream = cm.FileStream(FNAME)
enc = stream.encode
dec = stream.decode

# Test if data can be written and read normally
wrote = test.success(lambda: enc(123))
read  = test.success(lambda: dec())

# Early exit if simple reading/writing fails
if not wrote or not read:
    try:
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
if test.success(lambda: cm.FileStream(FNAME).decode()):
    test.equal(123, cm.FileStream(FNAME).decode())

# Clear the file contents
open(FNAME, "wb")

# Re-create stream objects
stream = cm.FileStream(FNAME)
enc = stream.encode
dec = stream.decode

# Test all items from the test values
for v in test_values:
    if test.success(lambda: enc(v)) and test.success(lambda: dec()):
        enc(v)
        test.equal(v, dec())

# Test the test values object itself
if test.success(lambda: enc(test_values)) and test.success(lambda: dec()):
    enc(test_values)
    test.equal(test_values, dec())

# Test if string-keys-only is enforced when requested
test.exception(lambda: enc({1: 2}, str_keys=True), TypeError)
test.exception(lambda: dec(enc({1: 2}), str_keys=True), TypeError)

# Clear the file and write 1 byte for the reading offset check
open(FNAME, "wb").write(b'\0')

# Test if the reading offset argument is enforced
stream_rdoff = cm.FileStream(FNAME, reading_offset=1)
stream_rdoff.encode(test_values)
test.equal(test_values, stream_rdoff.decode())

test.print()

os.remove(FNAME)

