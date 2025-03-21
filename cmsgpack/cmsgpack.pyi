import typing_extensions as typing


class Extensions:
    " Class for using MessagePack extension types during serialization. "

    pass_memoryview: bool
    
    def __init__(self, types: dict | None=None, pass_memoryview: bool=False):
        ...
    
    def add(self, id: int, type: type, encfunc: typing.Callable, decfunc: typing.Callable, /) -> typing.NoReturn:
        " Add an extension type for encoding and decoding. "
        ...
    
    def add_encode(self, id: int, type: type, encfunc: typing.Callable, /) -> typing.NoReturn:
        " Add an extension type for just encoding. "
        ...
    
    def add_decode(self, id: int, decfunc: typing.Callable, /) -> typing.NoReturn:
        " Add an extension type for just decoding. "
        ...
    
    def remove(self, id: int, type: type) -> typing.NoReturn:
        " Remove the encoding and decoding entry for the given ID and type. "
        ...
    
    def remove_encode(self, type: type) -> typing.NoReturn:
        " Remove just the encoding entry for the given type. "
        ...
    
    def remove_decode(self, id: int) -> typing.NoReturn:
        " Remove just the decoding entry for the given ID. "
        ...
    
    def clear(self) -> typing.NoReturn:
        " Clears the entire extensions object. "
        ...


# Global extensions object
extensions: Extensions


def encode(obj: any, /, extensions: Extensions=None) -> bytes:
    " Encode Python data to bytes. "
    ...

def decode(encoded: typing.Buffer, /, extensions: Extensions=None) -> any:
    " Decode any MessagePack-encoded data. "
    ...


class Stream:
    " Class-based wrapper for `encode()` and `decode()` that stores optional arguments. "
    
    def __init__(self, file_name: None=None, extensions: Extensions=None):
        ...
    
    def encode(self, obj: any) -> bytes:
        " Encode Python data to bytes using the class's stored arguments. "
        ...
    
    def decode(self, encoded: typing.Buffer) -> any:
        " Decode any MessagePack-encoded data using the class's stored arguments. "
        ...

class FileStream:
    " Class-based wrapper for `encode()` and `decode()` that stores optional arguments. "
    
    def __init__(self, file_name: str, extensions: Extensions=None):
        ...
    
    def encode(self, obj: any) -> None:
        " Encode Python data to bytes using the class's stored arguments. "
        ...
    
    def decode(self) -> any:
        " Decode any MessagePack-encoded data using the class's stored arguments. "
        ...

@typing.overload
def Stream(file_name: None = None, extensions: Extensions = None) -> Stream: ...
@typing.overload
def Stream(file_name: str, extensions: Extensions = None) -> FileStream: ...
def Stream(file_name: typing.Optional[str] = None, extensions: Extensions = None) -> typing.Union[Stream, FileStream]: ...

