import typing_extensions as typing

class ExtTypesEncode:
    """
    Class for using ext types for encoding data.
    
    > Returns
    Object that can be passed along to `encode` methods.
    
    > pairs
    A dictionary that contains the custom types you want to support (key), and the callables used for converting each custom type to bytes (value). Like so: `{type: callable}`.
    When `type` is found during serialization, `callable` will be invoked, passing the object of said type as argument.
    The callable should return a tuple with the ID (int, -128 to 127) to later identify the type with later on index 0,
    and a bytes object containing the data to be stored on index 1, like so: `return id, my_data`.
    
    """
    
    def __init__(self, pairs: dict) -> self:
        ...

Buffer = typing.Buffer
class ExtTypesDecode:
    """
    Class for using ext types for decoding data.
    
    > Returns
    Object that can be passed along to `decode` methods.
    
    > pairs
    A dictionary that contains the ID that was used for encoding the type during encoding (key), and a callable used for converting the stored data back into an object. Like so: `{ID: callable}`.
    When an ext type is found, the ID stored alongside it is used to decide what callable to invoke. If our `pairs` was `{1: abc, 2: def}` and we find an ext type with ID 1, `abc` will be called.
    The callable will receive a bytes object (by default) that contains the bytes that were received alongside the ID during serialization.
    
    > arg_type
    The datatype a callable will receive when ext data is passed to a callable from `pairs`.
    This defaults to bytes but can be set to `memoryview`, which is better in terms of performance, but may be more limiting.
    
    """
    
    def __init__(self, pairs: dict, arg_type=bytes) -> self:
        ...

def encode(obj: any, ext_types: ExtTypesEncode=None, strict_keys: bool=False) -> bytes:
    """
    Encode a value to bytes.
    
    > Returns
    A bytes object holding the object as MessagePack-encoded bytes.
    
    > obj
    The object to encode to bytes.
    
    > ext_types
    `ExtTypesEncode` object used for encoding custom types (ext types).
    
    > strict_keys
    Whether to be strict with map keys. Strict means only objects of string type are allowed to be a map key.
    
    """
    ...

def decode(encoded: Buffer, ext_types: ExtTypesDecode=None, strict_keys: bool=False) -> any:
    """
    Decode any MessagePack-encoded buffer.
    
    > encoded
    The encoded data to decode. This can be any MessagePack-format data, in the form of any readable buffer.
    
    > ext_types
    `ExtTypesDecode` object used for decoding custom types (ext types).
    
    > strict_keys
    Whether to be strict with map keys. Strict means only objects of string type are allowed to be a map key.
    
    """
    ...

class Encoder:
    """
    Create an Encoder object to encode objects with.
    
    This object has an `encode` method that does the exact same as `cmsgpack.encode`. The difference is that all optional arguments are parsed at the
    creation of the Encoder object instead of at every encode call, so the Encoder provides better performance.
    
    > Returns
    An Encoder object with the `encode` method.
    
    For more information on this class' arguments, see all optional arguments of `cmsgpack.encode`.
    
    """
    
    def __init__(self, ext_types: ExtTypesEncode=None, strict_keys: bool=False) -> self:
        ...
    
    def encode(self, obj: any) -> bytes:
        """
        Encode a value to bytes using the Encoder.
        
        This does the exact same as `cmsgpack.encode` does, except this only accepts the object to encode.
        Additional arguments were handled at the creation of the Encoder.
        
        """
        ...

class Decoder:
    """
    Create a Decoder object to decode data with.
    
    This object has a `decode` method that does the exact same as `cmsgpack.decode`. The difference is that all optional arguments are parsed at the
    creation of the Decoder object instead of at every decode call, so the Decoder provides better performance.
    
    > Returns
    A Decoder object with the `decode` method.
    
    For more information on this class' arguments, see all optional arguments of `cmsgpack.decode`.
    
    """
    
    def __init__(self, ext_types: ExtTypesDecode=None, strict_keys: bool=False) -> self:
        ...
    
    def decode(self, encoded: Buffer) -> any:
        """
        Decode bytes to an object using the Decoder.
        
        This does the exact same as `cmsgpack.decode` does, except this only accepts the encoded data.
        Additional arguments were handled at the creation of the Decoder.
        
        """
        ...

