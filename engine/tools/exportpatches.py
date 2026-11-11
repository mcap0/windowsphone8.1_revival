#!/usr/bin/env python3
"""exportpatches.py — write our WebKit changes as patches, without git commits.

engine/webkit is the tag (webkitgtk-2.54.1) plus uncommitted changes:
  0001-metrosurf-overlay.patch  tag -> MetroSurf's LGPL overlay (ref/MetroSurf/webkit), as published
  0002-lumia.patch              overlay -> our working tree (every change of ours, file by file)
Re-run after editing engine/webkit; review the result before relying on it."""
import os
import subprocess
import sys

ENGINE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEBKIT = os.path.join(ENGINE, "webkit")
OVERLAY = os.path.join(ENGINE, "..", "ref", "MetroSurf", "webkit")
PATCHES = os.path.join(ENGINE, "patches")


def git(*args):
    return subprocess.run(["git", "-C", WEBKIT] + list(args), check=True, capture_output=True, text=True).stdout


def head_text(path):
    r = subprocess.run(["git", "-C", WEBKIT, "show", "HEAD:" + path], capture_output=True)
    return r.stdout if r.returncode == 0 else None


def unified(path, before, after):
    """diff -u of two byte strings, labelled a/<path> b/<path>."""
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        a = os.path.join(tmp, "a")
        b = os.path.join(tmp, "b")
        open(a, "wb").write(before or b"")
        open(b, "wb").write(after or b"")
        r = subprocess.run(["diff", "-u", "--label", "a/" + path if before is not None else "/dev/null",
                            "--label", "b/" + path, a, b], capture_output=True)
        return r.stdout.decode("utf-8", "replace")


def main():
    changed = [line[3:] for line in git("status", "--porcelain", "--untracked-files=all").splitlines()]
    overlay_patch, ours = [], []
    for path in sorted(set(changed)):
        head = head_text(path)
        overlay_file = os.path.join(OVERLAY, path)
        overlay = open(overlay_file, "rb").read() if os.path.isfile(overlay_file) else None
        current_file = os.path.join(WEBKIT, path)
        current = open(current_file, "rb").read() if os.path.isfile(current_file) else None
        if overlay is not None and overlay != head:
            overlay_patch.append(unified(path, head, overlay))
        base = overlay if overlay is not None else head
        if current != base:
            ours.append(unified(path, base, current))
    with open(os.path.join(PATCHES, "0001-metrosurf-overlay.patch"), "w") as f:
        f.write("".join(overlay_patch))
    with open(os.path.join(PATCHES, "0002-lumia.patch"), "w") as f:
        f.write("".join(ours))
    print("0001-metrosurf-overlay.patch: %d files" % len(overlay_patch))
    print("0002-lumia.patch: %d files" % len(ours))
    for d in ours:
        print("   " + d.splitlines()[1][6:] if len(d.splitlines()) > 1 else "")


if __name__ == "__main__":
    sys.exit(main())
