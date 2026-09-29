import struct, sys

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'build/audiobook_player.elf'
    data = open(path, 'rb').read()
    assert data[:4] == b'\x7fELF', 'not ELF'
    is64 = (data[4] == 2)
    if not is64:
        (e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags,
         e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from('<HHIIIIIHHHHHH', data, 16)
    else:
        (e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags,
         e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from('<HHIQQQIHHHHHH', data, 16)
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
    shstr = shdrs[e_shstrndx]
    def shname(n):
        return data[shstr['offset'] + n:].split(b'\x00')[0].decode('latin1')
    symtab = strtab = None
    for sh in shdrs:
        nm = shname(sh['name'])
        if nm == '.symtab': symtab = sh
        elif nm == '.strtab': strtab = sh
    assert symtab and strtab, 'no symtab'
    strtab_data = data[strtab['offset']:strtab['offset'] + strtab['size']]
    symentsize = 16 if not is64 else 24
    target = {}
    for i in range(symtab['size'] // symentsize):
        off = symtab['offset'] + i * symentsize
        if not is64:
            (st_name, st_value, st_size, st_info, st_other, st_shndx) = struct.unpack_from('<IIIBBH', data, off)
        else:
            (st_name, st_info, st_other, st_shndx, st_value, st_size) = struct.unpack_from('<IBBHQQ', data, off)
        name = strtab_data[st_name:].split(b'\x00')[0].decode('latin1')
        if name in ('cjk_unicode_map', 'cjk_unicode_map_num', 'cjk_font_raw', 'cjk_font_glyph_bytes'):
            target[name] = dict(value=st_value, size=st_size, shndx=st_shndx)
    def vaddr_to_off(vaddr):
        for sh in shdrs:
            if sh['addr'] <= vaddr < sh['addr'] + sh['size'] and sh['type'] != 8:
                return sh['offset'] + (vaddr - sh['addr'])
        return None
    map_addr = target['cjk_unicode_map']['value']
    raw_addr = target['cjk_font_raw']['value']
    # count entries by address gap (map immediately precedes raw in same section)
    NUM = (raw_addr - map_addr) // 4
    if 'cjk_font_glyph_bytes' in target:
        gbytes = struct.unpack_from('<I', data, vaddr_to_off(target['cjk_font_glyph_bytes']['value']))[0]
    else:
        gbytes = 32
    raw_base = vaddr_to_off(raw_addr)
    print('map addr=0x%x count=%d glyph_bytes=%d' % (map_addr, NUM, gbytes))
    entries = []
    for i in range(NUM):
        uni, idx = struct.unpack_from('<HH', data, vaddr_to_off(map_addr) + i*4)
        entries.append((uni, idx))
    def glyph(idx):
        return ' '.join('%02X'%x for x in data[raw_base + idx*gbytes: raw_base + idx*gbytes + 16])
    for u in (0x83B2, 0x8000, 0x7761, 0x5929, 0x6D0B):
        hit = [(uni,idx) for uni,idx in entries if uni==u]
        if hit:
            uni, idx = hit[0]
            print('U+%04X -> idx=%d  glyph16=%s' % (u, idx, glyph(idx)))
        else:
            print('U+%04X NOT in map' % u)
    # scan for any idx that equals 5077 to see which unicode points to 耀
    yao_idx = 5077
    pts = [hex(u) for u,idx in entries if idx==yao_idx]
    print('entries pointing to idx %d (耀):' % yao_idx, pts[:20], '... total', len(pts))
    # scan for any idx that equals 5498 (莲)
    lian_idx = 5498
    pts2 = [hex(u) for u,idx in entries if idx==lian_idx]
    print('entries pointing to idx %d (莲):' % lian_idx, pts2[:20], '... total', len(pts2))

main()
