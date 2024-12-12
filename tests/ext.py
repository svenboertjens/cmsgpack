import unittest
import cmsgpack


# Define custom types for testing
class CustomType:
    def __init__(self, data):
        self.data = data


class AnotherCustomType:
    def __init__(self, value):
        self.value = value


# Serialization and deserialization callables
def custom_type_encoder(obj):
    return 1, obj.data.encode()


def custom_type_decoder(data):
    return CustomType(data.decode())


def another_custom_type_encoder(obj):
    return 2, obj.value.to_bytes(4, 'big')


def another_custom_type_decoder(data):
    return AnotherCustomType(int.from_bytes(data, 'big'))


class TestCMsgPackExtTypes(unittest.TestCase):
    def test_encode_decode_custom_type(self):
        # Create ExtTypesEncode and ExtTypesDecode objects
        encode_ext = cmsgpack.ExtTypesEncode({CustomType: custom_type_encoder})
        decode_ext = cmsgpack.ExtTypesDecode({1: custom_type_decoder})

        # Original object
        obj = CustomType("example")

        # Encode and decode
        encoded = cmsgpack.encode(obj, ext_types=encode_ext)
        decoded = cmsgpack.decode(encoded, ext_types=decode_ext)

        # Assert the result
        self.assertIsInstance(decoded, CustomType)
        self.assertEqual(decoded.data, obj.data)

    def test_encode_decode_multiple_custom_types(self):
        # Create ExtTypesEncode and ExtTypesDecode objects
        encode_ext = cmsgpack.ExtTypesEncode({
            CustomType: custom_type_encoder,
            AnotherCustomType: another_custom_type_encoder
        })
        decode_ext = cmsgpack.ExtTypesDecode({
            1: custom_type_decoder,
            2: another_custom_type_decoder
        })

        # Original objects
        obj1 = CustomType("example")
        obj2 = AnotherCustomType(42)

        # Encode and decode
        encoded1 = cmsgpack.encode(obj1, ext_types=encode_ext)
        encoded2 = cmsgpack.encode(obj2, ext_types=encode_ext)

        decoded1 = cmsgpack.decode(encoded1, ext_types=decode_ext)
        decoded2 = cmsgpack.decode(encoded2, ext_types=decode_ext)

        # Assert the results
        self.assertIsInstance(decoded1, CustomType)
        self.assertEqual(decoded1.data, obj1.data)

        self.assertIsInstance(decoded2, AnotherCustomType)
        self.assertEqual(decoded2.value, obj2.value)

    def test_memoryview_argument_type(self):
        # Create ExtTypesEncode and ExtTypesDecode objects with memoryview
        encode_ext = cmsgpack.ExtTypesEncode({CustomType: custom_type_encoder})
        decode_ext = cmsgpack.ExtTypesDecode({1: custom_type_decoder}, memoryview)

        # Original object
        obj = CustomType("example")

        # Encode and decode
        encoded = cmsgpack.encode(obj, ext_types=encode_ext)
        decoded = cmsgpack.decode(encoded, ext_types=decode_ext)

        # Assert the result
        self.assertIsInstance(decoded, CustomType)
        self.assertEqual(decoded.data, obj.data)

    def test_unsupported_type(self):
        # Create ExtTypesEncode with no mappings
        encode_ext = cmsgpack.ExtTypesEncode({})

        # Object to encode
        obj = CustomType("example")

        # Expect an exception for unsupported type
        with self.assertRaises(TypeError):
            cmsgpack.encode(obj, ext_types=encode_ext)

    def test_invalid_decoder_callable(self):
        # Create ExtTypesEncode and ExtTypesDecode with invalid decoder
        encode_ext = cmsgpack.ExtTypesEncode({CustomType: custom_type_encoder})
        decode_ext = cmsgpack.ExtTypesDecode({1: lambda x: x / 0})  # Invalid decoder

        # Object to encode
        obj = CustomType("example")

        # Encode
        encoded = cmsgpack.encode(obj, ext_types=encode_ext)

        # Expect an exception during decode
        with self.assertRaises(ZeroDivisionError):
            cmsgpack.decode(encoded, ext_types=decode_ext)

    def test_invalid_encoder_callable(self):
        # Create ExtTypesEncode with invalid encoder
        encode_ext = cmsgpack.ExtTypesEncode({CustomType: lambda x: x / 0})  # Invalid encoder

        # Object to encode
        obj = CustomType("example")

        # Expect an exception during encode
        with self.assertRaises(TypeError):
            cmsgpack.encode(obj, ext_types=encode_ext)


if __name__ == "__main__":
    unittest.main()
