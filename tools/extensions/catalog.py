#!/usr/bin/env python3
"""Validate, verify and sign the Luling extension catalog.

    python tools/extensions/catalog.py validate --index extensions/index.json
    python tools/extensions/catalog.py verify   --index extensions/index.json [--offline]
    python tools/extensions/catalog.py sign     --index extensions/index.json \
        --private <RSAFULLPRIVATEBLOB.hex> [--output extensions/index.sig.json]

validate  checks the document against the rules the client enforces.
verify    additionally downloads every package and its signature and checks the
          package hash, the manifest hash and the publisher signature.
sign      writes the catalog signature document over the exact index bytes.

The client pins the official publisher in src/app/store_catalog.cpp. This tool
reads the same pin from that file so the two cannot drift.
"""
import argparse
import datetime
import hashlib
import io
import json
import re
import sys
import urllib.request
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sign as signing  # noqa: E402

HEX64 = re.compile(r'^[0-9a-fA-F]{64}$')
ID = re.compile(r'^[A-Za-z0-9._-]{1,64}$')
MAX_PACKAGE_BYTES = 512 * 1024 * 1024
MAX_SIGNATURE_BYTES = 16 * 1024
MAX_PUBLISHERS = 64
MAX_EXTENSIONS = 512


def pin_from_source(source=Path(__file__).resolve().parents[2] / 'src/app/store_catalog.cpp'):
    """Read the official key and publisher id the client pins."""
    text = source.read_text(encoding='utf-8')
    key = re.search(r'kCatalogPublicKeyHex\s*=\s*((?:"[0-9a-f]+"\s*)+);', text)
    publisher = re.search(r'kCatalogPublisherId\s*=\s*"([0-9a-f]{64})"', text)
    if not key or not publisher:
        raise ValueError('Could not read the pinned catalog key from ' + str(source))
    public_key = ''.join(re.findall(r'"([0-9a-f]+)"', key.group(1)))
    return publisher.group(1), public_key


def fail(message):
    raise ValueError(message)


def check_asset(value, package):
    if not isinstance(value, dict):
        fail('asset is not an object')
    url = value.get('url')
    size = value.get('size')
    sha256 = value.get('sha256')
    if not isinstance(url, str) or not url.startswith(
            'https://github.com/' if package else 'https://'):
        fail('asset url is not allowed: %r' % (url,))
    if not isinstance(size, int) or not 1 <= size <= MAX_PACKAGE_BYTES:
        fail('asset size is invalid')
    if not isinstance(sha256, str) or not HEX64.match(sha256):
        fail('asset sha256 is invalid')
    return {'url': url, 'size': size, 'sha256': sha256.lower()}


def validate(document, official_pin):
    if not isinstance(document, dict) or document.get('formatVersion') != 1:
        fail('formatVersion must be 1')
    publishers = document.get('publishers')
    extensions = document.get('extensions')
    if not isinstance(publishers, list) or len(publishers) > MAX_PUBLISHERS:
        fail('publishers is invalid')
    if not isinstance(extensions, list) or len(extensions) > MAX_EXTENSIONS:
        fail('extensions is invalid')

    official_id, official_key = official_pin
    known = {}
    for publisher in publishers:
        if not isinstance(publisher, dict):
            fail('publisher is not an object')
        publisher_id = publisher.get('id')
        public_key = publisher.get('publicKey')
        name = publisher.get('name')
        if not isinstance(publisher_id, str) or not HEX64.match(publisher_id):
            fail('publisher id is invalid')
        if not isinstance(name, str) or not name:
            fail('publisher name is empty')
        try:
            blob = signing.public_blob(bytes.fromhex(public_key))
        except (TypeError, ValueError):
            fail('publisher key is invalid')
        if hashlib.sha256(blob).hexdigest() != publisher_id.lower():
            fail('publisher id does not match its key')
        if publisher.get('official'):
            if publisher_id.lower() != official_id or public_key.lower() != official_key:
                fail('unknown official publisher')
        if publisher_id in known:
            fail('duplicate publisher')
        known[publisher_id] = public_key

    if official_id not in known:
        fail('the official publisher is missing')

    seen = set()
    for extension in extensions:
        if not isinstance(extension, dict):
            fail('extension is not an object')
        extension_id = extension.get('id')
        if not isinstance(extension_id, str) or not ID.match(extension_id) or \
                extension_id in seen:
            fail('extension id is invalid or duplicated: %r' % (extension_id,))
        seen.add(extension_id)
        if extension.get('publisher') not in known:
            fail('extension %s names an unknown publisher' % extension_id)
        if not isinstance(extension.get('name'), str) or not extension.get('name'):
            fail('extension %s has no name' % extension_id)
        versions = extension.get('versions')
        if not isinstance(versions, list) or not 1 <= len(versions) <= 32:
            fail('extension %s has no versions' % extension_id)
        for release in versions:
            if not isinstance(release, dict):
                fail('release is not an object')
            if not isinstance(release.get('version'), str) or not release.get('version'):
                fail('release version is empty')
            manifest_sha = release.get('manifestSha256')
            if not isinstance(manifest_sha, str) or not HEX64.match(manifest_sha):
                fail('release manifestSha256 is invalid')
            check_asset(release.get('package'), package=True)
            check_asset(release.get('signature'), package=False)
    return known


def download(url, maximum):
    request = urllib.request.Request(url, headers={'User-Agent': 'luling-catalog/1'})
    with urllib.request.urlopen(request, timeout=60) as response:
        length = response.headers.get('Content-Length')
        if length and int(length) > maximum:
            fail('response too large: %s' % url)
        data = response.read(maximum + 1)
        if len(data) > maximum:
            fail('response too large: %s' % url)
        return data


def verify_packages(document, known):
    failures = []
    for extension in document['extensions']:
        publisher_key = bytes.fromhex(known[extension['publisher']])
        for release in extension['versions']:
            label = '%s %s' % (extension['id'], release['version'])
            try:
                package_asset = check_asset(release['package'], package=True)
                signature_asset = check_asset(release['signature'], package=False)
                package = download(package_asset['url'], MAX_PACKAGE_BYTES)
                if len(package) != package_asset['size']:
                    fail('package size does not match the catalog')
                if hashlib.sha256(package).hexdigest() != package_asset['sha256']:
                    fail('package sha256 does not match the catalog')
                signature_bytes = download(signature_asset['url'], MAX_SIGNATURE_BYTES)
                if len(signature_bytes) != signature_asset['size']:
                    fail('signature size does not match the catalog')
                if hashlib.sha256(signature_bytes).hexdigest() != signature_asset['sha256']:
                    fail('signature sha256 does not match the catalog')
                with zipfile.ZipFile(io.BytesIO(package)) as archive:
                    manifest = archive.read('manifest.json')
                if hashlib.sha256(manifest).hexdigest() != release['manifestSha256'].lower():
                    fail('manifest does not match the catalog')
                signature = json.loads(signature_bytes.decode('utf-8'))
                if signature.get('formatVersion') != 1 or \
                        signature.get('algorithm') != signing.ALGORITHM:
                    fail('signature document is unsupported')
                if signature.get('publisher') != extension['publisher']:
                    fail('signature names another publisher')
                if not signing.verify_signature(
                        publisher_key, signing.MANIFEST_PREFIX + manifest,
                        bytes.fromhex(signature.get('signature', ''))):
                    fail('publisher signature is invalid')
                parsed = json.loads(manifest.decode('utf-8'))
                if parsed.get('id') != extension['id']:
                    fail('manifest id does not match the catalog')
                if parsed.get('version') not in (None, '', release['version']):
                    fail('manifest version does not match the catalog')
            except Exception as error:  # noqa: BLE001 - report every failure
                failures.append('%s: %s' % (label, error))
    return failures


def check_catalog_signature(index_path, pin, signature_path):
    signature = json.loads(Path(signature_path).read_text(encoding='utf-8'))
    if signature.get('formatVersion') != 1 or \
            signature.get('algorithm') != signing.ALGORITHM:
        fail('catalog signature document is unsupported')
    if signature.get('publisher') != pin[0]:
        fail('catalog signature names another publisher')
    index = Path(index_path).read_bytes()
    if not signing.verify_signature(bytes.fromhex(pin[1]),
                                    signing.CATALOG_PREFIX + index,
                                    bytes.fromhex(signature.get('signature', ''))):
        fail('catalog signature does not verify')
    return True


def make_entry(package_path, signature_path, package_url, signature_url, version,
               released, min_luling):
    package = Path(package_path).read_bytes()
    signature = Path(signature_path).read_bytes()
    with zipfile.ZipFile(io.BytesIO(package)) as archive:
        manifest = archive.read('manifest.json')
    parsed = json.loads(manifest.decode('utf-8'))
    if parsed.get('version') not in (None, '', version):
        fail('manifest version does not match --version')
    entry = {
        'version': version,
        'released': released,
        'manifestSha256': hashlib.sha256(manifest).hexdigest(),
        'package': {
            'url': package_url,
            'size': len(package),
            'sha256': hashlib.sha256(package).hexdigest(),
        },
        'signature': {
            'url': signature_url,
            'size': len(signature),
            'sha256': hashlib.sha256(signature).hexdigest(),
        },
    }
    if min_luling:
        entry['minLuling'] = min_luling
    return entry


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    for command in ('validate', 'verify', 'sign', 'entry'):
        node = sub.add_parser(command)
        node.add_argument('--index', default='extensions/index.json')
        if command == 'verify':
            node.add_argument('--offline', action='store_true',
                              help='do not download packages')
            node.add_argument('--signature', default='extensions/index.sig.json')
        if command == 'sign':
            node.add_argument('--private', required=True)
            node.add_argument('--output', default='extensions/index.sig.json')
        if command == 'entry':
            node.add_argument('--package', required=True)
            node.add_argument('--signature', required=True)
            node.add_argument('--package-url', required=True)
            node.add_argument('--signature-url', required=True)
            node.add_argument('--version', required=True)
            node.add_argument('--released', default='')
            node.add_argument('--min-luling', default='')
    args = parser.parse_args(argv)

    if args.command == 'entry':
        entry = make_entry(args.package, args.signature, args.package_url,
                           args.signature_url, args.version,
                           args.released or datetime.date.today().isoformat(),
                           args.min_luling)
        print(json.dumps(entry, indent=2, ensure_ascii=False))
        return 0

    index_path = Path(args.index)
    document = json.loads(index_path.read_text(encoding='utf-8'))
    pin = pin_from_source()
    known = validate(document, pin)

    if args.command == 'sign':
        return signing.main(['sign', '--private', args.private, '--domain', 'catalog',
                             '--input', str(index_path), '--output', args.output])

    if args.command == 'verify' and not args.offline:
        check_catalog_signature(index_path, pin, args.signature)
        failures = verify_packages(document, known)
        if failures:
            for failure in failures:
                print('FAIL', failure)
            return 1
        print('verified %d package(s)' % sum(len(e['versions']) for e in document['extensions']))

    print('ok', index_path)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
