"""Reject floating direct or imported revisions in a resolved west manifest."""

from pathlib import Path
import re
import sys

import yaml


def main():
    manifest = yaml.safe_load(Path(sys.argv[1]).read_text(encoding="utf-8"))["manifest"]
    projects = manifest["projects"]
    floating = [p["name"] for p in projects
                if not re.fullmatch(r"[0-9a-f]{40}", str(p.get("revision", "")))]
    if floating:
        raise SystemExit("Unpinned dependencies: " + ", ".join(floating))
    print(f"PASS: all {len(projects)} resolved dependency revisions are immutable SHAs.")


if __name__ == "__main__":
    main()
