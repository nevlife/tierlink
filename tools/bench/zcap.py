# zcap.py PCAP TCP_PORT UDP_PORT: Zenoh batches on the edge's links, by direction, link, reliability
# and priority. Up is toward the server's ports. TCP batches carry a 2-byte length; a UDP datagram
# is one batch. Only the first transport message of each batch is read.
import struct
import sys
from collections import defaultdict

PRIO = ['Control', 'RealTime', 'InterHigh', 'InterLow', 'DataHigh', 'Data', 'DataLow', 'Backgr']
KIND = {0: 'OAM', 1: 'INIT', 2: 'OPEN', 3: 'CLOSE', 4: 'KEEPALIVE', 5: 'FRAME', 6: 'FRAGMENT'}


def vle(b, i):
    v, s = 0, 0
    while i < len(b):
        x = b[i]
        i += 1
        v |= (x & 0x7F) << s
        s += 7
        if not x & 0x80:
            break
    return v, i


def batch_info(b):
    h = b[0]
    kind = KIND.get(h & 0x1F, 'other')
    if kind not in ('FRAME', 'FRAGMENT'):
        return kind, None, None
    rel = bool(h & 0x20)
    _, i = vle(b, 1)
    prio = 5
    more = bool(h & 0x80)
    while more and i < len(b):
        eh = b[i]
        i += 1
        enc = (eh >> 5) & 3
        if enc == 1:
            v, i = vle(b, i)
            if eh & 0x0F == 1:
                prio = v & 7
        elif enc == 2:
            n, i = vle(b, i)
            i += n
        more = bool(eh & 0x80)
    return kind, rel, PRIO[prio]


def packets(path):
    with open(path, 'rb') as f:
        g = f.read(24)
        endian = '<' if g[:4] == b'\xd4\xc3\xb2\xa1' else '>'
        while True:
            h = f.read(16)
            if len(h) < 16:
                return
            ts, tus, incl, _ = struct.unpack(endian + 'IIII', h)
            yield ts + tus / 1e6, f.read(incl)


def main():
    path, tcp_port, udp_port = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    total = defaultdict(lambda: [0, 0])
    per_sec = defaultdict(lambda: defaultdict(int))
    streams = {}
    t0 = None
    for ts, p in packets(path):
        if len(p) < 34 or p[12:14] != b'\x08\x00':
            continue
        ip = p[14:]
        ihl = (ip[0] & 0x0F) * 4
        proto = ip[9]
        l4 = ip[ihl:]
        sport, dport = struct.unpack('>HH', l4[:4])
        t0 = t0 or ts
        sec = int(ts - t0)
        if proto == 17 and udp_port in (sport, dport):
            d = 'up' if dport == udp_port else 'down'
            batches = [l4[8:]]
            link = 'UDP'
        elif proto == 6 and tcp_port in (sport, dport):
            d = 'up' if dport == tcp_port else 'down'
            link = 'TCP'
            seq = struct.unpack('>I', l4[4:8])[0]
            off = (l4[12] >> 4) * 4
            data = l4[off:]
            st = streams.setdefault(d, {'next': None, 'buf': b''})
            if l4[13] & 0x02:
                st['next'] = (seq + 1) & 0xFFFFFFFF
                continue
            if not data or st['next'] is None:
                continue
            delta = (seq - st['next']) & 0xFFFFFFFF
            if delta >= 0x80000000:
                skip = (st['next'] - seq) & 0xFFFFFFFF
                if skip >= len(data):
                    continue
                data = data[skip:]
            elif delta:
                continue
            st['next'] = (st['next'] + len(data)) & 0xFFFFFFFF
            st['buf'] += data
            batches = []
            while len(st['buf']) >= 2:
                n = struct.unpack('<H', st['buf'][:2])[0]
                if len(st['buf']) < 2 + n:
                    break
                batches.append(st['buf'][2:2 + n])
                st['buf'] = st['buf'][2 + n:]
        else:
            continue
        for b in batches:
            if not b:
                continue
            kind, rel, prio = batch_info(b)
            key = (d, link, kind, '' if rel is None else ('rel' if rel else 'be'), prio or '')
            total[key][0] += 1
            total[key][1] += len(b)
            per_sec[(d, link, prio or kind)][sec] += len(b)
    dur = max(1, max((s for v in per_sec.values() for s in v), default=1))
    print(f'{"dir":4} {"link":4} {"kind":9} {"rel":4} {"priority":10} {"batches":>8} {"bytes":>10} {"kbit/s":>8}')
    for k in sorted(total):
        n, b = total[k]
        print(f'{k[0]:4} {k[1]:4} {k[2]:9} {k[3]:4} {k[4]:10} {n:8} {b:10} {b * 8 / dur / 1000:8.1f}')
    if len(sys.argv) > 4:
        print('\nper second, kbit/s')
        keys = sorted(per_sec)
        print('t   ' + ' '.join(f'{"/".join(k):>22}' for k in keys))
        for s in range(dur + 1):
            print(f'{s:3} ' + ' '.join(f'{per_sec[k].get(s, 0) * 8 / 1000:22.0f}' for k in keys))


main()
