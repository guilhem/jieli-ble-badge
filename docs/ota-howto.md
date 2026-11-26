# OTA over BLE — OEM→custom, custom→custom & custom→OEM

How to flash and update firmware on the JieLi AC707N / BR35 e-badge **entirely over BLE**
(no USB, no opening the case), using the `qix` client in [`tools/qix-ble`](../tools/qix-ble).

> ⚠️ **Security research / educational use only** — on hardware you own. See
> [DISCLAIMER.md](../DISCLAIMER.md). A failed OTA can require USB-ISP recovery.

> **E87 only.** The DG01 has no `FD00` service — its OTA is pure RCSP, via `qix rcsp-flash`.

The badge is reflashed **directly over the OEM app-only BLE OTA** (the path the ZRun app uses).
The custom firmware is packaged **fw-custom** — your `app.bin` spliced into an OEM `.ufw`,
which keeps the **OEM uboot** byte-identical — so a single `qix flash --oem` hands the image to the
resident OEM uboot, which rewrites the CODE partition and boots the custom:

```
OEM ──► custom firmware     ( qix flash --oem  ·  Qix FD00, opcode 0xC0  ·  fw-custom )
custom ──► custom           ( qix flash --oem  ·  Qix FD00, opcode 0xC0  ·  fw-custom )
custom ──► OEM              ( jluboottool read ·  dump2ufw.py            ·  qix rcsp-flash, RCSP AE00 )
```

All tooling addresses the badge **by MAC** (constant, lives in the preserved `key_mac`); only the
advertised **name** changes `OEM → custom firmware`.

> **Note — the STAGER chain is legacy / not used here.** An older `qix provision` / `deploy-fw`
> path installed a minimal loader ("STAGER" bridge) exposing raw-flash `0xD0–0xD3` + apply `0xD4`.
> It is **not used** for this build: the custom firmware embeds its resources in the code image,
> so the direct `qix flash` path is sufficient (the loader firmware is no longer shipped). The
> stager subcommands remain in the client only as a legacy option for a custom that must write the
> separate `ui_res` / `virfat` resource partitions (which the app-only `0xC0` path cannot reach).

---

## 1. Prerequisites (read before any flash)

- **Host:** the `qix` client is built on [`bleak`](https://github.com/hbldh/bleak), so the BLE
  transport works on **Linux (BlueZ), macOS (CoreBluetooth) and Windows (WinRT)**; every
  BlueZ/DBus helper in it is gated behind `sys.platform == "linux"` and is a no-op elsewhere.
  **The recipes below are the Linux ones and are what has been validated on hardware** — the
  `rfkill` / `hciconfig` / `qix cleanup` / `QIX_CONNECT_VIA_DBUS=1` steps are BlueZ workarounds
  and are neither needed nor available on macOS/Windows, where bleak's native backend manages
  connection state itself. Install the client: `pip install -e tools/qix-ble`.
- **Third-party crypto (`jltech`):** vendored as pure Python in `tools/ufw-repack/jltech/` —
  nothing to obtain, no `crcmod`. Only `scripts/gen_chipkey_keyfile.py` still needs the external
  `jl-misctools` mirror.
- **Chipkey:** these images embed the device flashing key `0x9847` (E87, PID 1558). A different
  PID needs a different chipkey/`isd_config.ini` — do **not** reuse `0x9847`.
- **Power:** badge battery **≥ 30 %**, or USB connected for the **whole** flash window. A
  brown-out while the code region is being rewritten can brick the unit → USB-ISP recovery.
- **Single connection slot:** the badge accepts **one** BLE connection. Turn the phone's
  Bluetooth **fully off** (closing the ZRun app is not enough).
- **TEST_MODE gate:** do **not** run `qix dump --auth` since the last power-cycle — it sets a
  TEST_MODE flag that makes the badge **silently reject** `REQ_UPDATE`. Power-cycle to clear it.
- **Address by MAC**, never by name (`qix scan` to find it). The name changes across stages; the
  MAC does not.
- **BLE-HID pairing / stale bond:** the badge is a BLE-HID device. Clear any stale host bond
  before flashing and let `qix flash` pair fresh (`QIX_FLASH_PAIR=1`) — full recipe in
  [custom → custom](#3-custom--custom).
- **custom → OEM:** the restore leg needs a **vanilla** `.ufw` built from a dump of *your*
  stock image — see [custom → OEM](#4-custom--oem-restore-the-stock-firmware) and
  [dump-firmware.md](dump-firmware.md). Everything it needs
  (`dump2ufw.py` + its vendored `jltech/` + the shipped `loaderdl-base.ufw`) is already in
  this repo; the wired *dump* additionally needs the `tools/jl-uboot-tool` submodule.

### Select the E87 media image

Build the [E87 media firmware](build-badge-firmware.md) with `--board e87 --ota`.
Set `IMAGE` in your shell to the path printed on its “OTA image” line. The commands
below use that variable for both the first installation and later updates.

```sh
read -r -p "Path to the newly built E87 .ufw: " IMAGE
```

Use the packaged image from `firmware-builds/`. The tracked
`firmware/custom-fw-ac707n.ufw` supplies the bootloader and flash layout for
packaging. Installing that older build gives you the legacy menu. The SDK's
raw `update.ufw` uses a different bootloader and can leave the E87 unable to boot.


---

## 2. OEM → custom

The badge currently runs the **stock OEM** firmware (powered on, advertising). This host's BlueZ
needs a clean adapter + the deterministic connect — the **bare `qix flash` hangs**.

```sh
# 1. Adapter up + clear any zombie connection. A hung/failed attempt leaves a stale link that
#    blocks the next (errors: "Operation already in progress" / "le-connection-abort-by-local"):
sudo rfkill unblock bluetooth && sudo hciconfig hci0 up
qix cleanup <MAC> --remove
qix scan | grep -i <MAC>              # confirm it advertises; caches it for the DBus connect

# 2. Flash. Go STRAIGHT to the real flash — a --probe-only that returns accepted leaves the badge
#    in update-mode (it won't re-advertise cleanly), so don't probe-then-flash. Takes minutes.
QIX_CONNECT_VIA_DBUS=1 QIX_FLASH_PAIR=1 QIX_CONNECT_ATTEMPTS=1 QIX_CONNECT_TIMEOUT=60 \
  qix flash <MAC> "$IMAGE" --oem --req-timeout 20
```

If the connect still fails or hangs, **power-cycle the badge** (single-slot; it wedges after a
failed or half-open connect) and retry from step 1.

- Opcode `0xC0` (Qix FD00). `--oem` runs auth + bootstrap and the slower (~15 s) `REQ_UPDATE`
  timeout the stock firmware needs.
- The fw-custom keeps the OEM uboot, which rewrites the CODE and reboots into the custom
  (advertised name → `EC-BADGE`).
- This delivers the full EC-BADGE build (its UI/resources are embedded in the code image), so no
  separate resource-writing step is needed.

---

## 3. custom → custom

The badge already runs the EC-BADGE custom. Put it on **Settings → Update** (**Ajustes → Actualizar** on older builds) (advertising
`EC-BADGE`, serving the Qix FD00 OTA), then:

```sh
# 1. Adapter up (do NOT use btmgmt on hosts where it hangs; it is not needed):
sudo rfkill unblock bluetooth && sudo hciconfig hci0 up

# 2. Clear any stale host bond, then re-cache the badge with an LE scan.
#    The Update screen serves the OTA cleartext WITHOUT bonding, so a leftover BlueZ LTK makes
#    the badge drop the link on the first write (UART: `LE disconnect reason=0x13`). Use
#    `bluetoothctl remove`, NOT QIX_FORGET_BOND (that wipes the cache -> "Not Connected").
bluetoothctl remove <MAC>
bluetoothctl --timeout 15 scan le | grep -i <MAC>

# 3. Flash (deterministic DBus connect + fresh HID pair). The ~1 MB transfer takes SEVERAL
#    MINUTES — do NOT interrupt it or wrap it in a short timeout.
QIX_CONNECT_VIA_DBUS=1 QIX_FLASH_PAIR=1 QIX_CONNECT_ATTEMPTS=4 QIX_CONNECT_TIMEOUT=40 \
  qix flash <MAC> "$IMAGE" --oem --req-timeout 20
```

This is the exact path validated end-to-end this session. Resume after a link drop works without
re-issuing `0xC0`.

> **The receiver must carry the OEM uboot** (which the fw-custom packaging guarantees, and
> which is preserved across every OTA). A badge that was USB-ISP'd with a raw SDK `make` image
> (native uboot) cannot do the OTA CODE-write and bricks silently at apply — recover by USB-ISP'ing
> the fw-custom `.ufw` code-slice.

---

## 4. custom → OEM (restore the stock firmware)

Goes the other way: the badge runs EC-BADGE custom, and you flash a `.ufw` built from a
**byte-exact dump of *your* stock OEM firmware** back onto it, returning the unit to factory
state. Unlike `qix flash`, this leg rides the **native RCSP OTA** (`qix rcsp-flash`, service
AE00 — served by the custom's own Update screen) with a **vanilla** `.ufw`, no Qix wrapper.
The badge addresses you by MAC, so **use your own dump**, not one taken from another unit.

**Take the dump *before* the first custom flash** — a badge that already runs the custom no
longer contains the original to dump. The wired dump step is [dump-firmware.md](dump-firmware.md);
the turn-around from dump to OTA image is two commands:

```sh
# 1. Dump (wired, USB / UBOOT mode — never over BLE). From tools/jl-uboot-tool:
cd tools/jl-uboot-tool
python3 jluboottool.py "read 0x0 0x400000 oem-dump.bin"          # whole 4 MB, byte-exact

# 2. Repack (back at the repo root): dump.bin → vanilla RCSP .ufw for rcsp-flash
cd ../..
python3 tools/ufw-repack/dump2ufw.py oem-dump.bin oem-restore.ufw
```

`dump2ufw.py` wraps the CODE region (`dump[0:0xFC000]`, verbatim, in its on-flash scrambled
form) into a loader-download `.ufw`, recomputing the flash.bin dcrc + listcrc + hdrcrc. Its
loader rewrites CODE `[0, 0x17E000)` **in place** — it never stages over the resource
partitions (SDFILE `0x17E000` / VIRFAT `0x33E000`), so whatever `ui_res`/`virfat` content was
on the badge (e.g. a lock background) survives. The loader bundle ships prebuilt as
`tools/ufw-repack/loaderdl-base.ufw` — you only supply the dump.

Then flash — the preflight is the same as the [custom → custom](#3-custom--custom) leg
(adapter up, `bluetoothctl remove <MAC>`, re-scan; badge on **Settings → Update** (**Ajustes → Actualizar** on older builds),
advertising `EC-BADGE`) but with `rcsp-flash`:

```sh
sudo rfkill unblock bluetooth && sudo hciconfig hci0 up
bluetoothctl remove <MAC>
bluetoothctl --timeout 15 scan le | grep -i <MAC>

# ~1 MB transfer, takes SEVERAL MINUTES — do NOT interrupt it.
qix rcsp-flash <MAC> oem-restore.ufw
```

On apply the loader rewrites the CODE partition with the stock image and the badge boots it —
advertising under its stock OEM name again. The **MAC** (`key_mac`) lives outside the CODE
span and is preserved, which is why the restore belongs to your unit. Recovery from a restored
OEM is just the [OEM → custom](#2-oem--custom) leg again (`qix flash --oem`), so the round
trip is complete.

> Prefer a pure wired restore (badge on the bench, or partially bricked)? The same dump can be
> written back **over USB-ISP, without OTA** — see the [restore over USB-ISP](dump-firmware.md#4-restore-over-usb-isp-wired-no-ota) section in [dump-firmware.md](dump-firmware.md).

---

## 5. Verify the result

```sh
qix scan                       # advertised name changed to "EC-BADGE"
```
- Visual: the badge boots the new build — press a button to wake and see the lock image, then
  swipe to the LVGL menu.
- The badge disconnects and reboots on its own 5–30 s after the transfer.
- On UART (`/dev/ttyUSB0` @ 115200): `OTA fully received` → `apply VERIFY OK` → `cpu_reset` →
  `[VM UPDATE RECOVER]` (OEM uboot rewrites CODE) → `update module init ok` → boot banner
  `>>>>> BADGE-FWMARK 016 <<<<<` → `menu: build`.

---

## 6. Connection env vars & troubleshooting

Env vars (see [`tools/qix-ble/README.md`](../tools/qix-ble/README.md)):

- `QIX_CONNECT_VIA_DBUS=1` — raw BlueZ `Device1.Connect()` (skips bleak's flaky scan; needs the
  device cached by an LE scan first — the [custom → custom](#3-custom--custom) scan step).
- `QIX_FLASH_PAIR=1` — fresh Just-Works pair on `qix flash` (the badge is BLE-HID; without it the
  link can tear down on the first data write).
- `QIX_CONNECT_ATTEMPTS` (default 4) — connection retry rounds.
- `QIX_BREDR_OFF=1` — pre-flight `btmgmt bredr off` + tighter conn params. **Skip it on hosts
  where `btmgmt` hangs** (recover a wedged adapter with `sudo rfkill unblock bluetooth`); it is
  usually unnecessary.

| Symptom | Cause | Fix |
|---|---|---|
| `REQ_UPDATE` silently rejected / `state=0` | TEST_MODE flag set by a prior `qix dump --auth` | Power-cycle the badge, don't dump before flashing |
| Link drops on first data write, `LE disconnect reason=0x13` | BLE-HID, stale host LTK vs the cleartext Update screen | `bluetoothctl remove <MAC>` + re-scan (custom → custom), let it pair fresh |
| `connect failed … Device1 not found` | device not cached / not advertising | ensure badge on the Update screen, then re-scan |
| `flash OK` + `apply VERIFY OK` + `cpu_reset`, then **silent brick** | receiver carried the SDK-native uboot (bad USB-ISP image) | recover by USB-ISP'ing the fw-custom `.ufw` code-slice (OEM uboot) — **not** a battery issue |
| `UfwInvalid` from `qix flash` | `.ufw` lacks the 27-byte Qix wrapper | `wrap_qix.py wrap` it |
| `rcsp-flash` rejects / no OTA service | badge is not serving the native RCSP Update path | badge must be running the **custom** on **Settings → Update** (**Ajustes → Actualizar** on older builds) (advertising `EC-BADGE`); `rcsp-flash` needs the vanilla `.ufw` from `dump2ufw.py`, not a Qix-wrapped one |
| `dump2ufw.py` errors `dump too small` | dump is shorter than the CODE slot (`0xFC000`) | re-dump the full 4 MB: `jluboottool.py "read 0x0 0x400000 <dump.bin>"` |
| Custom needs its own `ui_res`/`virfat` resources | the `0xC0` path is code-only | (legacy) use the STAGER `provision`/`deploy-fw --resources` path |

---

## 7. Notes

- **No shell script performs the OTA** — flashing is entirely `qix` (this doc). The `scripts/` are
  build / repack / chipkey / setup helpers (see [`scripts/README.md`](../scripts/README.md)); they
  produce the `.ufw`, they do not flash it.
- The Qix OTA (`0xC0`) apply ends in the **resident OEM uboot** rewriting the CODE partition
  (`[0, 0x17E000)` is write-protected otherwise). The MAC (`key_mac`) is preserved across it.
  The custom → OEM leg uses the *native* RCSP loader instead, which rewrites the same
  CODE span in place and likewise leaves `key_mac` and the resource partitions alone.
- Building the firmware is out of scope here — see
  [build-m1-firmware.md](build-m1-firmware.md) and [jieli-sdk-build.md](jieli-sdk-build.md).
