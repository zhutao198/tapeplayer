#!/usr/bin/env python3
import re
import sys
import os
from collections import OrderedDict

# ==============================================
# TAPEB00K V1.3 PADS Logic ASCII Generator
# Fixes: PASS->BI, I/O->BI, EPAD consistency, ASCII encoding
# ==============================================

# Map non-standard pin types to PADS-standard types
PIN_TYPE_MAP = {
    'PASS': 'BI',
    'I/O': 'BI',
    'IN': 'IN',
    'OUT': 'OUT',
    'OD': 'OD',
    'GND': 'GND',
    'PWR': 'PWR',
    'NC': 'NC',
}

IC_GATES = OrderedDict([
    ('U1', ('ESP32-S3-WROOM-1', OrderedDict([
        ('POWER', [(2, '3V3', 'PWR'), (41, 'GND', 'GND'), ('EPAD', 'EPAD', 'GND')]),
        ('I2S', [(38, 'I2S_DOUT', 'OUT'), (39, 'I2S_WS', 'OUT'), (40, 'I2S_BCK', 'OUT')]),
        ('SD_CARD', [(10, 'SD_CS', 'OUT'), (11, 'SD_MOSI', 'OUT'), (12, 'SD_MISO', 'IN'), (13, 'SD_SCLK', 'OUT')]),
        ('I2C', [(17, 'SDA', 'BI'), (18, 'SCL', 'OUT')]),
        ('BUTTONS', [(5, 'PLAY', 'IN'), (6, 'STOP', 'IN'), (8, 'PREV', 'IN'), (9, 'NEXT', 'IN'), (14, 'REW', 'IN'), (15, 'FF', 'IN')]),
        ('USB', [(24, 'USB_DP', 'BI'), (25, 'USB_DN', 'BI')]),
        ('ENCODER', [(20, 'ENC_A', 'IN'), (21, 'ENC_B', 'IN')]),
        ('SYSTEM', [(3, 'EN', 'IN'), (4, 'GPIO0', 'IN')]),
        ('NC', [(16, 'NC', 'NC'), (19, 'NC', 'NC'), (22, 'NC', 'NC'), (23, 'NC', 'NC'), (37, 'NC', 'NC')]),
    ]))),
    ('U2', ('MAX98357A', OrderedDict([
        ('POWER', [(3, 'GND', 'GND'), (9, 'VDD', 'PWR'), (10, 'GND', 'GND'), (14, 'GND', 'GND'), (16, 'GND', 'GND'), (18, 'GND', 'GND')]),
        ('I2S', [(4, 'WS', 'IN'), (5, 'BCK', 'IN'), (6, 'DIN', 'IN')]),
        ('GND2', [(7, 'GND', 'GND'), (8, 'GND', 'GND'), (11, 'GND', 'GND'), (15, 'GND', 'GND'), (17, 'GND', 'GND')]),
        ('OUTPUT', [(12, 'OUT_P', 'OUT'), (13, 'OUT_N', 'OUT')]),
        ('CTRL', [(19, 'GAIN', 'IN'), (20, 'SD_MODE', 'IN')]),
    ]))),
    ('U3', ('ME6211C33', OrderedDict([
        ('POWER', [(1, 'VIN', 'PWR'), (3, 'VOUT', 'PWR'), (5, 'GND', 'GND')]),
        ('NC', [(2, 'CE', 'NC'), (4, 'NC', 'NC')]),
    ]))),
    ('U4', ('TP4056', OrderedDict([
        ('POWER', [(3, 'GND', 'GND'), (4, 'VCC', 'PWR'), (6, 'BAT', 'PWR'), (8, 'CE', 'IN')]),
        ('STATUS', [(1, 'CHRG', 'OD'), (5, 'PROG', 'IN'), (7, 'FULL', 'OD')]),
        ('NC', [(2, 'TEMP', 'NC')]),
    ]))),
    ('J1', ('USB_Type-C', OrderedDict([
        ('POWER', [('A1', 'GND', 'GND'), ('A4', 'VBUS', 'PWR'), ('B1', 'GND', 'GND'), ('B4', 'VBUS', 'PWR')]),
        ('DATA', [('A2', 'DP', 'BI'), ('A3', 'DN', 'BI'), ('B2', 'DN', 'BI'), ('B3', 'DP', 'BI')]),
        ('NC', [('A5', 'CC1', 'NC'), ('A6', 'DP', 'NC'), ('A7', 'DN', 'NC'), ('A8', 'SBU1', 'NC'), ('B5', 'CC2', 'NC'), ('B6', 'DP', 'NC'), ('B7', 'DN', 'NC'), ('B8', 'SBU2', 'NC')]),
    ]))),
    ('J2', ('MicroSD_Card', OrderedDict([
        ('POWER', [(4, 'VCC', 'PWR'), (6, 'GND', 'GND')]),
        ('SPI', [(2, 'CS', 'IN'), (3, 'MOSI', 'IN'), (5, 'SCLK', 'IN'), (7, 'MISO', 'OUT')]),
        ('DETECT', [(8, 'CD', 'IN')]),
        ('NC', [(1, 'DAT2', 'NC')]),
    ]))),
    ('J3', ('OLED_SSD1306', OrderedDict([
        ('POWER', [(1, 'VCC', 'PWR'), (4, 'GND', 'GND')]),
        ('I2C', [(2, 'SCL', 'IN'), (3, 'SDA', 'BI')]),
        ('ADDR', [(5, 'SA0', 'IN')]),
    ]))),
])

def norm_type(ptype):
    return PIN_TYPE_MAP.get(ptype, 'BI')

def make_passive_gates(refdes, part_type):
    if refdes.startswith('LED_'):
        return {'part_type': part_type,
                'gates': OrderedDict([('LED', [(1, 'A', 'BI'), (2, 'K', 'BI')])])}
    if refdes.startswith('SW_'):
        return {'part_type': part_type,
                'gates': OrderedDict([('SWITCH', [(1, 'SW1', 'BI'), (2, 'SW2', 'BI')])])}
    if refdes == 'ENC1':
        return {'part_type': part_type,
                'gates': OrderedDict([('ENCODER', [(1, 'A', 'BI'), (2, 'B', 'BI'), (3, 'GND', 'GND'), (4, 'SW', 'BI'), (5, 'GND', 'GND')])])}
    if refdes == 'J4':
        return {'part_type': part_type,
                'gates': OrderedDict([('SPKR', [(1, 'SPK_P', 'BI'), (2, 'SPK_N', 'BI')])])}
    if refdes == 'J5':
        return {'part_type': part_type,
                'gates': OrderedDict([('CONN', [(3, 'P3', 'BI')])])}
    if refdes[0] in ('R', 'C'):
        return {'part_type': part_type,
                'gates': OrderedDict([('PASSIVE', [(1, 'P1', 'BI'), (2, 'P2', 'BI')])])}
    return None

def get_config(refdes, part_type):
    if refdes in IC_GATES:
        val = IC_GATES[refdes]
        return {'part_type': val[0], 'gates': val[1]}
    pg = make_passive_gates(refdes, part_type)
    if pg:
        return pg
    return {'part_type': part_type,
            'gates': OrderedDict([('GATE1', [(1, 'P1', 'BI'), (2, 'P2', 'BI')])])}

def parse_netlist(filepath):
    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()

    blocks = []
    depth = 0
    start = -1
    for i, ch in enumerate(content):
        if ch == '[':
            if depth == 0:
                start = i
            depth += 1
        elif ch == ']':
            depth -= 1
            if depth == 0 and start >= 0:
                blocks.append(content[start:i+1])
                start = -1

    components = OrderedDict()
    nets = OrderedDict()

    for block in blocks:
        lines = [l.strip() for l in block.strip('[]').split('\n') if l.strip()]
        first = lines[0] if lines else ''
        if first.startswith('N_'):
            net_name = first
            conns = []
            for l in lines[1:]:
                if l.startswith('N_'):
                    conns.append(l)
                elif '-' in l:
                    conns.append(l)
            nets[net_name] = conns
        else:
            refdes = first
            footprint = lines[1] if len(lines) > 1 else ''
            part_type = lines[2] if len(lines) > 2 else footprint
            components[refdes] = {'footprint': footprint, 'part_type': part_type}

    return components, nets

def fix_nets(nets):
    fixes = []
    if 'N_3V3' in nets:
        orig = list(nets['N_3V3'])
        nets['N_3V3'] = [c for c in nets['N_3V3'] if c not in ('ENC1-1', 'ENC1-2')]
        if len(nets['N_3V3']) != len(orig):
            fixes.append('Removed ENC1-1/ENC1-2 from N_3V3')
    if 'N_I2C_SDA' in nets:
        new_conns = []
        for c in nets['N_I2C_SDA']:
            new_conns.append('J3-3' if c == 'J3-1' else c)
        if new_conns != nets['N_I2C_SDA']:
            fixes.append('Corrected J3-1 to J3-3 in N_I2C_SDA')
        nets['N_I2C_SDA'] = new_conns
    if 'N_POWER_EN' in nets and 'N_EN_RESET' in nets:
        extra = [c for c in nets['N_POWER_EN'] if c not in nets['N_EN_RESET']]
        nets['N_EN_RESET'].extend(extra)
        del nets['N_POWER_EN']
        fixes.append('Merged N_POWER_EN into N_EN_RESET')
    if 'N_GAIN' in nets:
        gain_conns = [c for c in nets['N_GAIN'] if '-' in c and not c.startswith('N_')]
        if 'N_GND' in nets:
            for c in gain_conns:
                if c not in nets['N_GND']:
                    nets['N_GND'].append(c)
        else:
            nets['N_GND'] = gain_conns
        del nets['N_GAIN']
        fixes.append('Merged N_GAIN (U2-19) into N_GND')
    if 'N_OLED_ADDR' in nets:
        oled_conns = [c for c in nets['N_OLED_ADDR'] if '-' in c and not c.startswith('N_')]
        if 'N_GND' in nets:
            for c in oled_conns:
                if c not in nets['N_GND']:
                    nets['N_GND'].append(c)
        else:
            nets['N_GND'] = oled_conns
        del nets['N_OLED_ADDR']
        fixes.append('Merged N_OLED_ADDR (J3-5) into N_GND')
    return nets, fixes

def generate(output_path, components, nets):
    lines = ['*PADS-LOGIC*', '']

    def sort_key(rd):
        m = re.search(r'\d+', rd)
        num = int(m.group()) if m else 0
        order = {'U':0,'J':1,'R':2,'C':3,'LED':4,'SW':5,'ENC':6}
        return (order.get(rd.rstrip('0123456789_'), 9), num, rd)

    all_refdes = []
    seen = set()
    for rd in IC_GATES:
        all_refdes.append(rd); seen.add(rd)
    for rd in sorted(components.keys(), key=sort_key):
        if rd not in seen:
            all_refdes.append(rd); seen.add(rd)
    for conns in nets.values():
        for c in conns:
            if '-' in c and not c.startswith('N_'):
                rd = c.split('-')[0]
                if rd not in seen:
                    seen.add(rd); all_refdes.append(rd)

    # PART section
    lines.append('*PART*')
    for rd in all_refdes:
        info = components.get(rd, {'part_type': rd})
        cfg = get_config(rd, info['part_type'])
        lines.append('{} {}'.format(rd, cfg['part_type']))
    lines.append('')

    # GATES section
    lines.append('*GATES*')
    for rd in all_refdes:
        info = components.get(rd, {'part_type': rd})
        cfg = get_config(rd, info['part_type'])
        for gname in cfg['gates']:
            lines.append('{} {} {}'.format(rd, gname, cfg['part_type']))
    lines.append('')

    # PINS section
    lines.append('*PINS*')
    for rd in all_refdes:
        info = components.get(rd, {'part_type': rd})
        cfg = get_config(rd, info['part_type'])
        for gname, pins in cfg['gates'].items():
            for pin_num, label, ptype in pins:
                lines.append('{} {} {} {} {}'.format(
                    rd, pin_num, gname, pin_num, norm_type(ptype)))
    lines.append('')

    # Build pin->net mapping from original netlist connections
    pin_net = {}
    for net_name, conns in nets.items():
        for c in conns:
            if '-' in c and not c.startswith('N_'):
                key = c
                if key not in pin_net:
                    pin_net[key] = net_name

    # NET section
    lines.append('*NET*')
    net_pins = {}
    for key, net_name in pin_net.items():
        net_pins.setdefault(net_name, []).append(key)

    for net_name, pin_list in net_pins.items():
        lines.append('*SIGNAL* {}'.format(net_name))
        pads = ['{}.{}'.format(*k.split('-', 1)) for k in pin_list]
        lines.append(' '.join(pads))

    lines.append('')
    lines.append('*END*')

    # Write as pure ASCII (no BOM, no Unicode)
    with open(output_path, 'w', encoding='ascii') as f:
        f.write('\n'.join(lines))

    return lines

def report(lines, components, nets):
    parts = set()
    gates = []
    pins = []
    for l in lines:
        if l == '*PART*': sec = 'part'; continue
        if l == '*GATES*': sec = 'gates'; continue
        if l == '*PINS*': sec = 'pins'; continue
        if l == '*NET*': sec = 'net'; continue
        if l.startswith('*END*'): break
        if l.startswith('*'): continue
        if l.strip():
            if sec == 'part': parts.add(l.split()[0])
            elif sec == 'gates': gates.append(l)
            elif sec == 'pins': pins.append(l)
    signals = sum(1 for l in lines if l.startswith('*SIGNAL*'))

    print('Components: {}'.format(len(parts)))
    print('Gates:      {}'.format(len(gates)))
    print('Pins:       {}'.format(len(pins)))
    print('Signals:    {}'.format(signals))
    print('')
    for rd in sorted(parts, key=lambda x: (x[0], int(re.search(r'\d+', x).group()) if re.search(r'\d+', x) else 0)):
        info = components.get(rd, {'part_type': rd})
        cfg = get_config(rd, info['part_type'])
        print('  {}: {} ({} pins, {} gates)'.format(rd, cfg['part_type'],
            sum(len(p) for p in cfg['gates'].values()), len(cfg['gates'])))
    print('')
    for l in lines:
        if l.startswith('*SIGNAL*'):
            print('  ' + l.split('*SIGNAL*', 1)[-1].strip())

def main():
    input_path = 'hardware/SCH_TapeBook_V1.3.net'
    output_path = 'hardware/SCH_TapeBook_V1.3_logic.asc'

    if not os.path.exists(input_path):
        print('ERROR: input not found: ' + input_path)
        sys.exit(1)

    print('Parsing: ' + input_path)
    components, nets_raw = parse_netlist(input_path)
    print('  Components: {}'.format(len(components)))
    print('  Nets:       {}'.format(len(nets_raw)))

    nets, fixes = fix_nets(nets_raw)
    print('')
    print('Fixes:')
    for f in fixes:
        print('  - ' + f)

    print('')
    print('Generating: ' + output_path)
    lines = generate(output_path, components, nets)

    report(lines, components, nets)
    return 0

if __name__ == '__main__':
    main()
