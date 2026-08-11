# Beetle Lynx-Com

Experimental fork of the [Beetle Lynx](https://github.com/libretro/beetle-lynx-libretro)
libretro core that runs **two ComLynx-linked Atari Lynx machines in one core instance**,
so two-player ComLynx games can be played without a second machine and a second cartridge.

It is the libretro counterpart of the `lynx_com` module in
[this fork of Mednafen](https://github.com/Ceotom/mednafen), and carries the same
emulation work: the Lynx core already had a complete UART/ComLynx implementation, but
nothing was ever wired to the other end of the cable.

## What is different from Beetle Lynx

- Two machines run side by side, joined by ComLynx. Both run the same program, loaded
  from the same file; running two *different* programs is not supported.
- The picture is 320x102: machine 1 on the left, machine 2 on the right.
- Two controller ports. Port 1 drives machine 1, port 2 drives machine 2.
- Sound comes from one machine, selectable with a core option.
- **Save states are not interchangeable with Beetle Lynx**, in either direction, and
  they are roughly twice the size.
- The core is built as `mednafen_lynx_com_libretro` and shows up as "Beetle Lynx-Com",
  so it can be installed next to the stock core.

The 512-byte Lynx boot ROM, named `lynxboot.img`, must be present in the libretro
system directory, exactly as for Beetle Lynx.

## Status

Work in progress, on branch `comlynx`.

- [x] Core renamed and building as Beetle Lynx-Com
- [x] Shared core changes (link timing, transmitter, save state coverage)
- [x] Two-machine driver, 320x102 output, two input ports
- [ ] Core options and settings
- [ ] Save states and the save state self test

## Building

```sh
make -j$(nproc)
```

Produces `mednafen_lynx_com_libretro.dll` / `.so` / `.dylib` depending on the platform.
