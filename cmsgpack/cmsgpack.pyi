from typing_extensions import Callable, NoReturn, Buffer


class Extensions:
    " Class for using MessagePack extension types during serialization. "

    pass_memoryview: bool
    
    def __init__(self, types: dict | None=None, pass_memoryview: bool=False):
        ...
    
    def add(self, id: int, type: type, encfunc: Callable, decfunc: Callable, /) -> NoReturn:
        " Add an extension type for encoding and decoding. "
        ...
    
    def add_encode(self, id: int, type: type, encfunc: Callable, /) -> NoReturn:
        " Add an extension type for just encoding. "
        ...
    
    def add_decode(self, id: int, decfunc: Callable, /) -> NoReturn:
        " Add an extension type for just decoding. "
        ...
    
    def remove(self, id: int, type: type, /) -> NoReturn:
        " Remove the encoding and decoding entry for the given ID and type. "
        ...
    
    def remove_encode(self, type: type, /) -> NoReturn:
        " Remove just the encoding entry for the given type. "
        ...
    
    def remove_decode(self, id: int, /) -> NoReturn:
        " Remove just the decoding entry for the given ID. "
        ...
    
    def clear(self) -> NoReturn:
        " Clears the entire extensions object. "
        ...


# Global extensions object
extensions: Extensions


def encode(obj: any, /, str_keys: bool=False, extensions: Extensions=None) -> bytes:
    " Encode Python data to bytes. "
    ...

def decode(encoded: Buffer, /, str_keys: bool=False, extensions: Extensions=None) -> any:
    " Decode any MessagePack-encoded data. "
    ...


class Stream:
    " Wrapper for `encode`/`decode` that retains optional arguments. "

    str_keys: bool
    extensions: Extensions
    
    def __init__(self, str_keys: bool=False, extensions: Extensions=None):
        ...
    
    def encode(self, obj: any, /) -> bytes:
        " Encode Python data to bytes. "
        ...
    
    def decode(self, encoded: Buffer, /) -> any:
        " Decode any MessagePack-encoded data. "
        ...


class FileStream:
    " Wrapper for `encode`/`decode` that retains optional arguments and reads/writes a file directly. "

    reading_offset: int
    chunk_size: int
    str_keys: bool
    extensions: Extensions
    
    def __init__(self, file_name: str, reading_offset: int=0, chunk_size: int=16384, str_keys: bool=False, extensions: Extensions=None):
        ...
    
    def encode(self, obj: any, /) -> NoReturn:
        " Encode Python data and write it to the file. "
        ...
    
    def decode(self) -> any:
        " Decode MessagePack-encoded data read from the file. "
        ...

