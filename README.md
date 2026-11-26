# E87 Media Firmware

Wear a photo, a short looping clip, or a message on the E87’s round display. Send a file from Android or Ubuntu using the Bluetooth share menu. The badge saves it, puts it on screen, and remembers your selection after a restart.

[Install the firmware](#install-on-an-e87) · [Send your first photo](#send-your-first-photo) · [Media and diagnostics guide](docs/badge-media.md)

![Photo framing on the E87. A circle marks the center of a landscape JPEG. The round display shows the sun and peaks inside that circle.](docs/media/photo-framing.svg)

## Send your first photo

With the media firmware installed, try the [landscape JPEG shown above](docs/media/landscape.jpg?raw=true).

1. **On the E87**, press Button 1 and tap **Receive**. Wait for “Send a file by Bluetooth to EC-BADGE”. On first use, **Erase and prepare** clears old data from the 220 KiB reserved for the gallery. Choose **Back** to keep that data.
2. **On Android**, open the downloaded JPEG in Files and select **Share → Bluetooth → EC-BADGE**. **On Ubuntu**, select **EC-BADGE** in Bluetooth settings and choose **Send Files**.
3. If pairing is requested, compare the six digits. When they match, tap **Codes match** on the badge and confirm on the sender. On the tested Ubuntu setup the prompt expired after about ten seconds. Reopen **Receive** and retry if it closes.

The photo appears as soon as the transfer finishes. Swipe left or right to change what you wear. Native sharing has been tested on a physical E87 with a portrait from Android and a tree photo from Ubuntu.

## What fits on the badge

| Send | What appears on screen |
| --- | --- |
| Baseline JPEG, up to 16 MiB and 64 megapixels | The badge applies EXIF orientation and saves a 144 × 144 center crop for its 360 × 360 display. For a group photo, keep faces near the center. |
| UTF-8 text file, up to 512 bytes | A message with support for French accents. Scroll vertically to read longer text. |
| Silent 360 × 360 MJPEG AVI, up to 12 fps and 90,000 bytes | A looping clip. The [clip recipe](docs/badge-media.md#prepare-a-clip) converts an animated GIF to the supported format. |

**Gallery** holds four items. Only the largest slot can hold a clip. **Settings** remembers your brightness. Button 2 puts the display to sleep, and either button wakes it. Open **Settings → Update** for firmware updates and [Bluetooth diagnostics](docs/badge-media.md#read-diagnostics).

## Install on an E87

Check your hardware first. This build targets the **E87 / DragonJAR badge**, with a JieLi AC707N, board marking `qx7613_v1.3`, USB PID 1558 and two buttons. The DG01 wristband uses a separate image.

1. Before replacing the factory firmware, [back up your own badge](docs/dump-firmware.md) so you can restore it. Recovery after an interrupted update may require a wired connection.
2. On **x86-64 Linux**, build from this checkout. Allow at least 3 GB of disk space and follow the [build prerequisites](docs/build-badge-firmware.md#1-prerequisites).

   ```sh
   scripts/setup-badge-build.sh --skip-postbuild
   scripts/build-badge-fw.sh --board e87 --ota
   ```

3. Use the **newly built `.ufw` in `firmware-builds/`**. Keep the badge powered and the phone’s Bluetooth off during installation from Linux. Follow the [Bluetooth installation guide](docs/ota-howto.md) for your starting firmware. After reboot, a badge with an empty gallery shows its built-in image.

## Work on the firmware

The [media sources and host tests](firmware/badge-media) cover file reception, decoding, gallery storage and the on-device interface. The [build guide](docs/build-badge-firmware.md) explains how these sources and the [board integration patch](patches/badge) fit into JieLi’s SDK. The [test instructions](docs/badge-media.md#verify-on-an-e87) cover host checks and verification on a badge.

This firmware builds on the E87 board support and Bluetooth update tooling developed by Andrés Sabas and Heikki Perea at Electronic Cats. [Original project](https://github.com/ElectronicCats/jieli-ble-badge-research) · [MIT license](LICENSE)
