#!/usr/bin/env python3
"""Compare the Vulkan backend's pictures with OpenGL's, pixel by pixel.

usage: tools/vulkan/compare.py [options] RUN...

Each RUN (title, plaza, or a name from tools/regress/regress.py) is made
twice, headless with the deterministic clock, scripted input and the regress
settings (no settings file, no texture packs, skipped movies): once with
OpenGL and once with SMS_GX_BACKEND=vulkan. Their XFB captures (SMS_SHOTS)
are compared exactly: per field, the pixels that differ, the largest channel
difference, and an image (--out) of OpenGL | Vulkan | the difference x16.
Exits 1 when any field differs (or with --tolerance, differs by more).

--against EXE compares OpenGL with OpenGL instead: --exe's pictures with
those of another build (e.g. one of main, to check a change draws nothing
differently).

Runs are one at a time under a memory cap (systemd-run --user, when there),
and their captures are deleted unless --keep is given. --window runs both in
a window (macOS, which has no headless OpenGL).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'regress'))
import regress  # noqa: E402  (its runs, scripted input and base environment)

from PIL import Image, ImageChops  # noqa: E402

# Title and file select, the airstrip's dialogue (A every few seconds), then
# the plaza tour (plaza-tour.txt): runs, camera sweeps and warps through the
# plaza's episodes. SMS_WARP=1,0,1 sends the new file to the plaza.
TOUR_AP = regress.GATE_AP + ',A@3800' + ''.join(',A@%d+4' % f for f in range(4300, 5201, 150))
TOUR_ENV = {'SMS_WARP': '1,0,1', 'SMS_AUTOPRESS': TOUR_AP,
            'SMS_DEMO_PILOT': os.path.join(HERE, 'plaza-tour.txt')}
RUNS = dict(regress.RUNS)
RUNS['tour'] = dict(game='vanilla', kind='shots', env=TOUR_ENV, shots=list(range(5000, 9801, 400)), timeout=600,
                    desc='the plaza tour: warps through Delfino Plaza\'s episodes')


def read_ppm(path):
    return Image.open(path).convert('RGB')


def run_once(exe, disc, spec, backend, work, timeout, mem, label=None):
    d = os.path.join(work, label or backend)
    shutil.rmtree(d, ignore_errors=True)
    shots, save = os.path.join(d, 'shots'), os.path.join(d, 'save')
    os.makedirs(shots)
    os.makedirs(save)
    env = regress.base_env(save, shots)
    env.update(spec.get('env', {}))
    env['SMS_SHOTS'] = ','.join(str(n) for n in spec['shots'])
    if backend == 'vulkan':
        env['SMS_GX_BACKEND'] = 'vulkan'
    if WINDOW:  # macOS has no headless OpenGL: both in a window
        env['SMS_HEADLESS'] = '0'
        env.setdefault('SMS_WINDOW_SIZE', '1280x960')
    env.update(EXTRA_ENV)
    last = max(spec['shots'])
    marker = '[vi] captured field %d ' % last
    cmd = [exe, disc]
    if mem and shutil.which('systemd-run'):
        cmd = ['systemd-run', '--user', '--scope', '-q', '-p', 'MemoryMax=%s' % mem, '-p', 'MemorySwapMax=0', '--'] + cmd
    log_path = os.path.join(d, 'run.log')
    t0 = time.time()
    with open(log_path, 'wb') as log:
        p = subprocess.Popen(cmd, cwd=os.path.dirname(exe), env=env, stdout=log, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, start_new_session=True)
        ok = False
        while time.time() - t0 < timeout:
            if p.poll() is not None:
                break
            with open(log_path, 'rb') as f:
                if marker.encode() in f.read():
                    ok = True
                    break
            time.sleep(0.5)
        regress.kill_group(p)
    return d, ok, time.time() - t0


def compare(a_dir, b_dir, fields, out, name, tolerance):
    worst = 0
    lines = []
    for n in fields:
        pa = os.path.join(a_dir, 'shots', 'field%05d.ppm' % n)
        pb = os.path.join(b_dir, 'shots', 'field%05d.ppm' % n)
        if not (os.path.exists(pa) and os.path.exists(pb)):
            lines.append('  field %5d: missing (%s%s)' % (n, '' if os.path.exists(pa) else 'OpenGL ',
                                                         '' if os.path.exists(pb) else 'Vulkan'))
            worst = max(worst, 256)
            continue
        a, b = read_ppm(pa), read_ppm(pb)
        if a.size != b.size:
            lines.append('  field %5d: sizes differ %s vs %s' % (n, a.size, b.size))
            worst = max(worst, 256)
            continue
        diff = ImageChops.difference(a, b)
        bbox = diff.getbbox()
        if not bbox:
            lines.append('  field %5d: identical (%dx%d)' % (n, a.width, a.height))
            continue
        px = diff.load()
        count, peak = 0, 0
        for y in range(bbox[1], bbox[3]):
            for x in range(bbox[0], bbox[2]):
                m = max(px[x, y])
                if m:
                    count += 1
                    peak = max(peak, m)
        worst = max(worst, peak)
        lines.append('  field %5d: %d pixels differ (%.3f%%), largest channel difference %d, within %s' %
                     (n, count, 100.0 * count / (a.width * a.height), peak, bbox))
        if out:
            os.makedirs(out, exist_ok=True)
            amp = diff.point(lambda v: min(255, v * 16))
            sheet = Image.new('RGB', (a.width * 3 + 16, a.height), (40, 40, 40))
            sheet.paste(a, (0, 0))
            sheet.paste(b, (a.width + 8, 0))
            sheet.paste(amp, (a.width * 2 + 16, 0))
            sheet.save(os.path.join(out, '%s-field%05d.png' % (name, n)))
    print('%s:' % name)
    print('\n'.join(lines))
    return worst <= tolerance


EXTRA_ENV = {}
WINDOW = False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('runs', nargs='+')
    ap.add_argument('--exe', default=os.path.join(ROOT, 'build', 'macos-64' if sys.platform == 'darwin' else 'linux-64', 'sms'))
    ap.add_argument('--disc', default=None)
    ap.add_argument('--work', default=None, help='folder for the runs (default: a temporary one)')
    ap.add_argument('--out', default=None, help='write comparison images here')
    ap.add_argument('--keep', action='store_true', help='keep the captures and logs')
    ap.add_argument('--tolerance', type=int, default=0, help='largest channel difference allowed')
    ap.add_argument('--mem', default='2500M', help='memory cap per run (systemd-run), "" for none')
    ap.add_argument('--env', action='append', default=[], help='NAME=VALUE for both runs')
    ap.add_argument('--window', action='store_true', help='run in a window instead of headless (macOS)')
    ap.add_argument('--against', default=None, help='compare OpenGL with this build\'s OpenGL instead of Vulkan')
    args = ap.parse_args()
    args.exe = os.path.abspath(args.exe)
    if args.against:
        args.against = os.path.abspath(args.against)
    global WINDOW
    WINDOW = args.window
    for kv in args.env:
        k, _, v = kv.partition('=')
        EXTRA_ENV[k] = v
    disc = args.disc or regress.find_disc()
    if not disc:
        sys.exit('no disc image: pass --disc')
    work = args.work or tempfile.mkdtemp(prefix='sms-vk-compare-')
    ok = True
    try:
        for name in args.runs:
            spec = RUNS.get(name)
            if not spec or not spec.get('shots'):
                sys.exit('unknown run %r (one of %s)' % (name, ', '.join(k for k, v in RUNS.items() if v.get('shots'))))
            rw = os.path.join(work, name)
            gl, gl_ok, gl_s = run_once(args.exe, disc, spec, 'opengl', rw, spec['timeout'], args.mem)
            if args.against:
                vk, vk_ok, vk_s = run_once(args.against, disc, spec, 'opengl', rw, spec['timeout'], args.mem, 'against')
            else:
                vk, vk_ok, vk_s = run_once(args.exe, disc, spec, 'vulkan', rw, spec['timeout'], args.mem)
            print('%s: OpenGL %s in %.0f s, %s %s in %.0f s' % (name, 'done' if gl_ok else 'INCOMPLETE', gl_s,
                                                              'the other build' if args.against else 'Vulkan',
                                                              'done' if vk_ok else 'INCOMPLETE', vk_s))
            ok = compare(gl, vk, spec['shots'], args.out, name, args.tolerance) and ok and gl_ok and vk_ok
    finally:
        if args.keep:
            print('kept in %s' % work)
        else:
            shutil.rmtree(work, ignore_errors=True)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
