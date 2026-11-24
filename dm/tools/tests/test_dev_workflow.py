"""Developer preflight and bounded monitoring, using synthetic child processes."""
import importlib.util
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('dev_preflight', REPO / 'scripts/dev_preflight.py')
assert spec is not None and spec.loader is not None
preflight = importlib.util.module_from_spec(spec)
spec.loader.exec_module(preflight)
GAME = '0400486024DEC000'


class Preflight(unittest.TestCase):
    def test_venv_path_preserved_and_children_get_same_game(self):
        env = preflight.build_env(GAME, '/work/env/bin/python', {'PATH': '/usr/bin', 'OTHER': 'keep'})
        self.assertEqual(env['PATH'], '/work/env/bin' + os.pathsep + '/usr/bin')
        self.assertEqual(env['PYTHON'], '/work/env/bin/python')
        self.assertEqual(env['NX_PYTHON'], env['PYTHON'])
        self.assertEqual(env['DM_GAME'], GAME)
        self.assertEqual(env['OTHER'], 'keep')

    def test_apk_pins_python_and_test_app_without_implicit_real_packer(self):
        cmd, cwd = preflight.task_command('apk', GAME, '/work/env/bin/python')
        self.assertEqual(cwd, preflight.APP)
        self.assertIn('-PnxPython=/work/env/bin/python', cmd)
        self.assertIn('-PnxGames=' + GAME, cmd)
        self.assertIn('-PnxPackEngine=fake', cmd)
        real, _ = preflight.task_command('apk', GAME, '/python', Path('/work/stage'))
        self.assertIn('-PnxPackStage=/work/stage', real)
        self.assertNotIn('-PnxPackEngine=fake', real)

    def test_host_make_gets_interpreter_explicitly(self):
        cmd, _ = preflight.task_command('host', GAME, '/work/env/bin/python')
        self.assertIn('PYTHON=/work/env/bin/python', cmd)
        self.assertIn('GAME=' + GAME, cmd)

    def test_sdk_local_property_and_environment_fallback(self):
        with tempfile.TemporaryDirectory() as folder, mock.patch.object(preflight, 'APP', Path(folder)):
            self.assertEqual(preflight.sdk_root({'ANDROID_HOME': '/sdk'}), Path('/sdk'))
            self.assertIsNone(preflight.sdk_root({}))
            (Path(folder) / 'local.properties').write_text('sdk.dir=/other sdk\n')
            self.assertEqual(preflight.sdk_root({'ANDROID_HOME': '/sdk'}), Path('/other sdk'))

    def test_failed_checks_never_start_task(self):
        with mock.patch.object(sys, 'argv', ['dev_preflight', '--game', GAME, '--task', 'python', '--run']), \
             mock.patch.object(preflight, 'check', return_value=False), \
             mock.patch.object(preflight.subprocess, 'run') as run:
            self.assertEqual(preflight.main(), 1)
            run.assert_not_called()

    def test_signal_exit_is_reported_as_failure(self):
        with mock.patch.object(sys, 'argv', ['dev_preflight', '--game', GAME, '--task', 'python', '--run']), \
             mock.patch.object(preflight, 'check', return_value=True), \
             mock.patch.object(preflight.subprocess, 'run', return_value=subprocess.CompletedProcess([], -4)):
            self.assertEqual(preflight.main(), 132)


@unittest.skipUnless(os.name == 'posix', 'POSIX process groups')
class WatchCommand(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.log = self.folder / 'run.log'

    def run_child(self, code, *options, timeout='5'):
        return subprocess.run([sys.executable, str(REPO / 'scripts/watch_command.py'),
                               '--log', str(self.log), '--timeout', timeout, *options,
                               '--', sys.executable, '-u', '-c', code],
                              capture_output=True, text=True, timeout=10)

    def test_exit_status_and_merged_output(self):
        result = self.run_child('import sys; print("out"); print("err", file=sys.stderr); sys.exit(7)')
        self.assertEqual(result.returncode, 7, result)
        self.assertIn('out', self.log.read_text())
        self.assertIn('err', self.log.read_text())
        self.assertIn('status=7', result.stdout)

    def test_completion_stops_a_stream_with_split_utf8(self):
        result = self.run_child('import os,time; os.write(1,b"\\xe2"); time.sleep(.1); '
                                'os.write(1,b"\\x9c\\x93 done\\n"); time.sleep(30)', '--until', '✓ done')
        self.assertEqual(result.returncode, 0, result)
        self.assertIn('completion condition matched', result.stdout)

    def test_failure_wins_over_completion_in_same_output(self):
        result = self.run_child('print("done ERROR")', '--until', 'done', '--fail', 'ERROR')
        self.assertEqual(result.returncode, 1, result)

    def test_nonzero_exit_with_missing_condition_keeps_status(self):
        result = self.run_child('import sys; print("failed"); sys.exit(9)', '--until', 'done')
        self.assertEqual(result.returncode, 9, result)

    def test_missing_condition_is_failure_even_on_successful_exit(self):
        result = self.run_child('print("not yet")', '--until', 'done')
        self.assertEqual(result.returncode, 1, result)
        self.assertIn('completion condition missing', result.stdout)

    def test_deadline_stops_child_and_descendant(self):
        pidfile = self.folder / 'child.pid'
        code = ('import subprocess,sys,time; from pathlib import Path; '
                'p=subprocess.Popen([sys.executable,"-c","import time; time.sleep(30)"]); '
                f'Path({str(pidfile)!r}).write_text(str(p.pid)); '
                # Reap the child when TERM arrives, to avoid a platform zombie.
                'import signal; signal.signal(signal.SIGTERM,lambda *_: (p.wait(),sys.exit(0))); time.sleep(30)')
        result = self.run_child(code, timeout='0.5')
        self.assertEqual(result.returncode, 124, result)
        pid = int(pidfile.read_text())
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                break
            time.sleep(.05)
        else:
            os.kill(pid, signal.SIGKILL)
            self.fail('descendant survived the monitor deadline')

    def test_existing_log_is_preserved(self):
        self.log.write_text('keep')
        result = self.run_child('print("overwrite")')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.log.read_text(), 'keep')

    def test_output_is_bounded_while_log_is_complete(self):
        result = self.run_child('print("x"*200000); print("last")', '--tail', '2')
        self.assertEqual(result.returncode, 0, result)
        self.assertLess(len(result.stdout), 1300)
        self.assertGreater(self.log.stat().st_size, 200000)
        self.assertIn('last', result.stdout)


if __name__ == '__main__':
    unittest.main()
