"""Render a terminal escape stream to PNG so frame output can be inspected.
Supports: CSI positioning, SGR (38;2/48;2/38;5/48;5/0/7), half/full blocks,
and UTF-8. This is a test tool for verification only."""
import re, sys, zlib, struct

def png_write(path, w, h, rgb):
    raw = b''.join(b'\x00' + bytes(rgb[y*w*3:(y+1)*w*3]) for y in range(h))
    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    hdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)
    open(path,'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', hdr) +
        chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))

def to256(n):
    if n < 16:
        base = [(0,0,0),(128,0,0),(0,128,0),(128,128,0),(0,0,128),(128,0,128),
                (0,128,128),(192,192,192),(128,128,128),(255,0,0),(0,255,0),
                (255,255,0),(0,0,255),(255,0,255),(0,255,255),(255,255,255)]
        return base[n]
    if n < 232:
        n -= 16
        lv = [0,95,135,175,215,255]
        return (lv[n//36], lv[(n//6)%6], lv[n%6])
    v = 8 + (n-232)*10
    return (v,v,v)

def render(data, cols, rows, cw, ch, out_png):
    W, H = cols*cw, rows*ch
    px = bytearray(W*H*3)
    fg, bg = (255,255,255), (0,0,0)
    using_bg = False
    cx = cy = 0
    i = 0
    n = len(data)
    def put(x, y, rgb):
        for dy in range(ch):
            yy = y*ch+dy
            if yy >= H: return
            for dx in range(cw):
                xx = x*cw+dx
                if xx >= W: return
                o = (yy*W+xx)*3
                px[o:o+3] = bytes(rgb)
    while i < n:
        b = data[i]
        if b == 0x1b and i+1 < n:
            if data[i+1:i+2] == b'[':
                m = re.match(rb'\[([0-9;?]*)([A-Za-z])', data[i+1:])
                if not m: i += 1; continue
                params, cmd = m.group(1), m.group(2)
                i += 1 + m.end()
                if cmd == b'H':
                    parts = params.split(b';') if params else [b'1',b'1']
                    cy = int(parts[0] or 1)-1
                    cx = int(parts[1] or 1)-1 if len(parts)>1 else 0
                elif cmd == b'J':
                    px[:] = b'\x00'*len(px)
                elif cmd == b'm':
                    ps = [int(p or 0) for p in params.split(b';')] if params else [0]
                    k = 0
                    while k < len(ps):
                        p = ps[k]
                        if p == 0: fg, bg, using_bg = (255,255,255),(0,0,0),False
                        elif p == 7: fg, bg = bg, fg
                        elif p == 38 and k+1 < len(ps):
                            if ps[k+1] == 2: fg = tuple(ps[k+2:k+5]); k += 4
                            elif ps[k+1] == 5: fg = to256(ps[k+2]); k += 2
                        elif p == 48 and k+1 < len(ps):
                            if ps[k+1] == 2: bg = tuple(ps[k+2:k+5]); k += 4
                            elif ps[k+1] == 5: bg = to256(ps[k+2]); k += 2
                        elif p == 39: fg = (255,255,255)
                        elif p == 49: bg = (0,0,0); using_bg = False
                        k += 1
                continue
            elif data[i+1:i+2] == b'_':      # APC (kitty) - skip to ST
                j = data.find(b'\x1b\\', i)
                i = (j+2) if j > 0 else n
                continue
            elif data[i+1:i+2] in (b']', b'P'):   # OSC/DCS - skip to BEL or ST
                j1, j2 = data.find(b'\x07', i), data.find(b'\x1b\\', i)
                cands = [x for x in (j1, j2) if x > 0]
                i = (min(cands)+1) if cands else n
                continue
            else:
                i += 2
                continue
        # glyph
        if b < 0x80:
            ch_len = 1
            try: s = data[i:i+1].decode()
            except: s = ' '
        else:
            m2 = re.match(rb'[\xc0-\xff][\x80-\xbf]*', data[i:])
            try: s = m2.group(0).decode()
            except: s = ' '
            ch_len = len(m2.group(0)) if m2 else 1
        i += ch_len
        if s in ('\n',):
            cy += 1; cx = 0; continue
        if s == '\r':
            cx = 0; continue
        cp = ord(s[0]) if s else 32
        if using_bg and bg:
            put(cx, cy, bg)
        if cp == 0x2580:      # upper half: fg top, bg bottom
            for dy in range(ch//2):
                yy = cy*ch+dy
                if yy < H:
                    for dx in range(cw):
                        xx=cx*cw+dx
                        if xx<W:
                            o=(yy*W+xx)*3; px[o:o+3]=bytes(fg)
            for dy in range(ch//2, ch):
                yy = cy*ch+dy
                if yy < H:
                    for dx in range(cw):
                        xx=cx*cw+dx
                        if xx<W:
                            o=(yy*W+xx)*3; px[o:o+3]=bytes(bg)
        elif cp == 0x2588:    # full block
            put(cx, cy, fg)
        elif cp in (0x2591,0x2592,0x2593):
            put(cx, cy, tuple((a+b)//2 for a,b in zip(fg,bg)))
        elif s in (' ',):
            put(cx, cy, bg); 
        else:
            put(cx, cy, fg)
        cx += 1
        if cx >= cols:
            cx = 0; cy += 1
    png_write(out_png, W, H, px)
    print(f"{out_png}  {W}x{H}")

if __name__ == '__main__':
    src, cols, rows, cw, ch, out = sys.argv[1], *map(int, sys.argv[2:6]), sys.argv[6]
    render(open(src,'rb').read(), cols, rows, cw, ch, out)
