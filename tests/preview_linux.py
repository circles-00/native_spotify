"""Record the real native login screen and its hover transition without starting OAuth."""

import ctypes
import ctypes.util
import os
from pathlib import Path
import select
import subprocess
import sys
import time


def ready(stream):
    if not select.select([stream], [], [], 10)[0]:
        raise RuntimeError("Native window/display did not become ready")

    return stream.readline().strip()


def main():
    executable = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2]).resolve()
    read_fd, write_fd = os.pipe()

    server = subprocess.Popen(
        ["Xvfb", "-displayfd", str(write_fd), "-screen", "0", "1280x800x24", "-nolisten", "tcp"],
        pass_fds=(write_fd,),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    os.close(write_fd)
    application = None
    recorder = None
    display = None

    try:
        with os.fdopen(read_fd) as stream:
            number = ready(stream)

        environment = os.environ.copy()
        environment["DISPLAY"] = f":{number}"
        environment.pop("SPOTIFY_CLIENT_ID", None)
        environment["NATIVE_CONTROLLER_OFFLINE"] = "1"

        application = subprocess.Popen(
            [str(executable)], env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )

        if ready(application.stdout) != "first paint complete":
            raise RuntimeError(application.stderr.read())

        x11 = ctypes.CDLL(ctypes.util.find_library("X11"))
        xtst = ctypes.CDLL(ctypes.util.find_library("Xtst"))
        x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x11.XOpenDisplay.restype = ctypes.c_void_p
        x11.XFlush.argtypes = [ctypes.c_void_p]
        x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        xtst.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
        xtst.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
        display = x11.XOpenDisplay(environment["DISPLAY"].encode())

        if not display:
            raise RuntimeError("Cannot open native test display")

        def move(x, y):
            xtst.XTestFakeMotionEvent(display, -1, x, y, 0)
            x11.XFlush(display)

        recorder = subprocess.Popen(
            ["ffmpeg", "-y", "-loglevel", "error", "-f", "x11grab", "-framerate", "30",
             "-video_size", "1280x800", "-i", environment["DISPLAY"], "-t", "5",
             "-c:v", "libx264", "-preset", "veryfast", "-crf", "20", "-pix_fmt", "yuv420p", str(output)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )

        time.sleep(0.35)
        move(40, 40)
        subprocess.run(["import", "-window", "root", str(output.with_suffix(".login.png"))], env=environment, check=True)
        move(640, 466)
        time.sleep(0.6)
        subprocess.run(["import", "-window", "root", str(output.with_suffix(".hover.png"))], env=environment, check=True)

        move(40, 40)
        time.sleep(0.6)
        move(640, 466)

        _, error = recorder.communicate(timeout=10)
        if recorder.returncode:
            raise RuntimeError(error.decode())

        print(f"Recorded native login hover transitions: {output}")
        print(f"Captured native login screen: {output.with_suffix('.login.png')}")

    finally:
        if display:
            x11.XCloseDisplay(display)

        for process in (recorder, application, server):
            if process and process.poll() is None:
                process.terminate()
                process.wait(timeout=5)


if __name__ == "__main__":
    main()
