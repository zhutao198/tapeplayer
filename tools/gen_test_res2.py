#!/usr/bin/env python3
"""
Generate a minimal PADS-LOGIC-V9.0-CP936 ASCII file for TapeBook.
Includes CAEDECAL definitions for pin symbols and 2-pin resistor symbol.
"""

HEADER      = '*PADS-LOGIC-V9.0-CP936* DESIGN EXPORT FILE FROM PADS LOGIC V9.5'
PIN_DECAL   = 'PIN150           32000 32000 100 10 100 10 2 1 0 0 0 1'
PIN_LINES   = ['OPEN   2   1   255', '0     0    ', '-300  0    ']

# 2-pin resistor CAEDECAL (using PIN150 for both terminals)
RES_DECAL_HEAD = 'RES2             32000 32000 100 10 100 10 3 4 0 2 26 0'

# PART entry for R1
R1_PART  = 'R1           RES2              0    0    0 0 97 10 97 10 2 2 100 0 0 2'

def gen(output_path):
    lines = []
    lines.append(HEADER)
    lines.append('*SCH*        GENERAL PARAMETERS OF THE SCHEMATIC DESIGN')
    lines.append('')
    lines.append('CUR SHEET    1')
    lines.append('SHEET SIZE   A')
    lines.append('')
    lines.append('*CAEDECAL*  ITEMS')
    lines.append('')

    # Define PIN150 decal (from reference file)
    lines.append(PIN_DECAL)
    lines.append('TIMESTAMP 2026.07.11.00.00.00')
    lines.append('"Default Font"')
    lines.append('"Default Font"')
    lines.append('0 0 0 0 0 0 "Default Font"')
    lines.append('REF-DES')
    lines.append('0 0 0 0 0 0 "Default Font"')
    lines.append('PART-TYPE')
    for ll in PIN_LINES:
        lines.append(ll)
    lines.append('')

    # Define PINSHORT decal (100-unit pin)
    lines.append('PINSHORT          34000 34000 100 10 100 10 4 1 0 0 0 1')
    lines.append('TIMESTAMP 2026.07.11.00.00.00')
    lines.append('"Default Font"')
    lines.append('"Default Font"')
    lines.append('60 10 0 1 100 10 "Default Font"')
    lines.append('REF-DES')
    lines.append('140 10 0 8 100 10 "Default Font"')
    lines.append('PART-TYPE')
    lines.append('-530 10 0 1 100 10 "Default Font"')
    lines.append('*')
    lines.append('-70 10 0 1 100 10 "Default Font"')
    lines.append('*')
    lines.append('OPEN   2   10  255')
    lines.append('0     0    ')
    lines.append('100   0    ')
    lines.append('')

    # Define RES2 part decal (2-pin resistor with simple body graphic)
    lines.append(RES_DECAL_HEAD)
    lines.append('TIMESTAMP 2026.07.11.00.00.01')
    lines.append('"Default Font"')
    lines.append('"Default Font"')
    lines.append('200 -186 0 0 106 10 "Default Font"')
    lines.append('REF-DES')
    lines.append('0 0 0 0 100 10 "Default Font"')
    lines.append('PART-TYPE')
    lines.append('200 280 0 0 107 10 "Default Font"')
    lines.append('Comment')
    # Body graphic: rectangle
    lines.append('CLOSED 5   10  255')
    lines.append('0     0    ')
    lines.append('0     200  ')
    lines.append('300   200  ')
    lines.append('300   0    ')
    lines.append('0     0    ')
    # Pin 1 terminal (left, using PINSHORT)
    lines.append('T0     100   0 1 20 0 0 0 -210 0 0 9 PINSHORT')
    lines.append('P0 0 0 0 0 0 0 1 208')
    # Pin 2 terminal (right, using PINSHORT)
    lines.append('T300   100   0 0 20 0 0 0 -210 0 0 9 PINSHORT')
    lines.append('P0 0 0 0 0 0 0 1 208')
    lines.append('')

    # Sheet
    lines.append('*SHT*   1 SHEET1 -1 $$$NONE')
    lines.append('*CAE*        GENERAL PARAMETERS FOR THE SHEET')
    lines.append('')
    lines.append('*PART*       ITEMS')
    lines.append('')
    lines.append(R1_PART)
    lines.append('"Default Font"')
    lines.append('"Default Font"')
    lines.append('0 0 0 0 97 10 0 "Default Font"')
    lines.append('REF-DES')
    lines.append('0 0 0 0 97 10 0 "Default Font"')
    lines.append('PART-TYPE')
    lines.append('0 0 0 0 97 10 0 "Default Font"')
    lines.append('Comment')
    lines.append('1 -50 0 0 0')
    lines.append('2 -50 0 0 0')
    lines.append('')
    lines.append('*CONNECTION*')
    lines.append('*SIGNAL* TEST 0 0')
    lines.append('R1.1')
    lines.append('R1.2')
    lines.append('*NETNAMES*')
    lines.append('')
    lines.append('*END*     OF ASCII OUTPUT FILE')

    with open(output_path, 'wb') as f:
        for line in lines:
            f.write((line + '\r\n').encode('cp936', errors='replace'))

    print('Generated: ' + output_path)
    print('Lines: {}'.format(len(lines)))

if __name__ == '__main__':
    gen('hardware/test_res2.txt')
