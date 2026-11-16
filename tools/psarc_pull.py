#!/usr/bin/env python3
"""psarc_pull.py REMOTE_PSARC OUTDIR [PATTERN...]: reads a PSARC 1.4 archive on the console over FTP (tools/drive.py's
connection) by byte ranges only: its header and table of contents, its manifest (entry 0, the file names), then each file
whose name contains a PATTERN (case-insensitive), decompressed (zlib blocks) into OUTDIR. Without PATTERNs: the names only."""
import sys, os, io, struct, zlib, ftplib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive

class R:
    def __init__(self, path):
        self.f = drive.ftp(); self.path = path
    def read(self, off, n):
        out = bytearray()
        self.f.voidcmd('TYPE I')
        conn = self.f.transfercmd('RETR ' + self.path, rest=off)
        while len(out) < n:
            b = conn.recv(min(1 << 20, n - len(out)))
            if not b: break
            out += b
        conn.close()
        try: self.f.voidresp()
        except ftplib.all_errors: pass
        return bytes(out)

def main():
    remote, outdir, pats = sys.argv[1], sys.argv[2], [p.lower() for p in sys.argv[3:]]
    r = R(remote)
    h = r.read(0, 32)
    magic, ver, comp, toclen, esz, n, bsz, flags = struct.unpack('>4sI4sIIIII', h)
    assert magic == b'PSAR', h[:4]
    toc = r.read(32, toclen - 32)
    ents = []
    for i in range(n):
        e = toc[i * esz:(i + 1) * esz]
        blk = struct.unpack('>I', e[16:20])[0]
        size = int.from_bytes(e[20:25], 'big'); off = int.from_bytes(e[25:30], 'big')
        ents.append((blk, size, off))
    bl = toc[n * esz:]
    w = 2 if bsz <= 65536 else 3 if bsz <= 1 << 24 else 4
    zs = [int.from_bytes(bl[i:i + w], 'big') for i in range(0, len(bl) - w + 1, w)]
    def get(i):
        blk, size, off = ents[i]
        out = bytearray(); pos = off
        nb = (size + bsz - 1) // bsz
        lens = [zs[blk + k] or bsz for k in range(nb)]
        raw = r.read(off, sum(lens)); p = 0
        for k, L in enumerate(lens):
            c = raw[p:p + L]; p += L
            want = min(bsz, size - len(out))
            if L == want or c[:1] != b'\x78':
                out += c
            else:
                try: out += zlib.decompress(c)
                except zlib.error: out += c
        return bytes(out[:size])
    names = get(0).decode('utf-8', 'replace').split('\n')
    print('%d entries, block %d, manifest %d names' % (n, bsz, len(names)))
    if not pats:
        for nm in names: print(nm)
        return
    os.makedirs(outdir, exist_ok=True)
    for i, nm in enumerate(names, 1):
        if any(p in nm.lower() for p in pats):
            d = get(i)
            dst = os.path.join(outdir, nm.lstrip('/'))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, 'wb').write(d)
            print('%s %d bytes' % (nm, len(d)))

main()
