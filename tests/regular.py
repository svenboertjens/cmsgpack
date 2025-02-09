import cmsgpack

from test_values import test_values

def test(enc, dec):
    encoded = enc(test_values)
    decoded = dec(encoded)

    if (type(decoded) != type(test_values)):
        print("\n----\nDecoded object is not the same type as the test values object\n\n----\n")
    elif len(decoded) != len(test_values):
        print(f"\n # Decoded object had {len(decoded)} items, test value has {len(test_values)}\n")

    for val in test_values:
        decoded = dec(enc(val))
        
        if (val != decoded):
            print(f"----\nShould be: ({type(val).__name__})'{str(val)[:100]}'\n\nGot: ({type(decoded).__name__})'{str(decoded)[:100]}'\n----\n")

test(cmsgpack.encode, cmsgpack.decode)

enc = cmsgpack.Encoder()
dec = cmsgpack.Decoder()

test(enc.encode, dec.decode)

