import re
d = open('/tmp/b.out','rb').read()
ESC = bytes([27])
nbsu, nesu = d.count(ESC + b'[?2026h'), d.count(ESC + b'[?2026l')
fg = len(re.findall(re.escape(ESC + b'[38;2;'), d))
bg = len(re.findall(re.escape(ESC + b'[48;2;'), d))
cur = len(re.findall(re.escape(ESC) + rb'\[\d+;\d+H', d))
apc = d.count(ESC + b'_G')
# any ESC not introducing CSI, OSC, DCS, APC, ST, or charset
bad = [m.start() for m in re.finditer(re.escape(ESC) + rb'(?![\x5b\x5dP_\\(%])', d)]
print("total bytes       %s" % format(len(d), ','))
print("frames BSU/ESU    %d / %d   balanced=%s" % (nbsu, nesu, nbsu == nesu))
print("full-block glyphs %s" % format(d.count(chr(0x2588).encode()), ','))
print("half-block glyphs %s" % format(d.count(chr(0x2580).encode()), ','))
print("SGR fg 38;2       %s" % format(fg, ','))
print("SGR bg 48;2       %s" % format(bg, ','))
print("cursor moves      %s" % format(cur, ','))
print("APC sequences     %d" % apc)
print("malformed ESC     %d" % len(bad))
if bad:
    print("   first at %d: %r" % (bad[0], d[max(0,bad[0]-40):bad[0]+20]))
if nbsu:
    per = len(d) // nbsu
    print("bytes/frame       %s  (880 cells -> %.1f B/cell)" % (format(per, ','), per/880.0))
