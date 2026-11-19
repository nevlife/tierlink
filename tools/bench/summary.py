# summary.py PROFILE RUN_DIR...: one table row per run and per profile segment.
# A segment is the time between two "up" lines of the profile, on the first pass of the profile.
# rate%: video bit rate in the segment's last 10 s over the netem rate; p95: video delay;
# lost: video frames the transport dropped; fec: pieces rebuilt from parity.
import re
import sys


def pct(values, q):
    if not values:
        return float('nan')
    v = sorted(values)
    return v[min(len(v) - 1, int(q * len(v)))]


def rate_bps(netem):
    m = re.search(r'rate (\d+(?:\.\d+)?)(k|m)?bit', netem)
    if not m:
        return None
    return float(m.group(1)) * {'k': 1e3, 'm': 1e6, None: 1.0}[m.group(2)]


def segments(profile):
    lines = []
    for line in open(profile):
        parts = line.split(None, 2)
        if len(parts) == 3 and not parts[0].startswith('#') and parts[1] == 'up':
            lines.append((float(parts[0]), parts[2].strip()))
    out = []
    for i, (at, netem) in enumerate(lines):
        end = lines[i + 1][0] if i + 1 < len(lines) else None
        if end is not None and end > at:
            out.append((at, end, rate_bps(netem)))
    return out


def load(run):
    shape = float(open(f'{run}/shape.txt').readline().split()[1])
    items = []
    recv_start = None
    for line in open(f'{run}/recv.txt'):
        p = line.split()
        if p[0] == 'start':
            recv_start = float(p[1])
        elif len(p) >= 5:
            rebuilt = int(p[5]) if len(p) > 5 else 0
            items.append(
                (p[0], float(p[1]) + recv_start - shape, float(p[2]), int(p[3]), int(p[4]), rebuilt))
    return items


def main():
    profile, runs = sys.argv[1], sys.argv[2:]
    segs = segments(profile)
    head = f'{"run":24} {"video p50":>9} {"p95":>6} {"max":>6} {"lost":>5} {"fec":>4} {"key":>4} '
    head += f'{"state p99":>9} {"telem p99":>9} {"pcd":>4} | ' + ' | '.join(
        f'{a:.0f}-{b:.0f}s rate% p95' for a, b, _ in segs)
    print(head)
    for run in runs:
        items = load(run)
        end = segs[-1][1] if segs else max(t for _, t, *_ in items)
        video = [i for i in items if i[0] in 'vk' and 0 <= i[1] < end]
        d = [i[2] for i in video]
        state = [i[2] for i in items if i[0] == 's' and 0 <= i[1] < end]
        telem = [i[2] for i in items if i[0] == 'm' and 0 <= i[1] < end]
        pcd = sum(1 for i in items if i[0] == 'p' and 0 <= i[1] < end)
        row = f'{run.rstrip("/").split("/")[-1]:24} {pct(d, 0.5):9.0f} {pct(d, 0.95):6.0f} '
        row += f'{max(d, default=0):6.0f} {sum(i[4] for i in video):5} {sum(i[5] for i in video):4} '
        row += f'{sum(1 for i in video if i[0] == "k"):4} {pct(state, 0.99):9.0f} '
        row += f'{pct(telem, 0.99):9.0f} {pcd:4} |'
        for a, b, bps in segs:
            tail = [i for i in video if max(a, b - 10) <= i[1] < b]
            seconds = b - max(a, b - 10)
            used = sum(i[3] for i in tail) * 8 / seconds
            share = used / bps * 100 if bps else float('nan')
            p95 = pct([i[2] for i in video if a <= i[1] < b], 0.95)
            row += f' {share:11.0f} {p95:4.0f} |'
        print(row)


main()
