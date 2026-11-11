#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""FM-1 Manager: install firmware on an M-VAVE FM-1, put M-VAVE's own firmware back, and keep backups:
the saved data of firmwares that offer it (over USB-MIDI, while the FM-1 runs), and whole-flash images
(through the chip's update mode; macOS and Linux).

The work is done by fm1-base's tools (fm1_install.py, fm1_backup.py, fm1_rescue.py: the repository's fm1-base
submodule, or packaged inside the app), run as separate processes; this window starts them, shows what they say
and keeps the files in order, in Documents/FM-1 Manager (or FM1M_HOME, or beside a copy with a "portable" file):
  firmware/        the firmware library (.fwsc)
  backups/data/    saved-data backups (.fm1b)
  backups/flash/   whole-flash images (.bin, each with a .json beside it)
"""
import hashlib
import io
import json
import os
import queue
import re
import shlex
import shutil
import subprocess
import sys
import threading
import time
import urllib.request

import tkinter as tk
from tkinter import filedialog, messagebox, simpledialog, ttk

FROZEN = getattr(sys, "frozen", False)               # the packaged app (PyInstaller)
CODE = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
# fm1-base's tools: beside the app when packaged, else from the repository's fm1-base submodule
TOOL_DIRS = [d for d in (os.path.join(CODE, "tools"),
                         os.path.join(CODE, "..", "fm1-base", "tools"),
                         os.path.join(CODE, "..", "fm1-base", "bsp", "tools")) if os.path.isdir(d)]


def home_dir():
    """where the library and the backups live: FM1M_HOME, a folder beside a portable copy (a "portable" file
    there), else Documents/FM-1 Manager"""
    if os.environ.get("FM1M_HOME"):
        return os.environ["FM1M_HOME"]
    here = os.path.dirname(os.path.abspath(sys.executable if FROZEN else __file__))
    if os.path.exists(os.path.join(here, "portable")):
        return here
    docs = os.path.join(os.path.expanduser("~"), "Documents")
    return os.path.join(docs if os.path.isdir(docs) else os.path.expanduser("~"), "FM-1 Manager")


ROOT = home_dir()
LIB = os.path.join(ROOT, "firmware")
DATA = os.path.join(ROOT, "backups", "data")
FLASH = os.path.join(ROOT, "backups", "flash")
NAMES = os.path.join(ROOT, "library.json")
for _d in reversed(TOOL_DIRS):
    sys.path.insert(0, _d)


def tool_file(script):
    if script == "flash_tool.py":
        return os.path.join(CODE, script)
    for d in TOOL_DIRS:
        if os.path.isfile(os.path.join(d, script)):
            return os.path.join(d, script)
    raise FileNotFoundError(script)


import fm1_install as I  # noqa: E402  (package parsing only: no MIDI is opened in this process)

WINDOWS = sys.platform.startswith("win")
MAC = sys.platform == "darwin"

# M-VAVE's FM-1 firmware, by the sha256 of the .fwsc (as fm1_rescue.py knows them)
STOCK = {"db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a": "V15",
         "d017fb65e70aca97ee29d944470c3515a74581df5c0576bf70de917c7b2b61d5": "V16"}
STOCK_URL = "https://yms-file-store.oss-cn-hongkong.aliyuncs.com/software/firmware/FM-1.fwsc"
V16 = "d017fb65e70aca97ee29d944470c3515a74581df5c0576bf70de917c7b2b61d5"

# firmware identities (the package's / the FM-1's "FM-1_..." name), longest prefix first:
# the name shown, and the saved-data backup magic when the firmware has one
KNOWN = [("FM-1_099", "m-bootloader installer", None),
         ("FM-1_0", "M-VAVE stock firmware", None),
         ("FM-1_57", "KIST", "KISB"),
         ("FM-1_920", "ChoralRoot", None),
         ("FM-1_3", "Polly", "PLYB")]
MAGICS = os.path.join(ROOT, "magics.json")     # magics the user has given, by identity


def known(identity):
    for prefix, name, magic in KNOWN:
        if identity.startswith(prefix):
            return name, magic
    return None, None


def load_json(path, default):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def save_json(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(obj, f, indent=1, sort_keys=True)
    os.replace(tmp, path)


def human(n):
    if n < 1024:
        return f"{n} B"
    return f"{n / 1024:.0f} KB" if n < 1 << 20 else f"{n / (1 << 20):.1f} MB"


def deps():
    """the Python modules each part needs -> {part: [missing]}"""
    miss = {"midi": [], "flash": []}
    for mod, pkg in (("mido", "mido"), ("rtmidi", "python-rtmidi")):
        try:
            __import__(mod)
        except ImportError:
            miss["midi"].append(pkg)
    for mod, pkg in (("usb", "pyusb"), ("libusb_package", "libusb-package")):
        try:
            __import__(mod)
        except ImportError:
            miss["flash"].append(pkg)
    return miss


# ------------------------------------------------------------------------------------------- library ---
class Package:
    def __init__(self, path, names):
        self.path = path
        raw = open(path, "rb").read()
        self.size = len(raw)
        self.sha = hashlib.sha256(raw).hexdigest()
        try:
            self.identity = I.product_of(raw)
        except Exception:
            self.identity = "?"
        self.stock = STOCK.get(self.sha)
        self.loader = I.LOADER_MARK in raw
        base, _ = known(self.identity)
        if self.stock:
            base = f"M-VAVE stock {self.stock}"
        self.name = names.get(self.sha) or base or os.path.splitext(os.path.basename(path))[0]
        self.ok = bool(re.fullmatch(r"[^_]+_\d+", self.identity))
        self.kind = ("M-VAVE" if self.stock else "fm1-base / Felucca" if self.loader else
                     "other (needs force)" if self.ok else "not an FM-1 package")
        self.added = time.strftime("%Y-%m-%d", time.localtime(os.path.getmtime(path)))


# ------------------------------------------------------------------------------------------- the window ---
class App:
    def __init__(self, root):
        self.root = root
        self.q = queue.Queue()
        self.busy = None                      # the running job's name
        self.proc = None
        self.device = None                    # identity text of the connected FM-1, or None
        root.title("FM-1 Manager")
        root.geometry("920x700")
        root.minsize(760, 520)
        for d in (LIB, DATA, FLASH):
            os.makedirs(d, exist_ok=True)
        seed = os.path.join(CODE, "firmware")          # the firmware that came with the app, once
        seeded = os.path.join(ROOT, ".seeded")
        if os.path.isdir(seed):
            done = set(load_json(seeded, []))
            for fn in os.listdir(seed):
                if fn.endswith(".fwsc") and fn not in done:
                    if not os.path.exists(os.path.join(LIB, fn)):
                        shutil.copy2(os.path.join(seed, fn), os.path.join(LIB, fn))
                    done.add(fn)
            save_json(seeded, sorted(done))
        self.names = load_json(NAMES, {})
        self.magics = load_json(MAGICS, {})
        self.build()
        self.refresh_all()
        miss = deps()
        if miss["midi"]:
            self.log("Missing Python modules for USB-MIDI: " + ", ".join(miss["midi"]) +
                     ". Start the Manager with its launcher, or: pip install " + " ".join(miss["midi"]))
        self.root.after(80, self.pump)
        self.root.after(300, self.check_device)

    # ----------------------------------------------------------------------------------------- layout ---
    def build(self):
        st = ttk.Style()
        try:
            st.theme_use("clam" if not (MAC or WINDOWS) else st.theme_use())
        except tk.TclError:
            pass
        top = ttk.Frame(self.root, padding=(10, 8))
        top.pack(fill="x")
        ttk.Label(top, text="FM-1:").pack(side="left")
        self.dev_var = tk.StringVar(value="not checked yet")
        ttk.Label(top, textvariable=self.dev_var, font=("TkDefaultFont", 11, "bold")).pack(side="left", padx=6)
        self.check_btn = ttk.Button(top, text="Check connection", command=self.check_device)
        self.check_btn.pack(side="right")
        ttk.Label(top, text="MIDI port (optional):").pack(side="right", padx=(12, 4))
        self.port_var = tk.StringVar()
        ttk.Entry(top, textvariable=self.port_var, width=16).pack(side="right")

        pw = ttk.PanedWindow(self.root, orient="vertical")
        pw.pack(fill="both", expand=True, padx=10)
        nb = ttk.Notebook(pw)
        pw.add(nb, weight=3)
        self.nb = nb
        nb.add(self.build_firmware(nb), text="  Firmware  ")
        nb.add(self.build_data(nb), text="  Saved data  ")
        nb.add(self.build_flash(nb), text="  Whole flash  ")

        bottom = ttk.Frame(pw)
        pw.add(bottom, weight=2)
        bar = ttk.Frame(bottom)
        bar.pack(fill="x", pady=(6, 2))
        self.status = tk.StringVar(value="Ready")
        ttk.Label(bar, textvariable=self.status).pack(side="left")
        self.cancel_btn = ttk.Button(bar, text="Stop", command=self.cancel, state="disabled")
        self.cancel_btn.pack(side="right")
        ttk.Button(bar, text="Copy log", command=self.copy_log).pack(side="right", padx=4)
        self.prog = ttk.Progressbar(bar, length=220, mode="determinate", maximum=100)
        self.prog.pack(side="right", padx=8)
        lf = ttk.Frame(bottom)
        lf.pack(fill="both", expand=True, pady=(0, 8))
        self.text = tk.Text(lf, height=10, wrap="word", font=("TkFixedFont", 10), state="disabled",
                            background="#fbfaf7", relief="flat", borderwidth=1)
        sb = ttk.Scrollbar(lf, command=self.text.yview)
        self.text.configure(yscrollcommand=sb.set)
        sb.pack(side="right", fill="y")
        self.text.pack(fill="both", expand=True)
        self.text.tag_configure("err", foreground="#9e2a1f")
        self.text.tag_configure("head", foreground="#1e1a16", font=("TkFixedFont", 10, "bold"))

    def tree(self, parent, cols):
        f = ttk.Frame(parent)
        t = ttk.Treeview(f, columns=[c for c, _, _ in cols], show="headings", selectmode="browse", height=5)
        for c, label, w in cols:
            t.heading(c, text=label)
            t.column(c, width=w, anchor="w", stretch=c == cols[0][0])
        sb = ttk.Scrollbar(f, command=t.yview)
        t.configure(yscrollcommand=sb.set)
        sb.pack(side="right", fill="y")
        t.pack(fill="both", expand=True)
        f.pack(fill="both", expand=True, pady=(8, 6))
        return t

    def buttons(self, parent, spec):
        f = ttk.Frame(parent)
        f.pack(fill="x")
        out = {}
        for label, cmd, side in spec:
            b = ttk.Button(f, text=label, command=cmd)
            b.pack(side=side, padx=(0, 6) if side == "left" else (6, 0))
            out[label] = b
        return out

    def build_firmware(self, nb):
        p = ttk.Frame(nb, padding=8)
        ttk.Label(p, text="Firmware in the library. Select one and install it on the connected FM-1; the FM-1 "
                          "stays on its current firmware until the new one is written and checked.",
                  wraplength=840, justify="left").pack(anchor="w")
        self.fw = self.tree(p, [("name", "Name", 260), ("id", "Identity", 120), ("kind", "Kind", 170),
                                ("size", "Size", 80), ("added", "Added", 100)])
        self.fw.bind("<Double-1>", lambda e: self.install())
        self.fw_btns = self.buttons(p, [
            ("Install on FM-1", self.install, "left"),
            ("Add firmware…", self.add_firmware, "left"),
            ("Download M-VAVE V16", self.download_stock, "left"),
            ("Rename…", self.rename_fw, "left"),
            ("Remove", self.remove_fw, "left"),
            ("Open folder", lambda: self.open_folder(LIB), "right")])
        self.force_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(p, text="Allow firmware without the Felucca update loader (the FM-1 still checks it)",
                        variable=self.force_var).pack(anchor="w", pady=(6, 0))
        return p

    def build_data(self, nb):
        p = ttk.Frame(nb, padding=8)
        ttk.Label(p, text="What a firmware saves (its setup, presets, songs), backed up over USB while it runs. "
                          "Only firmwares built with backup support answer (KIST, Polly and others on fm1-base). "
                          "A restore goes back to the firmware that made it, and the FM-1 asks on its screen "
                          "first: SEL yes, HOME no.", wraplength=840, justify="left").pack(anchor="w")
        self.bk = self.tree(p, [("file", "Backup", 300), ("fw", "Firmware", 160), ("magic", "Magic", 70),
                                ("size", "Size", 80), ("date", "Taken", 140)])
        self.bk_btns = self.buttons(p, [
            ("Back up now", self.data_backup, "left"),
            ("Restore to FM-1", self.data_restore, "left"),
            ("Details", self.data_info, "left"),
            ("Import…", self.data_import, "left"),
            ("Delete", self.data_delete, "left"),
            ("Open folder", lambda: self.open_folder(DATA), "right")])
        return p

    def build_flash(self, nb):
        p = ttk.Frame(nb, padding=8)
        msg = ("A complete image of the FM-1's 1 MB flash: firmware, bootloader and everything saved, read "
               "through the chip's own update mode (switch the FM-1 off, hold REC and switch it on). "
               "Restore writes an image back; Put M-VAVE firmware back works even on an FM-1 that "
               "won't start. These need your computer's password.")
        if WINDOWS:
            msg = ("Whole-flash backup and restore work on macOS and Linux only (Windows would need a "
                   "different USB driver). Use a Mac or Linux computer for these.")
        ttk.Label(p, text=msg, wraplength=840, justify="left").pack(anchor="w")
        self.fl = self.tree(p, [("file", "Image", 260), ("boot", "Bootloader", 110), ("chip", "Flash chip", 100),
                                ("note", "Note", 200), ("date", "Taken", 130)])
        self.data_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(p, text="Restore also restores M-VAVE's data area (0xE0000 onward): only with an image "
                               "of this same FM-1", variable=self.data_var).pack(anchor="w", pady=(0, 6))
        self.fl_btns = self.buttons(p, [
            ("Back up whole flash", self.flash_backup, "left"),
            ("Restore image", self.flash_restore, "left"),
            ("Put M-VAVE firmware back", self.flash_stock, "left"),
            ("Note…", self.flash_note, "left"),
            ("Delete", self.flash_delete, "left"),
            ("Open folder", lambda: self.open_folder(FLASH), "right")])
        if WINDOWS:
            for b in self.fl_btns.values():
                if b.cget("text") != "Open folder":
                    b.state(["disabled"])
        return p

    # ---------------------------------------------------------------------------------------- listings ---
    def refresh_all(self):
        self.refresh_fw()
        self.refresh_data()
        self.refresh_flash()

    def refresh_fw(self):
        self.pkgs = {}
        self.fw.delete(*self.fw.get_children())
        for fn in sorted(os.listdir(LIB), key=str.lower):
            if not fn.lower().endswith(".fwsc"):
                continue
            try:
                pk = Package(os.path.join(LIB, fn), self.names)
            except OSError:
                continue
            iid = self.fw.insert("", "end", values=(pk.name, pk.identity, pk.kind, human(pk.size), pk.added))
            self.pkgs[iid] = pk

    def refresh_data(self):
        self.backups = {}
        self.bk.delete(*self.bk.get_children())
        for fn in sorted(os.listdir(DATA), reverse=True):
            if not fn.lower().endswith((".fm1b", ".bin", ".bak")):
                continue
            path = os.path.join(DATA, fn)
            head = open(path, "rb").read(4)
            magic = head.decode("ascii", "replace") if len(head) == 4 else "?"
            fw = next((n for _, n, m in KNOWN if m == magic), None) or \
                next((i for i, m in self.magics.items() if m == magic), "?")
            date = time.strftime("%Y-%m-%d %H:%M", time.localtime(os.path.getmtime(path)))
            iid = self.bk.insert("", "end", values=(fn, fw, magic, human(os.path.getsize(path)), date))
            self.backups[iid] = path

    def stock_images(self):
        """the flash entries of the M-VAVE packages in the library: {version: image}"""
        out = {}
        import fm1_rescue as R
        for pk in getattr(self, "pkgs", {}).values():
            if pk.stock and pk.stock not in out:
                try:
                    out[pk.stock] = R.package_flash_image(open(pk.path, "rb").read())
                except Exception:
                    pass
        return out

    def refresh_flash(self):
        self.images = {}
        self.fl.delete(*self.fl.get_children())
        heads = {v: im[:0x4000] for v, im in self.stock_images().items()}
        for fn in sorted(os.listdir(FLASH), reverse=True):
            if not fn.lower().endswith(".bin"):
                continue
            path = os.path.join(FLASH, fn)
            meta = load_json(path[:-4] + ".json", {})
            with open(path, "rb") as f:
                head = f.read(0x4000)
            boot = next((f"M-VAVE {v}" for v, h in heads.items() if h == head), "other" if heads else "?")
            chip = {"856014": "P25Q80", "B36014": "UC25HQ80"}.get(meta.get("flash_id", ""), meta.get("flash_id", "?"))
            date = time.strftime("%Y-%m-%d %H:%M", time.localtime(os.path.getmtime(path)))
            iid = self.fl.insert("", "end", values=(fn, boot, chip, meta.get("note", ""), date))
            self.images[iid] = path

    def selected(self, tree, table, what):
        sel = tree.selection()
        if not sel:
            messagebox.showinfo("FM-1 Manager", f"Select {what} first.")
            return None
        return table.get(sel[0])

    # ------------------------------------------------------------------------------------------- jobs ---
    def log(self, s, tag=None):
        self.text.configure(state="normal")
        self.text.insert("end", s.rstrip("\n") + "\n", tag)
        self.text.see("end")
        self.text.configure(state="disabled")

    def copy_log(self):
        self.root.clipboard_clear()
        self.root.clipboard_append(self.text.get("1.0", "end"))
        self.status.set("Log copied")

    def tool(self, script):
        if os.environ.get("FM1M_FAKE"):       # development: the tools against a simulated FM-1
            return [sys.executable, "-u", os.environ["FM1M_FAKE"], script]
        if FROZEN:                            # the app runs its own tools: FM-1 Manager --tool fm1_install.py ...
            return [sys.executable, "--tool", script]
        return [sys.executable, "-u", tool_file(script)]

    def run(self, name, argv, done=None, elevate=False, logfile=None):
        """start one job; its output goes to the log, done(rc) after it ends (in the window's thread)"""
        if self.busy:
            messagebox.showinfo("FM-1 Manager", f"Wait until '{self.busy}' has finished.")
            return
        self.busy = name
        self.status.set(name + " …")
        self.prog.configure(mode="indeterminate")
        self.prog.start(12)
        self.cancel_btn.state(["!disabled"])
        self.log(f"— {name} —", "head")
        threading.Thread(target=self.worker, args=(argv, done, elevate, logfile), daemon=True).start()

    def worker(self, argv, done, elevate, logfile):
        env = dict(os.environ, PYTHONUNBUFFERED="1", PYTHONIOENCODING="utf-8")
        rc = 1
        try:
            if elevate:
                rc = self.run_elevated(argv, env, logfile)
            else:
                self.proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
                                             cwd=ROOT, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
                buf = b""
                while True:
                    ch = self.proc.stdout.read1(4096) if hasattr(self.proc.stdout, "read1") else self.proc.stdout.read(1)
                    if not ch:
                        break
                    buf += ch
                    while True:
                        m = re.search(rb"[\r\n]", buf)
                        if not m:
                            break
                        line, buf = buf[:m.start()], buf[m.end():]
                        self.q.put(("line", line.decode("utf-8", "replace")))
                if buf:
                    self.q.put(("line", buf.decode("utf-8", "replace")))
                rc = self.proc.wait()
        except Exception as e:
            self.q.put(("line", f"error: {e}"))
        finally:
            self.proc = None
            self.q.put(("done", (rc, done)))

    def run_elevated(self, argv, env, logfile):
        """the whole-flash tools need root: macOS asks with its own password window, Linux with pkexec;
        the output goes to a log file, read here while it runs"""
        open(logfile, "w").close()
        cmd = " ".join(shlex.quote(a) for a in argv) + f" > {shlex.quote(logfile)} 2>&1"
        if hasattr(os, "getuid"):           # what root wrote there goes back to you
            cmd += f"; rc=$?; chown -R {os.getuid()}:{os.getgid()} {shlex.quote(FLASH)}; exit $rc"
        if hasattr(os, "geteuid") and os.geteuid() == 0:
            full = ["/bin/sh", "-c", cmd]
        elif MAC:
            script = 'do shell script "%s" with administrator privileges' % cmd.replace("\\", "\\\\").replace('"', '\\"')
            full = ["osascript", "-e", script]
        elif shutil.which("pkexec"):
            full = ["pkexec", "/bin/sh", "-c", "cd %s && %s" % (shlex.quote(ROOT), cmd)]
        else:
            self.q.put(("line", "error: no pkexec here to ask for the password. Run this in a terminal instead:"))
            self.q.put(("line", "  sudo " + " ".join(shlex.quote(a) for a in argv)))
            return 1
        self.proc = subprocess.Popen(full, env=env, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        pos = 0
        while True:
            alive = self.proc.poll() is None
            try:
                with open(logfile, "rb") as f:
                    f.seek(pos)
                    chunk = f.read()
                    pos += len(chunk)
                for line in re.split(rb"[\r\n]+", chunk):
                    if line:
                        self.q.put(("line", line.decode("utf-8", "replace")))
            except OSError:
                pass
            if not alive:
                break
            time.sleep(0.2)
        err = self.proc.stderr.read().decode("utf-8", "replace").strip()
        if err:
            self.q.put(("line", err if "cancel" not in err.lower() else "cancelled (no password given)"))
        return self.proc.returncode

    def cancel(self):
        p = self.proc
        if p and p.poll() is None:
            if not messagebox.askyesno("FM-1 Manager", "Stop the running job? If it is writing, the FM-1 stays "
                                       "in update mode and the same job run again finishes it."):
                return
            p.terminate()

    def pump(self):
        try:
            while True:
                kind, v = self.q.get_nowait()
                if kind == "line":
                    self.on_line(v)
                else:
                    rc, done = v
                    self.prog.stop()
                    self.prog.configure(mode="determinate")
                    self.root.after(60, lambda v=100 if rc == 0 else 0: self.prog.configure(value=v))
                    self.cancel_btn.state(["disabled"])
                    name, self.busy = self.busy, None
                    self.status.set(f"{name}: done" if rc == 0 else f"{name}: failed (code {rc})")
                    if done:
                        done(rc)
        except queue.Empty:
            pass
        self.root.after(80, self.pump)

    def on_line(self, s):
        s = s.replace("\033[K", "").rstrip()
        if not s:
            return
        m = re.match(r"PROGRESS (\d+)", s)
        if m:
            self.determinate(int(m.group(1)))
            return
        m = re.search(r"writing\s+(\d+)%", s) or re.search(r"writing (\d+)/(\d+)", s)
        if m:
            self.determinate(int(m.group(1)) if m.lastindex == 1 else 100 * int(m.group(1)) // int(m.group(2)))
            self.status.set(f"{self.busy} … {s.strip()}")
            return                                   # progress lines would flood the log
        if s.startswith(("checking the package", "receiving ", "sending ")):
            self.status.set(f"{self.busy} … {s}")
            return
        self.log(s, "err" if s.lower().startswith(("error", "fm1_rescue:", "attempt")) else None)

    def determinate(self, v):
        if str(self.prog.cget("mode")) != "determinate":
            self.prog.stop()
            self.prog.configure(mode="determinate")
        self.prog.configure(value=max(0, min(100, v)))

    def port_args(self):
        p = self.port_var.get().strip()
        return ["--port", p] if p else []

    def open_folder(self, path):
        if WINDOWS:
            os.startfile(path)  # noqa
        else:
            subprocess.Popen(["open" if MAC else "xdg-open", path])

    # ----------------------------------------------------------------------------------------- device ---
    def check_device(self, then=None):
        if self.busy:
            return
        out = []

        def done(rc):
            line = next((s for s in out if "[" in s and "port:" in s), None)
            if rc == 0 and line:
                ident = line.split()[0]
                self.device = ident
                name, _ = known(ident)
                mode = "  (in update mode: finish the install)" if "update loader" in line else ""
                port = line.split("port:", 1)[1].split("  flash:")[0].strip()
                flash = line.split("flash:", 1)[1].strip() if "flash:" in line else ""
                self.dev_var.set(f"{name or ident}  ·  {ident}  ·  {port}" + (f"  ·  {flash}" if flash else "") + mode)
            else:
                self.device = None
                self.dev_var.set("not found (USB data cable? another MIDI app open?)")
            if then:
                then()

        orig = self.on_line

        def grab(s):
            out.append(s)
            orig(s)
        self.on_line = grab

        def finish(rc):
            self.on_line = orig
            done(rc)
        self.run("Checking the FM-1", self.tool("fm1_install.py") + ["--info"] + self.port_args(), finish)

    # --------------------------------------------------------------------------------------- firmware ---
    def add_firmware(self):
        paths = filedialog.askopenfilenames(title="Add firmware", filetypes=[("FM-1 firmware", "*.fwsc"),
                                                                            ("All files", "*")])
        for p in paths:
            dest = os.path.join(LIB, os.path.basename(p))
            if os.path.abspath(p) == os.path.abspath(dest):
                continue
            if os.path.exists(dest) and not messagebox.askyesno("FM-1 Manager", f"Replace {os.path.basename(p)} "
                                                                "in the library?"):
                continue
            shutil.copy2(p, dest)
            self.log(f"added {os.path.basename(p)}")
        self.refresh_fw()

    def download_stock(self):
        dest = os.path.join(LIB, "FM-1-V16.fwsc")
        if any(pk.sha == V16 for pk in self.pkgs.values()):
            messagebox.showinfo("FM-1 Manager", "M-VAVE's V16 is already in the library.")
            return
        if self.busy:
            return
        self.busy = "Downloading M-VAVE V16"
        self.status.set(self.busy + " …")
        self.prog.configure(mode="indeterminate")
        self.prog.start(12)
        self.log("— downloading M-VAVE's FM-1 firmware —", "head")

        def get():
            rc = 1
            try:
                with urllib.request.urlopen(STOCK_URL, timeout=60) as r:
                    raw = r.read()
                sha = hashlib.sha256(raw).hexdigest()
                if sha != V16:
                    name = STOCK.get(sha)
                    self.q.put(("line", "error: the file M-VAVE serves is " + (f"{name}, not V16" if name else
                                "not a version known here (sha256 " + sha[:16] + "…): not kept") + "."))
                    if name:
                        open(os.path.join(LIB, f"FM-1-{name}.fwsc"), "wb").write(raw)
                        rc = 0
                else:
                    open(dest, "wb").write(raw)
                    self.q.put(("line", f"saved {os.path.basename(dest)} (checksum matches V16)"))
                    rc = 0
            except Exception as e:
                self.q.put(("line", f"error: download failed: {e}. Get FM-1.fwsc from www.m-vave.com/download "
                                    "(PC Firmware) and add it with Add firmware…"))
            self.q.put(("done", (rc, lambda _: self.refresh_all())))
        threading.Thread(target=get, daemon=True).start()

    def rename_fw(self):
        pk = self.selected(self.fw, self.pkgs, "a firmware")
        if not pk:
            return
        s = simpledialog.askstring("Rename", "Name shown for this firmware:", initialvalue=pk.name, parent=self.root)
        if s is not None:
            if s.strip():
                self.names[pk.sha] = s.strip()
            else:
                self.names.pop(pk.sha, None)
            save_json(NAMES, self.names)
            self.refresh_fw()

    def remove_fw(self):
        pk = self.selected(self.fw, self.pkgs, "a firmware")
        if pk and messagebox.askyesno("FM-1 Manager", f"Remove {os.path.basename(pk.path)} from the library? "
                                      "(The file is deleted from the firmware folder.)"):
            os.remove(pk.path)
            self.refresh_fw()

    def install(self):
        pk = self.selected(self.fw, self.pkgs, "a firmware")
        if not pk:
            return
        if not pk.ok:
            messagebox.showerror("FM-1 Manager", f"{os.path.basename(pk.path)} is not an FM-1 firmware package.")
            return
        force = not pk.loader and not pk.stock
        if force and not self.force_var.get():
            messagebox.showwarning("FM-1 Manager", f"{pk.name} has no Felucca update loader and is not M-VAVE's "
                                   "firmware. If you trust it, tick 'Allow firmware without the Felucca update "
                                   "loader' and install again; the FM-1 still checks the package before writing.")
            return
        self.check_device(then=lambda: self.install_ask(pk, force))

    def install_ask(self, pk, force):
        if not self.device:
            messagebox.showerror("FM-1 Manager", "The FM-1 was not found. Connect it with a USB data cable straight "
                                 "to the computer, switch it on as normal, close other MIDI apps, then try again.")
            return
        cur, magic = known(self.device)
        magic = magic or self.magics.get(self.device)
        if magic and not pk.identity.startswith(self.device[:7]):
            a = messagebox.askyesnocancel("FM-1 Manager", f"The FM-1 runs {cur or self.device}. Back up what it "
                                          "has saved first?\n\nYes: back up, then install.  No: install now.")
            if a is None:
                return
            if a:
                self.data_backup(then=lambda rc: rc == 0 and self.install_go(pk, force))
                return
        self.install_go(pk, force)

    def install_go(self, pk, force):
        cur = known(self.device)[0] or self.device
        if not messagebox.askokcancel("Install", f"Install {pk.name} ({pk.identity}) over {cur}?\n\n"
                                      "Don't unplug the cable or switch the FM-1 off until this window says it "
                                      "is done. It can take a few minutes; the FM-1 restarts at the end."):
            return
        argv = self.tool("fm1_install.py") + [pk.path, "--yes"] + self.port_args() + (["--force"] if force else [])

        def done(rc):
            if rc == 0:
                self.log(f"{pk.name} is installed.")
                self.root.after(1500, self.check_device)
            elif rc == 6:
                self.log("error: the FM-1 reports another identity than the package's, or is not an FM-1.", "err")
            elif rc == 3:
                self.log("error: the FM-1 was not found.", "err")
            elif rc in (4, 5):
                self.log("The install did not finish. If the FM-1 is in update mode, install the same firmware "
                         "again to finish; if it won't start, see Whole flash → Put M-VAVE firmware back.", "err")
        self.run(f"Installing {pk.name}", argv, done)

    # -------------------------------------------------------------------------------------- saved data ---
    def data_backup(self, then=None):
        def go():
            if not self.device:
                messagebox.showerror("FM-1 Manager", "The FM-1 was not found.")
                return
            name, magic = known(self.device)
            magic = magic or self.magics.get(self.device)
            if not magic:
                magic = simpledialog.askstring(
                    "Backup", f"{name or self.device} is not known here. If it has backup support, give its "
                    "four-letter backup magic (from its manual or source, app.h APP_BACKUP_MAGIC):", parent=self.root)
                if not magic:
                    return
                magic = magic.strip()
                if len(magic) != 4:
                    messagebox.showerror("FM-1 Manager", "The magic is four letters.")
                    return
                self.magics[self.device] = magic
                save_json(MAGICS, self.magics)
            path = os.path.join(DATA, f"{(name or self.device).replace(' ', '_')}-{time.strftime('%Y%m%d-%H%M%S')}.fm1b")
            argv = self.tool("fm1_backup.py") + ["backup", path, "--magic", magic] + self.port_args()

            def done(rc):
                self.refresh_data()
                if rc == 5:
                    self.log("Stop playing on the FM-1, then back up again.", "err")
                elif rc == 4:
                    self.log("No answer to the backup request: this firmware may not have backup support.", "err")
                if then:
                    then(rc)
            self.run(f"Backing up {name or self.device}", argv, done)
        self.check_device(then=go)

    def data_restore(self):
        path = self.selected(self.bk, self.backups, "a backup")
        if not path:
            return
        if not messagebox.askokcancel("Restore", f"Restore {os.path.basename(path)}?\n\nIt replaces everything this "
                                      "firmware has saved. The FM-1 shows RESTORE FROM USB? — press SEL to accept, "
                                      "HOME to refuse."):
            return
        argv = self.tool("fm1_backup.py") + ["restore", path, "--yes"] + self.port_args()

        def done(rc):
            if rc == 6:
                self.log("This backup is for another firmware or version than the one the FM-1 runs.", "err")
            elif rc == 5:
                self.log("Stop playing on the FM-1, then restore again.", "err")
        self.run("Restoring saved data", argv, done)

    def data_info(self):
        path = self.selected(self.bk, self.backups, "a backup")
        if path:
            self.run("Backup details", self.tool("fm1_backup.py") + ["info", path])

    def data_import(self):
        for p in filedialog.askopenfilenames(title="Import backups"):
            shutil.copy2(p, os.path.join(DATA, os.path.basename(p)))
        self.refresh_data()

    def data_delete(self):
        path = self.selected(self.bk, self.backups, "a backup")
        if path and messagebox.askyesno("FM-1 Manager", f"Delete {os.path.basename(path)}?"):
            os.remove(path)
            self.refresh_data()

    # ------------------------------------------------------------------------------------- whole flash ---
    def flash_ready(self):
        if WINDOWS:
            return False
        miss = deps()["flash"]
        if miss:
            messagebox.showerror("FM-1 Manager", "Missing Python modules: " + ", ".join(miss) +
                                 ". Start the Manager with its launcher, which installs them.")
            return False
        return True

    def uboot_hint(self, what):
        return messagebox.askokcancel(what, "1. Switch the FM-1 off and keep it connected by USB.\n"
                                      "2. Press OK, and give your computer's password if asked.\n"
                                      "3. Hold REC and switch the FM-1 on; keep holding until the log says it "
                                      "was found (the screen stays dark).\n\nThe Manager waits two minutes.")

    def flash_backup(self):
        if not self.flash_ready() or not self.uboot_hint("Back up whole flash"):
            return
        log = os.path.join(FLASH, ".flash_tool.log")
        self.run("Backing up the whole flash", self.tool("flash_tool.py") + ["backup", FLASH], elevate=True,
                 logfile=log, done=lambda rc: self.after_flash(log))

    def after_flash(self, log):
        try:
            os.remove(log)
        except OSError:
            pass
        self.refresh_flash()

    def flash_restore(self):
        path = self.selected(self.fl, self.images, "an image")
        if not path or not self.flash_ready():
            return
        data = self.data_var.get()
        if not messagebox.askokcancel("Restore image", f"Write {os.path.basename(path)} back to the FM-1?\n\n"
                                      "The FM-1's flash as it is now is backed up first. Only changed sectors are "
                                      "written; the bootloader is never touched and must match the image's." +
                                      ("\n\nM-VAVE's data area is restored too." if data else "")):
            return
        if not self.uboot_hint("Restore image"):
            return
        log = os.path.join(FLASH, ".flash_tool.log")
        self.run("Restoring a whole-flash image", self.tool("flash_tool.py") + ["restore", path, FLASH] +
                 (["--data"] if data else []), elevate=True, logfile=log, done=lambda rc: self.after_flash(log))

    def flash_stock(self):
        if not self.flash_ready():
            return
        pk = [p.path for p in self.pkgs.values() if p.stock]
        if not pk:
            messagebox.showinfo("FM-1 Manager", "Add M-VAVE's firmware to the library first (Firmware → Download "
                                "M-VAVE V16, or Add firmware… with FM-1.fwsc).")
            return
        if not messagebox.askokcancel("Put M-VAVE firmware back", "This writes M-VAVE's firmware through the "
                                      "chip's update mode, for an FM-1 that won't start or can't install. The "
                                      "flash is backed up first.\n\nIf the FM-1 still starts, Firmware → Install "
                                      "with M-VAVE's package is simpler."):
            return
        if not self.uboot_hint("Put M-VAVE firmware back"):
            return
        log = os.path.join(FLASH, ".flash_tool.log")
        self.run("Putting M-VAVE's firmware back", self.tool("flash_tool.py") + ["stock", FLASH] + pk,
                 elevate=True, logfile=log, done=lambda rc: self.after_flash(log))

    def flash_note(self):
        path = self.selected(self.fl, self.images, "an image")
        if not path:
            return
        mp = path[:-4] + ".json"
        meta = load_json(mp, {})
        s = simpledialog.askstring("Note", "A note for this image (what was on the FM-1):",
                                   initialvalue=meta.get("note", ""), parent=self.root)
        if s is not None:
            meta["note"] = s
            try:
                save_json(mp, meta)
            except OSError as e:
                messagebox.showerror("FM-1 Manager", f"Could not write the note: {e}")
            self.refresh_flash()

    def flash_delete(self):
        path = self.selected(self.fl, self.images, "an image")
        if path and messagebox.askyesno("FM-1 Manager", f"Delete {os.path.basename(path)}? It can't be undone."):
            for p in (path, path[:-4] + ".json"):
                try:
                    os.remove(p)
                except OSError:
                    pass
            self.refresh_flash()


def run_tool(argv):
    """FM-1 Manager --tool SCRIPT ARGS...: one of the tools, run by the packaged app in a process of its own"""
    if sys.stdout is None:                   # a windowed app on Windows: the pipe the Manager gave, by its number
        try:
            sys.stdout = io.TextIOWrapper(io.FileIO(1, "w", closefd=False), line_buffering=True)
        except OSError:
            sys.stdout = open(os.devnull, "w")
    if sys.stderr is None:
        sys.stderr = sys.stdout
    script = argv[0]
    sys.argv = [script] + argv[1:]
    try:
        _run(script)
    except SystemExit:
        raise
    except Exception as e:                   # (a MIDI system that won't open, say): one line, not a traceback
        print(f"error: {type(e).__name__}: {e}", flush=True)
        if "ALSA" in str(e):
            print("error: Linux's MIDI (the ALSA sequencer) is not available: sudo modprobe snd-seq", flush=True)
        return 1
    return 0


def _run(script):
    import runpy
    if FROZEN:                               # the tools are packaged as modules (fm1_manager.spec)
        runpy.run_module(script[:-3], run_name="__main__", alter_sys=True)
    else:
        sys.path.insert(0, os.path.dirname(tool_file(script)))
        runpy.run_path(tool_file(script), run_name="__main__")


def main():
    if len(sys.argv) > 2 and sys.argv[1] == "--tool":
        sys.exit(run_tool(sys.argv[2:]))
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
