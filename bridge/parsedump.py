import struct

path = r'C:\Users\admin\AppData\Local\Returnal\Steam\Saved\Crashes\UE4CC-Windows-2D56B795483DECDD2153B9989E03A282_0000\UE4Minidump.dmp'
data = open(path, 'rb').read()
assert data[:4] == b'MDMP'
num_streams = struct.unpack_from('<I', data, 8)[0]
dir_rva = struct.unpack_from('<I', data, 12)[0]
streams = {}
for i in range(num_streams):
    st, sz, rva = struct.unpack_from('<III', data, dir_rva + i * 12)
    streams.setdefault(st, []).append((sz, rva))
print('stream types:', sorted(streams.keys()))

mods = []
for sz, rva in streams.get(4, []):
    n = struct.unpack_from('<I', data, rva)[0]
    off = rva + 4
    for i in range(n):
        base, msize = struct.unpack_from('<QI', data, off)
        name_rva = struct.unpack_from('<I', data, off + 0x14)[0]
        ln = struct.unpack_from('<I', data, name_rva)[0]
        name = data[name_rva + 4:name_rva + 4 + ln].decode('utf-16-le', 'replace')
        mods.append((base, msize, name))
        off += 108
mods.sort()

def resolve(addr):
    for base, size, name in mods:
        if base <= addr < base + size:
            return '%s+0x%x' % (name, addr - base)
    # nearest below
    best = None
    for base, size, name in mods:
        if base <= addr and (best is None or base > best[0]):
            best = (base, name)
    if best:
        return '?? (nearest below: %s+0x%x)' % (best[1], addr - best[0])
    return '?? unmapped-below-all'

print()
print('=== modules named sl*, Returnal-Streamline* ===')
for base, size, name in mods:
    if 'sl' in name.lower() and ('interposer' in name.lower() or 'dlss' in name.lower() or 'reflex' in name.lower() or 'sl.common' in name.lower() or 'nvngx' in name.lower()):
        print('%016x %08x %s' % (base, size, name))
print('--- all modules mentioning streamline/interposer/ngx ---')
for base, size, name in mods:
    ln = name.lower()
    if 'interposer' in ln or 'streamline' in ln or 'ngx' in ln or 'dlss' in ln:
        print('%016x %08x %s' % (base, size, name))

print()
print('=== modules within 0x10000000 of fault addr 0x7ffdb9c7a220 (sorted by base) ===')
for base, size, name in mods:
    if 0x7ffdb8000000 <= base <= 0x7ffdbb000000:
        print('%016x %08x end=%016x %s' % (base, size, base + size, name))

print()
print('=== exception ===')
for sz, rva in streams.get(6, []):
    tid, = struct.unpack_from('<I', data, rva)
    code, flags = struct.unpack_from('<II', data, rva + 8)
    eaddr, = struct.unpack_from('<Q', data, rva + 24)
    nparam, = struct.unpack_from('<I', data, rva + 32)
    params = struct.unpack_from('<15Q', data, rva + 40)
    ctx_size, ctx_rva = struct.unpack_from('<II', data, rva + 160)
    print('thread', tid, 'code', hex(code), 'addr', hex(eaddr), 'nparam', nparam,
          'params', [hex(p) for p in params[:4]])
    print('RAISING ADDR:', resolve(eaddr))
    regs = {}
    for rn, roff in (('Rax', 0x78), ('Rcx', 0x80), ('Rdx', 0x88), ('Rbx', 0x90),
                     ('Rsp', 0x98), ('Rbp', 0xA0), ('Rsi', 0xA8), ('Rdi', 0xB0),
                     ('R8', 0xB8), ('R9', 0xC0), ('R10', 0xC8), ('R11', 0xD0),
                     ('R12', 0xD8), ('R13', 0xE0), ('R14', 0xE8), ('R15', 0xF0),
                     ('Rip', 0xF8)):
        regs[rn] = struct.unpack_from('<Q', data, ctx_rva + roff)[0]
    for rn in ('Rip', 'Rax', 'Rcx', 'Rdx', 'R8', 'R9', 'R10', 'R11', 'Rsp', 'Rbp', 'Rbx', 'Rsi', 'Rdi'):
        print('  %-4s %016x  %s' % (rn, regs[rn], resolve(regs[rn])))
    # stack scan from current thread: find thread record
    tl = streams.get(3, [])
    toff = tl[0][1]
    nthr = struct.unpack_from('<I', data, toff)[0]
    troff = toff + 4
    for i in range(nthr):
        tid_i, = struct.unpack_from('<I', data, troff)
        if tid_i == tid:
            st_start, = struct.unpack_from('<Q', data, troff + 24)
            st_size, st_rva = struct.unpack_from('<II', data, troff + 32)
            print('  stack range %016x - %016x (rva %x)' % (st_start, st_start + st_size, st_rva))
            rsp = regs['Rsp']
            if st_start <= rsp < st_start + st_size:
                so = rsp - st_start
                print('  --- stack qwords from RSP (module hits marked) ---')
                for q in range(0, min(0x300, st_size - so), 8):
                    v, = struct.unpack_from('<Q', data, st_rva + so + q)
                    tag = ''
                    for base, msize, name in mods:
                        if base <= v < base + msize:
                            tag = '  <- %s+0x%x' % (name, v - base)
                            break
                    print('  [%03x] %016x%s' % (q, v, tag))
        troff += 48
