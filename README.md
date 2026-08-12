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
  they are twice the size and then some — 277736 bytes against 138099, because there
  are two machines with 64K of RAM each. Rewind buffers fill about twice as fast.
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
  never gets past its own title screen in two-player. ~~It costs everything else:
  a game's link traffic multiplies two to four times, which measurably costs Hockey,
  Zarlor Mercenary and Basketbrawl their frame rate and makes Checkered Flag stutter.~~
  (Not actual for this port for some reason)
  Turning it on also staggers the two machines further apart at start, because
  California Games needs both together and neither alone. Applied when content is
  loaded, so it needs a restart to take effect.

**Netplay:** the transmit interrupt option changes link timing, so two peers that
disagree about it will desynchronise. Unlike the Mednafen module this core cannot fold
a setting into the room's identity, so nothing stops such a session from connecting —
check that both sides match before starting one.

## Compatibility

All 33 ComLynx-capable titles in the No-Intro set react to the cable, Raiden's
prototype included. That was measured rather than eyeballed: every game was run twice
from the same source, once normally and once against a build whose only difference is
that the cable is out, and the two runs compared frame by frame. A game whose picture
changes took a different path, and the link is the only thing that could have sent it
there. A screenshot of a single build would not answer this — a machine on its title
screen may be running an attract loop, which looks exactly like a dead link.

Thirty-two of them notice with no input at all, the first divergence landing **6 to 19
seconds after boot** and usually within seven. Only Raiden, a prototype that is broken
in one-player too, needs a button pressed first; it then diverges at 8 seconds like the
rest. Most of the 32 announce the result themselves by printing the number of consoles
they found.

Two are worth knowing about specifically:

- **California Games** sees the link on its own, but needs `ComLynx: Transmit IRQ on
  TXRDY` to complete the handshake. With it on, one machine offers "PRESS BUTTON A TO
  START 2 PLAYER GAME" while the other reports "2 PLAYERS CONNECTED"; with it off it
  never gets that far.
- **Gauntlet** prints no console counter at any point, so it can only be judged by what
  it does. It reaches character select with the two machines holding *different*
  characters, and stays link-dependent for at least three minutes of play — well past
  the watchdog that used to end the alliance after exactly 40 packets.

Comparing one sampled frame instead of the sequence is a trap worth naming, because it
fails quietly and in one direction only: two builds running entirely different code can
still draw the same picture at some particular instant. World Class Soccer matches on a
quarter of its frames, California Games and Turbo Sub on three quarters, so a single
sample lands on a coincidence often enough to report a working link as a dead one.

## Status

On branch `comlynx`. Everything below is done; the core plays linked games with two
pads, survives save states and rewind mid-match, and holds a netplay session.

- [x] Core renamed and building as Beetle Lynx-Com
- [x] Shared core changes (link timing, transmitter, save state coverage)
- [x] Two-machine driver, 320x102 output, two input ports
- [x] Core options and settings
- [x] Save states and the save state self test
- [x] Library sweep and Linux build
- [x] Hands-on session: two pads, save/load and rewind during a match, netplay

## Building

```sh
make -j$(nproc)
```

Produces `mednafen_lynx_com_libretro.dll` / `.so` / `.dylib` depending on the platform.

Built and run on both Windows (gcc 16, MinGW-w64) and Linux (gcc 13, x86-64). The two
produce identical pictures frame for frame and identical audio sample counts, which is
what netplay between peers on different platforms depends on.

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
