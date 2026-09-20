# photopainter-jpg

Firmware for the Waveshare PhotoPainter that reads JPEGs straight off the SD card.

The stock firmware only shows 24-bit BMPs in the exact panel palette, so every photo has to go
through Waveshare's converter tool first. That's fine the first time. It gets old fast, and it makes
the frame useless for anyone who won't run a converter before every photo.

This does the converting on the device: put `.jpg` files in `pic/`, done. No WiFi, no app, no cloud —
the RP2040 in this thing has no radio at all, which is the whole reason I picked it.

## Flash it

1. Grab `photopainter-jpg.uf2` from [Releases](../../releases).
2. Hold **BOOT**, plug in USB, release. A drive called `RPI-RP2` shows up.
3. Copy the `.uf2` onto it. The frame reboots on its own.

## Use it

Put your photos in a folder called `pic` on the SD card:

```
pic/
  IMG_0234.jpg
  summer.jpg
  grandpa.bmp
```

Any size, any aspect ratio, straight from a phone. The frame picks the next one every 24 hours,
sorted by filename. BMPs still work like before.

Delete `index.txt` and `fileList.txt` from the card after you change the photos — they're
bookkeeping files the firmware regenerates.

While USB is plugged in the frame stays awake and the **NEXT** button jumps to the next picture.
Handy for checking your photos, and how you'd use it as a desk toy.

## What it does to your photos

- Scales down and center-crops to 800×480, so the picture fills the panel.
- Portrait photos get turned 90°, so they fill the panel too — turn the frame on its side.
  If they end up rotated the wrong way for you, flip `PORTRAIT_CW` in `lib/GUI/GUI_JPGfile.c`.
- Reads EXIF orientation, so phone pictures aren't sideways.
- Floyd-Steinberg dithering against the colours the panel actually produces, not the ideal RGB
  values. Skin tones and skies come out noticeably better than with a naive palette match.

A 12 MP photo takes about 15 seconds to decode, plus the ~33 seconds the panel needs to refresh.
The RP2040 runs at 250 MHz for this.

## Change the interval

In `main.c`:

```c
alarmTime.hours += 24;      // every 24 hours
// alarmTime.minutes += 30; // every 30 minutes
```

Then rebuild.

## Build it yourself

You need the [Raspberry Pi Pico extension](https://marketplace.visualstudio.com/items?itemName=raspberry-pi.raspberry-pi-pico)
for VS Code — it installs the SDK and the ARM toolchain for you. Open the folder, run
**Raspberry Pi Pico: Import Project**, then **Compile**. The `.uf2` lands in `build/`.

From a terminal it's the usual:

```sh
cmake -S . -B build -G Ninja
ninja -C build
```

## Which hardware

The [PhotoPainter](https://www.waveshare.com/photopainter.htm) with the **RP2040** and the 7.3"
7-colour (ACeP / "F") panel.

Not tested on the ESP32-S3 version or the newer Spectra 6 "(B)" model. The B version should be close
— same MCU, different panel driver — but I don't have one to try.

## Limits

- Baseline JPEG only. Progressive JPEGs get a message on the panel instead of garbage. Phone and
  camera photos are baseline; some web downloads aren't.
- No HEIC. Export as JPEG.
- 8.3 filenames are safest. Long names work but FatFs is picky about some characters.

## Credits

Built on Waveshare's [PhotoPainter firmware](https://github.com/waveshareteam/PhotoPainter) and
ChaN's [TJpgDec](http://elm-chan.org/fsw/tjpgd/). Panel colour values borrowed from Pimoroni's Inky
library, which has clearly spent more time staring at these things than I have.

MIT, same as upstream. TJpgDec keeps its own licence — see the header in `lib/tjpgd/tjpgd.c`.

---

Built by [Alexander West](https://thewest.cc).
