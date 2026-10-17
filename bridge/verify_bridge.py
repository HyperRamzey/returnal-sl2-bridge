"""Verify the built bridge DLL: export surface + import table."""
import os
import struct
import sys

def pe_headers(data):
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

def r2o(secs, rva):
    for va, vsz, ra, rsz in secs:
        if va <= rva < va + max(vsz, rsz):
            return ra + (rva - va)
    return None

def exports(data, magic, opt, secs):
    dd = opt + (112 if magic == 0x20b else 96)
    exp_rva, _ = struct.unpack_from('<II', data, dd)
    if not exp_rva:
        return []
    o = r2o(secs, exp_rva)
    (_, _, _, _, _, _, nfunc, nnames, aof, aon, aono) = struct.unpack_from('<IIHHIIIIIII', data, o)
    names_off, ords_off, funcs_off = r2o(secs, aon), r2o(secs, aono), r2o(secs, aof)
    out = []
    for i in range(nnames):
        nrva = struct.unpack_from('<I', data, names_off + 4 * i)[0]
        nm = data[r2o(secs, nrva):].split(b'\0')[0].decode('latin1')
        ordi = struct.unpack_from('<H', data, ords_off + 2 * i)[0]
        frva = struct.unpack_from('<I', data, funcs_off + 4 * ordi)[0]
        out.append((nm, frva))
    return out

def imports(data, magic, opt, secs):
    dd = opt + (112 if magic == 0x20b else 96) + 8   # data directory entry 1 = imports
    imp_rva, _ = struct.unpack_from('<II', data, dd)
    res = {}
    if not imp_rva:
        return res
    o = r2o(secs, imp_rva)
    while True:
        ilt, ts, fc, nrva, iat = struct.unpack_from('<IIIII', data, o)
        if not nrva and not iat:
            break
        dll = data[r2o(secs, nrva):].split(b'\0')[0].decode('latin1')
        res[dll] = []
        o += 20
    return res

def main():
    path = sys.argv[1]
    data = open(path, 'rb').read()
    magic, opt, secs = pe_headers(data)
    exps = exports(data, magic, opt, secs)
    imps = imports(data, magic, opt, secs)

    print('=== %s ===' % os.path.basename(path))
    print('exports: %d' % len(exps))
    stock = set()
    stockfile = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'interposer133_exports.txt')
    if os.path.exists(stockfile):
        for line in open(stockfile, encoding='utf-8-sig'):
            line = line.strip()
            if line and not line.startswith('#') and not line.startswith('exports:'):
                stock.add(line)
    have = set(n for n, _ in exps)
    if stock:
        missing = sorted(stock - have)
        print('stock 1.3.3 names missing from bridge: %d' % len(missing))
        for m in missing[:20]:
            print('   MISSING', m)
        extra = sorted(have - stock)
        print('extra names in bridge: %d' % len(extra))
        for m in extra:
            print('   extra', m)
    print('imports:')
    for dll, fns in imps.items():
        print('   ', dll)
    return 0

if __name__ == '__main__':
    sys.exit(main())
