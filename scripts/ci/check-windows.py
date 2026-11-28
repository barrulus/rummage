#!/usr/bin/env python3
"""Exercise installed window reuse on the private compositor supplied by check-wayland.sh."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time


def main():
    program = str(Path(sys.argv[1]) / "bin/rummage")
    with tempfile.TemporaryDirectory(prefix="rummage-windows-") as directory:
        root = Path(directory)
        first, second = root / "first", root / "second"
        first.mkdir()
        second.mkdir()
        (first / "one.txt").write_text("first item\n")
        (first / "two.txt").write_text("second item\n")
        log_path = root / "window.log"
        env = dict(os.environ, WAYLAND_DEBUG="1")
        with log_path.open("w") as log:
            browser = subprocess.Popen([program, str(first)], env=env, stdout=log, stderr=log)
            try:
                def text():
                    return log_path.read_text(errors="replace")

                def wait_for(condition):
                    deadline = time.monotonic() + 15
                    while not condition(text()):
                        if browser.poll() is not None or time.monotonic() >= deadline:
                            raise RuntimeError("Window check failed:\n" + text())
                        time.sleep(0.05)

                def toplevels(value):
                    return len(re.findall(r"\.get_toplevel\(", value))

                def launch(*arguments):
                    result = subprocess.run([program, *map(str, arguments)], env=env,
                                            capture_output=True, text=True, timeout=15)
                    if result.returncode:
                        raise RuntimeError(result.stderr)
                    # Forwarding happens before a second Wayland connection is created.
                    if "get_registry(" in result.stderr:
                        raise RuntimeError("Launch opened a separate Wayland connection")

                wait_for(lambda value: ".attach(" in value)
                assert 'librummage-photos.so' not in Path(f'/proc/{browser.pid}/maps').read_text(), \
                    'Fresh file browsing loaded the photo module'
                launch("--new-window", second)
                wait_for(lambda value: toplevels(value) == 2)
                launch("--open-folders", first, second)
                launch("--properties", first / "one.txt", first / "two.txt")
                wait_for(lambda value: toplevels(value) == 4 and
                         'set_title("Properties — two.txt")' in value)
                # A second properties request remains a separate, nonmodal window.
                launch("--properties", first / "one.txt")
                wait_for(lambda value: toplevels(value) == 5)
                browser.terminate()
                if browser.wait(timeout=10) != 0:
                    raise RuntimeError("Multiwindow shutdown failed:\n" + text())
                if re.search(r"AddressSanitizer|runtime error:", text()):
                    raise RuntimeError(text())
            finally:
                if browser.poll() is None:
                    browser.kill()
                    browser.wait()
    print("Installed browser reused tabs, created independent windows, and shut down cleanly.")


if __name__ == "__main__":
    main()
