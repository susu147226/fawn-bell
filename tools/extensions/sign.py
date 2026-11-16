#!/usr/bin/env python3
"""Luling extension signing: keys, manifest signatures and catalog signatures.

Windows CNG RSA-PKCS1-SHA256, byte compatible with src/app/store_signature.cpp.
Keys are CNG blobs in lower-case hex: RSAFULLPRIVATEBLOB (keep it secret) and
RSAPUBLICBLOB (publish it). A publisher id is the SHA-256 of the exact public
blob.

Signed bytes are domain separated:
    Luling.Extension.Catalog.Signature\\0v1\\0 + catalog index bytes
    Luling.Extension.Manifest.Signature\\0v1\\0 + manifest.json bytes
"""
import argparse
import ctypes
import hashlib
import json
import os
import struct
import sys
from pathlib import Path

CATALOG_PREFIX = b'Luling.Extension.Catalog.Signature\0v1\0'
MANIFEST_PREFIX = b'Luling.Extension.Manifest.Signature\0v1\0'
DOMAINS = {'catalog': CATALOG_PREFIX, 'manifest': MANIFEST_PREFIX}
ALGORITHM = 'RSA-PKCS1-SHA256'


class Pkcs1PaddingInfo(ctypes.Structure):
    _fields_ = [('pszAlgId', ctypes.c_wchar_p)]


class Cng:
    def __init__(self):
        if os.name != 'nt':
            raise ValueError('CNG signature operations require Windows')
        self.dll = ctypes.WinDLL('bcrypt.dll')
        void = ctypes.c_void_p
        ulong = ctypes.c_ulong
        for name, args in {
            'BCryptOpenAlgorithmProvider': [ctypes.POINTER(void), ctypes.c_wchar_p,
                                            ctypes.c_wchar_p, ulong],
            'BCryptCloseAlgorithmProvider': [void, ulong],
            'BCryptImportKeyPair': [void, void, ctypes.c_wchar_p, ctypes.POINTER(void),
                                    void, ulong, ulong],
            'BCryptExportKey': [void, void, ctypes.c_wchar_p, void, ulong,
                                ctypes.POINTER(ulong), ulong],
            'BCryptDestroyKey': [void],
            'BCryptSignHash': [void, void, void, ulong, void, ulong,
                               ctypes.POINTER(ulong), ulong],
            'BCryptGenerateKeyPair': [void, ctypes.POINTER(void), ulong, ulong],
            'BCryptFinalizeKeyPair': [void, ulong],
        }.items():
            fn = getattr(self.dll, name)
            fn.argtypes, fn.restype = args, ctypes.c_long
        self.algorithm = void()
        self.key = void()
        self.check(self.dll.BCryptOpenAlgorithmProvider(
            ctypes.byref(self.algorithm), 'RSA', None, 0))

    @staticmethod
    def check(status):
        if status < 0:
            raise ValueError('CNG operation failed: %08x' % (status & 0xffffffff))

    def __enter__(self):
        return self

    def __exit__(self, *_):
        if self.key:
            self.dll.BCryptDestroyKey(self.key)
        if self.algorithm:
            self.dll.BCryptCloseAlgorithmProvider(self.algorithm, 0)

    def generate(self, bits):
        self.check(self.dll.BCryptGenerateKeyPair(self.algorithm, ctypes.byref(self.key),
                                                  bits, 0))
        self.check(self.dll.BCryptFinalizeKeyPair(self.key, 0))

    def import_private(self, blob):
        self.check(self.dll.BCryptImportKeyPair(self.algorithm, None,
                                                'RSAFULLPRIVATEBLOB',
                                                ctypes.byref(self.key),
                                                ctypes.c_char_p(blob), len(blob), 0))

    def export(self, blob_type):
        size = ctypes.c_ulong(0)
        self.check(self.dll.BCryptExportKey(self.key, None, blob_type, None, 0,
                                            ctypes.byref(size), 0))
        buffer = ctypes.create_string_buffer(size.value)
        self.check(self.dll.BCryptExportKey(self.key, None, blob_type, buffer, size.value,
                                            ctypes.byref(size), 0))
        return buffer.raw[:size.value]

    def sign(self, message):
        digest = hashlib.sha256(message).digest()
        padding = Pkcs1PaddingInfo('SHA256')
        padding_ref = ctypes.cast(ctypes.byref(padding), ctypes.c_void_p)
        digest_ref = ctypes.c_char_p(digest)
        size = ctypes.c_ulong(0)
        self.check(self.dll.BCryptSignHash(self.key, padding_ref, digest_ref,
                                           len(digest), None, 0,
                                           ctypes.byref(size), 2))
        signature = ctypes.create_string_buffer(size.value)
        self.check(self.dll.BCryptSignHash(self.key, padding_ref, digest_ref,
                                           len(digest), signature, size.value,
                                           ctypes.byref(size), 2))
        return signature.raw[:size.value]


def public_blob(blob):
    if len(blob) < 24:
        raise ValueError('Invalid public key')
    magic, bits, exponent, modulus, prime1, prime2 = struct.unpack('<6I', blob[:24])
    if (magic != 0x31415352 or not 2048 <= bits <= 4096 or bits % 8 or
            modulus != bits // 8 or not 1 <= exponent <= 8 or prime1 or prime2 or
            len(blob) != 24 + exponent + modulus):
        raise ValueError('Only RSA 2048-4096 CNG public blobs are accepted')
    return blob


def fingerprint(public):
    return hashlib.sha256(public_blob(public)).hexdigest()


def read_hex(path):
    text = Path(path).read_text().strip()
    if not text or len(text) % 2 or any(c not in '0123456789abcdefABCDEF' for c in text):
        raise ValueError('Invalid key hex: ' + str(path))
    return bytes.fromhex(text)


def write_hex(path, blob):
    Path(path).write_text(blob.hex(), encoding='ascii')


def public_of(private_blob):
    """Derive the RSAPUBLICBLOB from an RSAFULLPRIVATEBLOB."""
    if len(private_blob) < 24:
        raise ValueError('Truncated private blob')
    magic, bits, exponent_len, modulus_len, prime1_len, prime2_len = struct.unpack(
        '<6I', private_blob[:24])
    if magic != 0x33415352:
        raise ValueError('Not an RSAFULLPRIVATEBLOB')
    # CNG orders the private blob as exponent, modulus, then the primes.
    exponent = private_blob[24:24 + exponent_len]
    modulus = private_blob[24 + exponent_len:24 + exponent_len + modulus_len]
    return struct.pack('<6I', 0x31415352, bits, exponent_len, modulus_len, 0, 0) + \
        exponent + modulus


def verify_signature(public_key_blob, message, signature):
    """RSA-PKCS1-SHA256 verification with a CNG public blob."""
    dll = ctypes.WinDLL('bcrypt.dll')
    void = ctypes.c_void_p
    ulong = ctypes.c_ulong
    for name, args in {
        'BCryptOpenAlgorithmProvider': [ctypes.POINTER(void), ctypes.c_wchar_p,
                                        ctypes.c_wchar_p, ulong],
        'BCryptCloseAlgorithmProvider': [void, ulong],
        'BCryptImportKeyPair': [void, void, ctypes.c_wchar_p, ctypes.POINTER(void),
                                void, ulong, ulong],
        'BCryptDestroyKey': [void],
        'BCryptVerifySignature': [void, void, void, ulong, void, ulong, ulong],
    }.items():
        fn = getattr(dll, name)
        fn.argtypes, fn.restype = args, ctypes.c_long
    public_blob(public_key_blob)
    algorithm = void()
    key = void()
    Cng.check(dll.BCryptOpenAlgorithmProvider(ctypes.byref(algorithm), 'RSA', None, 0))
    try:
        Cng.check(dll.BCryptImportKeyPair(algorithm, None, 'RSAPUBLICBLOB',
                                          ctypes.byref(key),
                                          ctypes.c_char_p(public_key_blob),
                                          len(public_key_blob), 0))
        digest = hashlib.sha256(message).digest()
        padding = Pkcs1PaddingInfo('SHA256')
        padding_ref = ctypes.cast(ctypes.byref(padding), ctypes.c_void_p)
        status = dll.BCryptVerifySignature(
            key, padding_ref, ctypes.c_char_p(digest), len(digest),
            ctypes.c_char_p(signature), len(signature), 2)
        return status >= 0
    finally:
        if key:
            dll.BCryptDestroyKey(key)
        dll.BCryptCloseAlgorithmProvider(algorithm, 0)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)

    generate = sub.add_parser('generate', help='create a new RSA key pair')
    generate.add_argument('--private', required=True, help='RSAFULLPRIVATEBLOB output (.hex)')
    generate.add_argument('--public', required=True, help='RSAPUBLICBLOB output (.hex)')
    generate.add_argument('--bits', type=int, default=2048)

    finger = sub.add_parser('fingerprint', help='print the publisher id of a public key')
    finger.add_argument('--public', required=True)

    sign = sub.add_parser('sign', help='sign a manifest or catalog index')
    sign.add_argument('--private', required=True)
    sign.add_argument('--domain', required=True, choices=sorted(DOMAINS))
    sign.add_argument('--input', required=True)
    sign.add_argument('--output', required=True)

    args = parser.parse_args(argv)

    if args.command == 'generate':
        with Cng() as cng:
            cng.generate(args.bits)
            private = cng.export('RSAFULLPRIVATEBLOB')
            public = cng.export('RSAPUBLICBLOB')
        write_hex(args.private, private)
        write_hex(args.public, public)
        print('publisher', fingerprint(public))
        return 0

    if args.command == 'fingerprint':
        print(fingerprint(read_hex(args.public)))
        return 0

    private = read_hex(args.private)
    message = Path(args.input).read_bytes()
    with Cng() as cng:
        cng.import_private(private)
        signature = cng.sign(DOMAINS[args.domain] + message)
    document = {
        'formatVersion': 1,
        'algorithm': ALGORITHM,
        'publisher': fingerprint(public_of(private)),
        'signature': signature.hex(),
    }
    Path(args.output).write_text(json.dumps(document, indent=2) + '\n', encoding='utf-8')
    print('signed', args.input, '->', args.output)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
