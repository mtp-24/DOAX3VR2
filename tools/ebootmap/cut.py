# cut.py ASM START END : disassembly lines in [START,END), import calls named
import sys,json,re
asm=sys.argv[1]; a=int(sys.argv[2],16); b=int(sys.argv[3],16)
st=json.load(open(asm+'.stubs.json'))
for l in open(asm):
    m=re.match(r'\s*([0-9a-f]+):\s+(.*)',l)
    if not m: continue
    x=int(m.group(1),16)
    if x<a: continue
    if x>=b: break
    t=m.group(2).strip()
    c=re.search(r'(callq|jmp)\s+0x([0-9a-f]+)',t)
    if c and '0x'+c.group(2) in st: t+='   ; '+st['0x'+c.group(2)]
    print('%x: %s'%(x,re.sub(r'\s+',' ',t)))
