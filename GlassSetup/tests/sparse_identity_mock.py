#!/usr/bin/env python3
"""Sparse-package identity invariants (H2): one author certificate for everything.

Checks, from the repository sources only (no Windows, no signtool):
  * both AppxManifest*.xml share Identity Name/Publisher and the executable/COM paths;
  * manifest Publisher == Subject of Shared/certs/GlassAuthorCodeSigning.cer (MSIX rule);
  * kSparsePackageFamilyName == Name + '_' + Crockford-base32(SHA-256(Publisher UTF-16LE)[:8]);
  * the GlassSign.targets thumbprint pin is the SHA-1 of that certificate;
  * the payload ships the .cer/.txt next to the platform and the menu files under GlassShellSparse\\;
  * no second ("dev") certificate, no trust-store writes, no old package family anywhere.
"""
from pathlib import Path
import hashlib
import re
import xml.etree.ElementTree as ET

repo = Path(__file__).resolve().parents[2]


def read(rel):
    return (repo / rel).read_text(encoding='utf-8-sig')


# --- constants header -----------------------------------------------------
constants = read('GlassShared/include/GlassShellMenuConstants.h')


def const(name):
    match = re.search(r'inline constexpr wchar_t ' + name + r'\[\]\s*=\s*L"((?:[^"\\]|\\.)*)"', constants)
    assert match, name
    return match.group(1).replace('\\\\', '\\')


package_name = const('kSparsePackageName')
publisher = const('kSparsePublisher')
family = const('kSparsePackageFamilyName')
application_id = const('kSparseApplicationId')
shell_ext = const('kShellExtFileName')
shell_host = const('kShellHostFileName')
subdir = const('kSparsePackageSubdir')
assert subdir == 'GlassShellSparse\\'
assert const('kShellExtRelativePath') == subdir + shell_ext
assert const('kShellHostRelativePath') == subdir + shell_host
assert const('kSparseMsixFileName') == 'GlassShellSparse.msix'
cer_name = const('kAuthorCertificateFileName')
txt_name = const('kAuthorCertificateReadmeFileName')

# --- package family name --------------------------------------------------
CROCKFORD = '0123456789abcdefghjkmnpqrstvwxyz'


def family_suffix(publisher_string):
    digest = hashlib.sha256(publisher_string.encode('utf-16-le')).digest()[:8]
    bits = ''.join(f'{b:08b}' for b in digest) + '0'  # 65 bits -> 13 symbols
    return ''.join(CROCKFORD[int(bits[i:i + 5], 2)] for i in range(0, 65, 5))


assert family == package_name + '_' + family_suffix(publisher), \
    f'kSparsePackageFamilyName must be {package_name}_{family_suffix(publisher)} for Publisher {publisher!r}, header has {family}'
assert family == 'GlassProject.GlassShellMenu_83517z3ecdax2'
# Known vector: Microsoft Store apps, Publisher "CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US".
assert family_suffix('CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US') == '8wekyb3d8bbwe'

# --- manifests ------------------------------------------------------------
NS = {
    'm': 'http://schemas.microsoft.com/appx/manifest/foundation/windows10',
    'com': 'http://schemas.microsoft.com/appx/manifest/com/windows10',
}
manifests = {}
for rel in ('GlassShellExt/Package/AppxManifest.xml', 'GlassShellExt/Package/AppxManifest.UnregisterStub.xml'):
    root = ET.fromstring(read(rel))
    identity = root.find('m:Identity', NS)
    assert identity is not None, rel
    assert identity.get('Name') == package_name, rel
    assert identity.get('Publisher') == publisher, (rel, identity.get('Publisher'))
    display = root.find('m:Properties/m:PublisherDisplayName', NS)
    assert display is not None and display.text and publisher.endswith(display.text), rel
    app = root.find('m:Applications/m:Application', NS)
    assert app is not None and app.get('Id') == application_id and app.get('Executable') == shell_host, rel
    classes = root.findall('.//com:Class', NS)
    assert classes and all(c.get('Path') == shell_ext for c in classes), rel
    manifests[rel] = identity.get('Version')
assert len(set(manifests.values())) == 1, manifests  # stub must stay on the same version to replace the real package
unsigned_marker = 'OID.2.25.311729368913984317654407730594956997722'
assert unsigned_marker not in publisher  # the package is signed, never the "unsigned package" identity

# --- author certificate (DER) --------------------------------------------
cer_path = repo / 'Shared/certs' / cer_name
der = cer_path.read_bytes()
assert der[:1] == b'\x30', 'not a DER certificate'


def der_elements(data):
    """Yield (tag, value) for a flat DER sequence body."""
    pos = 0
    while pos < len(data):
        tag = data[pos]
        length = data[pos + 1]
        pos += 2
        if length & 0x80:
            count = length & 0x7F
            length = int.from_bytes(data[pos:pos + count], 'big')
            pos += count
        yield tag, data[pos:pos + length]
        pos += length


def der_body(data):
    tag, value = next(der_elements(data))
    return value


def rdn_string(name_der):
    parts = []
    for _tag, rdn in der_elements(name_der):  # SET
        for _t, atv in der_elements(rdn):  # SEQUENCE
            oid, value = list(der_elements(atv))
            if oid[1] == b'\x55\x04\x03':  # 2.5.4.3 commonName
                text = value[1].decode('utf-8') if value[0] in (0x0C, 0x13) else value[1].decode('utf-16-be')
                parts.append('CN=' + text)
    return ', '.join(parts)


tbs = der_body(der_body(der))
tbs_items = list(der_elements(tbs))
# [0] version, serial, sigalg, issuer, validity, subject, spki, ...
index = 0
if tbs_items[0][0] == 0xA0:
    index = 1
issuer = tbs_items[index + 2][1]
subject = tbs_items[index + 4][1]
assert rdn_string(subject) == publisher, (rdn_string(subject), publisher)
assert rdn_string(issuer) == publisher, 'the author certificate is self-signed'
thumbprint = hashlib.sha1(der).hexdigest().upper()
assert thumbprint == 'A801E5F5F430D6D19164C856F79B8EFD23F83EB8', thumbprint

# --- signing pin, payload, verification script ----------------------------
sign_targets = read('Shared/GlassSign.targets')
pin = re.search(r'<GlassSignThumbprint Condition="[^"]*">([0-9A-Fa-f]{40})</GlassSignThumbprint>', sign_targets)
assert pin and pin.group(1).upper() == thumbprint, 'GlassSign.targets pin must match the shipped .cer'
assert 'GlassShellSparse.msix' in sign_targets or 'GlassShellExtSignPaths' in read('GlassShellExt/GlassShellExt.vcxproj')

readme = read('Shared/certs/' + txt_name)
assert publisher in readme and thumbprint in readme and family.split('_')[0] in readme
assert 'TrustedPeople' in readme and 'Import-Certificate' in readme  # manual, documented, user-driven step

payload = read('GlassSetup/msi/GlassSetupPayload.targets')
for item in (cer_name, txt_name, 'GlassShellSparse\\GlassShellExt.dll', 'GlassShellSparse\\GlassShellHost.exe',
             'GlassShellSparse\\GlassShellSparse.msix', 'GlassShellSparse\\AppxManifest.xml',
             'GlassShellSparse\\AppxManifest.UnregisterStub.xml'):
    assert item in payload, item

verify = read('Shared/scripts/Test-GlassSignatures.ps1')
for item in ('GlassShellSparse\\GlassShellExt.dll', 'GlassShellSparse\\GlassShellHost.exe', 'GlassShellSparse\\GlassShellSparse.msix'):
    assert item in verify, item
assert 'GlassShellExt.dll\'' not in verify.replace('GlassShellSparse\\GlassShellExt.dll\'', '')  # no stale root-level entry

for project, pattern in (('GlassShellExt/GlassShellExt.vcxproj', r'<OutDir>[^<]*GlassShellSparse\\</OutDir>'),
                         ('GlassShellHost/GlassShellHost.vcxproj', r'<OutDir>[^<]*GlassShellSparse\\</OutDir>')):
    text = read(project)
    assert len(re.findall(pattern, text)) == 4, project

pack = read('GlassShellExt/Package/BuildSparsePackage.ps1')
for forbidden in ('New-SelfSignedCertificate', 'Export-PfxCertificate', 'Import-Certificate', 'TrustedPeople', 'signtool', 'Cert:\\'):
    assert forbidden not in pack, forbidden

# --- no second identity or trust-store writes anywhere ----------------------
scan_dirs = ('GlassShellExt', 'GlassShellHost', 'GlassSettings', 'GlassSetup/src', 'GlassSetup/GlassUnins',
             'GlassSetup/msi', 'GlassSetup/res', 'GlassShared', 'Shared', 'contributing')
old_identity = ('t6mpd150h6a5c', 'GlassProject Dev', '2F8603BEC098465EED7768AC8FD3BF79E8DF65DF', 'ShellDevCertificate',
                'CertAddCertificateContextToStore', 'CertAddEncodedCertificateToStore')
suffixes = {'.cpp', '.h', '.cs', '.xaml', '.ps1', '.targets', '.props', '.vcxproj', '.wxs', '.xml', '.md', '.txt', '.rc', '.resw'}
for directory in scan_dirs:
    for path in (repo / directory).rglob('*'):
        if not path.is_file() or path.suffix.lower() not in suffixes or 'Build' in path.parts or 'bin' in path.parts or 'obj' in path.parts:
            continue
        if path.name == 'New-GlassSelfSignedCert.ps1':
            continue  # the author's own one-time certificate creation script
        text = path.read_text(encoding='utf-8', errors='replace')
        for needle in old_identity:
            assert needle not in text, (path, needle)
# The only code touching Trusted People is the read-only signer check in ShellSparsePackage.cpp.
sparse_cpp = read('GlassSettings/Native/ShellSparsePackage.cpp')
assert 'IsThumbprintInTrustedPeople' in sparse_cpp and 'CERT_STORE_READONLY_FLAG' in sparse_cpp

print('PASS: sparse identity: Publisher == author certificate subject, family name derivation, pin, payload and no second certificate')
