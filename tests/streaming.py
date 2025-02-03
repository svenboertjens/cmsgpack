import cmsgpack

test_values = [
    1, 2, 3,
    'abc', 'def', 'ghi'
]

f = 'test.bin'

enc = cmsgpack.Encoder(file_name=f)
dec = cmsgpack.Decoder(file_name=f)

for val in test_values:
    enc.encode(val)
    assert val == dec.decode()


import os
os.remove(f)

