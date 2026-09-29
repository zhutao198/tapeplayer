import struct, sys

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'build/audiobook_player.elf'
    data = open(path, 'rb').read()
    assert data[:4] == b'\x7fELF', 'not ELF'
    ei_class = data[4]  # 1=32, 2=64
    is64 = (ei_class == 2)
    # ELF header
    if not is64:
        (e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags,
         e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from('<HHIIIIIHHHHHH', data, 16)
    else:
        # 64-bit
        (e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags,
         e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from('<HHIQQQIHHHHHH', data, 16)
    # section header parse
    shdrs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        if not is64:
            (sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info,
             sh_addralign, sh_entsize) = struct.unpack_from('<IIIIIIIIII', data, off)
        else:
            (sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info,
             sh_addralign, sh_entsize) = struct.unpack_from('<IIQQQQIIQQ', data, off)
        shdrs.append(dict(name=sh_name, type=sh_type, flags=sh_flags, addr=sh_addr,
                          offset=sh_offset, size=sh_size, link=sh_link, info=sh_info,
                          addralign=sh_addralign, entsize=sh_entsize))
    # section name string table
    shstr = shdrs[e_shstrndx]
    def shname(n):
        s = data[shstr['offset'] + n:].split(b'\x00')[0].decode('latin1')
        return s
    # find symbol tables
    symtab = None
    strtab = None
    for sh in shdrs:
        nm = shname(sh['name'])
        if nm == '.symtab':
            symtab = sh
        elif nm == '.strtab':
            strtab = sh
    assert symtab and strtab, 'no symtab/strtab'
    strtab_data = data[strtab['offset']:strtab['offset'] + strtab['size']]
    # parse symbols
    symentsize = 16 if not is64 else 24
    target = {}
    for i in range(symtab['size'] // symentsize):
        off = symtab['offset'] + i * symentsize
        if not is64:
            (st_name, st_value, st_size, st_info, st_other, st_shndx) = struct.unpack_from('<IIIBBH', data, off)
        else:
            (st_name, st_info, st_other, st_shndx, st_value, st_size) = struct.unpack_from('<IBBHQQ', data, off)
        name = strtab_data[st_name:].split(b'\x00')[0].decode('latin1')
        if name in ('cjk_font_raw', 'cjk_unicode_map', 'cjk_font_glyph_bytes'):
            target[name] = dict(value=st_value, size=st_size, shndx=st_shndx)
    print('symbols:', {k: (hex(v['value']), v['size']) for k, v in target.items()})
    # map virtual addr -> file offset via section headers
    def vaddr_to_off(vaddr):
        for sh in shdrs:
            if sh['addr'] <= vaddr < sh['addr'] + sh['size'] and sh['type'] != 8:  # not NOBITS
                return sh['offset'] + (vaddr - sh['addr'])
        return None
    raw = target['cjk_font_raw']
    base_off = vaddr_to_off(raw['value'])
    gbytes = struct.unpack_from('<I', data, vaddr_to_off(target['cjk_font_glyph_bytes']['value']))[0] if 'cjk_font_glyph_bytes' in target else 32
    print('cjk_font_glyph_bytes =', gbytes, ' base_file_off =', hex(base_off))
    def glyph_bytes(idx):
        off = base_off + idx * gbytes
        return data[off:off + gbytes]
    for idx, name in [(5498, 'LIAN(莲)'), (5077, 'YAO(耀)'), (3601, 'YANG(洋)'), (4421, 'SHUI(睡)'), (1425, 'TIAN(天)')]:
        b = glyph_bytes(idx)
        print(name, 'idx', idx, ' '.join('%02X' % x for x in b[:16]))

main()
