d = open('build/audiobook_player.elf', 'rb').read()
print('GLYPH in elf:', b'GLYPH' in d)
print('MARKER in elf:', b'MARKER_REGEN_FONT_BUILD_2026' in d)
print('RGN in elf:', b'RGN' in d)
