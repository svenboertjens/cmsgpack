import typing_extensions as typing

class ExtTypesEncode:
    def __init__(self, pairs: dict) -> self:
        """
        Class for using ext types for encoding data.
        
        
        Returns:
        Object that can be passed along to `encode` methods.
        
        
        pairs:
        A dictionary that contains the custom types you want to support (key), and the callables used for converting each custom type to bytes (value). Like so: `{type: callable}`.
        
        When `type` is found during serialization, `callable` will be invoked, passing the object of said type as argument.
        
        The callable should return a tuple with the ID (int, -128 to 127) to later identify the type with later on index 0, and a bytes object containing the data to be stored on index 1, like so: `return id, my_data`.
        """
        ...

Buffer = typing.Buffer
class ExtTypesDecode:
    def __init__(self, pairs: dict, arg_type=bytes) -> self:
        """
        Class for using ext types for decoding data.
        
        
        Returns:
        Object that can be passed along to `decode` methods.
        
        
        pairs:
        A dictionary that contains the ID that was used for encoding the type during encoding (key), and a callable used for converting the stored data back into an object. Like so: `{ID: callable}`.
        
        When an ext type is found, the ID stored alongside it is used to decide what callable to invoke. If our `pairs` was `{1: abc, 2: def}` and we find an ext type with ID 1, `abc` will be called.
        
        The callable will receive a bytes object (by default) that contains the bytes that were received alongside the ID during serialization.
        
        
        argtype:
        The datatype a callable will receive when ext data is passed to a callable from `pairs`. This defaults to bytes but can be set to `memoryview`, which is better in terms of performance, but may be more limiting.
        """
        ...

def encode(obj: any, ext_types: ExtTypesEncode=None) -> bytes:
    """
    Encode a value to bytes.
    
    
    Returns:
    - Bytes object with the encoded object, in MessagePack format.
    
    obj:
    - The object to encode to bytes.
    
    ext:
    - `ExtTypesEncode` object used for encoding custom (ext) types.
    
    """
    ...

def decode(encoded: Buffer, ext_types: ExtTypesDecode=None) -> any:
    """
    Decode any MessagePack-encoded buffer.
    
    
    encoded:
    The encoded object to decode. Can be any readable buffer.
    
    
    ext:
    `ExtTypesDecode` object used for decoding custom (ext) types.
    
    """
    ...

class Encoder:
    def __init__(self, ext_types: ExtTypesEncode=None) -> self:
        ...
    
    def encode(self, obj: any) -> bytes:
        ...

class Decoder:
    def __init__(self, ext_types: ExtTypesDecode=None) -> self:
        ...
    
    def encode(self, encoded: Buffer) -> any:
        ...

