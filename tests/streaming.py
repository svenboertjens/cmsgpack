import cmsgpack

from test_values import test_values

f = 'test.bin'

enc = cmsgpack.Encoder(file_name=f)
dec = cmsgpack.Decoder(file_name=f)

try:
    for val in test_values:
        enc.encode(val)
        assert val == dec.decode()

except e:
    print(e)


import os
os.remove(f)

