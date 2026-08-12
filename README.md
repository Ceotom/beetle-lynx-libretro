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

## Core options

Beyond the ones Beetle Lynx already has (`Auto-rotate Screen`, `Color Format`,
`Force 60Hz`):

- **ComLynx: Sound From Machine** — which of the two machines is heard. Both are
  emulated in full either way; only one stream can go to the frontend. Takes effect
  immediately.
- **ComLynx: Transmit IRQ on TXRDY** — raise the transmit interrupt when the holding
  register frees rather than when the shift register empties. Default off. Only
  California Games needs it, and it needs it to pair up at all; with this off it
  never gets past its own title screen in two-player. It costs everything else:
  a game's link traffic multiplies two to four times, which measurably costs Hockey,
  Zarlor Mercenary and Basketbrawl their frame rate and makes Checkered Flag stutter.
  Turning it on also staggers the two machines further apart at start, because
  California Games needs both together and neither alone. Applied when content is
  loaded, so it needs a restart to take effect.

**Netplay:** the transmit interrupt option changes link timing, so two peers that
disagree about it will desynchronise. Unlike the Mednafen module this core cannot fold
a setting into the room's identity, so nothing stops such a session from connecting —
check that both sides match before starting one.

## Status

Work in progress, on branch `comlynx`.

- [x] Core renamed and building as Beetle Lynx-Com
- [x] Shared core changes (link timing, transmitter, save state coverage)
- [x] Two-machine driver, 320x102 output, two input ports
- [x] Core options and settings
- [x] Save states and the save state self test

## Building

```sh
make -j$(nproc)
```

Produces `mednafen_lynx_com_libretro.dll` / `.so` / `.dylib` depending on the platform.

### Save state self test

The core can test its own save states from the inside and report the result when the
game is closed. It runs to frame N with the link busy and saves S1, runs K more frames
to S2, reloads S1 and runs the same K frames to S3; S2 and S3 have to match byte for
byte, and so does the picture. K is 1, 60 and 3600.

```sh
make -j$(nproc) CPPFLAGS="-DLYNXCOM_TEST_STATE=1 -DLYNX_TEST_POISON=1"
```

The resulting core drives the machines from a synthesized input stream instead of the
pads, so it is a test build and nothing else. The two defines belong together: the
second one overwrites the emulation's carried-over state before the replay's load, and
without it the test cannot see a variable the state forgot — dropping four display
registers from the state makes it fail with the poison on and pass with it off.
