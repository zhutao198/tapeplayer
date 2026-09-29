import struct, sys
# extract full cjk_unicode_map from ELF (same logic as elf_map.py)
data = open('build/audiobook_player.elf','rb').read()
is64 = (data[4]==2)
if not is64:
    (e_type,e_machine,e_version,e_entry,e_phoff,e_shoff,e_flags,e_ehsize,e_phentsize,e_phnum,e_shentsize,e_shnum,e_shstrndx)=struct.unpack_from('<HHIIIIIHHHHHH',data,16)
else:
    (e_type,e_machine,e_version,e_entry,e_phoff,e_shoff,e_flags,e_ehsize,e_phentsize,e_phnum,e_shentsize,e_shnum,e_shstrndx)=struct.unpack_from('<HHIQQQIHHHHHH',data,16)
shdrs=[]
for i in range(e_shnum):
    off=e_shoff+i*e_shentsize
    if not is64:
        (sh_name,sh_type,sh_flags,sh_addr,sh_offset,sh_size,sh_link,sh_info,sh_addralign,sh_entsize)=struct.unpack_from('<IIIIIIIIII',data,off)
    else:
        (sh_name,sh_type,sh_flags,sh_addr,sh_offset,sh_size,sh_link,sh_info,sh_addralign,sh_entsize)=struct.unpack_from('<IIQQQQIIQQ',data,off)
    shdrs.append(dict(name=sh_name,type=sh_type,addr=sh_addr,offset=sh_offset,size=sh_size))
shstr=shdrs[e_shstrndx]
def shname(n): return data[shstr['offset']+n:].split(b'\x00')[0].decode('latin1')
symtab=strtab=None
for sh in shdrs:
    if shname(sh['name'])=='.symtab': symtab=sh
    elif shname(sh['name'])=='.strtab': strtab=sh
sdata=data[strtab['offset']:strtab['offset']+strtab['size']]
symentsize=16 if not is64 else 24
maddr=gnum=None
for i in range(symtab['size']//symentsize):
    off=symtab['offset']+i*symentsize
    if not is64:
        (st_name,st_value,st_size,st_info,st_other,st_shndx)=struct.unpack_from('<IIIBBH',data,off)
    else:
        (st_name,st_info,st_other,st_shndx,st_value,st_size)=struct.unpack_from('<IBBHQQ',data,off)
    name=sdata[st_name:].split(b'\x00')[0].decode('latin1')
    if name=='cjk_unicode_map': maddr=st_value
    if name=='cjk_unicode_map_num': gnum=st_value
if gnum is None: gnum=7541
def v2o(v):
    for sh in shdrs:
        if sh['addr']<=v<sh['addr']+sh['size'] and sh['type']!=8:
            return sh['offset']+(v-sh['addr'])
    return None
moff=v2o(maddr)
N=gnum
def ent(i):
    o=moff+i*4
    u,idx=struct.unpack_from('<HH',data,o)
    return u,idx
# simulate cjk_unicode_to_glyph_idx
def search(u):
    lo,hi=0,N-1
    while lo<=hi:
        mid=(lo+hi)//2
        mu,_=ent(mid)
        if mu==u: return ent(mid)[1]
        elif mu<u: lo=mid+1
        else: hi=mid-1
    return -1
for code,name in [(0x83B2,'莲'),(0x8000,'耀'),(0x6D0B,'洋'),(0x7761,'睡'),(0x5929,'天'),(0x8F7B,'轻'),(0x97F3,'音'),(0x4E50,'乐')]:
    print('%s U+%04X -> idx %d' % (name, code, search(code)))
# also report sortedness
prev=-1; bad=0
for i in range(N):
    u,_=ent(i)
    if u<prev: bad+=1
    prev=u
print('unsorted entries count:', bad, ' total', N)
