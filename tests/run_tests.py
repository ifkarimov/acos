#!/usr/bin/env python3

import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import tempfile


root = Path(__file__).resolve().parent.parent
program = str(root / "water_system")


def run(*arguments, expected=0):
    result = subprocess.run(
        [program, *map(str, arguments)], capture_output=True, text=True, timeout=10
    )
    assert result.returncode == expected, (arguments, result.returncode, result.stderr)
    return result.stdout


def check_output(output):
    assert "[THREADS] Started 12 worker threads" in output
    assert "[FINISH] Requested tick count is completed" in output
    assert "[FATAL]" not in output
    for actual, capacity in re.findall(r"actual = ([\d.]+)/([\d.]+)", output):
        assert 0 <= float(actual) <= float(capacity)
    for power, limit in re.findall(r"Total active power: ([\d.]+)/([\d.]+)", output):
        assert float(power) <= float(limit)
    for demand, received, remaining in re.findall(
        r"demand = ([\d.]+), received = ([\d.]+), remaining = ([\d.]+)", output
    ):
        assert float(received) <= float(demand)
        assert abs(float(demand) - float(received) - float(remaining)) < 1e-6
    for demand, received, deficit in re.findall(
        r"completed periods: demand = ([\d.]+), received = ([\d.]+), deficit = ([\d.]+)", output
    ):
        assert abs(float(demand) - float(received) - float(deficit)) < 1e-6

    measured, lost, duplicates = map(int, re.search(
        r"measured = (\d+), lost = (\d+), duplicates sent = (\d+)", output
    ).groups())
    received = int(re.search(r"Messages: received = (\d+)", output).group(1))
    pending = int(re.search(r"Messages still in transit: (\d+)", output).group(1))
    assert measured - lost + duplicates == received + pending
    ids = re.findall(r"\[SENSOR\].*\(message #(\d+)\)", output)
    assert len(ids) == len(set(ids)) == measured


with tempfile.TemporaryDirectory() as directory:
    test_program = str(Path(directory) / "model_tests")
    compiler = shlex.split(os.environ.get("CC", "cc"))
    subprocess.run(compiler + [
        "-D_POSIX_C_SOURCE=200809L", "-std=c17", "-Wall", "-Wextra", "-Wpedantic",
        "-Werror", "-I", str(root), str(root / "tests/model_tests.c"),
        str(root / "model.c"), str(root / "dispatcher.c"), "-o", test_program,
    ], check=True)
    subprocess.run([test_program], check=True, stdout=subprocess.DEVNULL, timeout=10)
    print("Model: water balance, pipe limits, power, messages, accidents and statistics passed")

    for arguments in [(-1,), ("abc",), (1, -1), (1, 0, -1), (1, 0, "12x"),
                      (1, 0, 1, 2), ("99999999999999999999999",), (1, 3600001)]:
        run(*arguments, expected=2)
    print("Invalid arguments passed")

    for seed in [0, 1, 2, 42, 100, 4294967295]:
        check_output(run(30, 5, seed))
    check_output(run(5, 0, 42))
    print("Finite runs and fast mode passed")

    # При большой задержке рабочие потоки должны проснуться сразу после остановки.
    for signum in [signal.SIGINT, signal.SIGTERM]:
        with tempfile.TemporaryFile(mode="w+") as errors:
            process = subprocess.Popen(
                [program, "0", "10000", "42"], stdout=subprocess.PIPE,
                stderr=errors, text=True,
            )
            try:
                while True:
                    line = process.stdout.readline()
                    assert line, "Program stopped before creating threads"
                    if "[THREADS]" in line:
                        break
                process.send_signal(signum)
                output, _ = process.communicate(timeout=3)
                assert process.returncode == 0
                assert "[FINISH] Interrupted by user" in output
                assert "SUMMARY" in output
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
    print("SIGINT and SIGTERM passed")

print("All tests passed")
