"""Exercise the native sign-in button and a real, isolated Brave browser. Never submit credentials."""

import ctypes
import ctypes.util
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time
import urllib.request


def ready(stream):
    if not select.select([stream], [], [], 10)[0]:
        raise RuntimeError("Display or native window did not start")
    return stream.readline().strip()


def main():
    executable = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2]).resolve()
    read_fd, write_fd = os.pipe()
    server = subprocess.Popen(
        ["Xvfb", "-displayfd", str(write_fd), "-screen", "0", "1280x800x24", "-nolisten", "tcp"],
        pass_fds=(write_fd,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    os.close(write_fd)
    application = None
    display = None
    browser_pid = None
    browser_fd = None

    with tempfile.TemporaryDirectory(prefix="spotify-oauth-", dir="/tmp/opencode") as temporary:
        try:
            with os.fdopen(read_fd) as stream:
                number = ready(stream)

            config = Path(temporary) / "config"
            config.mkdir()
            profile = Path(temporary) / "browser"
            (config / "mimeapps.list").write_text(
                "[Default Applications]\nx-scheme-handler/https=brave-browser.desktop\n"
            )
            (config / "brave-flags.conf").write_text("\n".join([
                f"--user-data-dir={profile}", "--no-first-run", "--no-default-browser-check",
                "--disable-background-networking", "--disable-component-update", "--disable-sync",
                "--remote-debugging-address=127.0.0.1", "--remote-debugging-port=0",
            ]))
            environment = os.environ.copy()
            environment.update(DISPLAY=f":{number}", XDG_CONFIG_HOME=str(config), NATIVE_CONTROLLER_OFFLINE="1")
            application = subprocess.Popen(
                [str(executable)], env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )

            if ready(application.stdout) != "first paint complete":
                raise RuntimeError("Native first paint failed")

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
                raise RuntimeError("Native display could not be opened")

            xtst.XTestFakeMotionEvent(display, -1, 640, 466, 0)
            xtst.XTestFakeButtonEvent(display, 1, 1, 0)
            xtst.XTestFakeButtonEvent(display, 1, 0, 0)
            x11.XFlush(display)

            deadline = time.monotonic() + 25
            targets = []

            while time.monotonic() < deadline:
                port_file = profile / "DevToolsActivePort"

                if port_file.exists():
                    port = int(port_file.read_text().splitlines()[0])
                    with urllib.request.urlopen(f"http://127.0.0.1:{port}/json/list", timeout=2) as response:
                        targets = json.load(response)
                    if any("accounts.spotify.com" in target.get("url", "") and target.get("title") for target in targets):
                        break

                time.sleep(0.1)

            lock = profile / "SingletonLock"
            if lock.is_symlink():
                browser_pid = int(os.readlink(lock).rsplit("-", 1)[1])
                browser_fd = os.pidfd_open(browser_pid)

            pages = [target for target in targets if target.get("type") == "page" and "accounts.spotify.com" in target.get("url", "")]
            if not pages:
                raise RuntimeError("The native button did not open Spotify in the isolated system browser")

            result = subprocess.run([
                "node", "--input-type=module", "-e", """
import { writeFileSync } from 'node:fs';
const socket = new WebSocket(process.argv[1]);
await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; });
let next = 0;
const waiting = new Map();
socket.onmessage = event => {
    const data = JSON.parse(event.data);
    if (waiting.has(data.id)) { waiting.get(data.id)(data); waiting.delete(data.id); }
};
const call = (method, params = {}) => new Promise(resolve => {
    const id = ++next;
    waiting.set(id, resolve);
    socket.send(JSON.stringify({ id, method, params }));
});
await call('Page.enable');
const page = await call('Runtime.evaluate', { expression: 'JSON.stringify({title:document.title,text:document.body.innerText})', returnByValue: true });
const screenshot = await call('Page.captureScreenshot', { format: 'png' });
writeFileSync(process.argv[2], Buffer.from(screenshot.result.data, 'base64'));
console.log(page.result.result.value);
socket.close();
""", pages[0]["webSocketDebuggerUrl"], str(output),
            ], capture_output=True, text=True, timeout=15, check=True)
            page = json.loads(result.stdout)
            print(json.dumps({
                "native_button_opened_real_browser": True,
                "browser_title": page["title"],
                "public_page_text": page["text"][:1600],
                "screenshot": str(output),
                "consent_submitted": False,
            }, indent=2))

        finally:
            if display:
                x11.XCloseDisplay(display)

            if browser_fd is not None:
                if not select.select([browser_fd], [], [], 0)[0]:
                    signal.pidfd_send_signal(browser_fd, signal.SIGTERM)
                    if not select.select([browser_fd], [], [], 10)[0]:
                        raise RuntimeError("Isolated browser did not shut down cleanly")
                os.close(browser_fd)

            for process in (application, server):
                if process and process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)


if __name__ == "__main__":
    main()
