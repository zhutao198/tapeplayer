#!/usr/bin/env python3
"""TapeBook PADS 9.5 Design ASCII Generator.
- Library-matched parts (RES/CAP/LED from USR.pt9): use library gate decals
- Custom parts (ICs/connectors): use inline CAEDECAL decals

Usage: py -3 tools/gen_pads_design_ascii.py
Output: hardware/SCH_TapeBook_V1.3_pads.txt
"""

import os
from collections import OrderedDict

# ============================================================
# LIBRARY GATE DECAL MAPPING (from USR.pt9)
# Parts that exist in USR.pt9 library use these decals.
# User must have USR.pt9 loaded in PADS.
# ============================================================
LIB_DECALS = {
    # Resistors
    'RES-0603-10K':  'GB_RES',
    'RES-0603-4.7K': 'GB_RES',
    'RES-0603-100K': 'GB_RES',
    'RES-0603-22R':  'GB_RES',
    # Capacitors
    'CAP-0603-0.1UF': 'GB_CAP',
    'CAP-0603-1UF':   'GB_CAP',
    'CAP-0603-100PF': 'GB_CAP',
    # LED
    'LED_0603': 'GB_DIODE',
}

# ============================================================
# PART DEFINITIONS
# ============================================================
PART_DEFS = OrderedDict()

def def_part(name, gates):
    PART_DEFS[name] = gates

# -- 2-pin passives --
def_part('RES-0603-10K',  OrderedDict([('PASSIVE', [(1, '1', 'BI'), (2, '2', 'BI')])]))
def_part('RES-0603-4.7K', OrderedDict([('PASSIVE', [(1, '1', 'BI'), (2, '2', 'BI')])]))
def_part('RES-0603-100K',OrderedDict([('PASSIVE', [(1, '1', 'BI'), (2, '2', 'BI')])]))
def_part('RES-0603-22R',  OrderedDict([('PASSIVE', [(1, '1', 'BI'), (2, '2', 'BI')])]))
def_part('CAP-0603-0.1UF',OrderedDict([('PASSIVE', [(1, '1', 'BI'), (2, '2', 'BI')])]))
def_part('CAP-0603-1UF',  OrderedDict([('PASSIVE', [(1, '1', 'BI'), (2, '2', 'BI')])]))
def_part('CAP-0603-100PF',OrderedDict([('PASSIVE', [(1, '1', 'BI'), (2, '2', 'BI')])]))
def_part('R_0603_CUSTOM', OrderedDict([('PASSIVE', [(1, '1', 'BI'), (2, '2', 'BI')])]))
def_part('CE_1210',       OrderedDict([('PASSIVE', [(1, '1', 'BI'), (2, '2', 'BI')])]))
def_part('LED_0603',      OrderedDict([('LED', [(1, 'A', 'BI'), (2, 'K', 'BI')])]))
def_part('SW_PB_6x6',     OrderedDict([('SWITCH', [(1, 'SW1', 'BI'), (2, 'SW2', 'BI')])]))
def_part('SPEAKER_2PIN',  OrderedDict([('SPKR', [(1, 'SPK_P', 'BI'), (2, 'SPK_N', 'BI')])]))
def_part('EC11', OrderedDict([
    ('ENCODER', [(1, 'A', 'BI'), (2, 'B', 'BI'), (3, 'GND', 'GND'),
                 (4, 'SW', 'BI'), (5, 'GND', 'GND')]),
]))
def_part('SS12D00', OrderedDict([
    ('SWITCH', [(1, 'COM', 'BI'), (2, 'NO', 'BI'), (3, 'NC', 'BI')]),
]))

# -- ICs & Connectors --
def_part('ESP32-S3-WROOM-1', OrderedDict([
    ('POWER', [(2, '3V3', 'PWR')]),
    ('GND', [(1, 'GND', 'GND'), (41, 'GND', 'GND'), ('EPAD', 'GND', 'GND')]),
    ('SYSTEM', [(3, 'EN', 'IN'), (4, 'GPIO0', 'IN')]),
    ('USB', [(24, 'USB_DP', 'BI'), (25, 'USB_DN', 'BI')]),
    ('I2S', [(38, 'I2S_DOUT', 'OUT'), (39, 'I2S_WS', 'OUT'), (40, 'I2S_BCK', 'OUT')]),
    ('SD_CARD', [(10, 'SD_CS', 'OUT'), (11, 'SD_MOSI', 'OUT'),
                 (13, 'SD_SCLK', 'OUT'), (12, 'SD_MISO', 'IN')]),
    ('I2C', [(17, 'SDA', 'BI'), (18, 'SCL', 'OUT')]),
    ('BUTTONS', [(5, 'PLAY', 'IN'), (6, 'STOP', 'IN'), (8, 'PREV', 'IN'),
                 (9, 'NEXT', 'IN'), (14, 'REW', 'IN'), (15, 'FF', 'IN')]),
    ('ENCODER', [(20, 'ENC_A', 'IN'), (21, 'ENC_B', 'IN')]),
    ('NC', [(16, 'GPIO16', 'NC'), (19, 'GPIO21', 'NC'),
            (22, 'GPIO48', 'NC'), (23, 'GPIO46', 'NC'), (37, 'GPIO7', 'NC')]),
]))

def_part('MAX98357A', OrderedDict([
    ('POWER', [(9, 'VDD', 'PWR')]),
    ('PGND', [(11, 'PGND', 'GND'), (18, 'PGND', 'GND')]),
    ('GND', [(3, 'GND', 'GND'), (7, 'GND', 'GND'), (8, 'GND', 'GND'),
             (10, 'GND', 'GND'), (14, 'GND', 'GND'), (15, 'GND', 'GND'),
             (16, 'GND', 'GND'), (17, 'GND', 'GND')]),
    ('INPUT', [(4, 'LRC', 'IN'), (5, 'BCLK', 'IN'), (6, 'DIN', 'IN'), (1, 'MCLK', 'IN')]),
    ('OUTPUT', [(12, 'OUT_P', 'OUT'), (13, 'OUT_N', 'OUT')]),
    ('CTRL', [(19, 'GAIN', 'IN'), (20, 'SD_MODE', 'IN')]),
]))

def_part('ME6211C33', OrderedDict([
    ('POWER', [(1, 'VIN', 'PWR'), (5, 'GND', 'GND'), (3, 'VOUT', 'PWR')]),
    ('CTRL', [(2, 'CE', 'IN')]),
    ('NC', [(4, 'NC', 'NC')]),
]))

def_part('TP4056', OrderedDict([
    ('POWER', [(4, 'VCC', 'PWR'), (6, 'BAT', 'PWR'), (3, 'GND', 'GND'), (8, 'GND', 'GND')]),
    ('STATUS', [(1, 'CHRG', 'OD'), (7, 'STDBY', 'OD'), (5, 'PROG', 'IN')]),
    ('NC', [(2, 'TEMP', 'NC')]),
]))

def_part('USB_Type-C', OrderedDict([
    ('POWER', [('A4', 'VBUS', 'PWR'), ('B4', 'VBUS', 'PWR'),
               ('A1', 'GND', 'GND'), ('B1', 'GND', 'GND')]),
    ('USB', [('A2', 'DP', 'BI'), ('B3', 'DP', 'BI'),
             ('A3', 'DN', 'BI'), ('B2', 'DN', 'BI')]),
    ('NC', [('A5', 'CC1', 'NC'), ('B5', 'CC2', 'NC'),
            ('A6', 'DPLUS', 'NC'), ('B6', 'DPLUS', 'NC'),
            ('A7', 'DN2', 'NC'), ('B7', 'DN2', 'NC'),
            ('A8', 'SBU1', 'NC'), ('B8', 'SBU2', 'NC')]),
]))

def_part('MicroSD_Card', OrderedDict([
    ('POWER', [(4, 'VCC', 'PWR'), (6, 'GND', 'GND')]),
    ('SPI', [(2, 'CS', 'IN'), (3, 'MOSI', 'IN'), (5, 'SCLK', 'IN'), (7, 'MISO', 'OUT')]),
    ('DETECT', [(8, 'CD', 'IN')]),
    ('NC', [(1, 'DAT2', 'NC')]),
]))

def_part('OLED_SSD1306', OrderedDict([
    ('POWER', [(1, 'VCC', 'PWR'), (4, 'GND', 'GND')]),
    ('I2C', [(2, 'SCL', 'IN'), (3, 'SDA', 'BI')]),
    ('ADDR', [(5, 'SA0', 'IN')]),
    ('RESET', [(6, 'RESET', 'IN')]),
]))

# ============================================================
# DECAL DISPATCH
# ============================================================
def is_library_part(name):
    return name in LIB_DECALS

def get_decal_name(name):
    if name in LIB_DECALS:
        return LIB_DECALS[name]
    return name  # inline: own name

def get_pcl_name(name):
    if name in LIB_DECALS:
        return 'UND'  # no PCB footprint reference for library parts
    return name       # inline: own name = both PARTTYPE + decal name

# ============================================================
# INSTANCES
# ============================================================
INSTANCES = [
    ('U4', 'TP4056', 2000, 5000, 0),
    ('J1', 'USB_Type-C', 1000, 6000, 0),
    ('U3', 'ME6211C33', 3000, 4000, 0),
    ('C7', 'CE_1210', 1000, 4000, 0),
    ('C8', 'CE_1210', 1500, 4500, 0),
    ('C5', 'CE_1210', 2500, 5000, 0),
    ('C6', 'CE_1210', 3500, 4000, 0),
    ('C1', 'CAP-0603-0.1UF', 3500, 3500, 0),
    ('C2', 'CAP-0603-0.1UF', 1000, 3500, 0),
    ('C3', 'CAP-0603-0.1UF', 1500, 3500, 0),
    ('C4', 'CAP-0603-0.1UF', 2000, 3500, 0),
    ('R_PROG', 'R_0603_CUSTOM', 2000, 3000, 0),
    ('R_DISCHG', 'RES-0603-100K', 3000, 3500, 0),
    ('LED_CHG', 'LED_0603', 500, 5000, 0),
    ('LED_FULL', 'LED_0603', 500, 5500, 0),
    ('U1', 'ESP32-S3-WROOM-1', 6000, 5000, 0),
    ('R1', 'RES-0603-10K', 7000, 7000, 0),
    ('R2', 'RES-0603-10K', 7500, 3000, 0),
    ('R3', 'RES-0603-10K', 8000, 3000, 0),
    ('R4', 'RES-0603-10K', 5000, 7000, 0),
    ('R5', 'RES-0603-10K', 5500, 7000, 0),
    ('R6', 'RES-0603-10K', 6000, 7000, 0),
    ('R7', 'RES-0603-10K', 6500, 7000, 0),
    ('C11', 'CAP-0603-1UF', 6500, 6500, 0),
    ('SW_BOOT', 'SW_PB_6x6', 5000, 8000, 0),
    ('U2', 'MAX98357A', 10000, 5000, 0),
    ('C12', 'CE_1210', 11000, 4000, 0),
    ('RSD_MODE', 'R_0603_CUSTOM', 11000, 6000, 0),
    ('J4', 'SPEAKER_2PIN', 12000, 5000, 0),
    ('J2', 'MicroSD_Card', 6000, 2000, 0),
    ('J3', 'OLED_SSD1306', 6000, 1000, 0),
    ('R8', 'RES-0603-4.7K', 7000, 1000, 0),
    ('R9', 'RES-0603-4.7K', 7500, 1000, 0),
    ('SW_PLAY', 'SW_PB_6x6', 5000, 10000, 0),
    ('SW_STOP', 'SW_PB_6x6', 5500, 10000, 0),
    ('SW_PREV', 'SW_PB_6x6', 6000, 10000, 0),
    ('SW_NEXT', 'SW_PB_6x6', 6500, 10000, 0),
    ('SW_REW', 'SW_PB_6x6', 7000, 10000, 0),
    ('SW_FF', 'SW_PB_6x6', 7500, 10000, 0),
    ('ENC1', 'EC11', 8500, 10000, 0),
    ('C9', 'CAP-0603-100PF', 8500, 9500, 0),
    ('C10', 'CE_1210', 9000, 9500, 0),
]

# ============================================================
# CONNECTIONS
# ============================================================
CONNECTIONS = {
    'N_3V3': ['U1-2', 'U3-3', 'U2-9', 'J2-4', 'J3-1', 'R1-1', 'R2-1',
              'R8-1', 'R9-1', 'RSD_MODE-1', 'ENC1-4'],
    'N_GND': ['U1-1', 'U1-41', 'U1-EPAD', 'U2-3', 'U2-7', 'U2-8', 'U2-10',
              'U2-14', 'U2-15', 'U2-16', 'U2-17', 'U2-18', 'U2-11', 'U2-19',
              'U3-5', 'U4-3', 'U4-8', 'J1-A1', 'J1-B1', 'J2-6', 'J3-4',
              'J3-5', 'SW_BOOT-2', 'SW_PLAY-2', 'SW_STOP-2', 'SW_PREV-2',
              'SW_NEXT-2', 'SW_REW-2', 'SW_FF-2',
              'C1-2', 'C2-2', 'C3-2', 'C4-2', 'C5-2', 'C6-2', 'C7-2', 'C8-2',
              'ENC1-3', 'ENC1-5'],
    'N_VBUS_5V': ['J1-A4', 'J1-B4', 'U4-4', 'C7-1'],
    'N_BAT_PLUS': ['U4-6', 'C8-1'],
    'N_SWITCHED_PWR': ['U3-1', 'C5-1'],
    'N_I2S_BCK': ['U1-40', 'U2-5'],
    'N_I2S_WS': ['U1-39', 'U2-4'],
    'N_I2S_DOUT': ['U1-38', 'U2-6'],
    'N_SD_CS': ['U1-10', 'J2-2', 'R2-2'],
    'N_SD_MOSI': ['U1-11', 'J2-3', 'R3-2'],
    'N_SD_MISO': ['U1-12', 'J2-7'],
    'N_SD_SCLK': ['U1-13', 'J2-5'],
    'N_I2C_SDA': ['U1-17', 'J3-3', 'R8-2'],
    'N_I2C_SCL': ['U1-18', 'J3-2', 'R9-2'],
    'N_BTN_PLAY': ['U1-5', 'SW_PLAY-1'],
    'N_BTN_STOP': ['U1-6', 'SW_STOP-1'],
    'N_BTN_PREV': ['U1-8', 'SW_PREV-1'],
    'N_BTN_NEXT': ['U1-9', 'SW_NEXT-1'],
    'N_BTN_REW': ['U1-14', 'SW_REW-1'],
    'N_BTN_FF': ['U1-15', 'SW_FF-1'],
    'N_GPIO0_BOOT': ['U1-4', 'R4-1', 'SW_BOOT-1'],
    'N_EN_RESET': ['U1-3', 'R1-2', 'C11-1'],
    'N_USB_DN': ['U1-25', 'J1-A3', 'J1-B2'],
    'N_USB_DP': ['U1-24', 'J1-A2', 'J1-B3'],
    'N_SPEAKER_OUT': ['U2-12', 'J4-1'],
    'N_SPEAKER_OUTN': ['U2-13', 'J4-2'],
    'N_SD_MODE': ['U2-20', 'RSD_MODE-2'],
    'N_SD_CD': ['J2-8', 'R6-2'],
    'N_ENC_A': ['U1-20', 'ENC1-1', 'C9-1'],
    'N_ENC_B': ['U1-21', 'ENC1-2', 'C10-1'],
    'N_CHRG_LED': ['U4-1', 'LED_CHG-1', 'R7-1'],
    'N_FULL_LED': ['U4-7', 'LED_FULL-1', 'R5-1'],
    'N_PROG': ['U4-5', 'R_PROG-1'],
}

# ============================================================
# GENERATORS
# ============================================================
TS = '2026.07.13.12.00.00'

def gen_caedecal():
    """CAEDECAL section — only for inline (non-library) parts."""
    lines = []

    # Pin terminal types
    for pname, draw in [
        ('PIN10', 'OPEN   2   1   255\n0     0\n-100  0'),
        ('PIN50', 'OPEN   2   1   255\n0     0\n-100  0'),
        ('PIN100', 'OPEN   2   1   255\n0     0\n-100  0'),
        ('PIN200', 'OPEN   2   1   255\n0     0\n-200  0'),
    ]:
        lines.append(f'{pname:<16}32000 32000 100 10 100 10 2 1 0 0 0 1')
        lines.append(f'TIMESTAMP {TS}')
        lines.append('"Default Font"')
        lines.append('"Default Font"')
        lines.append('210 0 0 8 107 10 "Default Font"')
        lines.append('REF-DES')
        lines.append('-20 0 0 1 107 10 "Default Font"')
        lines.append('PART-TYPE')
        lines.append(draw)
        lines.append('')

    # Inline decals for non-library parts
    for pname, gates in PART_DEFS.items():
        if is_library_part(pname):
            continue  # decal comes from USR.pt9 library
        total_pins = sum(len(pins) for pins in gates.values())
        if total_pins <= 4:
            body_w, body_h = 200, 100
        elif total_pins <= 10:
            body_w, body_h = 300, 200
        elif total_pins <= 20:
            body_w, body_h = 400, 300
        elif total_pins <= 30:
            body_w, body_h = 500, 400
        else:
            body_w, body_h = 600, 500

        lines.append(f'{pname:<24}32000 32000 100 10 100 10 2 1 0 2 24 0')
        lines.append(f'TIMESTAMP {TS}')
        lines.append('"Default Font"')
        lines.append('"Default Font"')
        lines.append('0 0 0 0 100 10 "Default Font"')
        lines.append('REF-DES')
        lines.append('0 0 0 0 100 10 "Default Font"')
        lines.append('PART-TYPE')
        lines.append('CLOSED 5   10  255')
        lines.append(f'0     {-body_h}')
        lines.append(f'0     0')
        lines.append(f'{body_w}     0')
        lines.append(f'{body_w}     {-body_h}')
        lines.append(f'0     {-body_h}')

        pin_list = []
        for gname, pins in gates.items():
            for pin_num, label, ptype in pins:
                pin_list.append((pin_num, label, ptype))
        n = len(pin_list)

        if n <= 2:
            for i in range(n):
                term_x = -50 if i == 0 else body_w + 50
                ty = int(-body_h / 2)
                lines.append(f'T0     {term_x}   {ty} 0 20 0 0 0 -210 0 0 9 PIN100')
                lines.append(f'P0 0 0 0 0 0 0 1 208')
        else:
            half = (n + 1) // 2
            for i in range(n):
                if i < half:
                    term_x = -50
                    spacing = max(int((body_h - 100) / max(half - 1, 1)), 20)
                    ty = int(-body_h + 50 + i * spacing)
                else:
                    term_x = body_w + 50
                    r = n - half
                    spacing = max(int((body_h - 100) / max(r - 1, 1)), 20)
                    ty = int(-body_h + 50 + (i - half) * spacing)
                lines.append(f'T0     {term_x}   {ty} 0 20 0 0 0 -210 0 0 9 PIN100')
                lines.append(f'P0 0 0 0 0 0 0 1 208')
        lines.append('')
    return '\n'.join(lines)

def gen_parttype():
    lines = []
    for pname, gates in PART_DEFS.items():
        total_pins = sum(len(pins) for pins in gates.values())
        decal = get_decal_name(pname)
        pcl = get_pcl_name(pname)
        lines.append(f'{pname} {pcl} 1 0 0 0')
        lines.append(f'TIMESTAMP {TS}')
        lines.append(f'GATE 1 {total_pins} 0')
        lines.append(decal)  # GATE name = CAEDECAL entry name (library or inline)
        for gname, pins in gates.items():
            for pin_num, label, ptype in pins:
                lines.append(f'{pin_num} 0 U {label}')
        lines.append('')
    return '\n'.join(lines)

def gen_part():
    lines = []
    for refdes, ptype, x, y, rot in INSTANCES:
        gates = PART_DEFS[ptype]
        total_pins = sum(len(pins) for pins in gates.values())
        body_lines = 8 + total_pins
        lines.append(f'{refdes:<10} {ptype:<24}{x:>5} {y:>5}   {rot} 0 100 10 100 10 3 0 {total_pins} 0 0 {body_lines}')
        lines.append('"Default Font"')
        lines.append('"Default Font"')
        lines.append('0 0 0 0 100 10 0 "Default Font"')
        lines.append('REF-DES')
        lines.append('0 0 0 0 100 10 0 "Default Font"')
        lines.append('PART-TYPE')
        lines.append('0 0 0 0 100 10 0 "Default Font"')
        lines.append('Comment')
        for i in range(total_pins):
            lines.append(f'{i} -50 0 0 0')
        lines.append('')
    return '\n'.join(lines)

def _pos(ref):
    for r, _, x, y, _ in INSTANCES:
        if r == ref:
            return x, y
    return 0, 0

def gen_connection():
    lines = []
    for net_name, conns in CONNECTIONS.items():
        lines.append(f'*SIGNAL* {net_name} 0 0')
        heads = [c.split('-') for c in conns if '-' in c and not c.startswith('N_')]
        for i in range(len(heads) - 1):
            ref1, pin1 = heads[i]
            ref2, pin2 = heads[i+1]
            x1, y1 = _pos(ref1)
            x2, y2 = _pos(ref2)
            lines.append(f'{ref1}.{pin1}        {ref2}.{pin2}        2 0')
            lines.append(f'{x1}   {y1}')
            lines.append(f'{x2}   {y2}')
        lines.append('')
    lines.append('*NETNAMES*')
    return '\n'.join(lines)

def gen_full():
    header = '*PADS-LOGIC-V9.0-CP936* DESIGN EXPORT FILE FROM PADS LOGIC V9.5'
    sch = f'''*SCH*        GENERAL PARAMETERS OF THE SCHEMATIC DESIGN

CUR SHEET    1
SHEET SIZE   A

*SHT*   1 SHEET1 -1 $$$NONE
*CAE*        GENERAL PARAMETERS FOR THE SHEET

SCALE        71.154
WINDOWCENTER 6000   5000
BITCS0       0
BITCS1       0
BITCS2       0

*TEXT*       FREE TEXT

*LINES*      LINES ITEMS'''
    caedecal = '*CAEDECAL*  ITEMS\n\n' + gen_caedecal()
    parttype = '*PARTTYPE*   ITEMS\n\n' + gen_parttype()
    part = '*PART*       ITEMS\n\n' + gen_part()
    conn = '*CONNECTION*\n\n' + gen_connection()
    end = '*END*     OF ASCII OUTPUT FILE'
    return '\n\n'.join([header, sch, caedecal, parttype, part, conn, end])

def main():
    output_path = os.path.join(os.path.dirname(os.path.dirname(__file__)),
                               'hardware', 'SCH_TapeBook_V1.3_pads.txt')
    content = gen_full()
    with open(output_path, 'w', encoding='utf-8') as f:
        f.write(content)
    print(f'Generated: {output_path}')
    print(f'Size: {len(content)} bytes, {content.count(chr(10))} lines')
    total = len(INSTANCES)
    lib = sum(1 for _, p, _, _, _ in INSTANCES if is_library_part(p))
    inline = total - lib
    print(f'Library decal (USR.pt9): {lib} instances')
    print(f'Inline decal:            {inline} instances')

if __name__ == '__main__':
    main()
