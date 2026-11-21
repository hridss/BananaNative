#!/usr/bin/env python3
# Shaders while playing on Linux: the part the Android app does (LayerWatch / LayerService, android/SHADERS.md "Shaders the library lacks"). The host writes a
# request for every shader its library lacks (DM_VK_LAYER, on by default, into $DM_TEMP/shader_layer/req) and maps the layer library a build writes
# (lib/v-<DMSH_VK_CHAIN>/dmsh_vk.lib, polled once a second); this watcher runs dm/gfx/tools/shader_layer.py on those requests while the game runs.
# AppRun starts it beside the host, in a session of its own, and it ends with the host: a batch in hand is stopped (Steam counts a game as running while
# any process it started runs), and shader_layer.py carries what that batch translated to the next build.
#   layer_watch.py --game <ID> --root <layer folder> --host-pid <pid> --src <source tree> --env <packer env> --log <file> --chain <hex> [--seed-romfs <spec> [--seed-main <spec>]]
# --seed-romfs: the pack has no shader library, so the game's starter list (dm/games/<ID>/shader_first.tsv) is written as requests first, once a version
# (shader_seed.py), from that RomFS spec (`<game.nup>:update/<v>/romfs`).
# Measured on the AYN Odin 2 Pro (12 GB, no swap; linux/odin-test/layertest.sh, 2026-11-17): Mario Kart World with no library, 1,767 requests built in
# 6 minutes beside the running game, the builder at most ~300 MB, MemAvailable never under 693 MiB.
# Guards (environment, MiB): a batch starts only with MemAvailable >= NX_LAYER_MIN_MB (500); a running batch is stopped below NX_LAYER_STOP_MB (250)
# (shader_layer.py carries the results of a stopped run to the next one); the layer folder stays under NX_LAYER_MAX_MB (2048) and the volume keeps
# 1 GiB free (--max-bytes); one build at a time per layer folder (a lock file). The builder runs at nice 19 in the idle I/O class with one worker.
import argparse, fcntl, os, shutil, signal, subprocess, sys, time

TICK_S = 5


def mem_available_mib():
    try:
        for l in open('/proc/meminfo'):
            if l.startswith('MemAvailable:'): return int(l.split()[1]) // 1024
    except OSError: pass
    return 1 << 30


def alive(pid):
    try: os.kill(pid, 0); return True
    except ProcessLookupError: return False
    except PermissionError: return True


def folder_bytes(root):
    n = 0
    for dp, _, fn in os.walk(root):
        for f in fn:
            try: n += os.lstat(os.path.join(dp, f)).st_size
            except OSError: pass
    return n


def pending(root, chain):
    """requests (req/<key>.ctl) not yet settled in this version's built.tsv"""
    try: req = {f[:-4] for f in os.listdir(os.path.join(root, 'req')) if f.endswith('.ctl')}
    except OSError: return 0
    built = set()
    try:
        for l in open(os.path.join(root, 'lib', f'v-{chain}', 'built.tsv')):
            p = l.split('\t')
            if p and p[0]: built.add(p[0].strip())
    except OSError: pass
    return len(req - built)


def env_mb(k, d):
    """A size in MiB from the environment, its default when unset or not a whole number"""
    try: return int(os.environ.get(k, d))
    except ValueError: return d


class Watch:
    def __init__(self, a):
        self.a = a; self.log = open(a.log, 'a', buffering=1)
        self.env = dict(os.environ, DOTNET_ROOT=os.path.join(a.env, 'dotnet'), XLATE=os.path.join(a.src, 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate-linux'),
                        J='1', DMSH_VK_PROCS='1', DMSH_VK_XLATE_J='1')
        for k in ('APPDIR', 'APPIMAGE', 'ARGV0', 'OWD', 'LD_LIBRARY_PATH', 'PYTHONPATH', 'PYTHONHOME'): self.env.pop(k, None)
        self.py = os.path.join(a.env, 'venv/bin/python'); self.tools = os.path.join(a.src, 'dm/gfx/tools')
        self.min_mb = env_mb('NX_LAYER_MIN_MB', 500); self.stop_mb = env_mb('NX_LAYER_STOP_MB', 250)
        self.max_mb = env_mb('NX_LAYER_MAX_MB', 2048); self.said = set(); self.builds = 0; self.shaders = 0

    def note(self, s):
        self.log.write(time.strftime('%F %T ') + 'shaders while playing: ' + s + '\n')

    def once(self, key, s):
        if key not in self.said: self.said.add(key); self.note(s)

    def run(self, args, label):
        """a tool at low priority, in a group of its own; stopped (TERM, results kept) when memory runs short. Returns its exit code, or None when stopped"""
        p = subprocess.Popen(['nice', '-n', '19', 'ionice', '-c3', self.py] + args, cwd=self.tools, env=self.env, stdin=subprocess.DEVNULL,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True, text=True)
        out = []
        os.set_blocking(p.stdout.fileno(), False)
        while p.poll() is None:
            time.sleep(1)
            try:
                chunk = p.stdout.read()
                if chunk: out.append(chunk)
            except (OSError, TypeError): pass
            gone = not alive(self.a.host_pid)
            if gone or mem_available_mib() < self.stop_mb:
                try: os.killpg(p.pid, signal.SIGTERM)
                except OSError: pass
                try: p.wait(10)
                except subprocess.TimeoutExpired:
                    try: os.killpg(p.pid, signal.SIGKILL)
                    except OSError: pass
                self.note(f'{label} stopped: ' + ('the game ended (Steam waits for every process of a game)' if gone else
                          f'{mem_available_mib()} MiB available (under {self.stop_mb})') + '; it goes on at the next start')
                return None
        try: out.append(p.stdout.read() or '')
        except (OSError, TypeError): pass
        self.last_out = ''.join(out)
        return p.returncode

    def seed(self):
        mark = os.path.join(self.a.root, f'.seeded-{self.a.chain}')
        first = os.path.join(self.a.src, 'dm/games', self.a.game, 'shader_first.tsv')
        if os.path.exists(mark) or not os.path.isfile(first): return
        t = time.time()
        args = ['shader_seed.py', '--game', self.a.game, '--root', self.a.root, '--first', first, '--romfs', self.a.seed_romfs, '--jobs', '2']
        if self.a.seed_main: args += ['--main', self.a.seed_main]   # (a game whose shader archives are indexed by NROs in main's boot stub: Mario Tennis Fever)
        rc = self.run(args, 'the starter list')
        if rc is None: return
        out = self.last_out.strip().splitlines()
        tail = [l for l in out if l.startswith('seeded')][-1:] if rc == 0 else out[-2:]
        self.note(f'the starter list: exit {rc}, {time.time() - t:.0f} s' + (': ' + ' | '.join(tail) if tail else ''))
        if rc == 0 and any(l.startswith('seeded ') and not l.startswith('seeded 0 ') for l in out): open(mark, 'w').close()   # (a run that found none is tried again)

    def build(self):
        free = shutil.disk_usage(self.a.root).free
        cap = min(self.max_mb << 20, folder_bytes(self.a.root) + free - (1 << 30))
        if cap <= 0:   # (shader_layer.py takes --max-bytes 0 as no limit: with 1 GiB or less free on the disk, or NX_LAYER_MAX_MB=0, nothing is built)
            self.once('quota', f'the disk keeps 1 GiB free ({free >> 20} MiB free) or NX_LAYER_MAX_MB is 0: nothing more is built'); return 3
        t = time.time()
        rc = self.run(['shader_layer.py', '--game', self.a.game, '--root', self.a.root, '--build', '--max-bytes', str(cap)], 'a build')
        if rc is None: return
        self.builds += 1
        timing = [l for l in self.last_out.splitlines() if l.startswith('shader_layer timing')][-1:]
        if rc == 3: self.once('quota', f'the shader folder is full ({self.max_mb} MiB, NX_LAYER_MAX_MB, or the disk keeps 1 GiB free): nothing more is built')
        elif rc != 0: self.note(f'a build ended with exit {rc} after {time.time() - t:.0f} s: ' + ' | '.join(self.last_out.strip().splitlines()[-3:]))
        elif timing: self.note(timing[0])
        return rc

    def loop(self):
        os.makedirs(self.a.root, exist_ok=True)
        lock = open(os.path.join(self.a.root, '.build.lock'), 'w')
        try: fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError: self.note('another build of this game runs: this one leaves it'); return
        self.note(f'on (game {self.a.game}, folder {self.a.root})')
        if self.a.seed_romfs: self.seed()
        full = False
        while alive(self.a.host_pid):
            n = pending(self.a.root, self.a.chain)
            if n and not full:
                m = mem_available_mib()
                if m < self.min_mb: self.once('mem', f'waiting: {m} MiB available, a build starts at {self.min_mb} (NX_LAYER_MIN_MB)')
                else:
                    self.said.discard('mem')
                    full = self.build() == 3
            time.sleep(TICK_S)
        self.note(f'the game ended: {self.builds} builds this run, {pending(self.a.root, self.a.chain)} requests left for the next one')


def main():
    ap = argparse.ArgumentParser()
    for k in ('--game', '--root', '--src', '--env', '--log', '--chain'): ap.add_argument(k, required=True)
    ap.add_argument('--host-pid', type=int, required=True); ap.add_argument('--seed-romfs'); ap.add_argument('--seed-main')
    a = ap.parse_args()
    w = Watch(a)
    try: w.loop()
    except Exception as e:   # (never the game's problem)
        w.note(f'stopped by an error: {e!r}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
