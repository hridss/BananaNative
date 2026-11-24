#!/usr/bin/env python3
"""Check a developer task's tools; optionally run it with one Python throughout.

Run with the Python 3.14 environment used for packing. Nothing is installed,
downloaded or changed by the checks. --run executes the selected task.
"""
import argparse
import importlib.metadata
import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

REPO = Path(__file__).resolve().parents[1]
APP = REPO / 'android/app'


def build_env(game, executable=None, environ=None):
    env = dict(os.environ if environ is None else environ)
    # Keep the venv path: resolving its symlink would select the base interpreter.
    python = os.path.abspath(executable or sys.executable)
    paths = [str(Path(python).parent)]
    if env.get('JAVA_HOME'):
        paths.append(str(Path(env['JAVA_HOME']) / 'bin'))
    env['PATH'] = os.pathsep.join(paths + [env.get('PATH', '')])
    env['DM_GAME'] = game
    env['PYTHON'] = python
    env['NX_PYTHON'] = python
    return env


def task_command(task, game, python, pack_stage=None):
    if task == 'python':
        return [python, '-m', 'unittest', 'discover', '-s', 'dm/tools/tests'], REPO
    if task == 'host':
        return ['make', '-C', 'dm/host/tests', 'GAME=' + game, 'PYTHON=' + python, 'run'], REPO
    if task == 'lint':
        return [python, 'dm/tools/lint_python.py'], REPO
    command = [str(APP / 'gradlew'), '-PnxGames=' + game, '-PnxPython=' + python,
               '-PnxLive=false', '-PnxPlay=false', '-PnxApp=']
    command += ['-PnxPackStage=' + str(pack_stage)] if pack_stage else ['-PnxPackEngine=fake']
    return command + ['apk'], APP


def sdk_root(env):
    local = APP / 'local.properties'
    if local.is_file():
        match = re.search(r'^sdk\.dir\s*=\s*(.+)$', local.read_text(), re.M)
        if match:
            return Path(match[1].strip().replace('\\:', ':').replace('\\\\', '\\'))
    value = env.get('ANDROID_HOME') or env.get('ANDROID_SDK_ROOT')
    return Path(value) if value else None


def check(task, game, env, pack_stage=None):
    errors = []

    def require(ok, message):
        print(('ok    ' if ok else '[RED] ') + message, flush=True)
        if not ok:
            errors.append(message)

    require(sys.version_info >= (3, 14), 'Python 3.14 or newer (CI uses 3.14); selected ' + sys.version.split()[0])
    require(bool(re.fullmatch(r'[0-9A-F]{16}', game)) and
            (REPO / 'dm/games' / game / 'game.tsv').is_file(), 'registered game profile: ' + game)
    python3 = shutil.which('python3', path=env['PATH'])
    try:
        result = subprocess.run([python3, '-c', 'import sys; print(sys.version_info[:2]); print(sys.prefix)'],
                                capture_output=True, text=True, timeout=10) if python3 else None
        require(result is not None and result.returncode == 0 and
                result.stdout.splitlines() == [str(tuple(sys.version_info[:2])), sys.prefix],
                'child python3 uses the selected Python environment')
    except (OSError, subprocess.TimeoutExpired) as exc:
        require(False, 'cannot run child python3: ' + str(exc))
    names = {'python': ('make', 'java', 'javac'), 'host': ('make', 'xcrun'),
             'lint': (), 'apk': ('java', 'javac')}[task]
    for name in names:
        require(shutil.which(name, path=env['PATH']) is not None, 'tool available: ' + name)
    if task == 'python':
        for name in ('capstone', 'lz4', 'fontTools'):
            require(importlib.util.find_spec(name) is not None, 'Python test dependency: ' + name)
    if task == 'host':
        require(sys.platform == 'darwin', 'host tests require macOS')
        print('note  audio tests need access to macOS audio services; a sandbox denial is not a test regression.')
    if task == 'lint':
        # Use the existing pins, not another list of tool versions.
        for name, want in re.findall(r'^([\w-]+)==([^\s#]+)',
                                     (REPO / '.github/requirements/lint.txt').read_text(), re.M):
            try:
                got = importlib.metadata.version(name)
            except importlib.metadata.PackageNotFoundError:
                got = 'missing'
            require(got == want, f'{name}: {got}, pinned {want}')
    if task == 'apk':
        java = str(Path(env['JAVA_HOME']) / 'bin/java') if env.get('JAVA_HOME') else shutil.which('java', path=env['PATH'])
        try:
            result = subprocess.run([java, '-version'], capture_output=True, text=True, timeout=10) if java else None
            match = re.search(r'version "(\d+)', result.stdout + result.stderr) if result else None
            require(bool(result and result.returncode == 0 and match and int(match[1]) >= 17), 'JDK 17 or newer')
        except (OSError, subprocess.TimeoutExpired) as exc:
            require(False, 'cannot run Java: ' + str(exc))
        gradle = (APP / 'build.gradle').read_text()
        sdk = sdk_root(env)
        require(sdk is not None and sdk.is_dir(), 'Android SDK from local.properties or ANDROID_HOME/ANDROID_SDK_ROOT')
        if sdk is not None:
            for pattern, folder in ((r"ndkVersion '([^']+)'", 'ndk'),
                                    (r"cmake \{[^\n]*version '([^']+)'", 'cmake'),
                                    (r'compileSdk (\d+)', 'platforms')):
                match = re.search(pattern, gradle)
                version = match[1] if match else ''
                name = 'android-' + version if folder == 'platforms' else version
                require(bool(version) and (sdk / folder / name).is_dir(), f'Android {folder}: {name}')
        if pack_stage:
            # Gradle owns the full stage validation; fail early on a missing folder.
            require(pack_stage.is_dir(), 'pack engine stage exists')
        else:
            print('note  APK uses the simulated pack engine: prepared packs can play; preparing new packs is unavailable.')
        print('note  CMake receives the selected Python explicitly, overriding a stale interpreter cache.')
    return not errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', required=True, help='base title ID; no default game')
    parser.add_argument('--task', required=True, choices=('python', 'host', 'lint', 'apk'))
    parser.add_argument('--pack-stage', type=Path, help='real on-device pack engine stage (apk only)')
    parser.add_argument('--run', action='store_true', help='run the task after all preflight checks pass')
    args = parser.parse_args()
    if args.pack_stage and args.task != 'apk':
        parser.error('--pack-stage is only for --task apk')
    if os.name != 'posix':
        parser.error('this developer wrapper currently supports macOS and Linux')
    env = build_env(args.game)
    stage = args.pack_stage.absolute() if args.pack_stage else None
    if not check(args.task, args.game, env, stage):
        return 1
    print('RESULT preflight passed', flush=True)
    if not args.run:
        return 0
    command, cwd = task_command(args.task, args.game, os.path.abspath(sys.executable), stage)
    try:
        status = subprocess.run(command, cwd=cwd, env=env, check=False).returncode
        if status < 0:
            print(f'[RED] task terminated by signal {-status}', file=sys.stderr)
            return 128 - status
        return status
    except OSError as exc:
        print('[RED] cannot start task: ' + str(exc), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
