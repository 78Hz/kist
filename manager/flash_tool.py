#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""FM-1 Manager's whole-flash helper: back up, restore or put stock back through the chip's own
update mode (the mask-ROM "UBOOT"), using fm1-base's tools/fm1_rescue.py for the USB transport,
the flash loader and every check it makes.

  flash_tool.py backup  DIR                    read the whole flash into DIR, then restart the FM-1
  flash_tool.py restore IMAGE DIR [--data]     write IMAGE back (DIR: a fresh backup first)
  flash_tool.py stock   DIR PACKAGE...         M-VAVE's firmware back (fm1_rescue's own flow)

Needs root on macOS and Linux (the system has to give the device up), pyusb and libusb-package.

Restore writes only what fm1_rescue would write and with the same steps: never the protected
head [0, 0x4000), which must already equal the image's (the same bootloader); every changed
4 KiB sector erased, written and read back, highest first; a final full read compared. By
default only the firmware area [0x4000, 0xE0000) is restored, which holds the firmware and what
fm1-base firmwares save. --data also restores [0xE0000, 1 MiB), where M-VAVE's firmware keeps
its own data: only from a backup of this same FM-1.

Lines starting "PROGRESS n" are for the Manager's progress bar.
"""
import argparse
import json
import os
import struct
import sys
import time

_here = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
for _d in (os.path.join(_here, "..", "fm1-base", "bsp", "tools"), os.path.join(_here, "tools")):
    if os.path.isdir(_d):
        sys.path.insert(0, _d)
import fm1_rescue as R  # noqa: E402


def say(*a, **k):
    k.pop("end", None)
    print(*a, flush=True)


def progress(n):
    print(f"PROGRESS {int(n)}", flush=True)


def read_all(dev, start, stop):
    """the flash with progress"""
    out = bytearray()
    step = 0x10000
    for a in range(0, R.FLASH_SIZE, step):
        out += dev.read(a, step)
        progress(start + (stop - start) * (a + step) / R.FLASH_SIZE)
    return bytes(out)


def session(bot, loader):
    dev = R.Uboot(bot)
    vendor, product = dev.inquiry()
    say(f"device: {vendor} {product}")
    if vendor != "WL82":
        raise R.Fail(f"not a WL82 UBOOT ({vendor!r})")
    dev.start_loader(loader)
    key, typ, fid = dev.chip()
    say(f"chip key {key:04X}, flash type {typ}, id {fid:06X}")
    if (key, typ) != (0x980F, 3) or fid not in R.FLASH_IDS:
        raise R.Fail("this is not the FM-1's chip and flash; nothing written")
    return dev, fid


def save_backup(now, fid, outdir, note):
    os.makedirs(outdir, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    path = os.path.join(outdir, f"fm1-flash-{stamp}.bin")
    with open(path, "wb") as f:
        f.write(now)
    meta = {"flash_id": f"{fid:06X}", "time": stamp, "note": note}
    with open(path[:-4] + ".json", "w") as f:
        json.dump(meta, f, indent=1)
    say(f"backup saved: {path}")
    return path


def write_any(dev, addr, data):
    """fm1_rescue's write_sector without its firmware-area ceiling (for --data); the head is still refused"""
    if addr < R.WRITE_MIN or addr >= R.FLASH_SIZE or addr % R.SECTOR or len(data) != R.SECTOR:
        raise R.Fail(f"refusing to write {addr:#x}")
    dev.cmd(R.CMD_ERASE_SECTOR, struct.pack(">I", addr))
    for off in range(0, R.SECTOR, R.IO):
        dev.write_block(R.CMD_WRITE_FLASH, addr + off, data[off:off + R.IO])
    if dev.read(addr, R.SECTOR) != data:
        raise R.Fail(f"sector {addr:#07x} did not verify")


def do_backup(bot, loader, a):
    dev, fid = session(bot, loader)
    say("reading the whole flash ...")
    now = read_all(dev, 0, 100)
    save_backup(now, fid, a.dir, "backup")
    say("restarting the FM-1")
    dev.run_app()
    return 0


def do_restore(bot, loader, a, image, meta):
    dev, fid = session(bot, loader)
    if meta.get("flash_id") and meta["flash_id"] != f"{fid:06X}":
        raise R.Fail(f"the image is from a {meta['flash_id']} flash, this FM-1 has {fid:06X}: not writing")
    say("reading the whole flash (a backup of it as it is now, first) ...")
    now = read_all(dev, 0, 25)
    save_backup(now, fid, a.dir, "before a restore")
    if now[:R.WRITE_MIN] != image[:R.WRITE_MIN]:
        raise R.Fail("the image's protected head (bootloader) differs from this FM-1's: not writing. "
                     "Put the matching M-VAVE firmware back first, or use an image taken since.")
    end = R.FLASH_SIZE if a.data else R.APP_MAX
    diff = [x for x in range(R.WRITE_MIN, end, R.SECTOR) if image[x:x + R.SECTOR] != now[x:x + R.SECTOR]]
    say(f"{len(diff)} of {(end - R.WRITE_MIN) // R.SECTOR} sectors differ")
    for i, x in enumerate(reversed(diff)):       # high to low, as fm1_rescue writes
        for attempt in range(3):
            try:
                if x + R.SECTOR <= R.APP_MAX:
                    dev.write_sector(x, image[x:x + R.SECTOR], R.APP_MAX)
                else:
                    write_any(dev, x, image[x:x + R.SECTOR])
                break
            except R.Fail as e:
                if attempt == 2:
                    raise R.Fail(f"{e}; the FM-1 is still in update mode: run the restore again")
        progress(25 + 60 * (i + 1) / max(len(diff), 1))
    say("final read ...")
    expect = image[:end] + now[end:]
    final = read_all(dev, 85, 100)
    if final != expect:
        bad = [x for x in range(0, R.FLASH_SIZE, R.SECTOR) if final[x:x + R.SECTOR] != expect[x:x + R.SECTOR]]
        raise R.Fail("final read differs in sectors " + " ".join(f"{x:#07x}" for x in bad) + "; run the restore again")
    say("restored. Restarting the FM-1 ...")
    dev.run_app()
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("backup")
    b.add_argument("dir")
    r = sub.add_parser("restore")
    r.add_argument("image")
    r.add_argument("dir")
    r.add_argument("--data", action="store_true")
    s = sub.add_parser("stock")
    s.add_argument("dir")
    s.add_argument("package", nargs="+")
    for p in (b, r, s):
        p.add_argument("--wait", type=float, default=120)
        p.add_argument("--loader")
    a = ap.parse_args()
    try:
        if a.cmd == "restore":
            image = open(a.image, "rb").read()
            if len(image) != R.FLASH_SIZE:
                raise R.Fail(f"{a.image}: not a whole-flash image ({len(image)} bytes, expected {R.FLASH_SIZE})")
            mp = a.image[:-4] + ".json"
            meta = json.load(open(mp)) if os.path.isfile(mp) else {}
        if a.cmd == "stock":
            packages = []
            for path in a.package:
                raw = open(path, "rb").read()
                name = R.STOCK.get(R.hashlib.sha256(raw).hexdigest())
                if name is None:
                    raise R.Fail(f"{path} is not one of M-VAVE's FM-1 packages")
                packages.append((name, R.package_flash_image(raw)))
        loader = R.get_loader(a.loader)
        if hasattr(os, "geteuid") and os.geteuid() != 0:
            raise R.Fail("needs administrator rights (the system has to give the device up)")
        try:
            import usb.core
            import usb.util
        except ImportError:
            raise R.Fail("pyusb is missing: pip install pyusb libusb-package")
        for attempt in range(1, 6):
            bot = R.open_device(usb, a.wait)
            try:
                if a.cmd == "backup":
                    return do_backup(bot, loader, a)
                if a.cmd == "restore":
                    return do_restore(bot, loader, a, image, meta)
                R.rescue(R.Uboot(bot), packages, loader, True, a.dir, say=say)
                return 0
            except (R.Fail, usb.core.USBError) as e:
                say(f"attempt {attempt}: {e}")
                if isinstance(e, R.Fail) and ("not writing" in str(e) or "not the FM-1" in str(e)):
                    return 1
            finally:
                try:
                    usb.util.dispose_resources(bot.dev)
                except Exception:
                    pass
        raise R.Fail("gave up")
    except R.Fail as e:
        say(f"error: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
