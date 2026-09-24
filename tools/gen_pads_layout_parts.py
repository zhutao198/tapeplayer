import re

def convert_netlist_to_pads_layout(netlist_file, output_file):
    with open(netlist_file, 'r', encoding='utf-8', errors='ignore') as f:
        content = f.read()

    # Clean up content
    lines = []
    for line in content.split('\n'):
        line = line.strip()
        if not line or line.startswith('*//') or line.startswith('**'):
            continue
        lines.append(line)

    # Parse components
    components = []
    i = 0
    while i < len(lines):
        if lines[i].startswith('*PART*'):
            i += 1
            while i < len(lines) and not lines[i].startswith('*'):
                if lines[i].strip() and ' ' in lines[i]:
                    comp, part = lines[i].split(' ', 1)
                    components.append((comp, part))
                i += 1
            continue
        i += 1

    # Parse nets
    nets = []
    i = 0
    while i < len(lines):
        if lines[i].startswith('*SIGNAL*'):
            net_name = lines[i].replace('*SIGNAL*', '').strip()
            i += 1
            connections = []
            while i < len(lines) and not lines[i].startswith('*'):
                if '-' in lines[i]:
                    comp, pin = lines[i].split('-', 1)
                    comp = comp.strip()
                    # Filter valid components
                    if any(comp.startswith(c[0]) for c in components):
                        connections.append(f'{comp}.{pin}')
                i += 1
            if connections:
                nets.append((net_name, connections))
            continue
        i += 1

    # Generate PADS Layout ASCII file
    with open(output_file, 'w', encoding='utf-8') as f:
        f.write('*PADS-PCB*\n')
        f.write('*PART*\n')
        for comp, part in components:
            # Ensure component naming is consistent
            comp = comp.strip()
            part = part.strip()
            if comp and part:
                f.write(f'{comp} {part}\n')
        f.write('\n*NET*\n')
        for net_name, connections in nets:
            f.write(f'*SIGNAL* {net_name}\n')
            f.write(' '.join(connections) + '\n')
        f.write('*END*\n')

    print(f'Generated PADS Layout ASCII: {output_file}')
    print(f'  Components: {len(components)}')
    print(f'  Nets: {len(nets)}')

# Run the conversion
convert_netlist_to_pads_layout('hardware/SCH_TapeBook_V1.3_pads.asc', 'hardware/SCH_TapeBook_V1.3_pads_layout.asc')
