# Ardosia

A dedicated writing firmware for the **Xteink X4** e-paper device. Pairs with any **Bluetooth LE (BLE)** keyboard and saves notes to MicroSD.

Ardosia is a fork of [MicroSlate](https://github.com/Josh-writes/microslate-firmware)
for the Xteink X4, adding support for USB-locked devices (recovery through the
Escape Hatch), Brazilian Portuguese input (ABNT2, dead keys, UTF-8) and
customisable sleep screens.

## Features

- **Bluetooth Keyboard** — BLE HID host, connects to any standard wireless keyboard. Stores up to 4 keyboards; auto-cycles through them on reconnect. Tested with Logitech Keys-To-Go 2 and Keychron K3.
- **Note Management** — browse, create, rename, and delete notes from an SD card
- **Named Notes** — each note has a title stored in the file; shown in the browser and editable without touching body text
- **Text Editor** — cursor navigation, word-wrap, fast e-paper refresh
- **Writing Modes** — three display modes to suit different writing styles:
  - *Scroll* — standard scrolling editor (default)
  - *Typewriter* — shows only the current line centered on a blank screen. Focused, distraction-free single-line writing
  - *Pagination* — page-based display instead of scrolling. Clean page flips instead of per-line scroll refreshes
- **Auto-Save** — content is silently saved to SD card after 10 seconds of idle or every 2 minutes during continuous typing; no manual save required. Every exit path (back button, Esc, power button, sleep, restart) also saves automatically
- **Safe Writes** — saves use a write-verify + `.bak` rotation pattern; a failed or interrupted write never destroys the previous version. Orphaned files from a crash are recovered automatically on next boot
- **Clean Mode** — hides all UI chrome while editing so only your text is on screen (Ctrl+Z to toggle)
- **Dark Mode** — inverted display
- **Display Orientation** — portrait, landscape, and inverted variants
- **Power Management** — ESP-IDF light sleep between loop iterations (CPU drops to 10MHz), BLE modem sleep keeps the radio alive, SD card sleeps between accesses, display analog circuits power down after each refresh, and the device enters deep sleep after 5 minutes of inactivity
- **WiFi Sync** — *the Sync entry is currently hidden from the main menu; Hotspot is not. See the WiFi Sync section.* One-button backup of all notes to your PC over WiFi. Saves network credentials for instant reconnect. The device runs an HTTP server and the PC pulls from it; downloads are read-only, but a `POST /notes` route can append to a note while sync or the hotspot is up
- **Standalone Build** — all libraries are bundled in the repo; no sibling projects required
- **Dual-Boot** — optional combined firmware that includes CrossPoint (an e-reader) in a second OTA slot. A "CrossPoint" entry appears in the main menu; selecting it reboots into the reader. CrossPoint gains a reciprocal "Ardosia" entry. Both apps work normally when flashed standalone.
- **Settings Backup** — BLE pairing info, WiFi credentials, and UI preferences are backed up to the SD card as JSON files. They are silently restored after a firmware flash so you don't need to re-pair your keyboard or re-enter WiFi passwords.

## Hardware Requirements

- Xteink X4 e-paper device (ESP32-C3, 800x480 display, physical buttons, SD slot)
- MicroSD card formatted as FAT32
- A **Bluetooth LE (BLE)** HID keyboard — confirm your keyboard uses BLE before pairing. The ESP32-C3 hardware has no Classic Bluetooth (BR/EDR) radio; Classic BT keyboards cannot connect regardless of firmware settings.

## Installation

Requires a Windows or Linux x86_64 machine (the ESP-IDF toolchain does not support Mac ARM or Raspberry Pi).

**Prerequisites**

- [PlatformIO](https://platformio.org/install/) (CLI or VS Code extension)

```bash
# Clone the repository
git clone https://github.com/Leticia-NdO/ardosia-firmware
cd ardosia-firmware

# Build
pio run
```

Ardosia targets units whose `serial download` is disabled by eFuse, so it is
flashed **from the SD card**, not over USB:

1. Copy `.pio/build/xteink_x4/firmware.bin` to the card under a descriptive name
   (**not** `update.bin` — the boot combo flashes that path unconditionally)
2. Hold **Back + Up** while powering on to reach the Escape Hatch
3. Choose *Flash Firmware* and select the `.bin`

Upstream MicroSlate also offers a browser installer at
[typeslate.com/tools/microslate](https://typeslate.com/tools/microslate/). It
installs **upstream MicroSlate, not Ardosia**, and it needs a device that still
accepts USB flashing.

All libraries are included in the `lib/` directory. The only external dependency fetched automatically by PlatformIO is **esp-nimble-cpp** (BLE stack).

### First Boot

1. Insert a FAT32-formatted MicroSD card
2. Power on the device — it boots to the main menu
3. Go to **Settings → Bluetooth** and scan for your keyboard
4. Select your keyboard from the list and press Enter to pair
5. Return to the main menu and start writing

The device remembers paired keyboards (up to 4) and reconnects automatically on subsequent boots. If multiple keyboards are stored, it cycles through them until one responds.

## Usage

### Main Menu

| Key | Action |
|-----|--------|
| Up / Down | Navigate |
| Left / Right | Also navigate (convenient in landscape) |
| 1 – 9, 0 | Move the selector to that numbered entry; 0 is the tenth (keyboard only) |
| Ctrl+N | Rename the entry (dual-boot apps only) |
| Enter | Select |

Options: **Browse Notes**, **New Note**, **Settings**, **Hotspot** — and **CrossPoint** if the dual-boot firmware is installed

**Sync** is built but hidden from the menu for now (`MENU_SHOW_SYNC` in `src/config.h`). It is listed last when shown, so toggling it never renumbers the entries above.

A dual-boot entry can be renamed: select it and press **Ctrl+N**. The name is
kept in shared NVS keyed by OTA slot, so the other app reads the same name and
it survives reflashing this one. Confirming an **empty** name clears the
override and the generic `OTA Slot N` comes back — that is the way out of a name
you regret. Names are cut to 31 bytes, on a character boundary.

Each entry is numbered on screen (`[3]  Settings`). Typing its digit on a
connected BLE keyboard moves the selector there and stops — it does not open
the entry. Press Enter to do that. Some entries are one-way — CrossPoint reboots
into the reader — so a mistyped digit should not be able to commit to one.

### File Browser

| Key | Action |
|-----|--------|
| Up / Down | Navigate list |
| Left / Right | Also navigate (convenient in landscape) |
| Enter | Open note |
| Ctrl+N | Edit title of selected note |
| Ctrl+D | Delete selected note (confirmation required) |
| Ctrl+K, or **hold select** | Quick menu for the selected note |
| Esc | Back to main menu |

Each note shows its word count on the right. The count is recorded when the note
is saved and kept in the card's note index, so opening the browser never has to
read the notes themselves. A note that has not been saved since the count
existed — one restored from a backup, or posted over sync — shows **—** until
the next time it is saved.

**Holding the select button** for half a second on a note opens a quick menu —
Rename, Delete, Cancel. It exists because the two actions above are Ctrl chords
and the case has no Ctrl key: without a keyboard there was no way to rename or
delete a note at all.

Because the button now carries two gestures, a *tap* fires when you let go
rather than when you press. On this screen and in the editor only.

Deleting asks first. A box opens over the list naming the note, with **Cancel**
and **Delete** — and it opens on Cancel, so pressing Enter again destroys
nothing. Left/Up and Right/Down move between the two buttons, Enter chooses,
Esc cancels. While the box is up it is the only thing that sees a key: typing
does not go on filtering the list underneath it, and no stray key dismisses it.

It deletes the note it named when it opened, not whatever the selector happens
to be on by then.

### Text Editor

| Key | Action |
|-----|--------|
| Arrow keys | Move cursor |
| Home / End | Start / end of line |
| Backspace / Delete | Remove characters |
| Tab | Cycle writing mode (Scroll → Typewriter → Pagination) |
| Ctrl+S | Save manually |
| Ctrl+N | Edit note title |
| Ctrl+Z | Toggle clean mode (hides UI chrome) |
| Ctrl+T | Toggle Typewriter mode |
| Ctrl+P | Toggle Pagination mode |
| Ctrl+Left / Right | Jump pages (Pagination mode only) |
| Ctrl+K, or **hold select** | Quick menu |
| Esc / Back button | Save and return to file browser |

**Hold the select button** for half a second (or press Ctrl+K) to open the quick
menu over the page:

| Row | What it does |
|-----|--------------|
| Font Size | Small → Medium → Large |
| Typeface | Sans / Mono |
| Dark Mode | Light / Dark |
| Writing Mode | Normal → Typewriter → Pagination |
| Rename | Opens the title editor |
| Delete | Deletes this note — asks first |
| Cancel | |

The four at the top show their current value and **leave the menu open**, so you
can cycle a setting and see what it landed on; the page is behind the box, and
the row is the only place the new value is legible. The bottom three close it.

This is the only way to reach any of that without a keyboard — every editor
shortcut is a Ctrl chord, and the case has no Ctrl key. It is also the only
place that shows what the font, mode and theme currently *are*: the chords
change them blind.

Deleting the open note clears the editor before it removes the file. That order
matters — auto-save runs on a 10-second idle and both Esc and the power button
save on the way out, so a note deleted with the buffer still armed would write
itself straight back onto the card.

The current writing mode is shown in the header: **[S]** Scroll, **[T]** Typewriter, **[P]** Pagination.

Auto-save runs silently after 10 seconds of idle or every 2 minutes during continuous typing — Ctrl+S is only needed if you want to save immediately.

### Writing Modes

**Scroll [S]** — Standard scrolling editor. Text scrolls as the cursor moves down the page.

**Typewriter [T]** — Only the current line is shown, centered vertically on a blank screen. When you press Enter, the previous line disappears and a fresh line appears. Text is still saved to the buffer normally. Combine with Clean Mode (Ctrl+Z) for a completely minimal writing experience.

**Pagination [P]** — Instead of scrolling when text fills the screen, the display flips to a new blank page. The current page is shown in the header (e.g. "Pg 1/3"). Use Ctrl+Left and Ctrl+Right to jump between pages. Eliminates per-line scroll refreshes — only one refresh per page transition.

### Title Edit

Accessed via Ctrl+N from the file browser or editor.

| Key | Action |
|-----|--------|
| Type | Enter title text |
| Backspace | Delete last character |
| Enter | Confirm |
| Esc | Cancel |

### Settings

Settings is in three tabs — **System**, **Editor**, **Controls**.

| Key | Action |
|-----|--------|
| Up / Down | Move between the rows of the open tab |
| Left / Right | Previous / next tab |
| F1 / F2 / F3 | Straight to System / Editor / Controls |
| Tab | Next tab |
| Enter | Change the selected setting |
| Shift+Enter | Change it the other way |
| 1–9 | Jump to that row **of the open tab**, without changing it |
| Esc | Back to the main menu |

This is **the one list screen where Left and Right do not double as Up and
Down.** The screen has two axes now and the case has four direction buttons and
no modifier key, so the arrows are the only way to reach the tabs without a
keyboard. Stepping a value backward moved to Shift+Enter.

Moving to a tab always lands on its first row — the same key lands in the same
place every time, rather than wherever you left that tab.

Tabs paid for two things at once. No tab holds more than four rows, so nothing
scrolls in landscape any more; and the digits restart inside each tab, so
**every** row has a number again — the flat list had eleven rows against ten
digits, and D-Pad Keys had to go without one.

In portrait the three tabs are within a pixel of filling the width, so the bar
measures itself and tightens its padding there. It never drops a tab.

#### System

| Setting | Values |
|---------|--------|
| Dark Mode | Light / Dark |
| Orientation | Portrait, Landscape CW, Inverted, Landscape CCW |
| Sleep Screen | Text, Slideshow, Shuffle |

Dark Mode is here rather than under Editor because it inverts every screen, not
just the page you type on.

#### Editor

| Setting | Values |
|---------|--------|
| Editor Font | Sans / Mono — the typeface of the note body only; menus are always monospace |
| Font Size | Small, Medium, Large |
| Writing Mode | Normal, Typewriter, Pagination |
| Note Order | A-Z, Z-A, Newest, Oldest |

Font Size and Writing Mode sit below Editor Font despite being adjusted more,
because **Ctrl+F**, **Ctrl+T** and **Ctrl+P** already reach them from the editor
— Settings is their fallback, not their front door. Note Order orders the notes
list rather than the editor; it is here because writing is what it belongs to,
and three rows do not justify a fourth tab.

#### Controls

| Setting | Values |
|---------|--------|
| Bluetooth | Opens Bluetooth scan to pair a new keyboard |
| Paired Keyboards | Manage saved keyboards (connect, forget, disconnect) |
| Keyboard | US, US-Intl, ABNT2 |
| D-Pad Keys | Standard / Natural — see below |

**D-Pad Keys** decides what the four direction buttons mean once the screen is
rotated. **Standard** is the original behaviour: a button means what is printed
on it, and on list screens Right also scrolls up and Left also scrolls down.
**Natural** rotates them with the screen, so the button that *points* up the
page is the one that scrolls up. In portrait the two are identical.

Natural is set for the case held with its **bottom edge to the right** and the
side keys above — verified on the device, not derived. If you hold it the other
way round, the two landscape cases in `src/dpad.cpp` are what to exchange; the
file says so at the top.

Switching **Editor Font** re-wraps the text: monospace gives every character the
widest slot, so fewer characters fit on a line at the same Font Size.

All settings persist across reboots.

### Paired Keyboards

Shows all keyboards saved on the device (up to 4). The currently active keyboard is labelled **active**; the last used keyboard when none is connected is labelled **last**.

| Key | Action |
|-----|--------|
| Up / Down | Navigate list |
| Enter | Switch to selected keyboard |
| D | Forget selected keyboard — asks first |
| Left | Disconnect selected keyboard (if currently active) |
| Esc | Back to Settings |

Forgetting asks before it acts, in the same box the notes list uses, opened on
Cancel. It used to happen on the spot, on a bare letter key — on the one screen
you are on *because* the keyboard is already misbehaving.

To pair a second keyboard, go to **Settings → Bluetooth**, scan, and connect. Both keyboards will then appear in the Paired Keyboards list. On each boot the device tries the last-used keyboard first, then works through the rest of the list until one connects.

### Bluetooth Settings

| Key | Action |
|-----|--------|
| Up / Down | Navigate device list |
| Enter | Connect to selected device (or start scan if list is empty) |
| Right | Re-scan for devices |
| Left | Disconnect current keyboard |
| Esc | Back to Settings |

A scan runs for 5 seconds and then stops. Up to 10 nearby devices are shown with name, address, and signal strength.

### WiFi Sync

> **The Sync entry is hidden for now**, switched off by `MENU_SHOW_SYNC` in
> `src/config.h`; set it back to `true` to bring it back. **Hotspot is not
> hidden** — it is still on the menu. Everything below still describes how the
> feature works: the firmware side is built and unchanged, only the Sync menu
> row is unreachable.

Back up all notes from the device to your PC over WiFi. The device and PC must be on the **same WiFi network**.

The direction is the opposite of what "sync" usually suggests: **the device is
the server and the PC pulls from it.** The device raises WiFi, announces itself
as `ardosia.local` over mDNS and serves `GET /api/files` and
`GET /notes/<name>`; `sync/ardosia_sync.py` polls for it, downloads anything it
does not already have, then `POST /api/sync-complete` tells the device to shut
the radio off.

#### One-time PC setup

1. Install [Python 3](https://www.python.org/downloads/) if you don't have it
2. Install the required library:
   ```bash
   pip install requests
   ```
3. Set the download folder. `LOCAL_DIR` at the top of `sync/ardosia_sync.py`
   defaults to `~/OneDrive/Documents/Ardosia Notes`, which only makes sense on a
   Windows machine with OneDrive. Point it wherever you want the notes.
4. Register auto-start:

**Windows** — double-click **`sync\install_sync.bat`**. The script starts
immediately and runs silently in the background on every login. To stop
auto-start later, double-click **`sync\uninstall_sync.bat`**.

**macOS / Linux** — there is no installer yet. Run the script yourself:
```bash
python3 sync/ardosia_sync.py
```
It polls for the device every 5 seconds and keeps running, so start it before
you press Sync on the device.

When a sync completes, a desktop notification lists the files that were
downloaded (Windows balloon, macOS notification, or Linux `notify-send`).

#### Syncing

1. Select **Sync** from the main menu on the device (with `MENU_SHOW_SYNC` on)
2. **First time:** pick your WiFi network and enter the password. The device asks to save credentials.
3. **After that:** the device auto-connects — just press Sync and wait
4. The device syncs automatically once connected — a progress log is shown on screen
5. When done, the device shows a summary and turns WiFi off automatically. A desktop notification lists the downloaded files

If the sync script isn't running, you can start it manually:
```bash
python3 sync/ardosia_sync.py
```

#### How sync works

- One-way backup (STA Sync): device → PC. The PC never uploads or deletes.
- Files already on the PC with the same name and size are skipped
- Files deleted from the device are **not** deleted from the PC — they stay as a backup
- **Sync (Hotspot)** raises a WPA2 access point (`Ardosia`) so another device can `POST /notes` and append to a note. POST is accepted only while the hotspot (or STA sync) is up; the path is sanitized and the body is capped.
- WiFi turns off automatically after sync completes, after 60 seconds of no HTTP activity, or — on the hotspot with nobody joined — after 5 minutes

#### Sync controls

| Key | Action |
|-----|--------|
| Up / Down | Navigate network list |
| Enter | Select network / confirm |
| Esc | Cancel / back |

## File Format

Notes are plain `.txt` files stored in `/notes/` on the SD card. Filenames are derived from the note title — spaces become underscores, everything is lowercased, and `.txt` is appended. For example, a note titled "My Note" becomes `my_note.txt`.

Files are fully compatible with any text editor on a computer. To add notes manually, drop `.txt` files into the `/notes/` folder on the SD card — the title shown on the device is derived from the filename.

## Project Structure

```
ardosia-firmware/
├── src/
│   ├── main.cpp          — setup, main loop, shared UI state
│   ├── sd_backup.h       — inline SD/JSON helpers for NVS backup and restore
│   ├── ble_keyboard.cpp  — BLE scanning, pairing, HID report handling
│   ├── input_handler.cpp — keyboard event queue and UI state dispatch
│   ├── text_editor.cpp   — text buffer and cursor management
│   ├── file_manager.cpp  — SD card file operations
│   ├── ui_renderer.cpp   — screen rendering for all UI modes
│   ├── wifi_sync.cpp     — WiFi sync server and state machine
│   └── config.h          — enums, buffer sizes, constants
├── sync/
│   ├── ardosia_sync.py      — PC sync script (Python, cross-platform)
│   ├── install_sync.bat     — register auto-start on Windows login
│   └── uninstall_sync.bat   — remove auto-start on Windows
├── lib/                  — all hardware/display libraries (bundled)
│   ├── GfxRenderer/
│   ├── EpdFont/
│   ├── EInkDisplay/
│   ├── hal/
│   ├── BatteryMonitor/
│   ├── InputManager/
│   ├── SDCardManager/
│   └── Utf8/
└── platformio.ini
```

## Troubleshooting

**Keyboard not showing in scan**
- Make sure the keyboard is in pairing mode and not connected to another device
- Press Right to re-scan after switching the keyboard to pairing mode
- **Classic Bluetooth keyboards will never appear** — the ESP32-C3 only has a BLE radio. This is a hardware constraint, not a software limitation. Verify your keyboard uses BLE before debugging further (check the manufacturer's specs; most keyboards sold after 2014 use BLE, but some older or multi-device keyboards still use Classic Bluetooth)

**Physical buttons not responding**
- BLE scanning can occasionally interfere with the ADC button reads
- Hold the BACK button for 3 seconds to restart the device

**Display appears frozen**
- E-paper refresh takes ~430ms — wait for it to complete before pressing more keys

**Serial monitor shows nothing on startup**
- The ESP32-C3 USB-CDC port re-enumerates after reset; startup logs are sent before the monitor reconnects. This is normal — the device is working correctly.

---

## Credits

Ardosia is a fork of **MicroSlate** by Josh (TypeSlate). The upstream project is
where almost all of this firmware comes from — support it at
[ko-fi.com/typeslate](https://ko-fi.com/typeslate).

- **MicroSlate** — [github.com/Josh-writes/microslate-firmware](https://github.com/Josh-writes/microslate-firmware)
- **TypeSlate** — [typeslate.com](https://typeslate.com), a distraction-free writing app for Windows

`lib/RecoveryBoot/` is vendored from the [FreeInk SDK](https://github.com/Free-Ink/freeink-sdk)
and the bundled `lib/` display, font, input and SD libraries come from
[crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) (MIT).
