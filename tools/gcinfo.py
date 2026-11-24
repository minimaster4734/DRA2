import sys, struct, collections
iso=open(sys.argv[1],'rb')
def rd(o,n): iso.seek(o); return iso.read(n)
be=lambda b:int.from_bytes(b,'big')
h=rd(0,0x440)
dol_off=be(h[0x420:0x424]); fst_off=be(h[0x424:0x428]); fst_sz=be(h[0x428:0x42c])
print('ID',h[:6],'title',h[0x20:0x60].split(b'\0')[0],'disc#',h[6],'ver',h[7])
print(f'DOL @0x{dol_off:x}  FST @0x{fst_off:x} size 0x{fst_sz:x}')
d=rd(dol_off,0x100)
tof=struct.unpack('>18I',d[0:72]); ta=struct.unpack('>18I',d[0x48:0x90]); ts=struct.unpack('>18I',d[0x90:0xd8])
bss,bsz,entry=struct.unpack('>3I',d[0xd8:0xe4])
tot=0
for i in range(18):
    if ts[i]: print(f"  {'T' if i<7 else 'D'}{i if i<7 else i-7}: file+0x{tof[i]:x} -> 0x{ta[i]:08x} size 0x{ts[i]:x}"); tot+=ts[i]
print(f'  BSS 0x{bss:08x} size 0x{bsz:x} entry 0x{entry:08x} total {tot/1e6:.2f}MB')
fst=rd(fst_off,fst_sz)
n=be(fst[8:12]); names=fst[n*12:]
def nm(o):
    e=names.index(b'\0',o); return names[o:e].decode('latin1')
files=[]
def walk(i,end,path):
    while i<end:
        e=fst[i*12:i*12+12]; isdir=e[0]; no=be(e[1:4]); a=be(e[4:8]); b=be(e[8:12])
        if isdir:
            walk(i+1,b,path+nm(no)+'/'); i=b
        else:
            files.append((path+nm(no),a,b)); i+=1
walk(1,n,'')
print(len(files),'files, total',sum(f[2] for f in files)/1e6,'MB')
ext=collections.Counter(); sz=collections.Counter()
for p,a,b in files:
    e=p.rsplit('.',1)[-1].lower() if '.' in p else '(none)'; ext[e]+=1; sz[e]+=b
for e,c in ext.most_common(): print(f'  .{e}: {c} files {sz[e]/1e6:.1f}MB')
open(sys.argv[2],'w').write('\n'.join(f'{a:#010x} {b:9d} {p}' for p,a,b in files))
