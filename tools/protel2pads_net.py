import sys, re

def convert(infile, outfile):
    with open(infile, 'r', encoding='utf-8') as f:
        text = f.read()

    # Remove comments
    text = re.sub(r'^\*.*$(?:\n|$)', '', text, flags=re.MULTILINE)
    # Parse blocks [ ... ]
    blocks = re.findall(r'\[(.*?)\]', text, re.DOTALL)

    parts = []
    nets = []

    for block in blocks:
        lines = [l.strip() for l in block.strip().split('\n') if l.strip()]
        if not lines:
            continue
        # Net block: first line starts with N_ or is GND net
        if re.match(r'^N_', lines[0]) or lines[0] in ('N_GND',):
            net_name = lines[0]
            conns = lines[1:]
            nets.append((net_name, conns))
        elif not any('-' in l for l in lines):
            # Component block: Designator, Device, Footprint(opt)
            des = lines[0]
            fp = lines[2] if len(lines) >= 3 else (lines[1] if len(lines) >= 2 else '')
            parts.append((des, fp))

    with open(outfile, 'w', encoding='utf-8') as f:
        f.write('*PADS-PCB*\n*PART*\n')
        for des, fp in parts:
            if des.startswith('LED_'):
                fp = 'LED_0603'
            elif des.startswith('ENC'):
                fp = 'EC11'
            f.write(f'{des} {fp}\n')
        f.write('\n*NET*\n')
        for net_name, conns in nets:
            f.write(f'*SIGNAL* {net_name}\n')
            pads = []
            for c in conns:
                m = re.match(r'(\w+)-(.+)', c)
                if m:
                    pads.append(f'{m.group(1)}.{m.group(2)}')
            if pads:
                f.write(' '.join(pads) + '\n')
        f.write('*END*\n')

    print(f'OK: {infile} -> {outfile}')
    print(f'  Parts: {len(parts)}, Nets: {len(nets)}')

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print('Usage: python protel2pads_net.py <input.net> <output.asc>')
        sys.exit(1)
    convert(sys.argv[1], sys.argv[2])
