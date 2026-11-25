#!/usr/bin/env python3

import os
from pathlib import Path
import re
import shlex
import signal
import socket
import subprocess
import tempfile
import time


root = Path(__file__).resolve().parent.parent
roles = [('reservoir', 2), ('district', 3), ('pump', 4), ('sensor', 2), ('dispatcher', 1)]
results = root / 'results'


def free_port():
    with socket.socket() as connection:
        connection.bind(('127.0.0.1', 0))
        return str(connection.getsockname()[1])


class Session:
    def __init__(self, ticks=0, delay=50, seed=42, server='water_server', host='127.0.0.1'):
        self.directory = tempfile.TemporaryDirectory()
        self.port = free_port()
        self.processes = []
        self.logs = {}
        self.workers = {}
        self.expected = {}
        self.server = self.launch(server, str(ticks), str(delay), str(seed), host=host)
        try:
            self.wait(self.server, '[READY]')
        except Exception:
            self.close()
            raise

    def launch(self, name, *arguments, host='127.0.0.1'):
        log = open(Path(self.directory.name) / f'{len(self.processes)}.log', 'w+')
        process = subprocess.Popen([str(root / name), host, self.port, *arguments], stdout=log, stderr=log)
        self.processes.append(process)
        self.logs[process] = log
        return process

    def text(self, process):
        return Path(self.logs[process].name).read_text()

    def wait(self, process, marker, count=1, timeout=8):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            text = self.text(process)
            if text.count(marker) >= count:
                return text
            assert process.poll() is None, (marker, process.returncode, text[-4000:])
            time.sleep(0.01)
        raise AssertionError((marker, self.text(process)[-4000:]))

    def participants(self):
        for role, count in roles:
            for index in range(count):
                self.worker(role, index)
        self.wait(self.server, '[START]')

    def worker(self, role, index):
        process = self.launch('water_client', role, str(index))
        self.workers[role, index] = process
        self.wait(process, '[REGISTER]')
        return process

    def observer(self):
        process = self.launch('water_observer')
        self.wait(process, '[REGISTER]')
        return process

    def admin(self, *arguments, expected=0):
        process = self.launch('water_admin', *map(str, arguments))
        self.expected[process] = expected
        assert process.wait(timeout=5) == expected, self.text(process)
        return self.text(process)

    def finish(self, expected=0):
        for process in self.processes:
            assert process.wait(timeout=8) == self.expected.get(process, expected), self.text(process)[-4000:]
        assert self.server.returncode == expected, self.text(self.server)[-4000:]

    def close(self):
        for process in self.processes:
            if process.poll() is None:
                process.kill()
            process.wait()
        for log in self.logs.values():
            log.close()
        self.directory.cleanup()

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.close()


def check_state(text, complete=True):
    for actual, capacity in re.findall(r'actual = ([\d.]+)/([\d.]+)', text):
        assert 0 <= float(actual) <= float(capacity)
    for power, limit in re.findall(r'Total active power: ([\d.]+)/([\d.]+)', text):
        assert float(power) <= float(limit)
    for demand, received, remaining in re.findall(r'demand = ([\d.]+), received = ([\d.]+), remaining = ([\d.]+)', text):
        assert abs(float(demand) - float(received) - float(remaining)) < 1e-6
    for demand, received, deficit in re.findall(r'completed periods: demand = ([\d.]+), received = ([\d.]+), deficit = ([\d.]+)', text):
        assert abs(float(demand) - float(received) - float(deficit)) < 1e-6
    measured, lost, duplicates = map(int, re.search(r'measured = (\d+), lost = (\d+), duplicates sent = (\d+)', text).groups())
    received = int(re.search(r'Messages: received = (\d+)', text).group(1))
    pending = int(re.search(r'Messages still in transit: (\d+)', text).group(1))
    assert measured - lost + duplicates == received + pending
    ids = re.findall(r'\[SENSOR\].*\(message #(\d+)\)', text)
    assert len(ids) == len(set(ids))
    if complete:
        assert len(ids) == measured


def finite():
    for seed in [0, 42, 4294967295]:
        with Session(ticks=20, delay=30, seed=seed, host='0.0.0.0') as session:
            observer = session.observer()
            session.participants()
            session.finish()
            text = session.text(observer)
            assert '[FINISH] Requested tick count is completed' in text
            assert '[FATAL]' not in text
            check_state(text)
            assert 'CURRENT STATUS' not in session.text(session.server)
            for worker in session.workers.values():
                assert 'CURRENT STATUS' not in session.text(worker)
                assert worker.returncode == 0, session.text(worker)[-4000:]
            if seed == 42:
                (results / 'network_observer.log').write_text(text)
                (results / 'network_server.log').write_text(session.text(session.server))
                (results / 'network_sensor.log').write_text(session.text(session.workers['sensor', 0]))
    with Session(ticks=5, delay=0) as session:
        observer = session.observer()
        session.participants()
        session.finish()
        check_state(session.text(observer))
    print('TCP: finite runs, fast mode, separate output and model invariants passed', flush=True)


def reconnect():
    with Session(delay=500) as session:
        first = session.observer()
        second = session.observer()
        session.participants()
        session.wait(first, '[STEP]')
        first.send_signal(signal.SIGINT)
        assert first.wait(timeout=3) == 0
        replacement = session.observer()
        assert 'actual =' in session.wait(replacement, 'Total active power:')
        session.wait(second, '[STEP]')

        # Каждая рабочая роль может исчезнуть и вернуться без сброса состояния
        for role, count in roles:
            for index in range(count):
                worker = session.workers[role, index]
                pauses = session.text(session.server).count('[PAUSE]')
                resumes = session.text(session.server).count('[RESUME]')
                if index == 0:
                    worker.kill()
                    session.expected[worker] = -signal.SIGKILL
                else:
                    worker.send_signal(signal.SIGTERM)
                worker.wait(timeout=3)
                session.wait(session.server, '[PAUSE]', pauses + 1)
                before = session.text(session.server).count('[STEP]')
                time.sleep(0.06)
                assert session.server.poll() is None
                assert session.text(session.server).count('[STEP]') == before
                session.worker(role, index)
                session.wait(session.server, '[RESUME]', resumes + 1)

        session.admin('disconnect', 'pump', 0)
        assert session.workers['pump', 0].wait(timeout=3) == 0
        blocked = session.launch('water_client', 'pump', '0')
        session.expected[blocked] = 2
        assert blocked.wait(timeout=3) == 2
        assert 'blocked' in session.admin('list')
        session.admin('connect', 'pump', 0)
        session.worker('pump', 0)
        assert 'pump 0: connected' in session.admin('list')
        duplicate = session.launch('water_client', 'pump', '0')
        session.expected[duplicate] = 2
        assert duplicate.wait(timeout=3) == 2

        listing = session.admin('list')
        observer_id = re.search(r'observer (\d+): connected', listing).group(1)
        session.admin('disconnect', 'observer', observer_id)
        session.admin('stop')
        session.finish()
        assert '[RESUME]' in session.text(replacement)
        (results / 'network_reconnect.log').write_text(session.text(session.server))
    print('TCP: multiple observers, all 12 reconnecting roles and admin commands passed', flush=True)


def shutdown():
    for signum in [signal.SIGINT, signal.SIGTERM]:
        with Session(delay=3600000) as session:
            observer = session.observer()
            session.participants()
            session.server.send_signal(signum)
            session.finish()
            assert '[FINISH] Interrupted by user' in session.text(observer)
            assert 'SUMMARY' in session.text(observer)
    with Session() as session:
        observer = session.observer()
        session.server.send_signal(signal.SIGTERM)
        session.finish()
        assert 'SUMMARY' in session.text(observer)
    with Session(delay=500) as session:
        session.observer()
        session.participants()
        session.server.kill()
        session.server.wait()
        for process in session.processes[1:]:
            assert process.wait(timeout=5) == 2, session.text(process)
    print('TCP: signals, shutdown before start and unexpected server death passed', flush=True)


def receive_until(connection, marker):
    data = b''
    while marker not in data:
        chunk = connection.recv(4096)
        if not chunk:
            break
        data += chunk
    assert marker in data, data[-2000:]
    return data


def protocol():
    with Session() as session:
        for data in [b'HELLO 2 observer 0\n', b'HELLO 1 unknown 0\n', b'HELLO 1 pump 4\n',
                     b'HELLO 1 observer 0 extra\n', b'HELLO 1 pump 99999999999999999\n',
                     b'HELLO\x00 1 observer 0\n', b'A' * 2048]:
            with socket.create_connection(('127.0.0.1', int(session.port)), timeout=3) as connection:
                connection.sendall(data)
                receive_until(connection, b'@error ')
        with socket.create_connection(('127.0.0.1', int(session.port)), timeout=3) as connection:
            # TCP может разделить одну команду на несколько чтений
            for piece in [b'HE', b'LLO 1 ', b'obser', b'ver 0', b'\n']:
                connection.sendall(piece)
                time.sleep(0.01)
            receive_until(connection, b'@ready ')
            connection.sendall(b'PING\nPING\n')
            data = b''
            while data.count(b'@pong\n') != 2:
                data += connection.recv(4096)
            connection.sendall(b'STEP\n')
            receive_until(connection, b'@error ')
        with socket.create_connection(('127.0.0.1', int(session.port)), timeout=3) as connection:
            connection.sendall(b'HELLO 1 pump 0\n')
            receive_until(connection, b'@ready ')
            connection.sendall(b'QUIT\n')
            receive_until(connection, b'@done 0\n')
        other = session.launch('water_server', '1', '20', '42')
        session.expected[other] = 2
        assert other.wait(timeout=3) == 2
        session.admin('stop')
        session.finish()
    print('TCP: fragmented and combined commands, invalid peers and occupied port passed', flush=True)


def slow_observer():
    with Session(delay=5) as session:
        with socket.socket() as connection:
            connection.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
            connection.connect(('127.0.0.1', int(session.port)))
            connection.sendall(b'HELLO 1 observer 0\n')
            # Наблюдатель перестаёт читать, его очередь не должна блокировать сервер
            session.observer()
            session.participants()
            deadline = time.monotonic() + 8
            while 'cannot receive output fast enough' not in session.text(session.server):
                assert time.monotonic() < deadline, session.text(session.server)[-2000:]
                assert session.server.poll() is None, session.text(session.server)[-2000:]
                time.sleep(0.05)
            session.admin('stop')
            session.finish()
    print('TCP: a slow observer cannot block the simulation or shutdown', flush=True)


def heartbeat():
    with Session(delay=3600000) as session:
        observer = session.observer()
        with socket.create_connection(('127.0.0.1', int(session.port)), timeout=12) as connection:
            connection.sendall(b'HELLO 1 reservoir 0\n')
            receive_until(connection, b'@ready ')
            for role, count in roles:
                for index in range(count):
                    if (role, index) != ('reservoir', 0):
                        session.worker(role, index)
            session.wait(session.server, '[START]')
            receive_until(connection, b'@error Client stopped responding')
            session.wait(observer, '[PAUSE]')
            assert all(p.poll() is None for p in session.workers.values())
        session.worker('reservoir', 0)
        session.wait(observer, '[RESUME]')
        session.admin('stop')
        session.finish()

    # Подключённый сервер перестал отвечать, но сам TCP-сокет ещё открыт
    with socket.socket() as listener, tempfile.TemporaryFile() as log:
        listener.bind(('127.0.0.1', 0))
        listener.listen()
        listener.settimeout(3)
        client = subprocess.Popen([str(root / 'water_observer'), '127.0.0.1', str(listener.getsockname()[1])], stdout=log, stderr=log)
        try:
            with listener.accept()[0] as connection:
                connection.settimeout(3)
                receive_until(connection, b'HELLO 1 observer 0\n')
                connection.sendall(b'@ready 1000\n')
                assert client.wait(timeout=13) == 2
        finally:
            if client.poll() is None:
                client.kill()
            client.wait()
    print('TCP: silent participant, live peers during a long period and silent server passed', flush=True)


def accident():
    # Полный резервуар с отключёнными насосами гарантирует физическую аварию
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory)
        source = (root / 'model.c').read_text().replace('.level = 82.0,', '.level = 120.0,')
        (path / 'model.c').write_text(source)
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        flags = ['-D_POSIX_C_SOURCE=200809L', '-std=c17', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-I', str(root)]
        subprocess.run(compiler + flags + ['-Dprintf=NetworkPrint', '-c', str(path / 'model.c'), '-o', str(path / 'model.o')], check=True)
        program = path / 'accident_server'
        subprocess.run(compiler + flags + [str(root / name) for name in ['server.c', 'network.c', 'network_output.c', 'network_dispatcher.o', 'network_status.o']] + [str(path / 'model.o'), '-o', str(program)], check=True)
        with Session(delay=50, server=str(program)) as session:
            observer = session.observer()
            session.participants()
            session.finish(expected=1)
            assert '[FATAL]' in session.text(observer)
            assert 'SUMMARY' in session.text(observer)
    print('TCP: physical accident reaches all clients with exit code 1', flush=True)


def arguments():
    for name, args in [
        ('water_server', []), ('water_server', ['bad-ip', '5000']),
        ('water_server', ['127.0.0.1', '0']), ('water_server', ['127.0.0.1', '65536']),
        ('water_server', ['127.0.0.1', '5000', '-1']),
        ('water_server', ['127.0.0.1', '5000', '1', '3600001']),
        ('water_server', ['127.0.0.1', '5000', '1', '5', '-1']),
        ('water_client', ['127.0.0.1', '5000', 'pump', '4']),
        ('water_client', ['127.0.0.1', '5000', 'unknown', '0']),
        ('water_observer', ['127.0.0.1']),
        ('water_admin', ['127.0.0.1', '5000', 'connect', 'observer', '0']),
        ('water_admin', ['127.0.0.1', '5000', 'stop', 'extra']),
    ]:
        result = subprocess.run([str(root / name), *args], capture_output=True, timeout=3)
        assert result.returncode == 2, (name, args, result)
    port = free_port()
    assert subprocess.run([str(root / 'water_observer'), '127.0.0.1', port], capture_output=True, timeout=7).returncode == 2
    print('TCP: invalid arguments and unavailable server passed', flush=True)


results.mkdir(exist_ok=True)
arguments()
finite()
reconnect()
shutdown()
protocol()
slow_observer()
heartbeat()
accident()
print('All TCP tests passed', flush=True)
