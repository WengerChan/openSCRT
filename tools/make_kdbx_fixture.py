#!/usr/bin/env python3
"""Write a KDBX 3.1 database for the KDBX reader tests.

Why a generator instead of a file somebody else published: the fixture has to be a
database this project can vouch for (no licence questions about a third party's test
data) and it has to be reproducible. It is also a second, independent implementation of
the format -- the reader in C++ is checked against a file this Python code wrote, and
the reference implementation (pykeepass) is checked against the same file.

The byte-level facts are in docs/evidence/kdbx-format-notes.md. The two that are easy
to get wrong, and that the reference implementation settles:

  * the field length is a UInt16 in KDBX 3.1 and an Int32 in KDBX 4.x;
  * the AES-KDF encrypts the composite key with AES-256-ECB, `rounds` times, and the
    transformed key is the SHA-256 of the result.

Usage:
    uv run --with pykeepass python tools/make_kdbx_fixture.py \\
        --output tests/fixtures/kdbx/kdbx31_with_keyfile.kdbx \\
        --key-file tests/fixtures/kdbx/kdbx31_with_keyfile.key \\
        --password "correct horse battery staple" --rounds 1000
"""

import argparse
import base64
import hashlib
import os
import struct
import zlib

from Cryptodome.Cipher import AES, Salsa20

SIGNATURE_1 = 0x9AA2D903
SIGNATURE_2 = 0xB54BFB67
VERSION_3_1 = 0x00030001

CIPHER_AES256 = bytes.fromhex("31C1F2E6BF714350BE5805216AFC5AFF")

# The header field ids of KDBX 3.1.
FIELD_END = 0
FIELD_CIPHER = 2
FIELD_COMPRESSION = 3
FIELD_MASTER_SEED = 4
FIELD_TRANSFORM_SEED = 5
FIELD_TRANSFORM_ROUNDS = 6
FIELD_ENCRYPTION_IV = 7
FIELD_INNER_RANDOM_STREAM_KEY = 8
FIELD_STREAM_START_BYTES = 9
FIELD_INNER_RANDOM_STREAM_ID = 10

INNER_STREAM_NONE = 0
INNER_STREAM_SALSA20 = 2


def header_field(field_id, value):
    """One header field: id, a 16-bit length, then the value."""
    return struct.pack("<BH", field_id, len(value)) + value


def composite_key(password, key_file):
    """SHA-256( SHA-256(password) || key file )."""
    password_hash = hashlib.sha256(password.encode("utf-8")).digest()
    return hashlib.sha256(password_hash + key_file).digest()


def aes_kdf(seed, rounds, composite):
    """The transformed key: the composite key encrypted with AES-256-ECB, R times."""
    value = composite
    for _ in range(rounds):
        value = AES.new(seed, AES.MODE_ECB).encrypt(value)
    return hashlib.sha256(value).digest()


def block_stream(payload):
    """The KDBX 3.1 payload framing: each block is index || SHA-256 || length || data."""
    out = b""
    index = 0
    for start in range(0, len(payload), 1024 * 1024):
        block = payload[start:start + 1024 * 1024]
        out += struct.pack("<I", index)
        out += hashlib.sha256(block).digest()
        out += struct.pack("<I", len(block))
        out += block
        index += 1
    out += struct.pack("<I", index)  # the terminator
    out += b"\x00" * 32
    out += struct.pack("<I", 0)
    return out


def inner_stream_key(seed):
    """The key of the inner random stream that protects the individual values."""
    return hashlib.sha256(seed).digest()


def protect(value, stream_key):
    """Encrypts one value with the inner random stream (Salsa20) and base64s it.

    The protected stream is id 2 (Salsa20): the key is SHA-256(stream key) and the
    nonce is the constant E8 30 09 4B 97 20 5D 2A. The state layout is the standard
    one -- it was measured against a real KDBX 3.1 database, and the reference
    implementation reads the result back.
    """
    key = inner_stream_key(stream_key)
    nonce = bytes.fromhex("e830094b97205d2a")
    encrypted = Salsa20.new(key=key, nonce=nonce).encrypt(value.encode("utf-8"))
    return base64.b64encode(encrypted).decode("ascii")


def xml_document(title, username, password, url, protected_password=None, stream_key=b""):
    """A minimal KeePass XML with one group and one entry.

    When `protected_password` is given, the password is stored the way KeePass
    stores it: encrypted with the inner random stream and marked Protected="True".
    """
    if protected_password is None:
        password_element = f"<String><Key>Password</Key><Value>{password}</Value></String>"
    else:
        encrypted = protect(protected_password, stream_key)
        password_element = (
            '<String><Key>Password</Key><Value Protected="True">'
            f"{encrypted}</Value></String>"
        )

    return (
        '<?xml version="1.0" encoding="utf-8" standalone="yes"?>\n'
        "<KeePassFile>\n"
        "  <Meta>\n"
        "    <Generator>openSCRT fixture generator</Generator>\n"
        "    <DatabaseName>openSCRT fixture</DatabaseName>\n"
        "  </Meta>\n"
        "  <Root>\n"
        "    <Group>\n"
        "      <UUID>F69os494TDK6douojK65vQ==</UUID>\n"
        "      <Name>Root</Name>\n"
        "      <IconID>48</IconID>\n"
        "      <Entries>\n"
        "        <Entry>\n"
        "          <UUID>WYb6RWho1l0Rz1F4C3q7Eg==</UUID>\n"
        "          <IconID>0</IconID>\n"
        f"          <String><Key>Title</Key><Value>{title}</Value></String>\n"
        f"          <String><Key>UserName</Key><Value>{username}</Value></String>\n"
        f"          {password_element}\n"
        f"          <String><Key>URL</Key><Value>{url}</Value></String>\n"
        "        </Entry>\n"
        "      </Entries>\n"
        "    </Group>\n"
        "  </Root>\n"
        "</KeePassFile>\n"
    ).encode("utf-8")


def build(password, key_file, rounds, title, username, entry_password, url, compress,
          protect_password=True):
    """Builds the whole file and returns its bytes."""
    master_seed = hashlib.sha256(b"openSCRT master seed").digest()
    transform_seed = hashlib.sha256(b"openSCRT transform seed").digest()
    encryption_iv = hashlib.sha256(b"openSCRT iv").digest()[:16]
    stream_start = hashlib.sha256(b"openSCRT stream start").digest()
    inner_key = hashlib.sha256(b"openSCRT inner key").digest()

    composite = composite_key(password, key_file)
    transformed = aes_kdf(transform_seed, rounds, composite)
    master_key = hashlib.sha256(master_seed + transformed).digest()

    header = b""
    header += struct.pack("<III", SIGNATURE_1, SIGNATURE_2, VERSION_3_1)
    header += header_field(FIELD_CIPHER, CIPHER_AES256)
    header += header_field(FIELD_COMPRESSION, struct.pack("<I", 1 if compress else 0))
    header += header_field(FIELD_MASTER_SEED, master_seed)
    header += header_field(FIELD_TRANSFORM_SEED, transform_seed)
    header += header_field(FIELD_TRANSFORM_ROUNDS, struct.pack("<Q", rounds))
    header += header_field(FIELD_ENCRYPTION_IV, encryption_iv)
    header += header_field(FIELD_INNER_RANDOM_STREAM_KEY, inner_key)
    header += header_field(FIELD_STREAM_START_BYTES, stream_start)
    header += header_field(FIELD_INNER_RANDOM_STREAM_ID,
                            struct.pack("<I", INNER_STREAM_SALSA20 if protect_password
                                        else INNER_STREAM_NONE))
    header += header_field(FIELD_END, b"\x0d\x0a\x0d\x0a")

    # The order matters: the stream start bytes sit in front of the block stream and are
    # *not* part of the compressed data -- the compression covers what the blocks carry.
    # KDBX 3.1 has no inner header inside the payload -- that is a KDBX 4 thing. The
    # protected stream id and key are header fields 8 and 10 here, and the payload is
    # the XML itself.
    content = xml_document(title, username, entry_password, url,
                           protected_password=entry_password if protect_password else None,
                           stream_key=inner_key)
    if compress:
        # The compression flag says GZip, so the content has to be GZip: 16 + MAX_WBITS
        # selects the GZip wrapper rather than the zlib one.
        compressor = zlib.compressobj(wbits=16 + zlib.MAX_WBITS)
        content = compressor.compress(content) + compressor.flush()
    payload = stream_start + block_stream(content)

    plain = AES.new(master_key, AES.MODE_CBC, encryption_iv).encrypt(pad_pkcs7(payload))
    return header + plain


def pad_pkcs7(data):
    """PKCS#7 padding, the way the KDBX payload is padded before it is encrypted."""
    padding = 16 - (len(data) % 16)
    return data + bytes([padding]) * padding


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True)
    parser.add_argument("--password", required=True)
    parser.add_argument("--key-file", help="write the key file here (32 bytes)")
    parser.add_argument("--rounds", type=int, default=1000,
                        help="the AES-KDF rounds; keep it low so the tests stay fast")
    parser.add_argument("--title", default="router")
    parser.add_argument("--username", default="netops")
    parser.add_argument("--entry-password", default="s3cret")
    parser.add_argument("--url", default="ssh://10.20.30.40:22")
    parser.add_argument("--plain-password", action="store_true",
                        help="store the entry password unprotected")
    args = parser.parse_args()

    key_file = b""
    if args.key_file:
        # A fixed key file, so the fixture is reproducible.
        key_file = hashlib.sha256(b"openSCRT fixture key file").digest()
        os.makedirs(os.path.dirname(os.path.abspath(args.key_file)), exist_ok=True)
        with open(args.key_file, "wb") as handle:
            handle.write(key_file)

    data = build(args.password, key_file, args.rounds, args.title, args.username,
                 args.entry_password, args.url, compress=True,
                 protect_password=not args.plain_password)
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "wb") as handle:
        handle.write(data)
    print(f"wrote {args.output}: {len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()}")
    if args.key_file:
        print(f"wrote {args.key_file}: {len(key_file)} bytes, "
              f"sha256 {hashlib.sha256(key_file).hexdigest()}")


if __name__ == "__main__":
    main()
