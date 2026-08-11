//
// Copyright (c) 2004 K. Wilkins
//
// This software is provided 'as-is', without any express or implied warranty.
// In no event will the authors be held liable for any damages arising from
// the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
//
// 2. Altered source versions must be plainly marked as such, and must not
//    be misrepresented as being the original software.
//
// 3. This notice may not be removed or altered from any source distribution.
//

//////////////////////////////////////////////////////////////////////////////
//                       Handy - An Atari Lynx Emulator                     //
//                          Copyright (c) 1996,1997                         //
//                                 K. Wilkins                               //
//////////////////////////////////////////////////////////////////////////////
// System object class                                                      //
//////////////////////////////////////////////////////////////////////////////
//                                                                          //
// This class provides the glue to bind of of the emulation objects         //
// together via peek/poke handlers and pass thru interfaces to lower        //
// objects, all control of the emulator is done via this class. Update()    //
// does most of the work and each call emulates one CPU instruction and     //
// updates all of the relevant hardware if required. It must be remembered  //
// that if that instruction involves setting SPRGO then, it will cause a    //
// sprite painting operation and then a corresponding update of all of the  //
// hardware which will usually involve recursive calls to Update, see       //
// Mikey SPRGO code for more details.                                       //
//                                                                          //
//    K. Wilkins                                                            //
// August 1997                                                              //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////
// Revision History:                                                        //
// -----------------                                                        //
//                                                                          //
// 01Aug1997 KW Document header added & class documented.                   //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////

#define SYSTEM_CPP

#include "../lynx/system.h"
#include "../mednafen-endian.h"

#include "../general.h"
#include "../mempatcher.h"
#include "../md5.h"
#include "../settings.h"

CSystem::CSystem(MDFNFILE *fp, const char *bios_path)
	:mCart(NULL),
	mRom(NULL),
	mMemMap(NULL),
	mRam(NULL),
	mCpu(NULL),
	mMikie(NULL),
	mSusie(NULL)
{
	mFileType=HANDY_FILETYPE_ILLEGAL;
	mSleepAdvanceLimit = 0;		// Set by the driving code, not by Reset()

	char clip[11];
	file_read(fp, clip, 11, 1);
	file_seek(fp, 0, SEEK_SET);
	clip[4]=0;
	clip[10]=0;

	if(!strcmp(&clip[6],"BS93"))
     mFileType=HANDY_FILETYPE_HOMEBREW;
	else if(!strcmp(&clip[0],"LYNX"))
     mFileType=HANDY_FILETYPE_LNX;
	else if(fp->size==128*1024 || fp->size==256*1024 || fp->size==512*1024)
		/* Invalid Cart (type). but 128/256/512k size -> set to RAW and try to load raw rom image */
		mFileType=HANDY_FILETYPE_RAW;

	// Create the system objects that we'll use

	// Attempt to load the cartridge errors caught above here...

	mRom = new CRom(bios_path);

	// An exception from this will be caught by the level above

	switch(mFileType)
	{
		case HANDY_FILETYPE_RAW:
		case HANDY_FILETYPE_LNX:
			mCart = new CCart(fp);
			mRam = new CRam(NULL);
			break;
		case HANDY_FILETYPE_HOMEBREW:
			mCart = new CCart(NULL);
			mRam = new CRam(fp);
			break;
		case HANDY_FILETYPE_SNAPSHOT:
		case HANDY_FILETYPE_ILLEGAL:
		default:
			mCart = new CCart(NULL);
			mRam = new CRam(NULL);
			break;
	}

	// These can generate exceptions

	mMikie = new CMikie(*this);
	mSusie = new CSusie(*this);

	// Instantiate the memory map handler
	mMemMap = new CMemMap(*this);

	// Now the handlers are set we can instantiate the CPU as is will use handlers on reset
	mCpu = new C65C02(*this);

	// Now init is complete do a reset, this will cause many things to be reset twice
	// but what the hell, who cares, I don't.....
	Reset();
}

CSystem::~CSystem()
{
	// Cleanup all our objects

	if(mCart!=NULL) delete mCart;
	if(mRom!=NULL) delete mRom;
	if(mRam!=NULL) delete mRam;
	if(mCpu!=NULL) delete mCpu;
	if(mMikie!=NULL) delete mMikie;
	if(mSusie!=NULL) delete mSusie;
	if(mMemMap!=NULL) delete mMemMap;
}

void CSystem::Reset(void)
{
	mMikie->startTS -= gSystemCycleCount;
	gSystemCycleCount=0;
	gNextTimerEvent=0;
	gCPUBootAddress=0;
	gSystemIRQ=false;
	gSystemNMI=false;
	gSystemCPUSleep=false;
	gSystemHalt=false;
	gSuzieDoneTime = 0;

	mMemMap->Reset();
	mCart->Reset();
	mRom->Reset();
	mRam->Reset();
	mMikie->Reset();
	mSusie->Reset();
	mCpu->Reset();

	// Homebrew hashup

	if(mFileType==HANDY_FILETYPE_HOMEBREW)
	{
		mMikie->PresetForHomebrew();

		C6502_REGS regs;
		mCpu->GetRegs(regs);
		regs.PC=(uint16)gCPUBootAddress;
		mCpu->SetRegs(regs);
	}
}

// Somewhat of a hack to make sure undrawn lines are black.
bool LynxLineDrawn[256];

int CSystem::StateAction(StateMem *sm, int load, int data_only, const char* sname_prefix)
{
 SFORMAT SystemRegs[] =
 {
	SFVAR(gSuzieDoneTime),
        SFVAR(gSystemCycleCount),
        SFVAR(gNextTimerEvent),
        SFVAR(gCPUBootAddress),
        SFVAR(gSystemIRQ),
        SFVAR(gSystemNMI),
        SFVAR(gSystemCPUSleep),
        SFVAR(gSystemHalt),
	SFARRAYN(GetRamPointer(), RAM_SIZE, "RAM"),
	// Which lines the machine has painted so far this frame; the rest are
	// filled with black when the frame ends.  For a single Lynx a state is
	// always taken between frames, where this has just been spent and is
	// about to be cleared, so it never mattered.  Machine 2 of a ComLynx
	// pair is mid-picture at that moment, and restoring it without this
	// blacks out a band of the lines it had already drawn.
	SFARRAYB(LynxLineDrawn, 256),
	SFEND
 };
 char section_name[64];

 snprintf(section_name, sizeof(section_name), "%sSYST", sname_prefix);

 int ret = MDFNSS_StateAction(sm, load, data_only, SystemRegs, section_name, false);
 ret &= mSusie->StateAction(sm, load, data_only, sname_prefix);
 ret &= mMemMap->StateAction(sm, load, data_only, sname_prefix);
 ret &= mCart->StateAction(sm, load, data_only, sname_prefix);
 ret &= mMikie->StateAction(sm, load, data_only, sname_prefix);
 ret &= mCpu->StateAction(sm, load, data_only, sname_prefix);
 return ret;
}

