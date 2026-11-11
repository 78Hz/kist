#!/bin/bash
# macOS: double-click to start the FM-1 Manager (the first start sets it up, which takes a minute)
cd "$(dirname "$0")" || exit 1
PY=python3
if [ -x /Library/Frameworks/Python.framework/Versions/Current/bin/python3 ]; then
    PY=/Library/Frameworks/Python.framework/Versions/Current/bin/python3     # python.org's: the newer Tk
fi
if ! command -v "$PY" >/dev/null 2>&1 || ! "$PY" -c "import tkinter" 2>/dev/null; then
    echo "Python 3 with Tk is needed. Install it from https://www.python.org/downloads/ and start this again."
    xcode-select --install >/dev/null 2>&1 || true
    read -r -p "Press Return to close." _
    exit 1
fi
if [ ! -x .venv/bin/python ] || ! .venv/bin/python -c "import mido, rtmidi, usb, libusb_package" 2>/dev/null; then
    echo "Setting up (once) ..."
    rm -rf .venv
    "$PY" -m venv .venv && .venv/bin/pip install --quiet --disable-pip-version-check -r requirements.txt || {
        read -r -p "Setup failed (see above). Press Return to close." _; exit 1; }
fi
exec .venv/bin/python fm1_manager.py
