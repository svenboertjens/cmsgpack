
# Test file-based serialization

import cmsgpack as cm

from test_values import test_values
from test import Test


FNAME = "classes_test.bin"

test = Test()


# TODO: This will test all streaming capabilities


test.print()

import os
os.remove(FNAME)

