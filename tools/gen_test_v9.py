#!/usr/bin/env python3
import sys

lines = [
    '*PADS-LOGIC-V9.0-CP936* DESIGN EXPORT FILE FROM PADS LOGIC V9.5',
    '*SCH*        GENERAL PARAMETERS OF THE SCHEMATIC DESIGN',
    '',
    'CUR SHEET    1              Current Active Sheet',
    'SHEET SIZE   A             ',
    '',
    '*SHT*   1 SHEET1 -1 $$$NONE',
    '*CAE*        GENERAL PARAMETERS FOR THE SHEET',
    '',
    '*PART*       ITEMS',
    '',
    'R1           RES               0    0    0 0 97 10 97 10 2 2 100 0 0 2',
    '"Default Font"',
    '"Default Font"',
    '0 0 0 0 97 10 0 "Default Font"',
    'REF-DES',
    '0 0 0 0 97 10 0 "Default Font"',
    'PART-TYPE',
    '0 0 0 0 97 10 0 "Default Font"',
    'Comment',
    '1 -50 0 0 0',
    '2 -50 0 0 0',
    '',
    '*CONNECTION*',
    '*SIGNAL* TEST 0 0',
    'R1.1',
    'R1.2',
    '*NETNAMES*',
    '',
    '*END*     OF ASCII OUTPUT FILE',
]

output = 'hardware/test_v9.txt'
with open(output, 'wb') as f:
    for line in lines:
        f.write((line + '\r\n').encode('cp936', errors='replace'))

print('OK: ' + output)
