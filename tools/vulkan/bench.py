#!/usr/bin/env python3
"""Frame rate of the Vulkan backend against OpenGL's, around Delfino Plaza.

usage: tools/vulkan/bench.py [options]

Plays the plaza tour (tools/vulkan/plaza-tour.txt: a run at the airstrip,
then warps through the plaza's episodes, sweeping the camera) in a window
with vsync off and the deterministic clock, which runs the game as fast as it
can draw: once with OpenGL, once with SMS_GX_BACKEND=vulkan, alternating for
--rounds rounds. The renderer's own timing (SMS_GX_STATS) gives the frame
times; frames of loading (shader compiles, texture uploads) are left out of
the steady numbers. Prints, per backend, the mean frame time and frame rate
and the 5% slowest windows; --scale raises the internal resolution
(SMS_GX_SCALE) to load the GPU.
"""
import argparse
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import compare  # noqa: E402  (its tour, and regress's environment)

STATS = re.compile(r'stats: (\d+) frames, [\d.]+ draws/frame, [\d.]+ vertices/frame, (\d+) shader compiles, '
                   r'(\d+) texture uploads, ([\d.]+) ms/frame total \(slowest ([\d.]+)\)')


def run(exe, disc, backend, args, work):
    d = os.path.join(work, backend)
    shutil.rmtree(d, ignore_errors=True)
    save = os.path.join(d, 'save')
    os.makedirs(save)
    env = compare.regress.base_env(save)
    env.update(compare.TOUR_ENV)
    env.update({'SMS_HEADLESS': '0', 'SMS_WINDOW_SIZE': args.window, 'SMS_GX_STATS': str(args.window_frames),
                'SMS_GX_SCALE': str(args.scale), 'SMS_FRAME_RATE': str(args.fps)})
    env.pop('SMS_VSYNC', None)
    if backend == 'vulkan':
        env['SMS_GX_BACKEND'] = 'vulkan'
    for kv in args.env:
        k, _, v = kv.partition('=')
        env[k] = v
    cmd = [exe, disc]
    if args.mem and shutil.which('systemd-run'):
        cmd = ['systemd-run', '--user', '--scope', '-q', '-p', 'MemoryMax=%s' % args.mem, '-p', 'MemorySwapMax=0', '--'] + cmd
    log_path = os.path.join(d, 'run.log')
    t0 = time.time()
    with open(log_path, 'wb') as log:
        p = subprocess.Popen(cmd, cwd=os.path.dirname(exe), env=env, stdout=log, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, start_new_session=True)
        done = False
        while time.time() - t0 < args.timeout and p.poll() is None:
            with open(log_path, 'rb') as f:
                if b'[pilot] log t=' in f.read():  # the tour's last step
                    done = True
                    break
            time.sleep(0.5)
        compare.regress.kill_group(p)
    text = open(log_path, errors='replace').read()
    windows = []
    for m in STATS.finditer(text):
        frames, compiles, uploads, ms, slowest = int(m.group(1)), int(m.group(2)), int(m.group(3)), float(m.group(4)), float(m.group(5))
        windows.append((frames, compiles, uploads, ms, slowest))
    return done, windows, text


def summarize(windows):
    steady = [w for w in windows if w[1] == 0 and w[2] == 0]
    if not steady:
        return None
    frames = sum(w[0] for w in steady)
    total = sum(w[0] * w[3] for w in steady)
    per = sorted(w[3] for w in steady)
    worst5 = per[int(len(per) * 0.95):] or per[-1:]
    return dict(frames=frames, mean=total / frames, median=statistics.median(per), worst5=statistics.mean(worst5),
                slowest=max(w[4] for w in steady), all=sum(w[0] for w in windows), allms=sum(w[0] * w[3] for w in windows))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exe', default=os.path.join(ROOT, 'build', 'macos-64' if sys.platform == 'darwin' else 'linux-64', 'sms'))
    ap.add_argument('--disc', default=None)
    ap.add_argument('--rounds', type=int, default=2)
    ap.add_argument('--scale', type=float, default=1.0, help='internal resolution (SMS_GX_SCALE)')
    ap.add_argument('--fps', type=int, default=30, help='SMS_FRAME_RATE')
    ap.add_argument('--window', default='1280x960')
    ap.add_argument('--window-frames', type=int, default=60, help='frames per timing window')
    ap.add_argument('--timeout', type=int, default=600)
    ap.add_argument('--mem', default='2500M')
    ap.add_argument('--env', action='append', default=[], help='NAME=VALUE for both backends')
    ap.add_argument('--work', default=None)
    args = ap.parse_args()
    args.exe = os.path.abspath(args.exe)
    disc = args.disc or compare.regress.find_disc()
    if not disc:
        sys.exit('no disc image: pass --disc')
    work = args.work or tempfile.mkdtemp(prefix='sms-vk-bench-')
    results = {'opengl': [], 'vulkan': []}
    try:
        for r in range(args.rounds):
            for backend in ('opengl', 'vulkan'):
                done, windows, text = run(args.exe, disc, backend, args, work)
                s = summarize(windows)
                if not done or not s:
                    print('round %d %s: the tour did not finish (see %s)' % (r + 1, backend, os.path.join(work, backend, 'run.log')))
                    continue
                results[backend].append(s)
                print('round %d %-6s: %5d steady frames, %.2f ms/frame mean (%.0f fps), median %.2f, 5%% slowest windows %.2f, '
                      'slowest frame %.1f ms' % (r + 1, backend, s['frames'], s['mean'], 1000.0 / s['mean'], s['median'],
                                                 s['worst5'], s['slowest']))
        print()
        means = {}
        for backend, rs in results.items():
            if not rs:
                continue
            mean = statistics.mean(x['mean'] for x in rs)
            means[backend] = mean
            print('%-6s: %.2f ms/frame (%.1f fps) over %d rounds, 5%% slowest windows %.2f ms' %
                  (backend, mean, 1000.0 / mean, len(rs), statistics.mean(x['worst5'] for x in rs)))
        if len(means) == 2:
            print('Vulkan is %.1f%% %s than OpenGL' % (abs(means['opengl'] / means['vulkan'] - 1) * 100,
                                                       'faster' if means['vulkan'] < means['opengl'] else 'slower'))
    finally:
        if args.work is None:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
