import typing_extensions as typing

Buffer = typing.Buffer


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
    
    def __init__(self, pairs: dict):
        ...

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
    
    def __init__(self, pairs: dict, arg_type=bytes, /):
        ...


def encode(obj: any, /, ext_types: ExtTypesEncode=None, strict_keys: bool=False) -> bytes:
    " Encode Python data to bytes. "
    ...

def decode(encoded: Buffer, /, ext_types: ExtTypesDecode=None, strict_keys: bool=False) -> any:
    " Decode any MessagePack-encoded data. "
    ...


class Encoder:
    " Class-based wrapper for `encode()` that stores optional arguments. "
    
    def __init__(self, file_name: str|None=None, ext_types: ExtTypesEncode=None, strict_keys: bool=False):
        ...
    
    def encode(self, obj: any) -> bytes | None:
        " Encode Python data to bytes using the class's stored arguments. "
        ...

class Decoder:
    " Class-based wrapper for `decode()` that retains optional arguments. "
    
    def __init__(self, file_name: str|None=None, ext_types: ExtTypesDecode=None, strict_keys: bool=False):
        ...
    
    def decode(self, encoded: Buffer=None) -> any:
        " Decode any MessagePack-encoded data using the class's stored arguments. "
        ...

