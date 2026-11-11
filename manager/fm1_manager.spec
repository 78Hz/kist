# -*- mode: python -*-
# PyInstaller: one FM-1 Manager app per platform (pyinstaller manager/fm1_manager.spec from the repository root,
# or from manager/). The tools run as modules of the app (FM-1 Manager --tool NAME.py ...), so they are hidden
# imports here; firmware/*.fwsc beside this file comes with the app and is copied into the library once.
import glob
import os
import sys

from PyInstaller.utils.hooks import collect_all

HERE = os.path.dirname(os.path.abspath(SPEC))
BASE = os.path.join(HERE, "..", "fm1-base")
PATHS = [HERE, os.path.join(BASE, "tools"), os.path.join(BASE, "bsp", "tools")]
TOOLS = ["fm1_install", "fm1_backup", "fm1ports", "fm1_resources", "fm1_rescue", "flash_tool"]

datas = [(f, "firmware") for f in glob.glob(os.path.join(HERE, "firmware", "*.fwsc"))]
datas += [(os.path.join(BASE, "LICENSE"), "licenses/fm1-base")]
binaries, hidden = [], list(TOOLS) + ["mido.backends.rtmidi", "rtmidi", "usb.core", "usb.util",
                                      "usb.backend.libusb1", "tkinter", "tkinter.ttk"]
if sys.platform != "win32":
    d, b, h = collect_all("libusb_package")
    datas, binaries, hidden = datas + d, binaries + b, hidden + h

a = Analysis([os.path.join(HERE, "fm1_manager.py")], pathex=PATHS, binaries=binaries, datas=datas,
             hiddenimports=hidden, excludes=["numpy", "PIL", "matplotlib"])
pyz = PYZ(a.pure)
if sys.platform == "darwin":
    exe = EXE(pyz, a.scripts, [], exclude_binaries=True, name="FM-1 Manager", console=False,
              target_arch=os.environ.get("FM1M_ARCH") or None)
    coll = COLLECT(exe, a.binaries, a.datas, name="FM-1 Manager")
    app = BUNDLE(coll, name="FM-1 Manager.app", bundle_identifier="io.github.78hz.fm1manager",
                 info_plist={"NSHighResolutionCapable": True, "CFBundleShortVersionString":
                             os.environ.get("FM1M_VERSION", "0.0.0")})
else:
    exe = EXE(pyz, a.scripts, a.binaries, a.datas, [], name="FM-1 Manager" if sys.platform == "win32"
              else "fm1-manager", console=False, upx=False)
