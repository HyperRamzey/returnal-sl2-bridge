"""Patch the Streamline signature gate in the game's pre-init module.

Background
----------
Returnal/Streamline 1.3.3 verifies that the file `sl.interposer.dll` sitting in
`Engine\\Plugins\\Streamline\\Binaries\\ThirdParty\\Win64` is signed by NVIDIA:
it calls WinVerifyTrust, requires two signatures (primary + secondary) and then
byte-compares the certificate public key found in the nested signature against a
hard pinned RSA-3072 key.  A replacement interposer can never satisfy this, and a
failure is silent: the game simply never enables Streamline (DLSS menu entries
disappear, no crash).

The check lives in the internal function
`?slVerifyEmbeddedSignature@@YA_NAEBVFString@@Z` inside
`Returnal-StreamlineRHIPreInit-Win64-Shipping.dll`.  It is NOT an export, so it
is located by address:

    RVA 0x3380  the check function itself
    RVA 0x2A4A  the single call site inside LoadStreamlineFunctions (RVA 0x2980)

Both values were recovered by static analysis of the shipping binary
(sha256 AD64... is the sibling interposer; see backup\\ for the exact module copy
this patch was developed against).  Before touching anything the script verifies:

  * the module hash matches the reference copy in .\\backup (pinned build), unless
    --force-hash is given,
  * the call at RVA 0x2A4A really targets RVA 0x3380 (structural check),
  * the three bytes at the target are not already the patch.

Then it writes `mov al,1 ; ret` (B0 01 C3).  Restore by copying the backup back
(install.ps1 -Remove does that).

Usage:
    python patch_gate.py <preinit.dll> [--check | --patch] [--force-hash]
"""

import os
import struct
import sys
import hashlib

GATE_RVA = 0x3380
CALLSITE_RVA = 0x2A4A
PATCH = bytes([0xB0, 0x01, 0xC3])       # mov al,1 ; ret
REF_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'backup')
REF_NAME = 'Returnal-StreamlineRHIPreInit.stock.dll'


def sections(data):
    e = struct.unpack_from('<I', data, 0x3C)[0]
    coff = e + 4
    nsec = struct.unpack_from('<H', data, coff + 2)[0]
    opt = coff + 20
    magic = struct.unpack_from('<H', data, opt)[0]
    so = opt + struct.unpack_from('<H', data, coff + 16)[0]
    secs = []
    for i in range(nsec):
        off = so + 40 * i
        vsz, va, rsz, ra = struct.unpack_from('<IIII', data, off + 8)
        secs.append((va, vsz, ra, rsz))
    return magic, opt, secs


def rva_to_off(secs, rva):
    for va, vsz, ra, rsz in secs:
        if va <= rva < va + max(vsz, rsz):
            return ra + (rva - va)
    return None


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    args = sys.argv[2:]
    mode = '--patch'
    for a in args:
        if a in ('--check', '--patch'):
            mode = a
    force_hash = '--force-hash' in args

    data = bytearray(open(path, 'rb').read())
    print('file  : %s' % path)
    print('sha256: %s' % hashlib.sha256(data).hexdigest())

    _, _, secs = sections(data)
    off_gate = rva_to_off(secs, GATE_RVA)
    if off_gate is None:
        print('FAIL: gate RVA not inside any section')
        return 1
    cur = bytes(data[off_gate:off_gate + 3])
    if cur == PATCH:
        print('gate  : RVA 0x%X already patched (%s)' % (GATE_RVA, cur.hex()))
        print('state : PATCHED (nothing to do)')
        return 0

    ref = os.path.join(REF_DIR, REF_NAME)
    if os.path.exists(ref):
        ref_hash = hashlib.sha256(open(ref, 'rb').read()).hexdigest()
        if ref_hash != hashlib.sha256(data).hexdigest():
            if force_hash:
                print('WARNING: module differs from the pinned build (--force-hash given)')
            else:
                print('FAIL: module does not match the pinned build in backup\\ (%s)' % ref_hash)
                print('      refusing to patch. Re-analyse the module or pass --force-hash.')
                return 1
        else:
            print('hash  : matches pinned build')
    else:
        print('note  : no pinned reference in %s (run install.ps1 once to create it)' % REF_DIR)

    _, _, secs = sections(data)

    off_gate = rva_to_off(secs, GATE_RVA)
    off_call = rva_to_off(secs, CALLSITE_RVA)
    if off_gate is None or off_call is None:
        print('FAIL: RVAs not inside any section')
        return 1

    # structural check: call at CALLSITE_RVA must target GATE_RVA
    if data[off_call] != 0xE8:
        print('FAIL: RVA 0x%X is not a call (byte 0x%02X) - different game build?'
              % (CALLSITE_RVA, data[off_call]))
        return 1
    rel = struct.unpack_from('<i', data, off_call + 1)[0]
    target = CALLSITE_RVA + 5 + rel
    if target != GATE_RVA:
        print('FAIL: call at 0x%X targets 0x%X, expected 0x%X' % (CALLSITE_RVA, target, GATE_RVA))
        return 1
    print('check : call at 0x%X -> 0x%X OK' % (CALLSITE_RVA, target))

    cur = bytes(data[off_gate:off_gate + 3])
    print('gate  : RVA 0x%X file offset 0x%X bytes %s' % (GATE_RVA, off_gate, cur.hex()))

    if mode == '--check':
        print('state : %s' % ('PATCHED' if cur == PATCH else 'stock'))
        return 0

    if cur == PATCH:
        print('state : already patched - nothing to do')
        return 0

    data[off_gate:off_gate + 3] = PATCH
    open(path, 'wb').write(data)
    verify = open(path, 'rb').read()[off_gate:off_gate + 3]
    ok = verify == PATCH
    print('state : patched -> %s (%s)' % (verify.hex(), 'OK' if ok else 'FAILED'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
