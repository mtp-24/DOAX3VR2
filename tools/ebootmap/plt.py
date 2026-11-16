# plt.py EBOOT ASM NAMEDIRS... : import slot -> name, PLT stub -> name, and callers of selected imports
import struct,sys,re,hashlib,base64,os,collections
SALT=bytes.fromhex("518D64A635DED8C1E6B039B1C3E55230")
def nid(n): return base64.b64encode(hashlib.sha1(n.encode()+SALT).digest()[:8][::-1]).decode().replace("/","-").rstrip("=")
names=set()
for root in sys.argv[3:]:
    for dp,dn,fn in os.walk(root):
        for f in fn:
            try: d=open(os.path.join(dp,f),'rb').read()
            except Exception: continue
            for m in re.finditer(rb'\b_?sce[A-Z][A-Za-z0-9_]{3,80}',d): names.add(m.group().decode())
db={nid(n):n for n in names}
b=open(sys.argv[1],'rb').read()
phoff,=struct.unpack_from("<Q",b,0x20); phn,=struct.unpack_from("<H",b,0x38)
for i in range(phn):
    t,fl,o,va,pa,fs,ms,al=struct.unpack_from("<IIQQQQQQ",b,phoff+i*56)
    if t==2: dyn=(o,fs)
    elif t==0x61000000: dl=o
tags={}
for k in range(0,dyn[1],16):
    tag,val=struct.unpack_from("<QQ",b,dyn[0]+k); tags.setdefault(tag,[]).append(val)
strtab=dl+tags[0x61000035][0]; symtab=dl+tags[0x61000039][0]
jmprel=dl+tags[0x61000029][0]; jsz=tags[0x6100002d][0]
def s(o): return b[strtab+o:b.index(b'\0',strtab+o)].decode()
slots={}
for k in range(0,jsz,24):
    off,info,add=struct.unpack_from("<QQq",b,jmprel+k)
    nm,=struct.unpack_from("<I",b,symtab+(info>>32)*24)
    h=s(nm).split('#')[0]
    slots[off]=db.get(h,'?'+h)
stub={}
for line in open(sys.argv[2]):
    m=re.match(r'\s*([0-9a-f]+):\s+jmpq\s+\*.*# 0x([0-9a-f]+)',line)
    if m and int(m.group(2),16) in slots: stub[int(m.group(1),16)]=slots[int(m.group(2),16)]
import json
json.dump({hex(k):v for k,v in stub.items()},open(sys.argv[2]+'.stubs.json','w'))
print(len(slots),'slots',len(stub),'stubs')
want=re.compile(os.environ.get('WANT','Hmd|VrTracker|Camera|VideoOut|\?'))
calls=collections.defaultdict(list)
for line in open(sys.argv[2]):
    m=re.match(r'\s*([0-9a-f]+):\s+(callq|jmp)\s+0x([0-9a-f]+)',line)
    if m:
        t=int(m.group(3),16)
        if t in stub and want.search(stub[t]): calls[stub[t]].append(m.group(1))
for n in sorted(calls): print(n,len(calls[n]),' '.join(calls[n][:14]))
