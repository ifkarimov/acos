#!/usr/bin/env python3

import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import tempfile
import time


root = Path(__file__).resolve().parent.parent


def children_from_output(output):
    return [int(pid) for pid in re.findall(r'\[PROCESS\].*pid = (\d+)', output)]


def check_cleanup(parent_pid, output):
    for pid in children_from_output(output):
        assert not Path(f'/proc/{pid}').exists(), f'Child {pid} is still present'
    assert not list(Path('/dev/shm').glob(f'water_{parent_pid}_*'))
    assert not list(Path('/dev/shm').glob(f'sem.water_{parent_pid}_*'))


def stop_test(program, group=False, kill_child=False):
    with tempfile.TemporaryFile(mode='w+') as log:
        process = subprocess.Popen([program, '0', '10000', '42'], stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                output = Path(f'/proc/self/fd/{log.fileno()}').read_text()
                if '[PROCESSES] Started 12' in output:
                    break
                assert process.poll() is None, output
                time.sleep(0.01)
            else:
                raise AssertionError('Workers did not start')

            children = children_from_output(output)
            assert len(children) == len(set(children)) == 12
            for pid in children:
                status = Path(f'/proc/{pid}/status').read_text()
                assert re.search(rf'^PPid:\s+{process.pid}$', status, re.MULTILINE)
                assert re.search(r'^Threads:\s+1$', status, re.MULTILINE)
            if kill_child:
                os.kill(children[0], signal.SIGKILL)
            elif group:
                os.killpg(process.pid, signal.SIGINT)
            else:
                process.send_signal(signal.SIGTERM)
            process.wait(timeout=3)
            log.seek(0)
            output = log.read()
            assert process.returncode == (2 if kill_child else 0), output
            assert '[FINISH]' in output
            check_cleanup(process.pid, output)
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()


def run_failure(program, variable, value, expected):
    environment = dict(os.environ, **{variable: str(value)})
    process = subprocess.Popen([program, '30', '1', '42'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=environment, start_new_session=True)
    try:
        output, errors = process.communicate(timeout=5)
        assert process.returncode == expected, (variable, value, output, errors)
        assert '[FINISH]' in output
        if variable == 'TEST_ACCIDENT':
            assert '[FATAL]' in output and 'fatal accident' in output
        check_cleanup(process.pid, output)
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()


compiler = shlex.split(os.environ.get('CC', 'cc'))
with tempfile.TemporaryDirectory() as directory:
    for unnamed in [False, True]:
        name = 'water_system_unnamed' if unnamed else 'water_system'
        program = str(root / name)
        stop_test(program)
        stop_test(program, group=True)
        stop_test(program, kill_child=True)
        print(f'{name}: 12 child processes, parent/group signals, killed worker and cleanup passed')

        test_program = str(Path(directory) / name)
        sources = ['main.c', 'model.c', 'dispatcher.c', 'output.c', 'simulation.c', 'ipc.c', 'tests/failures.c']
        flags = ['-DUSE_UNNAMED_SEMAPHORES'] if unnamed else []
        subprocess.run(compiler + ['-D_POSIX_C_SOURCE=200809L', '-std=c17', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-I', str(root)] + flags + [str(root / source) for source in sources] + ['-pthread', '-lrt', '-Wl,--wrap=fork', '-Wl,--wrap=sem_wait', '-Wl,--wrap=ModelInit', '-o', test_program], check=True)
        for index in range(1, 13):
            run_failure(test_program, 'TEST_FAIL_FORK', index, 2)
        run_failure(test_program, 'TEST_DIE_WITH_LOCK', 1, 2)
        run_failure(test_program, 'TEST_ACCIDENT', 1, 1)
        print(f'{name}: all 12 fork failures, abandoned semaphore and fatal accident passed')

print('Process tests passed')
