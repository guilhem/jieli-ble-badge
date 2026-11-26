# E87 Media Firmware

Display a photo, a looping clip, or a short message on the E87 badge. Send files with the built-in Bluetooth sharing on Android or Ubuntu. The gallery holds four items and remembers which one to show when you turn the badge on.

![A landscape photo and the center crop shown on the round E87 display.](docs/media/photo-framing.svg)

## Send a photo

1. On the badge, press **Button 1** and tap **Receive**.
2. Send a JPEG of your own or try the [example landscape](docs/media/landscape.jpg?raw=true).

   - **Android** — Open the photo and choose **Share → Bluetooth → EC-BADGE**.
   - **Ubuntu** — In Bluetooth settings, select **EC-BADGE → Send Files** and choose the photo.

3. If asked to pair, check that the codes match and confirm on both devices. The photo appears when the transfer finishes.

Swipe to change the displayed item. The [media guide](docs/badge-media.md) covers text files and short video loops.

## Install on an E87

If your badge still runs the factory firmware, [back it up](docs/dump-firmware.md) first. On x86-64 Linux, follow the [build prerequisites](docs/build-badge-firmware.md#1-prerequisites), then run these commands from the repository root.

```sh
scripts/setup-badge-build.sh --skip-postbuild
scripts/build-badge-fw.sh --board e87 --ota
```

Keep the badge powered and use the new `.ufw` from `firmware-builds/` with the [Bluetooth installation guide](docs/ota-howto.md).

Based on the [E87 board support by Andrés Sabas and Heikki Perea at Electronic Cats](https://github.com/ElectronicCats/jieli-ble-badge-research). [MIT license](LICENSE)
