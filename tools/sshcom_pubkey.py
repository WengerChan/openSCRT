#!/usr/bin/env python3
"""Derive the OpenSSH public key of an ssh.com (VanDyke) private key.

This is an independent implementation of the same format the C++ parser reads, on purpose:
if the two agree on the modulus and the exponent, the C++ side is not just agreeing with
itself. The public key it produces is what the CI's sshd is given, so the end-to-end test
can log in with the ssh.com private key.

Usage: sshcom_pubkey.py <ssh.com private key> [passphrase]
"""
import base64
import struct
import sys


def read_body(path):
    with open(path, "r", encoding="utf-8") as handle:
        lines = handle.read().split("\n")
    assert lines[0].strip() == "---- BEGIN SSH2 ENCRYPTED PRIVATE KEY ----", lines[0]
    body = []
    for line in lines[1:]:
        line = line.strip("\r")
        if line.strip() == "---- END SSH2 ENCRYPTED PRIVATE KEY ----":
            break
        if ":" in line:
            continue
        body.append(line.strip())
    return base64.b64decode("".join(body))


def parse(data):
    (magic,) = struct.unpack(">I", data[0:4])
    assert magic == 0x3F6FF9EB, hex(magic)
    (total,) = struct.unpack(">I", data[4:8])
    offset = 8
    (key_type_len,) = struct.unpack(">I", data[offset:offset + 4])
    offset += 4
    key_type = data[offset:offset + key_type_len].decode("ascii")
    offset += key_type_len
    (cipher_len,) = struct.unpack(">I", data[offset:offset + 4])
    offset += 4
    cipher = data[offset:offset + cipher_len].decode("ascii")
    offset += cipher_len
    (blob_len,) = struct.unpack(">I", data[offset:offset + 4])
    offset += 4
    blob = data[offset:offset + blob_len]
    assert cipher == "none", f"this script only handles the unencrypted form, got {cipher}"
    return key_type, blob


def read_bigint(blob, offset):
    (bits,) = struct.unpack(">I", blob[offset:offset + 4])
    offset += 4
    length = (bits + 7) // 8
    value = blob[offset:offset + length]
    return value, offset + length


def mpint(value):
    value = value.lstrip(b"\x00") or b""
    if value and value[0] & 0x80:
        value = b"\x00" + value
    return struct.pack(">I", len(value)) + value


def main():
    path = sys.argv[1]
    key_type, blob = parse(read_body(path))
    offset = 0
    (plain_len,) = struct.unpack(">I", blob[0:4])
    offset = 4
    exponent, offset = read_bigint(blob, offset)
    private_exponent, offset = read_bigint(blob, offset)
    modulus, offset = read_bigint(blob, offset)
    inverse_q, offset = read_bigint(blob, offset)
    q, offset = read_bigint(blob, offset)
    p, offset = read_bigint(blob, offset)

    public_blob = b"ssh-rsa" + mpint(exponent) + mpint(modulus)
    print(f"key type: {key_type}")
    print(f"modulus: {len(modulus)} bytes, exponent: {int.from_bytes(exponent, 'big')}")
    print(f"plain length field: {plain_len}")
    print("ssh-rsa " + base64.b64encode(public_blob).decode("ascii") + " openscrt-sshcom-fixture")


if __name__ == "__main__":
    main()
