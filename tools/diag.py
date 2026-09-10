import sys, os, zlib, struct

def read_ppm(p):
    d = open(p, 'rb').read()
    # P6\nW H\n255\n
    parts = d.split(b'\n', 3)
    assert parts[0] == b'P6'
    w, h = map(int, parts[1].split())
    px = parts[3]
    return w, h, px

def write_png(path, w, h, rgb):
    raw = b''.join(b'\x00' + rgb[y*w*3:(y+1)*w*3] for y in range(h))
    def chunk(t, data):
        c = t + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6))
    png += chunk(b'IEND', b'')
    open(path, 'wb').write(png)

def stats(name, px):
    n = len(px)//3
    mean = sum(px)/len(px)
    lo, hi = min(px), max(px)
    uniq = len(set(px[::97]))
    print(f"  {name:8s} mean={mean:7.2f} min={lo:3d} max={hi:3d} uniq~{uniq}")

def psnr(a, b):
    se = 0; n = 0
    for i in range(0, len(a), 3):
        for c in range(3):
            d = a[i+c] - b[i+c]
            se += d*d; n += 1
    mse = se/n
    return 99.0 if mse < 1e-9 else 10*__import__('math').log10(255*255/mse)

d = sys.argv[1] if len(sys.argv) > 1 else 'vfi_out'
pre = sys.argv[2] if len(sys.argv) > 2 else '960x540'
imgs = {}
for k in ('A', 'B', 'truth', 'gen'):
    w, h, px = read_ppm(os.path.join(d, f'{pre}_{k}.ppm'))
    imgs[k] = px
    stats(k, px)
    write_png(os.path.join(d, f'{pre}_{k}.png'), w, h, px)
print()
for k in ('A', 'B'):
    print(f"  PSNR(gen vs {k})   = {psnr(imgs['gen'], imgs[k]):6.2f} dB")
print(f"  PSNR(gen vs truth) = {psnr(imgs['gen'], imgs['truth']):6.2f} dB")
print(f"  PSNR(A   vs truth) = {psnr(imgs['A'], imgs['truth']):6.2f} dB")
print(f"  PSNR(B   vs truth) = {psnr(imgs['B'], imgs['truth']):6.2f} dB")
# where is the error concentrated?
w = 960
row_psnr = []
for y0 in range(0, 540, 54):
    se = 0; n = 0
    for y in range(y0, min(y0+54, 540)):
        for x in range(w):
            i = (y*w+x)*3
            for c in range(3):
                dd = imgs['gen'][i+c] - imgs['truth'][i+c]
                se += dd*dd; n += 1
    row_psnr.append(round(10*__import__('math').log10(255*255/(se/n)), 1))
print("\n  per-row-band PSNR (top->bottom, 10 bands):")
print("   ", row_psnr)
