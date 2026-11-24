#!/usr/bin/env python3
"""Run a command to completion or a log condition, with a deadline and saved output.

No shell is used. Output stays in --log; only a bounded final report is printed.
--until is for streams such as adb logcat: matching it stops the command group.
Exit: command status, 1 failed/missing condition, 124 deadline, 130 interrupted.
"""
import argparse
import codecs
from collections import deque
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time


def stop_group(process):
    """Own only this invocation's process group, including surviving children."""
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        process.wait()
        return
    # Wait on the child we own, rather than probing the group with signal 0
    # (macOS sandboxes can refuse that probe even after allowing SIGTERM).
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def watch(command, log, timeout, until=None, failures=(), tail_lines=8):
    start = time.monotonic()
    lines = deque(maxlen=tail_lines)
    pending = ''
    decoder = codecs.getincrementaldecoder('utf-8')(errors='replace')
    reason, code = '', 0

    def inspect(text):
        nonlocal reason, code
        lines.append(text[-500:])
        if any(pattern.search(text) for pattern in failures):
            reason, code = 'failure condition matched', 1
        elif not reason and until and until.search(text):
            reason, code = 'completion condition matched', 0

    # Refuse overwrite, including symlinks. Keep logs even on failure or timeout.
    with log.open('xb') as output, log.open('rb') as reader:
        process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            while True:
                chunk = reader.read(65536)
                if chunk:
                    pending += decoder.decode(chunk)
                    while '\n' in pending:
                        line, pending = pending.split('\n', 1)
                        inspect(line)
                    # Bound memory for output that never emits a newline. Retain
                    # an overlap so ordinary markers spanning reads are found.
                    if len(pending) > 65536:
                        inspect(pending)
                        pending = pending[-4096:]
                status = process.poll()
                if status is not None and not chunk:
                    pending += decoder.decode(b'', final=True)
                    if pending:
                        inspect(pending)
                    if not reason:
                        reason = 'completion condition missing' if until else f'command exited {status}'
                    # A successful marker cannot hide a command that exited badly.
                    code = (status if status >= 0 else 128 - status) if status else (code or int(bool(until and reason == 'completion condition missing')))
                    break
                if reason:
                    break
                if time.monotonic() - start >= timeout:
                    reason, code = 'deadline exceeded', 124
                    break
                if not chunk:
                    time.sleep(0.1)
        except KeyboardInterrupt:
            reason, code = 'interrupted', 130
        finally:
            stop_group(process)
    print(f'RESULT {reason}; status={code}; elapsed={time.monotonic() - start:.1f}s')
    print(f'log: {log}')
    for line in lines:
        print('  ' + line)
    return code


def positive_seconds(value):
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError('timeout must be finite and positive')
    return number


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log', required=True, type=Path, help='new log file; parent folder must exist')
    parser.add_argument('--timeout', required=True, type=positive_seconds, help='wall-clock deadline in seconds')
    parser.add_argument('--until', help='completion regex, matched on decoded output lines')
    parser.add_argument('--fail', action='append', default=[], help='failure regex (repeatable; wins over completion)')
    parser.add_argument('--tail', type=int, default=8, help='final output lines, 0..40 (each at most 500 characters)')
    parser.add_argument('command', nargs=argparse.REMAINDER, help='-- executable argument ...')
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if os.name != 'posix':
        parser.error('process-group monitoring currently supports macOS and Linux')
    if not command or not 0 <= args.tail <= 40:
        parser.error('give a command after --, and --tail between 0 and 40')
    try:
        until = re.compile(args.until) if args.until else None
        failures = [re.compile(pattern) for pattern in args.fail]
    except re.error as exc:
        parser.error('invalid condition: ' + str(exc))
    try:
        return watch(command, args.log.absolute(), args.timeout, until, failures, args.tail)
    except OSError as exc:
        print('[RED] cannot monitor command: ' + str(exc), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
