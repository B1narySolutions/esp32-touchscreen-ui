# PDR demo: a TONE3000 amp, picked online, running on the Seed

Someone picks an amp capture on [TONE3000](https://www.tone3000.com), and the Seed runs that exact model within
a minute. Works since 2026-10-15 (SEED_LINK_STATUS.md, "SD amp profiles on the real Seed").

## What runs

Only **A2** captures at **48 kHz**: the Seed's engine is A2-Lite, the same engine as the built-in amps. TONE3000
lists the architecture on each capture; download the A2 version. The touchscreen checks every file and says why it
can't use one (for example "NAM 0.5.4 model (A1 / standard), not A2" or "44100 Hz model, needs 48 kHz"). Full-size
NAM models are too big and too slow for the Seed, which is the memory problem people mean.

An A2 model is 7.5 KB, the same size as a built-in, so memory isn't an issue: the Seed holds one uploaded model at a
time next to its built-ins.

## Before the demo

- Touchscreen and Seed wired (3 wires, SW1 on UART2) and talking: Test Mode shows Connected.
- The Seed runs the build from `seed3-link-firmware/` (`build.sh`, then `flash.sh`).
- The SD card (FAT32, MBR) in the touchscreen, with a `/nam` folder. Have a laptop with an SD card reader.
- Try it once with a file already on the card.

## The demo

1. On TONE3000, the audience picks a capture with an A2 version; download its `.nam`.
2. Copy it to the card's `/nam` folder, and put the card back in the touchscreen.
3. Touchscreen: AMP > SD Library > **Rescan SD card**. The new file appears with its name.
4. Tap it: "Uploading to the Seed: N%" (under 0.1 s), then **"Running on the Seed"**. The Seed's console prints
   `>>> SD profile <crc> loaded`.
5. Play. The touchscreen's Test Mode shows the Seed running "uploaded <crc>", its real DSP load and meters.

If the Seed reboots, the touchscreen uploads the selected profile again by itself.

## Would make it smoother (not done yet)

- Send the `.nam` from the laptop to the touchscreen over USB, so the card never leaves the touchscreen.
- Show the profile's name, not its CRC, in the Seed's status line.
