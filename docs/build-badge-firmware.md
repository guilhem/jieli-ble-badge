# Build the custom badge firmware from source

Build the E87 media firmware on x86-64 Linux. The build combines the gallery
sources in this repository with board support from a patch to the pinned JieLi
SDK. Setup fetches the SDK directly from JieLi.

---

## TL;DR

```sh
git clone https://github.com/guilhem/jieli-ble-badge
cd jieli-ble-badge

scripts/setup-badge-build.sh --skip-postbuild
scripts/build-badge-fw.sh --board e87 --ota
```

First run is dominated by the 1.1 GB SDK clone — a few minutes on a good link to JieLi's CN
infrastructure, considerably longer on a bad one. After that: **~90 s** for a clean rebuild,
**~5 s** incremental (measured on this repo's reference build).

For the separate DG01 build, run setup without `--skip-postbuild` and use
`scripts/build-badge-fw.sh --board dg01 --ota`.

Install the resulting image over BLE with `qix flash <MAC> <image>.ufw --oem` (E87) or
`qix rcsp-flash <MAC> <image>.ufw` (DG01) — see [ota-howto.md](ota-howto.md). The build
prints the exact command for the board it just built.

---

## 1. Prerequisites

| Need | Why | Check |
|---|---|---|
| Linux x86-64 | The toolchain and post-build tools are Linux x86-64 ELF binaries | `uname -m` |
| `git`, `curl`, `tar`, `sha256sum`, `make`, `python3` | setup + build | — |
| ~3 GB free disk | 1.1 GB SDK checkout + ~1 GB build objects | `df -h .` |
| `sudo`, once | only to create the `/opt/jieli` symlink | — |
| `ulimit -n` ≥ 65536 | the LTO link opens ~1130 files at once | `ulimit -n` |

Everything else (`setup-badge-build.sh`) is fetched automatically.

---

## 2. What the setup does, and where each piece comes from

`scripts/setup-badge-build.sh` is idempotent — re-run it any time; it skips whatever is
already in place.

| # | Piece | Source | Lands in |
|---|---|---|---|
| 1 | **pi32v2 toolchain** (JieLi's clang 4.0.1 fork, target `pi32v2 -mcpu=r3`) | [Pinned JieLi archive](https://jl-update.oss-cn-shenzhen.aliyuncs.com/jieli-linux-toolchains-20250805.1.tar.xz) | `tools/jieli-toolchain/` + `/opt/jieli` symlink |
| 2 | **Vanilla JieLi SDK** | `gitlab.zh-jieli.com/e_badge/e_badge_707_sdk_200`, git submodule pinned at `d016768` | `tools/community-re/jieli-sdks/e_badge_707_sdk_200` |
| 3 | **badge-menu patch** (board and Bluetooth integration) | this repo, `patches/badge/badge-menu.patch` | applied into the SDK worktree |
| 4 | **Native post-build tools** (`isd_download`, `ufw_maker`, `fw_add`, `packres`, `json_to_res`, `fat_comm`, `remove_tailing_zeros`) | `pkgman.jieliapp.com/s/linux-postbuild` (public, no account) | `SDK/tools/linux/` |
| 5 | **Chipkey** `chipkey_9847.bin` | this repo, `tools/chipkey/` | `SDK/cpu/br35/tools/download/watch/` |

### On the SDK submodule

JieLi's GitLab allows **anonymous read-only clone** — no account, no token:

```sh
git ls-remote https://gitlab.zh-jieli.com/e_badge/e_badge_707_sdk_200
# d0167685d032d745d88fe50233302edd46941622  HEAD
```

The submodule is declared `shallow = true`, so `--init` pulls only that one commit
(~1.1 GB checkout instead of the full history). It is also `ignore = dirty`, because the
badge patch is applied *into* the submodule worktree by design — the submodule is
permanently "modified" while you are building, and that is expected.

After pulling a change to `badge-menu.patch`, reverse the patch from the revision
previously built before rerunning setup. For an SDK built from the animated SVG
revision at `258a3e9`, run this from the repository root. Replace that revision
with the one you built if it differs.

```sh
SDK=tools/community-re/jieli-sdks/e_badge_707_sdk_200
git show 258a3e9:patches/badge/badge-menu.patch | git -C "$SDK" apply --reverse &&
  scripts/setup-badge-build.sh --skip-postbuild &&
  scripts/build-badge-fw.sh --board e87 --clean --ota
```

### On the post-build tools

They do **not** ship with the SDK (which carries only the Windows `.exe` versions), and they
are **not** part of the toolchain package either. They live in JieLi's separate
*"下载目录工具（Linux版）" / download-directory tools (Linux)* package. `setup-badge-build.sh`
downloads it and checks its sha256 against the version this repo was validated with
(`jieli-linux-post-build-tools-20260908.1.tar.xz`). JieLi serves a rolling *latest* at that
URL, so a **sha mismatch is a warning, not an error** — it usually just means a newer release.

They are only needed for the **USB** images (`update.ufw`, `jl_isd.fw`). The **BLE OTA** path
needs nothing but `app.bin`, which the compiler produces regardless — see
[Build](#3-build).

### On the chipkey

`chipkey_9847.bin` is the flashing key burned into AC707N-class E87 units, published here as a
research artifact (see [DISCLAIMER.md](../DISCLAIMER.md)). Without it the build still succeeds
but emits a **keyless** image, and a chipkey-burned unit rejects it with a *Key mismatch*.
For a different unit: `scripts/setup-badge-build.sh --chipkey <id>`, reading
`tools/chipkey/chipkey_<id>.bin`, or `--chipkey none` to build keyless on purpose.

---

## 3. Build

```sh
scripts/build-badge-fw.sh [--board e87|dg01] [--clean] [--chipkey <id>] [--ota [PATH]] [--no-menu]
```

It verifies every prerequisite *before* compiling, raises `ulimit -n` itself, and checks the
artifacts afterwards — a build that produced nothing cannot report success.

| Flag | Effect |
|---|---|
| `--board e87\|dg01` | which board to build for (default `e87`). See [Two boards](#two-boards-e87-and-dg01) |
| `--clean` | `make clean` first (~90 s full rebuild vs ~5 s incremental) |
| `--chipkey <id>` | `BADGE_CHIPKEY=<id>` (default `9847` for `e87`, `b165` for `dg01`; `none` to omit) |
| `--ota [PATH]` | also package the fresh build as a flashable BLE OTA image. Default `PATH`: `firmware-builds/custom-fw-ac707n-<utc>.ufw` (e87) / `firmware-builds/custom-fw-dg01-<utc>.ufw` (dg01) |
| `--no-menu` | build the factory watch app instead of the badge menu |

### Two boards: E87 and DG01

The same badge-menu application runs on two different AC707N units. They share the SoC, the
360×360 round display resolution and the 4 MB flash, and **nothing else that matters to the
drivers** — an image built for one does not work on the other:

| | `--board e87` (default) | `--board dg01` |
|---|---|---|
| Unit | E87 / DragonJAR badge | DG01 wristband (`LJ733_MB_V1.1`) |
| Panel driver | `lcd_qspi_st77916_320x385.c`, retargeted 360×360 | `lcd_qspi_badge2_360x360.c` (lc9855-class, `DE`/`DF` paging) |
| Touch | AXS5106L, bit-banged I²C on **PB1/PB2** | CST816D, bit-banged I²C on **PA5/PA6** |
| Buttons | 2 × ADKEY (3 AD windows) | 1 button on **PB7**, read through the PMU `PADC0` path |
| Chipkey | `0x9847` | `0xB165` |
| OTA protocol | Qix (`FD00`, opcode `0xC0`) | pure native RCSP (`AE00`, `E1…E8`) |
| OTA image | Qix-wrapped *fw-custom* | vanilla `.ufw`, app-only, re-keyed |
| Flash command | `qix flash <MAC> <img> --oem` | `qix rcsp-flash <MAC> <img>` |

`--board` sets `BOARD=` on the make line. This selects the matching LCD, touch and button
drivers. The [media gallery and Bluetooth Object Push receiver](badge-media.md) are built
for `e87`. The build script copies their sources from `firmware/badge-media/` into the SDK
before compilation.

Switching boards or using `--no-menu` triggers a full rebuild because the SDK's
object rules do not track compiler defines. On Linux, the linker uses the declared
source order. Adding, removing or reordering a source updates the link even when
old objects remain in the build directory.

### The equivalent by hand

```sh
export PATH="/opt/jieli/pi32v2/bin:$PATH"
ulimit -n 65536
SDK=tools/community-re/jieli-sdks/e_badge_707_sdk_200/SDK
cp firmware/badge-media/*.[ch] "$SDK/apps/common/ui/lvgl_v8/"
cd "$SDK"
make clean && BADGE_CHIPKEY=9847 make MENU=1 BOARD=e87      # or BOARD=dg01
```

`MENU=1` adds `-DCONFIG_BADGE_MENU`, which is the single define that selects the badge-menu
application (LVGL touch UI) instead of the factory watch boot test. The firmware-internal
detail of what that define switches on is in the SDK's own `docs/BUILD.md` (created by the
patch).

---

## 4. What you get

| File | Size (this build) | What it is |
|---|---|---|
| `SDK/cpu/br35/tools/app.bin` | 944,320 B for the E87 media build | The raw application image used by the E87 BLE OTA repacker. |
| `SDK/output/update.ufw` | ~2.27 MB | Native update image — **USB flashing only**. Carries the *native* uboot, so it is **not** OTA-flashable. |
| `SDK/output/jl_isd.fw` / `jl_isd.bin` | ~1.24 MB / ~1.03 MB | Full flash image for USB-ISP |
| `firmware-builds/custom-fw-ac707n-<utc>.ufw` | 1 079 363 B | With `--ota`: the **BLE-flashable** image |

### Why the OTA image is repacked, not built

The badge's OEM BLE OTA hands the image to the **resident OEM uboot**, which is what rewrites
the CODE partition. An SDK-built `update.ufw` carries the *native* uboot instead and the badge
bricks silently at apply. So the OTA image is built by splicing your fresh `app.bin` into an
existing image that already carries the OEM bootloader, flash geometry and Qix
wrapper. The build script does this with `--ota` and prints the packaged image
path. Use the file under `firmware-builds/` for installation. Full
protocol details in [ota-howto.md](ota-howto.md).

### E87 media footprint

With SDK `d0167685` and the pinned pi32v2 toolchain, the application went from
991,232 bytes at `258a3e9` to 944,320 bytes. This leaves **51,296 bytes** in
the 995,616-byte OEM application slot. The OTA file still occupies 1,079,363
bytes because the repacker pads the slot. These are host builds.

The first optimization pass produced a 963,584-byte image. Each comparison
below restores one component setting in that build. Shared code and linker
alignment affect the measured differences.
In the patched SDK, the E87 switches for health and FlashDB are in
`SDK/apps/watch/board/br35/board_ac707n_demo/board_ac707n_demo_cfg.h`. The ANCS
and audio player switches are in `SDK/apps/watch/include/app_config.h`. Use the
manual build above with `BOARD=e87 MENU=1` and compare `app.bin` sizes.

| Component | Extra application bytes if restored | E87 media configuration |
|---|---:|---|
| FlashDB | 12,832 | Skip the watch database. Health and ANCS are disabled, as are the watch's small-file RCSP service and hands-free calling. The data reservation stays at `0x240000` (160 KiB). The gallery uses a separate 220 KiB area and saves preferences through `syscfg`. |
| Uncompressed fonts | 4,576 | Compress the two DejaVu bitmap tables with LVGL's native format. |
| Sport/health and IMU | 3,008 | Omit the health manager, its one-second timer and its algorithms. All IMU drivers for this board were already disabled. |
| Bluetooth audio players | 2,528 | Disable A2DP music and eSCO call playback. Classic Bluetooth carries Object Push transfers. The DAC and system tone player remain available for low-battery and other alerts. |
| Apple Notification Center Service (ANCS) | 1,088 | Omit watch notification handling and its 1,024-byte buffer. The Bluetooth bridge keeps its existing iOS pairing settings. |
| Eager menu construction | 192 | Allocate the scanner's 12 buttons and labels on first use and omit the unused placeholder screen. |

Successive E87 builds measured the changes to the home screen, logging and input handling.

| Change | Application bytes | Difference from preceding build |
|---|---:|---:|
| Text prompt for an empty gallery, embedded JPEG removed | 959,040 | −4,544 |
| Lossless compression of the 14 px Montserrat menu font | 956,768 | −2,272 |
| Disable nine routine driver and Bluetooth controller log channels | 943,744 | −13,024 |
| Wake the idle UI on input | 944,320 | +576 |

The menu font test covers its 157 glyphs and all kerning pairs. The logging
change uses the SDK's per-channel switches. Both logging builds contain the
same 273 assertion messages and all 112 identified update diagnostics.

These input changes suspend the UI task at the OS level. The E87 uses the
existing LDO configuration with PMU deep sleep disabled. The board history
records a boot hang with DCDC15 and deep sleep mode 2 enabled together.
Battery-current comparisons require a physical badge.

The RTC device, its wake callback and the application alarm handler remain
active because a hardware alarm can select RTC mode. The media profile also
retains its panel and touch drivers, BLE HID, the OTA service, JPEG/MJPEG
decoding and SVG/text rendering.

The two DejaVu bitmap tables use LVGL's existing decoder and occupy 13,234 bytes
instead of 17,838. The host font test checks the metrics and visible pixels at
16, 20 and 32 px. On blank gallery storage the first preparation check reads
225,296 bytes. Caching the result costs 96 application bytes and reduces the
next check to zero flash reads. An import still checks ownership and scans an
unowned slot in full before programming it.

On the same Linux host, packaging the same application took a median of 0.22 s
with the standard-library CRC routines, down from 1.81 s over three runs each.
Both repackers produced identical OTA files.

After SDK setup, run the host checks with Python 3, Pillow and a native C
compiler. Setting `BADGE_OPP_SDK` includes the SDK disconnect/reconnect tests.

```sh
BADGE_OPP_SDK=tools/community-re/jieli-sdks/e_badge_707_sdk_200 \
  python3 -B -m unittest discover -s firmware/badge-media/tests
python3 -B scripts/tests/test_badge_link.py
python3 -B tools/ufw-repack/test_repack_crc.py
```

---

## 5. Troubleshooting

Real failures, with the exact message you will see.

### `error: pathspec 'tools/community-re/...' did not match any file(s) known to git`

`git submodule update --init <path>` on a clone where the submodule gitlink is missing from
the index. Fixed in this repo; if you hit it on an old checkout, `git pull` and retry.

### `fatal: could not read Username for 'https://gitlab.zh-jieli.com'`

The clone was interrupted and git fell back to prompting. Anonymous read *does* work — retry.
It is a large, slow clone from CN infrastructure (~1.1 GB, tens of minutes on a bad link):

```sh
git submodule update --init --depth 1 tools/community-re/jieli-sdks/e_badge_707_sdk_200
```

### `file not recognized: File format not recognized` on some arbitrary `.o`

**`ulimit -n` is too low** — the classic one. The final link opens ~1130 objects and libraries
at once; past the descriptor limit `open()` fails and BFD misreports it as a bad file format.
The named `.o` is fine.

```sh
ulimit -n 65536      # 1024 is a common default
```

`env -i` does **not** reset ulimits, so a "clean" environment still carries the low one.
`build-badge-fw.sh` raises it for you; if the *hard* limit is also low, raise it in
`/etc/security/limits.conf` and re-login.

### `download.sh: line NN: .../SDK/tools/linux/isd_download: No such file or directory`

The native post-build tools are missing. **Historically this failed silently** — `make` still
exited `0`, no `.ufw` was produced, and the build looked successful. It now prints a loud
block and tells you `app.bin` was still built (which is all the BLE OTA path needs). To get
the tools: `scripts/setup-badge-build.sh`.

### `download.sh: using chipkey none`

No `chipkey_*.bin` in `SDK/cpu/br35/tools/download/watch/`. The build completes but the
packaged image is **keyless** and a chipkey-burned unit rejects it. See
[On the chipkey](#on-the-chipkey).

### `Device Offline` at the end of the build

**Not an error.** `isd_download` always finishes by trying a real USB flash and exits **245**
when no badge is attached — *after* it has already written `update.ufw` and `jl_isd.fw`. The
build judges the run by the images, not by that exit code.

### `badge-menu.patch neither applies nor is applied`

The SDK worktree is dirty (a half-applied patch, or your own edits). Reset it:

```sh
SDK=tools/community-re/jieli-sdks/e_badge_707_sdk_200
git -C "$SDK" checkout . && git -C "$SDK" clean -fd
scripts/setup-badge-build.sh
```

### `FAIL R2 STAGER: built without STAGER=1`

From `SDK/tools/check_custom_fw.py`, and **stale**: it predates the direct
`qix flash --oem` path and still insists on the legacy STAGER loader chain, which this
firmware no longer uses. The build only runs that checker under `make STAGER=1`, so you will
only see it if you invoke it by hand. Ignore the R2 line; R1 (app fits the OEM slot) and R3
(touch controller) are still meaningful.

---

## 6. Modifying the firmware

Edit the media sources in `firmware/badge-media/`. For changes to the SDK integration,
edit the SDK worktree and regenerate the patch:

```sh
SDK=tools/community-re/jieli-sdks/e_badge_707_sdk_200
cd "$SDK"
git add -A -- ':!SDK/tools/linux' ':!output' ':!SDK/output'
git diff --cached -- ':!*.a' ':!*.bin' ':!SDK/tools/linux/**' ':!*.cbp' ':!**/*.bat' \
  ':!**/json.txt' ':!SDK/build/fileList.dumy' ':!output/**' ':!SDK/output/**' \
  ':!SDK/apps/common/ui/lvgl_v8/badge_media_*' ':!SDK/apps/common/ui/lvgl_v8/badge_opp.*' \
  > ../../../../patches/badge/badge-menu.patch
git reset
```

The exclusions keep vendor binaries, Windows-only files and build artifacts out: the patch
stays **text-only and reviewable**. `SDK/build/fileList.dumy` in particular is regenerated by
`pre_build` on every build and is read by nothing — the source list that actually drives the
compile comes from `SDK/build/genFileList.c`, which *is* in the patch.

Vendor `.a` libraries are used **as shipped** — the build needs no binary library patch.

---

## 7. Per-board peripherals

Both boards share `board_ac707n_demo.c`. Everything board-specific hangs off
`TCFG_LCD_SPI_BADGE2_ENABLE`, which `--board dg01` sets (see
`board_ac707n_demo_cfg.h`).

### Touch panel

| | badge 1 (E87) | badge 2 (DG01) |
|---|---|---|
| Controller | AXS5106L (AiXin) | CST816D (Hynitron) |
| 7-bit address | `0x63` | `0x15` |
| Chip-ID register | `0x08` (2 B) | `0xA7` (1 B) — reads **`0xB6`** [M] |
| SDA / SCL | PB1 / PB2 | PA5 / PA6 (the HW-iic0 pins, bit-banged) |
| RST / INT | PA2 / PB8 | PA2 / PB8 |

Both are driven by the same software-I²C primitives and selected at compile time by
`badge_tp_init` / `badge_tp_read`. The driver **polls** (`axs_poll_task`); the INT line
is configured as an input but never used as an interrupt source.

> **The CST816D register base is not the AXS5106L's.** [M] This was wrong until
> 2026-09-17: the burst read started at register `0x01` but kept the AXS byte indices,
> which are relative to `0x00`, so every field was off by one register. `d[1]` — meant
> to be the finger count — was actually the *top nibble of X*, so a touch only ever
> registered while X happened to land in 256..511, and the coordinates it then reported
> were two unrelated register halves spliced together. The CST816x map is:
>
> | Reg | Field |
> |---|---|
> | `0x00` | GestureID (`01` up, `02` down, `03` left, `04` right, `05` click, `0B` double, `0C` long) |
> | `0x01` | FingerNum (low nibble) |
> | `0x02` | bits[7:6] event (0 down, 1 up, 2 contact), bits[3:0] X[11:8] |
> | `0x03` | X[7:0] |
> | `0x04` | bits[3:0] Y[11:8] |
> | `0x05` | Y[7:0] |
>
> The fix is the burst base: `CST_REG_TOUCH 0x00`.

A polled driver also has to stop the panel sleeping: the CST816x auto-sleeps after a
couple of idle seconds and then stops ACKing until a touch or a reset wakes it, which
makes touch come and go. `cst816d_init()` now writes `0xFE = 0x01` (DisAutoSleep) after
the ID probe; a failure there is logged, not fatal.

> **Confirmed on hardware 2026-09-18.** The DG01's probe prints
> `tp: CST816D on PA5/PA6, id=0xb6` on the UART, so both the controller and the pin pair
> are as assumed. The burst base is `0x01` — an earlier edit changed it to `0x00` on the
> strength of the generic CST816S datasheet and that was wrong; the authority is the
> vendor's own driver in this tree, `cst816d.c:899`, which reads from `0x02` with a 5-byte
> burst, putting `FingerNum` at register `0x02` rather than `0x01`.
>
> Orientation **is** transformed: touch tracks the display rotated 180°, corrected in
> `badge_tp_read_any()` (both axes inverted; the panel is square so no swap). [M]

### Battery gauge

`vbat_check()`'s discharge branch only ever *decremented* `cur_battery_percent`, making
the gauge a one-way downward ratchet: once seeded low it could never climb back on
battery power, however healthy the cell. It gets seeded low by a stale SOC restored
from VM (`bat_info_need_recheck()` returns the stored percent and discards the
measurement when the two are within 10 %), or by a first sample taken while the
backlight is at full and the cell sags — and the DG01 now boots with the display **on**.
Measured on the DG01 2026-09-17: stuck at 7 %, which on the curve is ~3542 mV.

The curve itself is not the difference: it is **byte-identical** to the OEM's (12 points
at `cfg_tool.bin+0x50`, 3300 mV @ 0 % → 4120 mV @ 100 %, as
`union battery_data {u8 rsv; u16 mV; u8 pct}`), and `config.dat` differs from the OEM's
in 7 bytes of 352, all of them CRCs. Both firmwares convert the same way:
`adc_get_voltage(AD_CH_PMU_VBAT_4) * 4`, interpolated on that curve.

Note how steep the curve is: **30 mV per 10 %** between 20 % and 50 %. A ~250 mV error
moves the reading from 40 % to 7 %, so the percentage is useless for telling a flat cell
from a mis-scaled ADC — which is why the raw mV is now shown next to it on the warning
and charge screens.

The BLE tool gate is now on **voltage**, not percentage, since the voltage has no such
memory: 3770 mV on the E87 (exactly the old 30 % point, so behaviour is unchanged there)
and provisionally 3400 mV on the DG01 until its vbat is checked against a meter.

> **The gate also guards OTA** (`badge_ble_battery_gate()` is called from the update
> entry). A gauge stuck low therefore locks the radio *and* locks over-the-air updates,
> with the charge screen owning the UI whenever USB is plugged in. The way out on a
> locked unit is to charge until the existing charge-branch snap-up lifts the gauge past
> the gate, then unplug.

## See also

- [ota-howto.md](ota-howto.md) — flashing the result over BLE (`qix flash --oem`)
- [lock-image.md](lock-image.md) — updating the lock-screen wallpaper without a reflash
- [patches/badge/README.md](../patches/badge/README.md) — what the patch contains
- [jieli-sdk-build.md](jieli-sdk-build.md) — the older wine+docker flow for the *vanilla* SDK
