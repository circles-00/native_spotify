"""Measure a disconnected, warmed-cache Linux frontend on an isolated Xvfb display."""

import json
import os
from pathlib import Path
import select
import statistics
import subprocess
import sys
import time


def read_ready(stream):
    if not select.select([stream], [], [], 10)[0]:
        raise RuntimeError("Timed out waiting for display/window startup")

    return stream.readline().strip()


def cpu_ticks(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().split(") ", 1)[1].split()
    return int(fields[11]) + int(fields[12])


def main():
    executable = Path(sys.argv[1]).resolve()
    read_fd, write_fd = os.pipe()

    display = subprocess.Popen(
        ["Xvfb", "-displayfd", str(write_fd), "-screen", "0", "1280x800x24", "-nolisten", "tcp"],
        pass_fds=(write_fd,),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    os.close(write_fd)
    application = None

    try:
        with os.fdopen(read_fd) as stream:
            number = read_ready(stream)

        environment = os.environ.copy()
        environment["DISPLAY"] = f":{number}"
        environment.pop("SPOTIFY_CLIENT_ID", None)
        environment["NATIVE_CONTROLLER_OFFLINE"] = "1"

        startup = []

        for _ in range(6):
            start = time.perf_counter()

            application = subprocess.Popen(
                [str(executable), "--smoke-test"],
                env=environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

            if read_ready(application.stdout) != "first paint complete":
                raise RuntimeError(application.stderr.read())

            startup.append((time.perf_counter() - start) * 1000)
            _, error = application.communicate(timeout=10)

            if application.returncode:
                raise RuntimeError(error)

        application = subprocess.Popen(
            [str(executable)],
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        if read_ready(application.stdout) != "first paint complete":
            raise RuntimeError(application.stderr.read())

        time.sleep(0.5)
        memory = Path(f"/proc/{application.pid}/status").read_text().splitlines()
        rss = next(line for line in memory if line.startswith("VmRSS:"))
        start_ticks = cpu_ticks(application.pid)
        start = time.perf_counter()

        time.sleep(2)
        cpu = (cpu_ticks(application.pid) - start_ticks) / os.sysconf("SC_CLK_TCK")
        cpu_percent = cpu / (time.perf_counter() - start) * 100

        if len(sys.argv) > 2:
            subprocess.run(["import", "-window", "root", sys.argv[2]], env=environment, check=True)

        print(json.dumps({
            "binary_bytes": executable.stat().st_size,
            "first_paint_ms_all_runs": [round(value, 2) for value in startup],
            "warm_first_paint_ms_median": round(statistics.median(startup[1:]), 2),
            "disconnected_rss": rss,
            "idle_cpu_percent_2s_sample": round(cpu_percent, 3),
            "scope": "Linux Xvfb; no OAuth/network/artwork; excludes display server and shared-library disk size",
        }, indent=2))

    finally:
        if application and application.poll() is None:
            application.terminate()
            application.wait(timeout=5)

        display.terminate()
        display.wait(timeout=5)


if __name__ == "__main__":
    main()
