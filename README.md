# fbDOOM: double buffered, playable over a serial console

Fork of [maximevince/fbDOOM](https://github.com/maximevince/fbDOOM) (DOOM on a
plain Linux framebuffer). The original description is in
[README.TXT](README.TXT).

This fork was made for a cheap DVB-T set-top box, the WIWA HD95 (MStar
MSD7818, MIPS 34Kc at 552 MHz, 64 MB RAM), running Linux 6.7. Nothing in it
is specific to that box: the changes work on any Linux framebuffer.

Related:
- kernel with the double buffered framebuffer driver (`mstar-gopfb`):
  https://github.com/mztulip/linux_msd781x
- ready-made kernel, rootfs and boot instructions for the box:
  https://github.com/mztulip/msd7818_rootfs_kernel

## Changes compared to upstream

1. **Fast 16 bpp drawing** (`fbdoom/i_video_fbdev.c`). Upstream converts
   every byte of every scaled pixel through the palette and then `write()`s
   the whole frame. On the 552 MHz MIPS that took about 160 ms per
   1600x1000 frame (~6 fps). Now, when the framebuffer is 16 bpp:
   - the palette is converted to the framebuffer pixel format once, in
     `I_SetPalette` (`palette16[]`),
   - each scaled line is built once in RAM and `memcpy`'d `scaling` times
     straight into the `mmap`ped framebuffer.

   Result: about **50 fps**. Other pixel depths use the original code.
2. **Double buffering with vsync.** If the framebuffer has
   `yres_virtual >= 2 * yres`, each frame is drawn into the hidden half and
   shown with `FBIOPAN_DISPLAY`. Then `FBIO_WAITFORVSYNC` waits until the
   flip has happened, so there is no tearing. It runs at 35 fps, DOOM's
   game tic rate. If the driver has only one buffer, it falls back to
   drawing into the visible one.
3. **Keys from a serial terminal** (`fbdoom/i_input_tty.c`). Started from a
   terminal that is not a virtual console (e.g. picocom on a UART), the game
   reads keys from stdin in raw mode, so no USB keyboard is needed. See
   [Serial console controls](#serial-console-controls).
4. `-fps` prints the frame rate every 5 seconds.
5. The console is put in `KD_GRAPHICS` while playing, and `Quit Game` really
   exits. Terminal, keyboard and framebuffer are restored by `atexit()`.
6. **Sound effects on the WIWA HD95** (`fbdoom/i_mstarsound.c`, optional,
   `MSTAR_SOUND=1`). A software mixer (8 channels, DMX sound lumps from the
   WAD resampled to 48 kHz stereo, volume and stereo separation) runs in its
   own thread and feeds the audio DSP of the MStar SoC. Sound comes out on
   HDMI and on the analog (SCART) output at the same time. See
   [Sound on the WIWA HD95](#sound-on-the-wiwa-hd95). Music is not supported yet.
7. The `Makefile` defaults to the Buildroot mipsel toolchain
   (`CROSS_COMPILE ?= mipsel-buildroot-linux-gnu-`) and builds with
   `-std=gnu89`.

## Building

```sh
cd fbdoom
make CROSS_COMPILE=mipsel-buildroot-linux-gnu-      # or any other cross compiler, or none for the host
${CROSS_COMPILE}strip -o fbdoom_s fbdoom            # optional: 2.1 MB -> 0.5 MB
```
For the set-top box, the toolchain comes from the Buildroot SDK
(`make sdk` in Buildroot, then add
`~/toolchains/mipsel/mipsel-buildroot-linux-gnu/bin` to `PATH`).

You also need a WAD file, e.g. the shareware
[doom1.wad](https://doomwiki.org/wiki/DOOM1.WAD).

## Running

```sh
./fbdoom -iwad doom1.wad
```
While the game runs, the virtual console is switched to `KD_GRAPHICS`, so
the text console (cursor blink, kernel messages) does not draw into the
framebuffer. With double buffering it would show up as flicker in every
other frame. `KD_TEXT` is restored at exit.

### Options

| Option | Effect |
|---|---|
| (none) | double buffering + vsync when the framebuffer allows it |
| `-singlebuffer` | draw into the visible buffer (tears) |
| `-novsync` | double buffering without waiting for vsync |
| `-fps` | print frames per second every 5 s (stderr) |
| `-serialkeys` | always read keys from the stdin terminal |
| `-vtkeys` | always use the virtual console keyboard, even when started from a serial terminal |
| `-keyhold N` | serial keys: how long a key stays pressed after the first press, ms (default 300) |
| `-scaling N` | scaling factor (default: the largest that fits, e.g. 5 for 1920x1080) |

### Serial console controls

| Key in the terminal | In the game |
|---|---|
| arrows or `W` `A` `S` `D` | move / turn |
| `Q` / `E` | strafe left / right |
| `F` | fire |
| space | use (doors, switches) |
| `1`-`7` | weapons |
| `Tab` | automap |
| `Enter`, `Esc` | menu |
| `Y` / `N` | menu answers |
| **`Ctrl-C`** | quit (the terminal is restored) |

A terminal only sends key presses (and autorepeat while a key is held),
never releases. A key therefore counts as held for 300 ms after the first
press (`-keyhold`), each autorepeat extends that by 120 ms, and it is
released when the repeats stop. Holding an arrow gives continuous movement,
a short tap a small step. Two keys cannot be held at the same time, because
the terminal only repeats the last one.

Kernel messages also go to the serial console. `dmesg -n 1` silences them.

### USB keyboard: `openvt`

With a USB keyboard the game reads raw key codes from a virtual console
(press and release, several keys at once). It needs a virtual console as
its controlling terminal. If you are logged in over serial, start it on one
with BusyBox `openvt`:
```sh
openvt -c 2 -s -w -- sh -c 'cd /home; ./fbdoom -iwad doom1.wad'
```
- `-c 2`: run on `tty2` (`tty1` usually has a login getty),
- `-s`: switch the screen to it,
- `-w`: wait until the game exits.

Quit the game with Backspace, then `chvt 1`. If it hangs:
`killall fbdoom; chvt 1`. The game's output goes to `tty2`, so redirect it
(`> /tmp/doom.log 2>&1`) if you want to read it.

## Sound on the WIWA HD95

The box has no ALSA driver. Its audio goes through two DSPs inside the SoC
(DEC decodes, SE post-processes and drives HDMI and the DAC), and they need
the DSP code of the box's original firmware. `i_mstarsound.c` uses
`mstar_audio.h` from the reverse engineering workspace: it loads that code,
starts the DSPs with the xPCM (PCM) decoder and hands it 4 KB chunks of
16-bit PCM whenever the DSP asks for them, paced to real time (about 64 ms
of latency). It needs root, because it uses `/dev/mem`.

`mstar_audio.h` and the DSP code headers (`dsp_fw.h`, `dec03_fw.h`,
`se31_fw.h`, `xpcm_fw.h`, `audio_init_table.h`) are **not in this repo**:
the DSP code is MStar's, taken from the stock firmware. Build with
them in `MSTAR_AUDIO_DIR`:

```sh
cd fbdoom
make NOSDL=1 MSTAR_SOUND=1 MSTAR_AUDIO_DIR=/path/to/tools/board
```

Without `MSTAR_SOUND=1` the game builds as before, without sound. Run it as
usual; `-nosound` or `-nosfx` turns the sound off.

## Performance on the WIWA HD95

1600x1000 (320x200 scaled x5) on 1920x1080 RGB565, measured with `-fps`
on the title screen demo:

| Version | fps |
|---|---|
| upstream drawing (per-byte palette lookup + `write()`) | ~6 |
| fast 16 bpp path, `-singlebuffer` | ~50 |
| fast path, double buffered, `-novsync` | ~49 |
| fast path, double buffered + vsync (default) | **~35**, no tearing |

Copying a 1600x1000 frame into the framebuffer takes about 12 ms
(269 MB/s), and the x5 scaling with 16-bit stores about 9 ms.
