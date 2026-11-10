#!/usr/bin/env python3

import os
import re
from pathlib import Path
import signal
import subprocess
import tempfile
import time


root = Path(__file__).resolve().parent.parent
program = str(root / 'water_independent')
roles = [('reservoir', 2), ('district', 3), ('pump', 4), ('sensor', 2), ('dispatcher', 1)]


def wait_text(log, marker, processes):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        text = Path(log.name).read_text()
        if marker in text:
            return text
        assert all(p.poll() is None for p in processes), text
        time.sleep(0.01)
    raise AssertionError(f'Timeout: {marker}')


def scenario(mode):
    session = f'test_{os.getpid()}_{mode}'
    processes = []
    with tempfile.NamedTemporaryFile(mode='w+') as log:
        try:
            ticks = '10' if mode == 'finite' else '0'
            controller = subprocess.Popen([program, session, 'controller', ticks, '20', '42'], stdout=log, stderr=log)
            processes.append(controller)
            wait_text(log, '[READY]', processes)
            if mode == 'startup':
                controller.send_signal(signal.SIGINT)
            else:
                for role, count in roles:
                    for index in range(count):
                        processes.append(subprocess.Popen([program, session, role, str(index)], stdout=log, stderr=log))
                wait_text(log, '[INDEPENDENT]', processes)
                if mode == 'duplicate':
                    duplicate = subprocess.run([program, session, 'pump', '0'], capture_output=True, timeout=3)
                    assert duplicate.returncode == 2
                    controller.send_signal(signal.SIGTERM)
                elif mode == 'signal':
                    processes[1].send_signal(signal.SIGTERM)
                elif mode == 'killed':
                    processes[1].kill()
            for process in processes:
                process.wait(timeout=10)
            output = Path(log.name).read_text()
            assert controller.returncode == (2 if mode == 'killed' else 0), output
            if mode != 'killed':
                assert all(p.returncode == 0 for p in processes), output
            else:
                assert all(p.returncode == 2 for p in processes[2:]), output
            assert '[FINISH]' in output and 'SUMMARY' in output
            assert not list(Path('/dev/shm').glob(f'*water_ind_{session}_*'))
            if mode == 'finite':
                measured, lost, duplicates = map(int, re.search(r'measured = (\d+), lost = (\d+), duplicates sent = (\d+)', output).groups())
                received = int(re.search(r'Messages: received = (\d+)', output).group(1))
                pending = int(re.search(r'Messages still in transit: (\d+)', output).group(1))
                assert measured - lost + duplicates == received + pending
                for actual, capacity in re.findall(r'actual = ([\d.]+)/([\d.]+)', output):
                    assert 0 <= float(actual) <= float(capacity)
                for power, limit in re.findall(r'Total active power: ([\d.]+)/([\d.]+)', output):
                    assert float(power) <= float(limit)
                (root / 'results/independent.log').write_text(output)
            print(f'Independent: {mode} passed')
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                    process.wait()


for mode in ['finite', 'startup', 'signal', 'duplicate', 'killed']:
    scenario(mode)

for arguments in [['bad/name', 'controller'], ['test', 'unknown', '0'], ['test', 'pump', '4'], ['test', 'controller', '-1']]:
    assert subprocess.run([program, *arguments], capture_output=True, timeout=3).returncode == 2
print('Independent argument checks passed')
