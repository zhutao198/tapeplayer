import re
txt = open(r'D:\zhutao\audio_player\hardware\V1\audio_player.txt', encoding='utf-8', errors='ignore').read()
lines = txt.split('\n')
sig = None
u1 = {}
for ln in lines:
    m = re.match(r'\*SIGNAL\*\s+(\S+)', ln)
    if m:
        sig = m.group(1)
        continue
    if ln.startswith('*PART*'):
        sig = None
        continue
    mu = re.search(r'U1\.(\d+)', ln)
    if sig and mu:
        u1[int(mu.group(1))] = sig

# ESP32-S3-WROOM-1 datasheet table 3-1: module pin -> GPIO
ds = {1:'GND',2:'3V3',3:'EN',4:'GPIO4',5:'GPIO5',6:'GPIO6',7:'GPIO7',8:'GPIO15',
      9:'GPIO16',10:'GPIO17',11:'GPIO18',12:'GPIO8',13:'GPIO19',14:'GPIO20',15:'GPIO3',
      16:'GPIO46',17:'GPIO9',18:'GPIO10',19:'GPIO11',20:'GPIO12',21:'GPIO13',22:'GPIO14',
      23:'GPIO21',24:'GPIO47',25:'GPIO48',26:'GPIO45',27:'GPIO0',28:'GPIO35',29:'GPIO36',
      30:'GPIO37',31:'GPIO38',32:'GPIO39',33:'GPIO40',34:'GPIO41',35:'GPIO42',36:'GPIO44',
      37:'GPIO43',38:'GPIO2',39:'GPIO1',40:'GND',41:'EPAD'}

net = {}
for pin, s in u1.items():
    g = ds.get(pin)
    if g and g.startswith('GPIO'):
        net[s] = g

alias = {'IO0':'BOOT', 'SD_SD':'SD_CD'}
doc = {'BOOT':'GPIO0','BAT_DET':'GPIO1','CHRG':'GPIO2','KEY_STOP':'GPIO14',
       'I2S_SD':'GPIO4','I2S_DIN':'GPIO5','I2S_BCLK':'GPIO6','I2S_LRC':'GPIO7',
       'TFT_SCL':'GPIO8','KEY_PLAY':'GPIO9','SD_CS':'GPIO10','SD_MOSI':'GPIO11',
       'SD_MISO':'GPIO12','SD_SCLK':'GPIO13','TFT_BLK':'GPIO15','TFT_DC':'GPIO16',
       'TFT_RES':'GPIO17','TFT_SDA':'GPIO18','SD_CD':'GPIO38','LCD_POW_EN':'GPIO39',
       'POW_EN':'GPIO40','KEY_FF':'GPIO41','KEY_REV':'GPIO42','KEY_PREV':'GPIO46',
       'KEY_NEXT':'GPIO47','WS2812':'GPIO48'}

print('=== DOC vs NETLIST (resolved GPIO) ===')
for k in doc:
    nk = alias.get(k, k)
    n = net.get(nk, '?')
    flag = 'OK' if n == doc[k] else 'DIFF'
    print('%-12s doc=%-8s netlist=%-8s %s' % (k, doc[k], n, flag))

print('=== netlist signals not in doc ===')
for s in sorted(net):
    if s not in alias.values() and s not in doc:
        print('  ', s, '=', net[s])
