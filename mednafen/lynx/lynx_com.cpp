/* Mednafen - Multi-system Emulator
 *
 * lynx_com.cpp - Two ComLynx-linked Atari Lynx machines in one core instance
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

//
// This drives the same Lynx core as Beetle Lynx, but with two machines running
// side by side and wired together over ComLynx.  It replaces the single-machine
// driving code that used to live at the bottom of system.cpp; what is left
// there is CSystem itself and the file-scope state the core keeps.
//
// The output surface holds both screens horizontally: machine 1 at x=0,
// machine 2 at x=160.  That layout is fixed, not a setting, because both halves
// have to stay byte-identical between netplay peers.
//

#include <string.h>
#include <stdlib.h>

#include "system.h"

#include "../mednafen-endian.h"
#include "../general.h"
#include "../mempatcher.h"
#include "../settings.h"

#include <libretro.h>

extern retro_log_printf_t log_cb;

// Cycles a machine may run ahead of the other before the scheduler hands over.
// Deliberately not a setting: netplay peers have to agree on it exactly, and a
// mismatch would desynchronize a session on the first exchanged byte.  The
// system is clocked at 16MHz and a bit at ComLynx's fastest 62500 baud lasts
// 256 cycles, so this leaves a fourfold margin on the finest timing the link
// can distinguish.
#define SYNC_QUANTUM		64

// Define LYNXCOM_TEST_STATE to have the core test its own save states and report
// the result at CloseGame.  Off by default; it is a test, not an instrument, and
// it drives the machines from a synthesized input stream rather than the pads.
// What it does and why it has to live in here is at the harness itself, further
// down.  Build it with:
//
//   make CPPFLAGS="-DLYNXCOM_TEST_STATE=1 -DLYNX_TEST_POISON=1"
//
// LYNX_TEST_POISON belongs with it: without it the test cannot see a forgotten
// display register.

// Machines constructed.  Everything below is written against this rather than
// against LYNXCOM_SCREENS, so a screen slot with no machine behind it stays a
// representable state.
const unsigned NumMachines = 2;

CSystem* machine[LYNXCOM_MAX_MACHINES];

static uint8* chee[LYNXCOM_SCREENS];

// Set by the frontend before the game is loaded; see check_variables() in
// libretro.cpp.  Plain globals rather than MDFN_GetSettingB(), because this
// fork's settings.cpp is a stub with its answers compiled in.
bool     LynxComTxRdyIRQ     = false;
unsigned LynxComSoundMachine = 0;

//
// The Lynx core keeps eight scalars plus LynxLineDrawn[] at file scope
// (system.cpp), so two machines cannot simply coexist.  Making that state a
// member of CSystem would touch nearly every line of the core and cost the
// single-machine build an indirection in its hottest paths, so instead each
// machine owns a copy of the block and the copy is swapped in around whatever
// code has to run as that machine.  288 bytes each way, and only when the
// active machine actually changes.
//
struct LynxGlobalState
{
 uint32 gSuzieDoneTime;
 uint32 gSystemCycleCount;
 uint32 gNextTimerEvent;
 uint32 gCPUBootAddress;
 uint32 gSystemIRQ;
 uint32 gSystemNMI;
 uint32 gSystemCPUSleep;
 uint32 gSystemHalt;
 bool   LynxLineDrawn[256];
};

// Outside of the scheduler loop this is the only valid copy of that state; the
// live globals are meaningful only while a machine is active.
static LynxGlobalState ctx[LYNXCOM_MAX_MACHINES];

// Signed cycle offset of each machine from machine 1, carried across frames.
// The scheduler holds it within a quantum, so a frame-local counter plus this
// is all that is needed -- no unbounded absolute clock to keep in sync, and
// machine 1's entry is 0 by definition.
//
// Sized for the largest link this could ever drive rather than for the two it
// drives now, because this array goes into the save state whole.
static int32 MachineSkew[LYNXCOM_MAX_MACHINES];

// Where each machine starts, in cycles behind machine 1.
//
// Two Lynxes are never switched on at the same instant, and some games depend on
// it without saying so: they trade a packet carrying a machine number, and two
// machines running in step both keep claiming the same number for ever.  Gauntlet
// and California Games sit on their title screens for exactly this reason -- with
// every offset zero their transmitted bytes are identical for as long as anyone
// cares to look.
//
// Modelling bus contention does not help and cannot: ComLynx is wired-AND, so two
// machines putting the same byte on the wire at the same moment produce that byte,
// undamaged, on real hardware too.  Measured, not assumed -- with contention
// modelled and half of California's traffic landing in a busy wire, both machines
// stayed deadlocked.  What breaks the tie on a real link is the offset itself.
//
// Not an arithmetic progression, on purpose.  With offsets i*S every pairwise
// difference is a multiple of S, and multiples are what deadlocks: 65537 cycles
// breaks Gauntlet's symmetry and 131071 does not, so i*65537 would leave machines
// 1 and 3 stuck with each other.  These are primes, all 28 pairwise differences
// are distinct, and none is an exact multiple of another.  All of them stay under
// one frame; the scheduler recomputes the skew each frame from where the machines
// actually got to, so a larger offset would be reshaped by frame boundaries
// rather than kept.
//
// 24001 for machine 2 is chosen by measurement, not taste: Gauntlet deadlocks at
// 12345 and at 50021, and this sits clear of both.  California Games wants a
// different one, and by a length rather than by taste: a machine hears its
// partner only until it starts transmitting itself, because writing SERDAT
// replaces the receive vector, so the head start has to cover a whole packet --
// eight bytes at 176us is 1408us, 22528 cycles -- plus the time to check one.  At
// 24001 its machine 2 catches six bytes of eight and then talks over the rest.
// That larger offset rides with the TXRDY interrupt setting, because neither
// alone gets the handshake through: see LYNXCOM_SKEW_TXRDY.
//
// The physics does not choose a value for us either -- two consoles are switched
// on some arbitrary distance apart, so every non-zero offset is equally real.
// What it does say is that the offset is not a difference in the emulator's
// start time: the boot code rebases every timer it programs, so a machine's own
// timeline is identical whatever the offset, and the difference only ever enters
// over the wire.  That is why no offset on its own ever fixed anything.
//
// Only the first two are backed by measurement, because only two machines run.
// Adding machines means checking 28 pairs, not one.
static const int32 MachineStartSkew[LYNXCOM_MAX_MACHINES] =
{
 0, 24001, 65537, 111029, 148013, 189011, 224011, 251003
};

// Machine 2's offset when the TXRDY interrupt setting is on.  Prime, and all 28
// pairwise differences stay distinct with it substituted in, so the properties
// argued for above survive.
static const int32 LYNXCOM_SKEW_TXRDY = 80021;

static void SaveGlobals(LynxGlobalState* s)
{
 s->gSuzieDoneTime	= gSuzieDoneTime;
 s->gSystemCycleCount	= gSystemCycleCount;
 s->gNextTimerEvent	= gNextTimerEvent;
 s->gCPUBootAddress	= gCPUBootAddress;
 s->gSystemIRQ		= gSystemIRQ;
 s->gSystemNMI		= gSystemNMI;
 s->gSystemCPUSleep	= gSystemCPUSleep;
 s->gSystemHalt		= gSystemHalt;

 memcpy(s->LynxLineDrawn, LynxLineDrawn, sizeof(s->LynxLineDrawn));
}

static void RestoreGlobals(const LynxGlobalState* s)
{
 gSuzieDoneTime		= s->gSuzieDoneTime;
 gSystemCycleCount	= s->gSystemCycleCount;
 gNextTimerEvent	= s->gNextTimerEvent;
 gCPUBootAddress	= s->gCPUBootAddress;
 gSystemIRQ		= s->gSystemIRQ;
 gSystemNMI		= s->gSystemNMI;
 gSystemCPUSleep	= s->gSystemCPUSleep;
 gSystemHalt		= s->gSystemHalt;

 memcpy(LynxLineDrawn, s->LynxLineDrawn, sizeof(s->LynxLineDrawn));
}

//
// One byte has finished shifting out of machine `objref`.  ComLynx is a single
// open-collector wire that every machine on the link shares, so this is written
// as a bus rather than as a point-to-point cable: the byte goes to everyone
// else, and stretching the link past two machines stays a matter of changing
// NumMachines.
//
// Called from inside the sending machine's Update(), with the sender's globals
// live -- and that is fine, because ComLynxRxData() touches nothing but CMikie
// members (mikie.cpp:331-345).  Handing the byte over needs no context swap.
//
// The sender is skipped deliberately.  The wire shorts Rx to Tx, so a machine
// hears everything it says, and the core already models that: a byte goes
// through ComLynxTxLoopback() at the moment transmission starts (mikie.cpp:382),
// as does each repeat of a break.  Echoing here as well would give every machine
// two copies of its own traffic.
//
static void ComLynxTx(int data, uint32 objref)
{
 //
 // Wait for the wire.  ComLynx is one line, so one byte is shifting out on it
 // at a time; without this, every machine puts a byte on it whenever it likes
 // and each receiver is handed two bytes -- its own echo and the partner's --
 // in the time the wire could carry one.  The 32-entry queue then delivers
 // them at the line rate, which is half the rate they arrive, so a machine
 // reading its own stream finds a partner byte wedged between two of its own
 // and falls a byte further behind with every packet.  Measured on California
 // Games: machine 2 read one byte in eight of what machine 1 sent, with the
 // queue never above 3 of 32 and not a byte dropped -- the loss was entirely
 // mUART_RX_DATA being overwritten before the program got to it, 5863 times.
 //
 // Holding the sender's countdown is the whole mechanism: SERCTL reports the
 // transmitter free from it (mikie.cpp:1389) and the transmit interrupt fires
 // off it (mikie.cpp:2230), so a game that waits its turn -- and any game that
 // works on real hardware does -- simply writes its next byte later.
 //
 // Delivery is not deferred with it, and deliberately so: the receive queue is
 // already a model of the wire, handing the program one byte per byte frame.
 // A byte queued behind one still in flight therefore pops exactly one frame
 // after it, which is when it would have been latched had it waited on the
 // wire.  Deferring delivery as well would charge that frame twice, the same
 // mistake as handing the byte over at the end of the sender's transmission
 // instead of the start.
 //
 // This replaces an earlier attempt at contention that discarded the late
 // byte, optionally raising a framing error on every receiver.  That changed
 // nothing -- California Games put half its traffic into a busy wire and
 // stayed just as deadlocked -- and it should not have: the wire is
 // wired-AND, so two machines putting the same byte on it at the same moment
 // produce that byte intact on real hardware too.  Losing bytes symmetrically
 // gives neither machine an advantage.  Serializing them does not lose any.
 //
 // Waiting has a price, and one game shows it: Xenophobe, whose exchange is
 // dense and symmetric, gained noticeable input lag.  Expected -- a byte now
 // waits for the partner to finish shifting, which is what one wire means --
 // and kept, because the wire is shared by however many machines are on it and
 // this is the behaviour that still makes sense when there are more than two.
 //
 // Holding does move a sender's later bytes closer together, and the programs
 // delimit packets by the silence between bytes, so it is fair to ask whether
 // the model keeping the wire honest erases the very thing the protocol reads.
 // Measured, and it does not: with the gaps counted on the receiving side and
 // split by whose byte fell on either side of them, holding, not holding, and
 // destroying the late byte all produced the same number of packet boundaries
 // (1631, 1614, 1630) against about 1720 expected, and the same handful inside
 // one sender's stream.  What was eating Gauntlet's packets was a timer that
 // expired the moment it was loaded (mikie.cpp, Poke of TIMxCNT).
 //
 {
  uint32 hold = 0;

  for(unsigned i = 0; i < NumMachines; i++)
  {
   if(i == objref)
    continue;

   // Bit times, in the partner's own Timer 4 units.  Both machines run the
   // same program at the same baud rate, so its bit times are this machine's.
   const uint32 busy = machine[i]->mMikie->ComLynxTxCountdown();

   if(busy > hold)
    hold = busy;
  }

  if(hold)
  {
   machine[objref]->mMikie->ComLynxTxDelay(hold);

   //
   // A byte offered to a wire that is already carrying one is the moment two
   // machines transmit at once, and on the hardware that is what a framing
   // error means -- the specification puts it as being able to "sense if two
   // Lynxes transmit at the same time", and cc65 says the same the other way
   // round: a framing error means another Lynx is probably sending too.  The
   // flag has never been raised in this core, only cleared, so a protocol
   // that arbitrates by collision has been running blind.
   //
   // Everyone sees it, the sender included: the wire shorts Rx to Tx, so a
   // machine hears its own transmission collide, and that is exactly how it
   // detects the collision.
   //
   // It fixes nothing.  The two games that will not pair up are unmoved by it,
   // byte for byte, and the ones that work are unmoved in the ways that matter
   // -- though it is not inert: it shifts the phase of Gauntlet's cycle, and
   // Battle Wheels' machines come out holding each other's slots, which that
   // protocol treats as a coin toss.  Here because a flag the program can read
   // was pinned to zero, which is a defect whatever these five ROMs do with it.
   //
   for(unsigned i = 0; i < NumMachines; i++)
    machine[i]->mMikie->ComLynxRxFramingError();
  }
 }

 for(unsigned i = 0; i < NumMachines; i++)
 {
  if(i == objref)
   continue;

  machine[i]->ComLynxRxData(data);
 }
}

static void Cleanup(void)
{
 // Machines are counted by LYNXCOM_MAX_MACHINES and ports by LYNXCOM_SCREENS,
 // and the two are only equal while this drives two of each.  Walking the
 // machines with the screen count would leave every machine past the second one
 // alive after CloseGame.
 for(unsigned i = 0; i < LYNXCOM_MAX_MACHINES; i++)
 {
  if(machine[i])
  {
   delete machine[i];
   machine[i] = NULL;
  }
 }

 for(unsigned i = 0; i < LYNXCOM_SCREENS; i++)
  chee[i] = NULL;

 memset(ctx, 0, sizeof(ctx));
 memset(MachineSkew, 0, sizeof(MachineSkew));
}

void Load(MDFNFILE *fp, const char *bios_path)
{
 memset(ctx, 0, sizeof(ctx));
 memset(MachineSkew, 0, sizeof(MachineSkew));

 // Every CRam registers its RAM with the cheat engine as it is built, so the
 // page table has to exist before the first machine does.  It used to be set up
 // by CSystem's constructor, which with two machines would have built it twice
 // and leaked the first one.
 MDFNMP_Init(65536, 1);

 for(unsigned i = 0; i < NumMachines; i++)
 {
  // CCart/CRam read the file from wherever it happens to be, and the
  // constructor only rewinds once at its very start, so every machine after
  // the first needs the position reset by hand.
  file_seek(fp, 0, SEEK_SET);

  machine[i] = new CSystem(fp, bios_path);

  // Without this a machine whose CPU goes to sleep jumps straight to its next
  // timer event -- measured at up to 2544 cycles -- and lands that far past
  // its partner in a single scheduler step, which is most of a byte at
  // ComLynx's fastest rate.  Capping the skip costs nothing and keeps the
  // spread at the quantum.
  machine[i]->mSleepAdvanceLimit = SYNC_QUANTUM;

  // The constructor ends in Reset(), which writes the globals, so this is
  // where machine i's context comes from.
  SaveGlobals(&ctx[i]);
 }

 // Plug the cable in.  Both halves of this are set by CMikie's constructor and
 // left alone by its Reset(), so the link survives MDFN_MSC_RESET; the cable
 // flag is part of the saved state as well, and always saved set.
 //
 // Not a setting: a core that exists only to link two machines has nothing to
 // offer with the cable out.
 for(unsigned i = 0; i < NumMachines; i++)
 {
  machine[i]->ComLynxTxCallback(ComLynxTx, i);
  machine[i]->ComLynxCable(true);

  // Take the receive pipeline off the leash Handy put it on.  It inserted 44
  // idle bit times between one queued byte and the next, which holds sustained
  // reception to a fifth of the line rate; with a partner on the wire that is
  // slower than the bus delivers, so the queue grows every frame until the
  // 32-entry buffer starts throwing bytes away without a word.  On the wire
  // bytes follow each other with no gap, so the byte frame alone is the whole
  // interval.  Battle Wheels puts "CHECK COMLYNX" on both screens with the
  // gap in and plays with it out; Malibu, whose traffic already fitted, is
  // unchanged to within a tenth of a percent either way.
  machine[i]->mMikie->ComLynxRxNextDelay(0);
 }

 // Every CRam::Reset() registered its own RAM over the last one.  Put machine
 // 1's back: a cheat list has no way to say which machine it means, so cheats
 // and the frontend's memory map apply to machine 1 only.
 MDFNMP_AddRAM(65536, 0x0000, machine[0]->GetRamPointer());

 switch(machine[0]->CartGetRotate())
 {
  case CART_ROTATE_LEFT:
   MDFNGameInfo->rotated = MDFN_ROTATE270;
   break;

  case CART_ROTATE_RIGHT:
   MDFNGameInfo->rotated = MDFN_ROTATE90;
   break;
 }

 if(LynxComSoundMachine >= NumMachines)
  LynxComSoundMachine = 0;

 // Stagger the machines. See MachineStartSkew.
 {
  for(unsigned i = 0; i < NumMachines; i++)
   MachineSkew[i] = MachineStartSkew[i];

  // The two travel together: neither alone gets California Games through its
  // handshake, and the offset on its own has never fixed anything for anyone.
  if(LynxComTxRdyIRQ && NumMachines > 1)
   MachineSkew[1] = LYNXCOM_SKEW_TXRDY;

  for(unsigned i = 0; i < NumMachines; i++)
   machine[i]->mMikie->ComLynxTxIRQOnHolding(LynxComTxRdyIRQ);
 }

 for(unsigned i = 0; i < NumMachines; i++)
  machine[i]->mMikie->miksynth.treble_eq(MDFN_GetSettingB("lynx.lowpass") ? -35 : 0);

 if(log_cb)
  log_cb(RETRO_LOG_INFO, "ComLynx: %u machines linked, sound from machine %u.\n",
	 NumMachines, LynxComSoundMachine + 1);
}

#ifdef LYNXCOM_TEST_STATE
//
// Save state round trip, driven from inside the emulation.
//
// What has to be shown is that a state is a complete description of a linked
// session: run to frame N and save S1; run K more frames and save S2; reload S1,
// run the same K frames again and save S3.  S2 and S3 have to match byte for
// byte, and the picture at the end of both passes has to match too.  Anything
// the driver forgot to save shows up as a divergence in the second pass, which
// is the only way to find it -- a missing variable is invisible in the state
// file by definition.
//
// It lives in here rather than in a script because the criterion is stated in
// exact frame counts.  Key presses land on a wall clock, so a driver outside the
// process can reach "about a second later" but never "exactly one frame later",
// and K = 1 is the case most likely to catch a stale pointer.
//
// Input is synthesized from the frame index instead of being read from the pads.
// That is what a movie recording would provide -- the same buttons at the same
// frames in both passes -- and without it the test only proves that an idle
// machine stays idle, which is the one case where nothing much is in flight.
//
// Build it with:  make CPPFLAGS="-DLYNXCOM_TEST_STATE=1 -DLYNX_TEST_POISON=1"
//
static const unsigned TestK[] = { 1, 60, 3600 };

enum { TEST_ARM, TEST_RUN, TEST_REPLAY, TEST_DONE };

struct TestSnapshot
{
 uint8* data;
 uint32 len;
};

static unsigned TestPhase;
static unsigned TestKIdx;
static uint64 TestFrame;		// frames since the game was loaded
static uint64 TestSeqBase;		// frame index N that S1 was taken at
static uint64 TestSeqPos;		// frames run since N, in whichever pass
static TestSnapshot TestS1;
static TestSnapshot TestS2;
static uint64 TestSurfaceHash;		// picture at the end of the first pass
static unsigned TestPassed, TestFailed;
static char TestReport[4][192];

static bool TestSave(TestSnapshot* s)
{
 StateMem st;

 memset(&st, 0, sizeof(st));

 if(!MDFNSS_SaveSM(&st, 0, 0, NULL, NULL, NULL))
 {
  free(st.data);
  return false;
 }

 free(s->data);
 s->data = st.data;
 s->len = st.len;

 return true;
}

static bool TestLoad(const TestSnapshot* s)
{
 StateMem st;

 memset(&st, 0, sizeof(st));
 st.data = s->data;
 st.len = s->len;

 return MDFNSS_LoadSM(&st, 0, 0) != 0;
}

static void TestFree(TestSnapshot* s)
{
 free(s->data);
 s->data = NULL;
 s->len = 0;
}

// Deterministic, differs between the machines, and a pure function of the frame
// index so both passes see it identically.  Low byte only: those are the
// direction and face buttons, and leaving the switches alone keeps the test off
// the console's own pause handling.
static uint16 TestInput(unsigned m, uint64 f)
{
 uint32 h = (uint32)f * 2654435761u;

 h ^= m * 0x9E3779B9u;
 h ^= h >> 13;
 h *= 1274126177u;
 h ^= h >> 16;

 return (uint16)(h & 0x00FF);
}

// The frame index about to be emulated.  During the two timed passes it is
// counted from N, so the replay feeds frame N+j exactly what the first pass fed
// frame N+j.
static uint64 TestInputFrame(void)
{
 if(TestPhase == TEST_RUN || TestPhase == TEST_REPLAY)
  return TestSeqBase + TestSeqPos + 1;

 return TestFrame;
}

// Is a byte actually on the wire?  The criterion asks for N to land in the
// middle of a ComLynx transmission, which is where link state that was left out
// of the save would do the most damage.
static bool TestLinkBusy(void)
{
 for(unsigned i = 0; i < NumMachines; i++)
 {
  if(machine[i]->mMikie->ComLynxTxCountdown() || machine[i]->mMikie->ComLynxRxWaiting())
   return true;
 }

 return false;
}

// Overwrite everything the emulation carries from frame to frame that a state
// could have forgotten, then load on top of it.  A complete state puts all of it
// back and the pass runs exactly as it would have; an incomplete one leaves the
// wrong values in place and the pass diverges.
//
// The value has to be different every time, and that is the whole lesson of the
// first version of this test.  Without poisoning it passed on a state that was
// missing three display registers, because a state is only ever taken at a frame
// boundary and machine 2 is at the same phase of its own picture at every such
// boundary -- so the stale values happened to equal the right ones.  Two passes
// poisoned identically would go wrong identically and hide the same way.
//
// Every value is wrong but *reachable*: a line counter within the screen, a
// cycle count within the counter's range.  The first attempt used 0xB16B00B5
// everywhere and it crashed the emulator instead of failing the test -- a DMA
// counter of three billion has DisplayRenderLine paint lines until the process
// dies, which says nothing about save states.  A state has to be judged by
// whether the emulation diverges, and for that the poison has to be a state the
// machine could genuinely have been in.
static uint32 TestPoisonSeq;

static void TestPoison(void)
{
 const uint32 n = ++TestPoisonSeq;

 for(unsigned i = 0; i < NumMachines; i++)
 {
#ifdef LYNX_TEST_POISON
  // Line within the screen, DMA counter within a frame's worth of lines,
  // framebuffer pointer within RAM.
  machine[i]->mMikie->PoisonDisplay(7 + (n * 13 + i * 29) % 90,
				    3 + (n * 17 + i * 31) % 95,
				    (n * 4093 + i * 8191) & 0xFFFC);
#endif

  // The borrowed globals, which this file holds rather than the core, and
  // which reach the state only by being lent to each machine's SYST section.
  ctx[i].gSuzieDoneTime = 1000000 + n * 7919;
  ctx[i].gSystemCycleCount = 2000000 + n * 6997;
  ctx[i].gNextTimerEvent = 2000000 + n * 7013;
  ctx[i].gCPUBootAddress = 0x0200 + n;
  ctx[i].gSystemIRQ = (n + i) & 1;
  ctx[i].gSystemNMI = (n + i + 1) & 1;
  ctx[i].gSystemCPUSleep = (n + i) & 1;
  ctx[i].gSystemHalt = 0;		// halting would just stop the machine

  for(unsigned y = 0; y < 256; y++)
   ctx[i].LynxLineDrawn[y] = ((y + n + i) & 3) != 0;

  // And the scheduler's own carried state, the one thing in section COML.
  MachineSkew[i] = (int32)((n * 3001 + i * 5003) % 60000);
 }

 // mColourMap is left alone on purpose: it is a lookup table built from the
 // pixel format, rebuilt only by DisplaySetAttributes, and no more part of the
 // emulated machine than the window is.
}

static uint64 TestHashSurface(const MDFN_Surface* surface)
{
 uint64 h = 1469598103934665603ull;

 if(!surface)
  return 0;

 for(int y = 0; y < LYNXCOM_SCREEN_H; y++)
 {
  for(int x = 0; x < LYNXCOM_SURFACE_W; x++)
  {
   const uint32 p = (surface->bpp == 16) ? surface->pixels[y * surface->pitch + x]
					 : ((const uint32*)surface->pixels)[y * surface->pitch + x];

   h = (h ^ p) * 1099511628211ull;
  }
 }

 return h;
}

static void TestFail(const char* what)
{
 snprintf(TestReport[TestKIdx < 4 ? TestKIdx : 3], sizeof(TestReport[0]),
	  "K=%-4u FAILED: %s", TestK[TestKIdx], what);
 TestFailed++;
 TestPhase = TEST_DONE;
}

static void TestStateStep(const MDFN_Surface* surface)
{
 switch(TestPhase)
 {
  case TEST_ARM:
	// Ten seconds in, at a frame with the link actually busy.  Earlier than
	// that and the games are still in their boot animation with nothing to
	// say to each other.
	//
	// The wire is only occupied for a small share of any frame, so a game
	// that talks at all offers such a moment within a few hundred frames.
	// One that never does has stopped talking, which is worth saying out
	// loud rather than silently testing nothing -- so after another ten
	// seconds the test starts anyway and says the wire was idle.
	if(TestFrame >= 600 && (TestLinkBusy() || TestFrame >= 1200))
	{
	 const bool busy = TestLinkBusy();

	 if(!TestSave(&TestS1))
	 {
	  TestFail("could not write S1");
	  break;
	 }

	 TestSeqBase = TestFrame;
	 TestSeqPos = 0;
	 TestPhase = TEST_RUN;

	 if(log_cb)
	  log_cb(RETRO_LOG_INFO, "StateTest: S1 taken at frame %llu, %u bytes, wire %s\n",
		 (unsigned long long)TestSeqBase, (unsigned)TestS1.len,
		 busy ? "busy" : "IDLE -- this game stopped talking");
	}
	break;

  case TEST_RUN:
	TestSeqPos++;

	if(TestSeqPos >= TestK[TestKIdx])
	{
	 if(!TestSave(&TestS2))
	 {
	  TestFail("could not write S2");
	  break;
	 }

	 TestSurfaceHash = TestHashSurface(surface);

	 // Only the replay's load is poisoned, and that asymmetry is the point:
	 // the first pass starts from whatever was live, the second from
	 // wreckage, so anything the state fails to restore differs between
	 // them.  Poisoning both loads would put both passes on the same wrong
	 // footing and blind the test again -- which is the failure mode this
	 // whole mechanism exists to fix.  Do not "fix" it by adding a second
	 // call at the reload further down.
	 TestPoison();

	 if(!TestLoad(&TestS1))
	 {
	  TestFail("could not read S1 back");
	  break;
	 }

	 TestSeqPos = 0;
	 TestPhase = TEST_REPLAY;
	}
	break;

  case TEST_REPLAY:
	TestSeqPos++;

	if(TestSeqPos >= TestK[TestKIdx])
	{
	 TestSnapshot s3 = { NULL, 0 };
	 bool ok_state, ok_pic;

	 if(!TestSave(&s3))
	 {
	  TestFail("could not write S3");
	  break;
	 }

	 ok_state = (s3.len == TestS2.len) && !memcmp(s3.data, TestS2.data, s3.len);
	 ok_pic = (TestHashSurface(surface) == TestSurfaceHash);

	 if(ok_state && ok_pic)
	  TestPassed++;
	 else
	  TestFailed++;

	 snprintf(TestReport[TestKIdx], sizeof(TestReport[0]),
		  "K=%-4u %s  state %u bytes %s, picture %s%s",
		  TestK[TestKIdx], (ok_state && ok_pic) ? "PASS" : "FAIL",
		  (unsigned)s3.len,
		  ok_state ? "identical" : "DIFFERS",
		  ok_pic ? "identical" : "DIFFERS",
#ifdef LYNX_TEST_POISON
		  ", poisoned");
#else
		  ", poisoned except the display registers (LYNX_TEST_POISON off)");
#endif

	 TestFree(&s3);
	 TestFree(&TestS2);

	 TestKIdx++;

	 if(TestKIdx >= sizeof(TestK) / sizeof(TestK[0]))
	 {
	  TestFree(&TestS1);
	  TestPhase = TEST_DONE;
	 }
	 else
	 {
	  // Back to N for the next length, so every K is measured from the same
	  // starting state and a failure names the length, not the moment.
	  if(!TestLoad(&TestS1))
	  {
	   TestFail("could not read S1 back");
	   break;
	  }

	  TestSeqPos = 0;
	  TestPhase = TEST_RUN;
	 }
	}
	break;
 }

 TestFrame++;
}
#endif

void CloseGame(void)
{
#ifdef LYNXCOM_TEST_STATE
 if(log_cb)
 {
  for(unsigned k = 0; k < sizeof(TestK) / sizeof(TestK[0]); k++)
  {
   if(TestReport[k][0])
    log_cb(RETRO_LOG_INFO, "StateTest: %s\n", TestReport[k]);
  }

  log_cb(RETRO_LOG_INFO, "StateTest: %u passed, %u failed%s\n", TestPassed, TestFailed,
	 (TestPhase == TEST_DONE) ? "" : " -- RUN TOO SHORT, not every length was reached");
 }

 TestFree(&TestS1);
 TestFree(&TestS2);

 TestPhase = TEST_ARM;
 TestKIdx = 0;
 TestFrame = 0;
 TestPassed = 0;
 TestFailed = 0;
 TestPoisonSeq = 0;
 memset(TestReport, 0, sizeof(TestReport));
#endif

 Cleanup();
}

//
// Paints the 160-pixel-wide column starting at x_offset black, on every line
// that line_drawn says the machine did not draw.  A NULL line_drawn means the
// whole column -- used for a screen slot with no machine behind it.
//
static void FillUndrawn(MDFN_Surface* surface, const uint32 color, const unsigned x_offset, const bool* line_drawn)
{
 for(int y = 0; y < LYNXCOM_SCREEN_H; y++)
 {
  if(line_drawn && line_drawn[y])
   continue;

  if(surface->bpp == 16)
  {
   uint16* row = surface->pixels + y * surface->pitch + x_offset;

   for(int x = 0; x < LYNXCOM_SCREEN_W; x++)
    row[x] = color;
  }
  else
  {
   uint32* row = (uint32*)surface->pixels + y * surface->pitch + x_offset;

   for(int x = 0; x < LYNXCOM_SCREEN_W; x++)
    row[x] = color;
  }
 }
}

void Emulate(EmulateSpecStruct *espec)
{
 espec->DisplayRect.x = 0;
 espec->DisplayRect.y = 0;
 espec->DisplayRect.w = LYNXCOM_SURFACE_W;
 espec->DisplayRect.h = LYNXCOM_SCREEN_H;

 for(unsigned i = 0; i < NumMachines; i++)
 {
  if(espec->SoundFormatChanged)
  {
   machine[i]->mMikie->mikbuf.set_sample_rate(espec->SoundRate ? espec->SoundRate : 44100, 60);
   machine[i]->mMikie->mikbuf.clock_rate((long int)(16000000 / 4));
   machine[i]->mMikie->mikbuf.bass_freq(60);
   machine[i]->mMikie->miksynth.volume(0.50);
  }

#ifdef LYNXCOM_TEST_STATE
  // The pads are ignored while the state test runs; it needs the same buttons
  // at the same frames in both passes, which a human at the keyboard cannot
  // give it.
  machine[i]->SetButtonData(TestInput(i, TestInputFrame()));
#else
  machine[i]->SetButtonData(chee[i][0] | (chee[i][1] << 8));
#endif
 }

 MDFNMP_ApplyPeriodicCheats();

 // Frame-local cycle counters.  They start at the skew left over from last
 // frame, so ts[i] - MachineSkew[i] is what machine i ran this frame and
 // ts[i] - ts[0] is the skew to carry into the next one.  64-bit because the
 // core wraps gSystemCycleCount at 0x80000000 and the two machines reach that
 // point at different moments.
 int64 ts[LYNXCOM_MAX_MACHINES];

 // FIXME, we should integrate this into mikie.*
 uint32 color_black;

 if(espec->surface->bpp == 16)
#if defined(ABGR1555)
  color_black = MAKECOLOR_15_1(30, 30, 30, 0);
#else
  color_black = MAKECOLOR_16(30, 30, 30, 0);
#endif
 else
  color_black = MAKECOLOR_32(30, 30, 30, 0);

 for(unsigned i = 0; i < NumMachines; i++)
 {
  ts[i] = MachineSkew[i];

  // Every machine skips or none does.  It has to be all of them, and it has to
  // cost the same either way: DisplayRenderLine charges its 80 RAM accesses per
  // line before it looks at mpSkipFrame, so a skipped frame takes exactly as
  // many cycles as a drawn one and the machines stay level across it.  Anyone
  // tempted to return early from that function to make skipping cheaper would
  // be trading a little time for a link that drifts apart during rewind.
  machine[i]->mMikie->mpSkipFrame = espec->skip;
  machine[i]->mMikie->mpDisplayCurrent = espec->surface;
  machine[i]->mMikie->mpDisplayXOffset = i * LYNXCOM_SCREEN_W;
  machine[i]->mMikie->startTS = ctx[i].gSystemCycleCount;

  // Only the frame master starts a picture here.  Its display ends exactly when
  // this host frame does, so host frame and emulated frame are the same thing
  // for it, just as they are for a single Lynx.
  //
  // Everyone else is mid-picture at this moment, and resetting their line
  // counter and their drawn-line record here would be wrong on both counts: the
  // lines they had left to paint would be blacked out as "undrawn" and then
  // repainted next frame, which is a row of the screen flickering between the
  // game and black, once per frame, forever.  Their bookkeeping happens on
  // their own frame boundary instead, down in the scheduler.
  if(i == 0)
  {
   memset(ctx[i].LynxLineDrawn, 0, sizeof(ctx[i].LynxLineDrawn));
   machine[i]->mMikie->mpDisplayCurrentLine = 0;
  }
 }

 //
 // Machine 1 is the frame master, exactly as the single machine used to be: the
 // frame ends when its display does.  Within the frame the machine that is
 // furthest behind always runs next, for at most a quantum at a time, which is
 // what keeps the pair interleaved finely enough for the link.
 //
 // Switching only ever happens between top-level CSystem::Update() calls, i.e.
 // on CPU instruction boundaries.  That is what makes the file-scope scratch in
 // susie.cpp safe: PaintSprites() is reached only from CMikie::Poke(CPUSLEEP)
 // (mikie.cpp:1000-1001), so it always begins and ends inside a single Update().
 //
 unsigned active = 0;

 RestoreGlobals(&ctx[active]);

 while(machine[0]->mMikie->mpDisplayCurrent && ts[0] < 700000)
 {
  unsigned m = 0;

  for(unsigned i = 1; i < NumMachines; i++)
  {
   if(ts[i] < ts[m])
    m = i;
  }

  if(m != active)
  {
   SaveGlobals(&ctx[active]);
   RestoreGlobals(&ctx[m]);
   active = m;
  }

  const int64 target = ts[m] + SYNC_QUANTUM;

  while(ts[m] < target)
  {
   const uint32 pre = gSystemCycleCount;

   machine[m]->Update();

   // CMikie::Update() subtracts 0x80000000 from the cycle counter once it gets
   // near the top of the range, which is the only way this can come out
   // negative.
   int64 d = (int64)gSystemCycleCount - (int64)pre;

   if(d < 0)
    d += 0x80000000;

   ts[m] += d;

   if(!machine[m]->mMikie->mpDisplayCurrent)
   {
    if(m == 0)
     break;

    // This machine just finished a picture of its own, mid-host-frame.  That
    // is its frame boundary, so this is where its drawn-line record is spent
    // and reset -- doing it on the host's boundary instead leaves whatever it
    // had not painted yet to be blacked out and repainted, i.e. flicker.
    FillUndrawn(espec->surface, color_black, m * LYNXCOM_SCREEN_W, LynxLineDrawn);
    memset(LynxLineDrawn, 0, sizeof(LynxLineDrawn));

    // Re-arming is not cosmetic.  DisplayRenderLine() bails out early while the
    // display pointer is NULL and so never charges the 80 RAM accesses a line
    // costs (mikie.cpp:492 against mikie.cpp:531), which would let a machine
    // outrun real hardware and drag the link off with it.  Hence the check
    // after every Update() rather than once per quantum.
    machine[m]->mMikie->mpDisplayCurrent = espec->surface;
    machine[m]->mMikie->mpDisplayCurrentLine = 0;
   }
  }
 }

 SaveGlobals(&ctx[active]);

 int64 elapsed[LYNXCOM_MAX_MACHINES];

 for(unsigned i = 0; i < NumMachines; i++)
 {
  elapsed[i] = ts[i] - MachineSkew[i];
  MachineSkew[i] = ts[i] - ts[0];
 }

 // The frame master's picture ends with this host frame, so its undrawn lines
 // are settled here.  Every other machine settled its own further up, on its own
 // frame boundary; a screen slot with no machine behind it is simply all black.
 FillUndrawn(espec->surface, color_black, 0, ctx[0].LynxLineDrawn);

 for(unsigned i = NumMachines; i < LYNXCOM_SCREENS; i++)
  FillUndrawn(espec->surface, color_black, i * LYNXCOM_SCREEN_W, NULL);

 if(espec->SoundBuf)
 {
  // Sound is closed off against the cycles its own machine ran, not against the
  // frame master's: the two differ by at most a quantum, and only the machine's
  // own count matches the timestamps CombobulateSound() wrote with.
  machine[LynxComSoundMachine]->mMikie->mikbuf.end_frame((blip_time_t)(elapsed[LynxComSoundMachine] >> 2));
  espec->SoundBufSize = machine[LynxComSoundMachine]->mMikie->mikbuf.read_samples(espec->SoundBuf, espec->SoundBufMaxSize) / 2; // divide by nr audio chn
 }
 else
  espec->SoundBufSize = 0;

 // Whatever the unheard machines piled up has to go somewhere, or Blip_Buffer
 // eventually overflows.  This also puts their buffer offset back to zero,
 // which is what startTS being re-based every frame assumes.
 for(unsigned i = 0; i < NumMachines; i++)
 {
  if(i != LynxComSoundMachine)
   machine[i]->mMikie->mikbuf.clear();
 }

#ifdef LYNXCOM_TEST_STATE
 // The end of the frame in every sense -- picture finished, audio closed off --
 // so a state taken here is the one the frontend's own save key would take.
 TestStateStep(espec->skip ? NULL : espec->surface);
#endif
}

void SetInput(unsigned port, const char *type, uint8 *ptr)
{
 if(port < LYNXCOM_SCREENS)
  chee[port] = (uint8 *)ptr;
}

int StateAction(StateMem *sm, int load, int data_only)
{
 int ret = 1;

 // Section names are prefixed per machine because the state section namespace
 // is flat and a duplicate name would be found twice.  Lending the globals to
 // each machine in turn is what lets CSystem::StateAction serialize them with
 // no new code: the SFVAR entries for gSystemCycleCount and friends point at
 // the live globals.
 for(unsigned i = 0; i < NumMachines; i++)
 {
  RestoreGlobals(&ctx[i]);
  ret &= machine[i]->StateAction(sm, load, data_only, i ? "M2_" : "M1_");
  SaveGlobals(&ctx[i]);		// a no-op when saving; on load this is how the values arrive
 }

 // Scheduler state.  Without it a loaded state would keep whatever skew the
 // running session happened to have, which is a link desynchronization that no
 // amount of correct per-machine state can undo.
 SFORMAT LinkRegs[] =
 {
  SFARRAY32N(MachineSkew, LYNXCOM_MAX_MACHINES, "MachineSkew"),
  SFEND
 };

 ret &= MDFNSS_StateAction(sm, load, data_only, LinkRegs, "COML", false);

 return ret;
}

void DoSimpleCommand(int cmd)
{
 switch(cmd)
 {
  case MDFN_MSC_POWER:
  case MDFN_MSC_RESET:
	for(unsigned i = 0; i < NumMachines; i++)
	{
	 RestoreGlobals(&ctx[i]);
	 machine[i]->Reset();
	 SaveGlobals(&ctx[i]);
	}

	// Every CRam::Reset() re-registers its own RAM over machine 1's, so
	// the cheat mapping has to be put back.
	MDFNMP_AddRAM(65536, 0x0000, machine[0]->GetRamPointer());

	// The link needs no re-arming: Reset() empties the receive queue but
	// leaves the cable flag and the Tx callback alone -- CMikie sets those
	// two in its constructor only (mikie.cpp:69-70).
	//
	// Reset puts every cycle counter back to zero, so the machines are
	// level again by definition.
	memset(MachineSkew, 0, sizeof(MachineSkew));
	break;
 }
}

static const InputDeviceInputInfoStruct IDII[] =
{
 { "a", "A (outer)", 8, IDIT_BUTTON_CAN_RAPID, NULL },
 { "b", "B (inner)", 7, IDIT_BUTTON_CAN_RAPID, NULL },
 { "option_2", "Option 2 (lower)", 5, IDIT_BUTTON_CAN_RAPID, NULL },
 { "option_1", "Option 1 (upper)", 4, IDIT_BUTTON_CAN_RAPID, NULL },

 { "left", "LEFT ←", 	/*VIRTB_DPAD0_L,*/ 2, IDIT_BUTTON, "right",		{ "up", "right", "down" } },
 { "right", "RIGHT →", 	/*VIRTB_DPAD0_R,*/ 3, IDIT_BUTTON, "left", 		{ "down", "left", "up" } },
 { "up", "UP ↑", 	/*VIRTB_DPAD0_U,*/ 0, IDIT_BUTTON, "down",		{ "right", "down", "left" } },
 { "down", "DOWN ↓", 	/*VIRTB_DPAD0_D,*/ 1, IDIT_BUTTON, "up", 		{ "left", "up", "right" } },

 { "pause", "PAUSE", 6, IDIT_BUTTON, NULL },
};

static InputDeviceInfoStruct InputDeviceInfo[] =
{
 {
  "gamepad",
  "Gamepad",
  NULL,
  NULL,
  sizeof(IDII) / sizeof(InputDeviceInputInfoStruct),
  IDII,
 }
};

static const InputPortInfoStruct PortInfo[] =
{
 { "port1", "Machine 1 (left screen)", sizeof(InputDeviceInfo) / sizeof(InputDeviceInfoStruct), InputDeviceInfo, 0 },
 { "port2", "Machine 2 (right screen)", sizeof(InputDeviceInfo) / sizeof(InputDeviceInfoStruct), InputDeviceInfo, 0 }
};

static InputInfoStruct InputInfo =
{
 sizeof(PortInfo) / sizeof(InputPortInfoStruct),
 PortInfo
};

MDFNGI EmulatedLynx =
{
 LYNXCOM_SURFACE_W,	// lcm_width
 LYNXCOM_SCREEN_H,	// lcm_height
 NULL,  // Dummy


 LYNXCOM_SURFACE_W,	// Nominal width
 LYNXCOM_SCREEN_H,	// Nominal height

 LYNXCOM_SURFACE_W,	// Framebuffer width
 LYNXCOM_SCREEN_H,	// Framebuffer height
};
