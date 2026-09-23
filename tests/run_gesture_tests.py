"""Compile the actual gesture function bodies with a deterministic host scheduler.

The firmware build separately checks Zephyr headers, DT instantiation and listeners.
This suite exercises state/timing/overflow paths without a keyboard or BLE changes.
"""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / "src/input_processor_gesture.c").read_text(encoding="utf-8")
    definition = re.search(r"^#define CYGNUS_GESTURE_EVENT_QUEUE_SIZE .+$", source, re.M)
    assert definition, "Production queue definition missing"
    core = source[source.index("enum cygnus_gesture_direction"):
                  source.index("#define GESTURE_BINDINGS")]
    with tempfile.TemporaryDirectory(prefix="cygnus-gesture-") as temporary:
        directory = Path(temporary)
        (directory / "gesture_core.inc").write_text(
            definition.group(0) + "\n" + core, encoding="utf-8")
        binary = directory / ("test.exe" if os.name == "nt" else "test")
        command = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run(command + ["-std=c11", "-Wall", "-Wextra", "-Werror",
                                  "-Wno-sign-compare", "-I", str(directory),
                                  str(root / "tests/gesture_test.c"), "-o", str(binary)],
                       check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
