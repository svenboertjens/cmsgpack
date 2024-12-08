import typing_extensions as typing

def encode(obj: any) -> bytes:
    """
    Encode a value to bytes.
    
    `Returns`:
    - Bytes object with the encoded object, in MessagePack format.
    
    `obj`:
    - The object to encode to bytes.
    
    """
    ...

Buffer = typing.Buffer
def decode(encoded: Buffer) -> any:
    """
    Decode any MessagePack-encoded buffer.
    
    `encoded`:
    - The encoded object to decode. Can be any readable buffer.
    
    """
    ...