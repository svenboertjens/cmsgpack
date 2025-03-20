
# Test file-based serialization

import cmsgpack as cm

from test_values import test_values
from test import Test


FNAME = "files_test.bin"

test = Test()


# Test if the encoder/decoder objects can be created
created_enc = test.success(lambda: cm.Encoder(file_name=FNAME))
created_dec = test.success(lambda: cm.Decoder(file_name=FNAME))

# Early exit if they can't be created at all
if not created_enc or not created_dec:
    test.print()
    exit()

# The objects to use for streaming
enc = cm.Encoder(file_name=FNAME).encode
dec = cm.Decoder(file_name=FNAME).decode

# Test if we can create another instance towards the same file
test.success(lambda: cm.Encoder(file_name=FNAME))
new_decoder_created = test.success(lambda: cm.Decoder(file_name=FNAME)) # Remember state for later

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
if not test.equal(123, dec(enc(123))):
    # Early exit if the read+decoded data isn't equal
    test.print()
    exit()

# Test if the same data will be read if we open a separate decoder object
if new_decoder_created: # Only test if we could create a new decoder in the first place
    if test.success(lambda: cm.Decoder(file_name=FNAME).decode()):
        test.equal(123, cm.Decoder(file_name=FNAME).decode())

# Test all items from the test values
for v in test_values:
    if test.success(lambda: dec(enc(v))):
        test.equal(v, dec(enc(v)))

# Test the test values object itself
if test.success(lambda: dec(enc(test_values))):
    test.equal(test_values, dec(enc(test_values)))


test.print()


try:
    import os
    os.remove(FNAME)
    
except:
    pass

