//-----------------------------------------------------------------------------
//
// This code is licensed to you under the terms of the GNU GPL, version 2 or,
// at your option, any later version. See the LICENSE.txt file for the text of
// the license.
//-----------------------------------------------------------------------------
// Low frequency Farpoint / Pyramid commands
//-----------------------------------------------------------------------------
#ifndef CMDLFPYRAMID_H__
#define CMDLFPYRAMID_H__
#include <string.h>
#include <inttypes.h>
#include "proxmark3.h"
#include "ui.h"
#include "util.h"
#include "graph.h"
#include "cmdparser.h"
#include "cmddata.h"
#include "cmdmain.h"
#include "cmdlf.h"
#include "protocols.h"  // for T55xx config register definitions
#include "lfdemod.h"    // parityTest
#include "crc.h"

int CmdLFPyramid(const char *Cmd);
int CmdPyramidDemod(const char *Cmd);
int CmdPyramidRead(const char *Cmd);
int CmdPyramidClone(const char *Cmd);
int CmdPyramidSim(const char *Cmd);

int detectPyramid(uint8_t *dest, size_t *size, int *waveStartIdx);
#endif

