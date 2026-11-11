#!/bin/sh
# Linux: start the FM-1 Manager (the first start sets it up). Needs python3, python3-venv and python3-tk:
#   Debian/Ubuntu: sudo apt install python3-venv python3-tk    Fedora: sudo dnf install python3-tkinter
cd "$(dirname "$0")" || exit 1
if ! python3 -c "import tkinter, venv" 2>/dev/null; then
    echo "Needs python3-tk and python3-venv: sudo apt install python3-venv python3-tk (Debian/Ubuntu)"
    exit 1
fi
if [ ! -x .venv/bin/python ] || ! .venv/bin/python -c "import mido, rtmidi, usb, libusb_package" 2>/dev/null; then
    echo "Setting up (once) ..."
    rm -rf .venv
    python3 -m venv .venv && .venv/bin/pip install --quiet --disable-pip-version-check -r requirements.txt || exit 1
fi
exec .venv/bin/python fm1_manager.py
