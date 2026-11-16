import struct,sys,re,hashlib,base64,os,subprocess
SALT=bytes.fromhex("518D64A635DED8C1E6B039B1C3E55230")
def nid(n): return base64.b64encode(hashlib.sha1(n.encode()+SALT).digest()[:8][::-1]).decode().replace("/","-").rstrip("=")
names=set()
for root in sys.argv[2:]:
    for dp,dn,fn in os.walk(root):
        for f in fn:
            p=os.path.join(dp,f)
            try: d=open(p,'rb').read()
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
strtab=dl+tags[0x61000035][0]; symtab=dl+tags[0x61000039][0]; symsz=tags[0x6100003f][0]
def s(o): return b[strtab+o:b.index(b'\0',strtab+o)].decode()
A="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"
libs={}
for v in tags.get(0x61000015,[]): libs[v>>48]=s(v&0xffffffff)
out={}
for k in range(0,symsz,24):
    nm,info,oth,shn,val,sz=struct.unpack_from("<IBBHQQ",b,symtab+k)
    n=s(nm)
    if '#' not in n: continue
    h,l,m=n.split('#')
    lid=0
    for c in l: lid=lid*64+A.index(c)
    out.setdefault(libs.get(lid,l),[]).append(db.get(h,'?'+h))
print(len(names),'names;', sum(len(v) for v in out.values()),'imports')
for l in sorted(out):
    v=out[l]; unk=sum(1 for x in v if x[0]=='?')
    print(f"{l}: {len(v)} ({unk} unnamed)")
    if re.search(sys.argv and os.environ.get('SHOW','Hmd|VrTracker|Camera|VideoOut|VrService|Move|Pad$|SystemService|GnmDriver'),l): print('   '+' '.join(sorted(v)))
