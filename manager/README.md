# FM-1 Manager

A desktop app for the M-VAVE FM-1: install any firmware (KIST, M-VAVE's own, and the others on fm1-base or
Felucca), put M-VAVE's firmware back, and keep backups. It runs on macOS, Windows and Linux.

## Getting it

Download it from the [latest release](https://github.com/78Hz/kist/releases/latest):

| Computer | File | First start |
|---|---|---|
| Mac with Apple silicon | `FM-1-Manager-macOS-arm64.zip` | Unzip it and move *FM-1 Manager* to Applications. The app isn't signed, so right-click it and choose **Open**, then **Open** again. |
| Mac with Intel | `FM-1-Manager-macOS-x86_64.zip` | Same as above. |
| Windows | `FM-1-Manager-Windows.exe` | If SmartScreen warns you, choose **More info → Run anyway**. |
| Linux (x86-64) | `fm1-manager-linux-x86_64` | `chmod +x fm1-manager-linux-x86_64`, then run it. |

The library and the backups are kept in *Documents/FM-1 Manager*. That folder isn't inside the app, so updating
the app keeps them. The KIST firmware that came with the app is added to the library the first time it starts.

Connect the FM-1 with a USB data cable straight to the computer, not through a hub. Close any other app that
uses MIDI.

## Firmware

- The library is the *firmware* folder. Add `.fwsc` files with **Add firmware…**.
- **Download M-VAVE V16** fetches M-VAVE's own firmware and checks its checksum.
- **Install on FM-1** writes the selected firmware. The FM-1 just has to be switched on as normal; it doesn't
  need to be in update mode. If the firmware running now can back up its saved data, the Manager offers to do
  that first.
- To go back to M-VAVE's firmware, install *M-VAVE stock V16*.
- If an install is cut off, install the same firmware again to finish it.

## Saved data

This backs up a firmware's setup, presets or songs over USB while it runs, and restores them later.

- It works with firmwares built with backup support, such as KIST and Polly. For a firmware it doesn't know,
  the Manager asks for that firmware's four-letter backup magic.
- It does not back up M-VAVE's own firmware. Use Whole flash for that.
- A restore goes back to the firmware that made the backup. The FM-1 asks on its screen first: **SEL** means
  yes, **HOME** means no.

## Whole flash (macOS and Linux)

This is a complete copy of the FM-1's 1 MB flash: firmware, bootloader and everything saved. It is read
through the chip's own update mode:

1. Switch the FM-1 off.
2. Press the button in the Manager and give your computer's password.
3. Hold **REC** and switch the FM-1 on.

The buttons:

- **Back up whole flash** saves an image.
- **Restore image** writes an image back:
  - It first saves the flash as it is now.
  - It writes only the 4 KB sectors that differ, checking each one, then checks the whole flash at the end.
  - It never writes the bootloader, and it refuses if the bootloader doesn't match the image's.
  - By default it restores the firmware area. Tick the box to also restore M-VAVE's data area; only do that
    with an image from the same FM-1.
- **Put M-VAVE firmware back** works even on an FM-1 that won't start. It needs M-VAVE's firmware in the
  library.

Windows would need a different USB driver for this, so these buttons are off there.

## If something goes wrong

A firmware that fails to start twice in a row shows **SAFE MODE**. USB still works then, so install again
from the Firmware tab. If even that fails, use **Whole flash → Put M-VAVE firmware back**. **Copy log** copies
everything the tools said, to send with a report.

## Running it from source

From a clone with its submodules (`git clone --recursive https://github.com/78Hz/kist`), start
`manager/FM-1 Manager.command` (macOS), `manager/FM-1 Manager.bat` (Windows) or `manager/fm1-manager.sh`
(Linux). The first start sets up a Python environment with `requirements.txt`. Python 3 with Tk is needed;
on Linux that means `python3-venv` and `python3-tk`.

To build the app yourself:

```sh
pip install pyinstaller -r manager/requirements.txt
cp build/kist-*.fwsc manager/firmware/   # optional: firmware to come with the app
pyinstaller manager/fm1_manager.spec
```

## How it works

The window runs fm1-base's own tools, unchanged, each in a process of its own:

- `fm1_install.py` installs firmware.
- `fm1_backup.py` backs up and restores saved data.
- `fm1_rescue.py` handles the chip's update mode, used through `flash_tool.py` for whole-flash backup and
  restore.

The packaged app carries the tools inside it and runs them as `FM-1 Manager --tool NAME.py …`.

The installer, saved-data and stock-restore steps are fm1-base's tools, which have been used on real FM-1s.
The Manager's whole-flash restore has so far only been tested against a simulated FM-1.

Licence: MPL-2.0, like the tools it runs.
