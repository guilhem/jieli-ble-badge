# Display media on the E87 badge

The media firmware displays photos, short looping clips, messages, and animated SVG text on the E87's round screen. It remembers the selected item and the brightness. The screen stays on while charging. On the tested E87, Button 1 opens the menu and Button 2 puts the screen to sleep. Either button wakes it.

The Badge menu shows the battery percentage and updates it once per second while open.

After two seconds without input on a still home screen, the interface checks
for work every 200 ms. The touch controller is sampled every 50 ms and a
detected press wakes the interface early. During a clip, an animation or a
transfer the interface runs at its active cadence. With the screen locked,
the analog button driver signals a confirmed press to wake the interface.

## Send a photo

1. Build and install the [custom E87 firmware](build-badge-firmware.md). When the gallery is empty, the home screen reads “Press Button 1 then choose Receive”.
2. Open the menu and choose **Receive**. The badge shows its Bluetooth name.
3. On Android, open a JPEG in Photos or Files and choose **Share → Bluetooth**. In Ubuntu's Bluetooth settings, select the badge and choose **Send Files**.
4. If prompted, compare the pairing code on both screens and confirm it on each device. On the tested Ubuntu setup, the pairing prompt closed after about ten seconds. If that happens, reopen **Receive**, start the send again and confirm directly on both devices.
5. The badge displays the new item after the transfer completes.

Swipe left or right on the media screen to cycle through saved items. Open **Gallery** to select or delete a file. **Settings** controls brightness. **Tools** contains the additional Bluetooth utilities.

| Media | File to send |
|---|---|
| Photo | Baseline JPEG (`.jpg` or `.jpeg`), up to 16 MiB and 64 MiPixels. The badge applies EXIF orientation, crops the center square, and stores a 144 × 144 image. |
| Message | A UTF-8 `.txt` file up to 512 bytes. The display font includes French accents. Scroll long messages vertically. |
| Animated text | A UTF-8 `.svg` file up to 4 KiB. It contains up to four independently positioned text lines, each with optional horizontal motion and fading. |
| Clip | A silent 360 × 360 AVI with MJPEG video at up to 12 frames per second and a file size of at most 90,000 bytes. |

Receive one file at a time. Each item uses one of the four gallery slots. Free the largest slot to receive another clip. Cancelled or rejected transfers keep previously saved items in the gallery.

Grayscale baseline JPEGs are supported. For color photos, use a baseline YCbCr JPEG with a single interleaved scan and 4:4:4, horizontal 4:2:2 or 4:2:0 sampling. Progressive JPEGs, CMYK files and vertical 4:2:2 sampling are unsupported. If a phone photo is rejected, export a baseline copy using 4:2:0 and share the resulting file. The [example landscape](media/landscape.jpg?raw=true) is a compatible JPEG.

An upgraded badge may show **Prepare gallery?** on the Receive screen. Choose **Erase and prepare** to erase old data in the reserved 220 KiB gallery area and start receiving. This preserves saved gallery items. Choose **Back** to keep the old data. If power is interrupted during preparation, use **Gallery → Prepare storage** to resume.

## Display animated text

1. Download [animated-text.svg](media/animated-text.svg?raw=true). Open it in a browser to see **Zoé** stay in place while the greeting moves and fades. The badge displays the composition on black.
2. On the badge, open **Menu → Receive**. On Android or Ubuntu, find the downloaded file in **Files** and choose **Share → Bluetooth**. On Ubuntu, you can also select the badge in **Settings → Bluetooth → Send Files** and choose the SVG.
3. Select the badge and confirm the pairing code if prompted. The composition opens after the transfer completes and appears as one item in **Gallery**.

The badge accepts a text-only SVG with a `viewBox="0 0 360 360"`. The root may also set the SVG namespace and `width="360" height="360"`. Use one to four direct `<text>` children, up to 128 characters per line after spaces and line breaks collapse. Both fonts cover printable ASCII, Latin-1, and `Œ œ Ÿ – — ‘ ’ “ ” … €` plus the narrow no-break space. Use precomposed accents such as `é`.

| Attribute | Accepted values |
|---|---|
| `x`, `y` | Coordinates from −2048 to 2048 in pixels. `y` positions the baseline. Both default to 0. |
| `text-anchor` | `start` (left, default), `middle` (center), or `end` (right). |
| `font-family`, `font-size` | `DejaVu Sans` or `DejaVu Serif` at 16, 20, or 32 px. The default is DejaVu Sans at 16 px. |
| `fill`, `fill-opacity` | A `#RRGGBB` color and an opacity from 0 to 1. Set a visible color for the black badge screen. SVG defaults are black and fully opaque. |

Each line can have one `<animate attributeName="x">` and one `<animate attributeName="fill-opacity">`. Use `from` and `to`, or a semicolon-separated `values` list with 2 to 16 evenly spaced values. Set `dur` from `1ms` to `86400s`, for example `6s` or `500ms`. The badge interpolates linearly and samples at up to 12 frames per second.

An animation plays once by default. Set `repeatCount="indefinite"` for a loop or `fill="freeze"` to hold the final value after a single play. A single play otherwise restores the original text attribute. Opening the item or waking the display restarts playback. Shapes, CSS, scripts, external resources, and other elements or attributes are rejected.

## Read diagnostics

Open **Settings → Update** on the badge. In Ubuntu's `bluetoothctl`, find the badge with `scan le`, then run the following commands using the address of your badge.

```text
scan off
connect <badge-address>
menu gatt
select-attribute c2e6fd11-e966-1000-8000-bef9c223df6a
read
```

The value is ASCII. It reports the diagnostic format version, saved item count, blank sector counts for the four slots, and flash read errors. The most recent receive result includes the stage, the byte count, and the OBEX response in hexadecimal. `last_receive` is `0` before a result, `1` for a saved item, or a negative [storage](../firmware/badge-media/badge_media_store.h) or [Bluetooth](../firmware/badge-media/badge_opp.h) error code.

`jpeg_stage` and `jpeg_detail` report the last JPEG decoding stage and the decoder result. `jpeg_soi` indicates whether the first two bytes matched the JPEG start marker. It is `-1` before a result, `0` for a mismatch and `1` for a match.

If the regular GATT client disconnects before reading, use the direct ATT client from the Ubuntu BlueZ package with the badge still in **Update**.

```sh
gatttool --device="$BADGE_ADDRESS" --addr-type=public --char-read --handle=0x003c
```

Set `BADGE_ADDRESS` to the address of your badge. The command prints the response as hexadecimal bytes. Exit **Update** to return to the media screen.

For boot and controller logs, connect the board's PA4 debug TX to RX on a 3.3 V USB–UART adapter and connect the grounds. Leave the adapter's TX and power pins disconnected. Use the adapter's device path with `picocom -b 115200 /dev/ttyUSB0`. The firmware exposes UART transmit output only.

To force an E87 restart, release both physical buttons, then hold them together for five seconds. The button scanner handles the gesture even if the menu is frozen. Release both buttons after the restart to re-arm it.

If a redraw, display sleep, or display wake stalls for five seconds, the E87 logs the operation and restarts. Both restart mechanisms are suspended while Update is open or a firmware update is active.

## Prepare a clip

On a computer with Python and Pillow, run this from the repository root with an animated `loop.gif`. Then send `loop.avi` from Bluetooth settings.

```sh
PYTHONPATH=tools/qix-ble python3 - <<'PY'
from pathlib import Path
from qix_ble.image import prepare_video

clip = prepare_video("loop.gif", fps=12, duration=1, jpeg_quality=60)
if len(clip) > 90_000:
    raise SystemExit("Clip exceeds 90,000 bytes. Shorten the duration or lower JPEG quality.")
Path("loop.avi").write_bytes(clip)
PY
```

## Verify on an E87

1. Send a full-size portrait JPEG from Android, then an accented text file and a clip from Ubuntu. Check the portrait orientation, the loop, and the last selected media after reboot. Repeat with a previously paired sender.
2. Send the same example SVG from Ubuntu and Android. Check both 32 px fonts and the scrolling fade, then put the display to sleep and verify that LCD transfers stop. Wake and reboot the badge and check that the saved SVG opens.
3. Cancel an upload on the badge and interrupt another from the sender. Check that the last media is still available. Fill the gallery, delete an item, and retry.
4. On battery power, measure average current with the same still photo at 30% and 50% brightness. Compare each setting with the previous firmware over the same interval after inactivity has settled. Repeat with the display locked, then wake it with each button. Record first-gesture latency, missed presses, video frame rate and file transfer time.
5. Check that charging leaves the media visible and that a firmware update preserves the gallery.
6. Open the Badge menu with the navigation button. Hold that button and check that focus advances once and touch still works. Release it, tap it again, then use the other button to select the focused item. Repeat immediately after waking the display.

With the patched SDK, a host C compiler, Python, and Pillow, run the simulated tests from the repository root.

```sh
BADGE_SDK=tools/community-re/jieli-sdks/e_badge_707_sdk_200 \
BADGE_OPP_SDK=tools/community-re/jieli-sdks/e_badge_707_sdk_200 \
  python3 -m unittest discover -s firmware/badge-media/tests
```

These tests exercise decoding, SVG placement and timing, debounced button input, the receive state machine, and simulated flash. The transfers, installed update, and current draw require a physical E87.
