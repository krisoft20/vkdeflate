#!/usr/bin/env python3
"""Draws docs/img/*.svg from docs/data/*.csv and results/*.csv, and refreshes the per-card table in
README.md between the <!-- cards --> markers. Standard library only.

    python tools/plot.py
"""
import csv
import glob
import math
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W = 760
FONT = "system-ui, -apple-system, 'Segoe UI', sans-serif"

LIGHT = """
.bg{fill:#ffffff}.p{fill:#0b0b0b}.s{fill:#52514e}.m{fill:#898781}
.grid{stroke:#e1e0d9;stroke-width:1}.axis{stroke:#c3c2b7;stroke-width:1}
.c1{fill:#2a78d6}.c2{fill:#eb6834}.c3{fill:#1baf7a}.cg{fill:#898781}
.l1{stroke:#2a78d6}.l2{stroke:#eb6834}.l3{stroke:#1baf7a}
.o1{fill:#86b6ef}.o2{fill:#2a78d6}.o3{fill:#104281}
.ring{stroke:#ffffff;stroke-width:2}
.box{fill:#f4f3ef;stroke:#c3c2b7;stroke-width:1}.hi{fill:#cde2fb;stroke:#2a78d6;stroke-width:1}
"""

DARK = """
.bg{fill:#0d1117}.p{fill:#f0efec}.s{fill:#c3c2b7}.m{fill:#898781}
.grid{stroke:#2c2c2a;stroke-width:1}.axis{stroke:#383835;stroke-width:1}
.c1{fill:#3987e5}.c2{fill:#d95926}.c3{fill:#199e70}.cg{fill:#898781}
.l1{stroke:#3987e5}.l2{stroke:#d95926}.l3{stroke:#199e70}
.o1{fill:#184f95}.o2{fill:#3987e5}.o3{fill:#9ec5f4}
.ring{stroke:#0d1117;stroke-width:2}
.box{fill:#1a1a19;stroke:#383835;stroke-width:1}.hi{fill:#0d366b;stroke:#3987e5;stroke-width:1}
"""


def esc(s):
    return str(s).replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


def num(v, digits=2):
    s = ('%.*f' % (digits, v)).rstrip('0').rstrip('.')
    return s if s else '0'


def ticks(vmax, want=5):
    step = 10 ** math.floor(math.log10(vmax / want))
    for m in (1, 2, 5, 10):
        if vmax / (step * m) <= want:
            step *= m
            break
    out = []
    v = 0.0
    while v < vmax + step * 0.5:
        out.append(v)
        v += step
    return out


class Svg:
    def __init__(self, h, title, sub):
        self.h = h
        self.el = ['<rect width="%d" height="%d" rx="8" class="bg"/>' % (W, h)]
        self.title = title
        self.text(16, 24, title, 'p', 15, weight=600)
        self.text(16, 43, sub, 's', 12)

    def text(self, x, y, s, cls, size=12, anchor='start', weight=400):
        self.el.append('<text x="%.1f" y="%.1f" class="%s" font-size="%d" font-weight="%d" text-anchor="%s">%s</text>'
                       % (x, y, cls, size, weight, anchor, esc(s)))

    def line(self, x0, y0, x1, y1, cls):
        self.el.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" class="%s"/>' % (x0, y0, x1, y1, cls))

    def rect(self, x, y, w, h, cls, rx=0, tip=None):
        t = '<title>%s</title>' % esc(tip) if tip else ''
        self.el.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="%d" class="%s">%s</rect>' % (x, y, w, h, rx, cls, t))

    def bar_h(self, x0, x1, y, h, cls, tip):
        r = min(4.0, (x1 - x0) / 2)
        d = 'M%.1f,%.1f H%.1f Q%.1f,%.1f %.1f,%.1f V%.1f Q%.1f,%.1f %.1f,%.1f H%.1f Z' % (
            x0, y, x1 - r, x1, y, x1, y + r, y + h - r, x1, y + h, x1 - r, y + h, x0)
        self.el.append('<path d="%s" class="%s"><title>%s</title></path>' % (d, cls, esc(tip)))

    def bar_v(self, x, w, y0, y1, cls, tip):
        r = min(4.0, (y0 - y1) / 2)
        d = 'M%.1f,%.1f V%.1f Q%.1f,%.1f %.1f,%.1f H%.1f Q%.1f,%.1f %.1f,%.1f V%.1f Z' % (
            x, y0, y1 + r, x, y1, x + r, y1, x + w - r, x + w, y1, x + w, y1 + r, y0)
        self.el.append('<path d="%s" class="%s"><title>%s</title></path>' % (d, cls, esc(tip)))

    def poly(self, pts, cls):
        self.el.append('<polyline points="%s" fill="none" stroke-width="2" stroke-linejoin="round" stroke-linecap="round" class="%s"/>'
                       % (' '.join('%.1f,%.1f' % p for p in pts), cls))

    def dot(self, x, y, cls, tip):
        self.el.append('<circle cx="%.1f" cy="%.1f" r="5" class="%s ring"><title>%s</title></circle>' % (x, y, cls, esc(tip)))

    def legend(self, items, y=64, x=16):
        for cls, label in items:
            self.rect(x, y - 9, 10, 10, cls, rx=2)
            self.text(x + 15, y, label, 's', 12)
            x += 15 + 6.4 * len(label) + 22

    def save(self, name, desc):
        for suffix, css in (('', LIGHT), ('-dark', DARK)):
            file = name.replace('.svg', suffix + '.svg')
            body = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d" role="img" '
                    'aria-labelledby="t d" font-family="%s">\n<title id="t">%s</title>\n<desc id="d">%s</desc>\n'
                    '<style>%s</style>\n%s\n</svg>\n'
                    % (W, self.h, W, self.h, FONT, esc(self.title), esc(desc), css, '\n'.join(self.el)))
            with open(os.path.join(ROOT, 'docs', 'img', file), 'w', encoding='utf-8', newline='\n') as f:
                f.write(body)
        print('plot: docs/img/%s and -dark' % name)


def rows(rel):
    path = os.path.join(ROOT, rel)
    if not os.path.exists(path):
        return []
    with open(path, encoding='utf-8', newline='') as f:
        return list(csv.DictReader(f))


def chart_pairs(data, name, title, sub, desc):
    data = sorted(data, key=lambda r: -r['khr'] / r['vk'])
    left, right, top, pitch, bh = 150, 60, 84, 38, 13
    vmax = max(max(r['khr'], r['vk']) for r in data)
    tk = ticks(vmax)
    scale = (W - left - right) / tk[-1]
    h = top + pitch * len(data) + 34
    s = Svg(h, title, sub)
    s.legend([('c1', 'vkdeflate'), ('cg', 'Khronos layer (NVIDIA shaders)')])
    base = top + pitch * len(data)
    for t in tk:
        x = left + t * scale
        s.line(x, top - 6, x, base, 'grid')
        s.text(x, base + 16, num(t, 1) + ' ms', 'm', 11, 'middle')
    for i, r in enumerate(data):
        y = top + i * pitch
        s.text(left - 10, y + bh + 5, r['stream'], 's', 12, 'end')
        s.bar_h(left, left + r['vk'] * scale, y, bh, 'c1', '%s, vkdeflate: %s ms' % (r['stream'], num(r['vk'])))
        s.bar_h(left, left + r['khr'] * scale, y + bh + 2, bh, 'cg', '%s, Khronos layer: %s ms' % (r['stream'], num(r['khr'])))
        s.text(left + max(r['khr'], r['vk']) * scale + 8, y + bh + 5, '%.1f×' % (r['khr'] / r['vk']), 'p', 12)
    s.line(left, top - 6, left, base, 'axis')
    s.save(name, desc)


def chart_saturation():
    data = rows('docs/data/saturation.csv')
    if not data:
        return
    left, right, top, ph = 56, 70, 70, 220
    xs = [math.log2(float(r['tiles'])) for r in data]
    ys = [float(r['gbps']) for r in data]
    tk = ticks(max(ys))
    sx = (W - left - right) / max(xs)
    sy = ph / tk[-1]
    s = Svg(top + ph + 54, 'A dispatch needs hundreds of tiles before the GPU is full',
            'GB/s of output, kernel time, first n tiles of mozilla on an RX 6650 XT')
    base = top + ph
    for t in tk:
        y = base - t * sy
        s.line(left, y, W - right, y, 'grid')
        s.text(left - 8, y + 4, num(t, 1), 'm', 11, 'end')
    pts = [(left + x * sx, base - y * sy) for x, y in zip(xs, ys)]
    s.poly(pts, 'l1')
    for (px, py), r in zip(pts, data):
        s.dot(px, py, 'c1', '%s tiles: %s GB/s, %s ms' % (r['tiles'], num(float(r['gbps'])), r['kernel_ms']))
        s.text(px, base + 18, r['tiles'], 'm', 11, 'middle')
    s.text(pts[0][0] + 8, pts[0][1] - 14, '%s GB/s' % num(float(data[0]['gbps'])), 'p', 12)
    s.text(pts[-1][0] + 10, pts[-1][1] + 4, '%s GB/s' % num(float(data[-1]['gbps']), 1), 'p', 12)
    s.line(left, base, W - right, base, 'axis')
    s.text((left + W - right) / 2, base + 40, 'tiles in one dispatch (64 KB each)', 's', 12, 'middle')
    s.save('saturation.svg', 'Line chart: output rate grows from %s GB/s at one tile to %s GB/s at 512 tiles.'
           % (num(float(data[0]['gbps'])), num(float(data[-1]['gbps']), 1)))


def chart_residency():
    data = rows('docs/data/residency.csv')
    if not data:
        return
    series = [('mozilla_512', 'mozilla, 512 tiles', 'l1', 'c1'), ('provinces_3125', 'provinces.bin, 3125 tiles', 'l2', 'c2'),
              ('silesia_2048', 'silesia, 2048 tiles', 'l3', 'c3')]
    left, right, top, ph = 56, 170, 84, 240
    waves = [int(r['waves_per_simd']) for r in data]
    vmax = max(float(r[k]) for r in data for k, _, _, _ in series)
    tk = ticks(vmax)
    sx = (W - left - right) / (max(waves) - min(waves))
    sy = ph / tk[-1]
    s = Svg(top + ph + 54, 'Kernel time grows when fewer waves fit on a SIMD',
            'kernel ms against waves per SIMD (shared memory padded to lower residency), RX 6650 XT')
    s.legend([(c, label) for _, label, _, c in series])
    base = top + ph
    for t in tk:
        y = base - t * sy
        s.line(left, y, W - right, y, 'grid')
        s.text(left - 8, y + 4, num(t, 1), 'm', 11, 'end')
    for w in waves:
        s.text(left + (w - min(waves)) * sx, base + 18, str(w), 'm', 11, 'middle')
    for key, label, lc, cc in series:
        pts = [(left + (int(r['waves_per_simd']) - min(waves)) * sx, base - float(r[key]) * sy) for r in data]
        pts.sort()
        s.poly(pts, lc)
        for (px, py), r in zip(pts, sorted(data, key=lambda r: int(r['waves_per_simd']))):
            s.dot(px, py, cc, '%s, %s waves: %s ms' % (label, r['waves_per_simd'], r[key]))
        s.text(pts[-1][0] + 12, pts[-1][1] + 4, '%s ms, %s' % (num(float(data[0][key])), label.split(',')[0]), 'p', 12)
    s.line(left, base, W - right, base, 'axis')
    s.text((left + W - right) / 2, base + 40, 'waves resident per SIMD', 's', 12, 'middle')
    s.save('residency.svg', 'Line chart: kernel time rises as residency drops from 8 to 2 waves per SIMD, for three streams.')


def grouped(groups, series, name, title, sub, desc, unit, label_first=False):
    left, right, top, ph, bw = 56, 20, 100, 240, 20
    vmax = max(v for _, vals in groups for v in vals)
    tk = ticks(vmax)
    sy = ph / tk[-1]
    pitch = (W - left - right) / len(groups)
    s = Svg(top + ph + 48, title, sub)
    s.legend(series)
    base = top + ph
    for t in tk:
        y = base - t * sy
        s.line(left, y, W - right, y, 'grid')
        s.text(left - 8, y + 4, num(t, 1), 'm', 11, 'end')
    gw = len(series) * bw + (len(series) - 1) * 2
    for gi, (label, vals) in enumerate(groups):
        x0 = left + gi * pitch + (pitch - gw) / 2
        for si, v in enumerate(vals):
            x = x0 + si * (bw + 2)
            s.bar_v(x, bw, base, base - v * sy, series[si][0], '%s, %s: %s %s' % (label, series[si][1], num(v), unit))
            if label_first and si == 0:
                s.text(x + bw / 2, base - v * sy - 6, num(v, 1), 'p', 11, 'middle')
        s.text(x0 + gw / 2, base + 18, label, 's', 12, 'middle')
    s.line(left, base, W - right, base, 'axis')
    s.save(name, desc)


def chart_lds():
    data = [r for r in rows('docs/data/lds.csv') if int(r['tiles']) > 200]
    if not data:
        return
    groups = [('%s (%s)' % (r['stream'].split(' first')[0], r['tiles']), [float(r['kb4_ms']), float(r['kb3_ms']), float(r['kb2_5_ms'])])
              for r in data]
    grouped(groups, [('o1', '4 KB, 8 waves per SIMD'), ('o2', '3 KB, 10 waves'), ('o3', '2.5 KB, 12 waves')], 'lds.svg',
            '3 KB of shared memory against 4 KB and 2.5 KB', 'kernel ms per stream (tiles) for three shared-memory budgets, RX 6650 XT',
            'Grouped bars: kernel time for 4 KB, 3 KB and 2.5 KB of shared memory on five streams.', 'ms')


def chart_cpu_gpu():
    data = rows('docs/data/cpu_gpu.csv')
    if not data:
        return
    groups = []
    for r in data:
        mb = float(r['out_mb'])
        groups.append((r['stream'].replace('_albedo', ''), [mb / float(r['gpu_kernel_ms']), mb / float(r['gpu_e2e_ms']), mb / float(r['cpu6_ms'])]))
    grouped(groups, [('c1', 'GPU kernel'), ('c2', 'GPU, end to end into host memory'), ('c3', 'CPU reference decoder, 6 threads')],
            'cpu_gpu.svg', 'Decode rate: GPU kernel, GPU into host memory, CPU with 6 threads',
            'GB/s of output per stream, RX 6650 XT over PCIe 4.0 x8 and a Ryzen 5 5600X',
            'Grouped bars: GPU kernel, GPU end to end and CPU six threads in GB/s for six streams.', 'GB/s', label_first=True)


def chart_format():
    s = Svg(250, 'A GDeflate stream: tiles of 64 KB, 32 bitstreams in each', 'tiles are independent of each other; inside a tile 32 bitstreams advance together')
    s.text(16, 84, 'stream', 's', 12)
    x = 80
    for i in range(8):
        cls = 'hi' if i == 2 else 'box'
        s.rect(x + i * 80, 68, 74, 26, cls, rx=4)
        s.text(x + i * 80 + 37, 85, 'tile %d' % i if i < 7 else '...', 's', 11, 'middle')
    s.line(x + 2 * 80, 94, 80, 128, 'axis')
    s.line(x + 2 * 80 + 74, 94, W - 40, 128, 'axis')
    s.text(16, 150, 'one tile', 's', 12)
    for i in range(32):
        s.rect(80 + i * 20, 130, 17, 34, 'hi' if i % 2 == 0 else 'box', rx=2)
    s.text(80, 182, 'lane 0', 'm', 11)
    s.text(80 + 31 * 20 + 17, 182, 'lane 31', 'm', 11, 'end')
    s.text(80, 208, '32-bit packets round-robin over 32 lanes: one workgroup decodes one tile, one invocation one lane,', 's', 12)
    s.text(80, 226, '32 symbols per round; matches may only point back inside the tile.', 's', 12)
    s.save('format.svg', 'Diagram: a stream is a row of independent 64 KB tiles; each tile interleaves 32 bitstreams.')


def gmean(vals):
    return math.exp(sum(math.log(v) for v in vals) / len(vals))


def cards():
    out = ['| GPU | driver | streams | vkdeflate, GB/s | Khronos layer, GB/s | vkdeflate / layer |', '|---|---|---|---|---|---|']
    for path in sorted(glob.glob(os.path.join(ROOT, 'results', '*.csv'))):
        with open(path, encoding='utf-8', newline='') as f:
            data = [r for r in csv.DictReader(f) if r.get('vkdeflate_ok') == '1']
        if not data:
            continue
        vk = gmean([float(r['out_bytes']) / 1e6 / float(r['vkdeflate_ms']) for r in data])
        both = [r for r in data if r.get('khronos_ms')]
        khr = gmean([float(r['out_bytes']) / 1e6 / float(r['khronos_ms']) for r in both]) if both else None
        ratio = gmean([float(r['khronos_ms']) / float(r['vkdeflate_ms']) for r in both]) if both else None
        out.append('| %s | %s | %d | %.1f | %s | %s |' % (data[0]['gpu'], data[0]['driver'], len(data), vk,
                                                      '%.1f' % khr if khr else '', '%.1f×' % ratio if ratio else ''))
    return '\n'.join(out)


def readme_table():
    path = os.path.join(ROOT, 'README.md')
    with open(path, encoding='utf-8') as f:
        text = f.read()
    a, b = '<!-- cards:begin -->', '<!-- cards:end -->'
    if a not in text or b not in text:
        return
    text = text[:text.index(a) + len(a)] + '\n' + cards() + '\n' + text[text.index(b):]
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print('plot: README.md results table')


def main():
    os.makedirs(os.path.join(ROOT, 'docs', 'img'), exist_ok=True)
    for path in sorted(glob.glob(os.path.join(ROOT, 'results', '*.csv'))):
        with open(path, encoding='utf-8', newline='') as f:
            data = [{'stream': r['stream'].replace('.l6', ''), 'vk': float(r['vkdeflate_ms']), 'khr': float(r['khronos_ms'])}
                    for r in csv.DictReader(f) if r.get('khronos_ms')]
        if data:
            gpu = os.path.splitext(os.path.basename(path))[0]
            chart_pairs(data, 'khronos_%s.svg' % gpu, 'vkdeflate against the Khronos layer, Silesia corpus',
                        'kernel ms per stream on %s, lower is better; the number is layer time over vkdeflate time' % gpu.replace('_', ' '),
                        'Paired horizontal bars per Silesia stream: kernel time of vkdeflate and of the Khronos layer.')
    full = [{'stream': '%s (%s)' % (r['stream'].split(' first')[0], r['tiles']), 'vk': float(r['vkdeflate_ms']), 'khr': float(r['khronos_ms'])}
            for r in rows('docs/data/khronos.csv') if int(r['tiles']) > 1]
    if full:
        chart_pairs(full, 'khronos_all.svg', 'vkdeflate against the Khronos layer, Silesia and game assets',
                    'kernel ms per stream (tiles) on an RX 6650 XT, lower is better; the number is layer time over vkdeflate time',
                    'Paired horizontal bars per stream: kernel time of vkdeflate and of the Khronos layer, fifteen streams.')
    chart_saturation()
    chart_residency()
    chart_lds()
    chart_cpu_gpu()
    chart_format()
    readme_table()


if __name__ == '__main__':
    main()
