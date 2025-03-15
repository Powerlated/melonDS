/*
    Copyright 2016-2024 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <stdio.h>
#include <string.h>
#include "NDS.h"
#include "GPU.h"
#include "GPU3D.h"

namespace melonDS
{
using Platform::Log;
using Platform::LogLevel;

// notes on color conversion
//
// * BLDCNT special effects are applied on 18bit colors
// -> layers are converted to 18bit before being composited
// -> 'brightness up' effect does: x = x + (63-x)*factor
// * colors are converted as follows: 18bit = 15bit * 2
// -> white comes out as 62,62,62 and not 63,63,63
// * VRAM/FIFO display modes convert colors the same way
// * 3D engine converts colors differently (18bit = 15bit * 2 + 1, except 0 = 0)
// * 'screen disabled' white is 63,63,63
// * [Gericom] bit15 is used as bottom green bit for palettes. TODO: check where this applies.
//   tested on the normal BG palette and applies there
//
// for VRAM display mode, VRAM must be mapped to LCDC
//
// FIFO display mode:
// * the 'FIFO' is a circular buffer of 32 bytes (16 pixels)
// * the buffer doesn't get empty, the display controller keeps reading from it
// -> if it isn't updated, the contents will be repeated every 16 pixels
// * the write pointer is incremented when writing to the higher 16 bits of the FIFO register (0x04000068)
// * the write pointer is reset upon VBlank
// * FIFO DMA (mode 4) is triggered every 8 pixels. start bit is cleared upon VBlank.
//
// sprite blending rules
// * destination must be selected as 2nd target
// * sprite must be semitransparent or bitmap sprite
// * blending is applied instead of the selected color effect, even if it is 'none'.
// * for bitmap sprites: EVA = alpha+1, EVB = 16-EVA
// * for bitmap sprites: alpha=0 is always transparent, even if blending doesn't apply
//
// 3D blending rules
//
// 3D/3D blending seems to follow these equations:
//   dstColor = srcColor*srcAlpha + dstColor*(1-srcAlpha)
//   dstAlpha = max(srcAlpha, dstAlpha)
// blending isn't applied if dstAlpha is zero.
//
// 3D/2D blending rules
// * if destination selected as 2nd target:
//   blending is applied instead of the selected color effect, using full 5bit alpha from 3D layer
//   this even if the selected color effect is 'none'.
//   apparently this works even if BG0 isn't selected as 1st target
// * if BG0 is selected as 1st target, destination not selected as 2nd target:
//   brightness up/down effect is applied if selected. if blending is selected, it doesn't apply.
// * 3D layer pixels with alpha=0 are always transparent.
//
// mosaic:
// * mosaic grid starts at 0,0 regardless of the BG/sprite position
// * when changing it midframe: new X setting is applied immediately, new Y setting is applied only
//   after the end of the current mosaic row (when Y counter needs reloaded)
// * for rotscaled sprites: coordinates that are inside the sprite are clamped to the sprite region
//   after being transformed for mosaic

// TODO: master brightness, display capture and mainmem FIFO are separate circuitry, distinct from
// the tile renderers.
// for example these aren't affected by POWCNT GPU-disable bits.
// to model the hardware more accurately, the relevant logic should be moved to GPU.cpp.

namespace GPU2D
{
Unit::Unit(u32 num, melonDS::GPU& gpu) : GPU(gpu)
{
    State.Num = num;
}

void Unit::Reset()
{
    State.Enabled = false;
    State.DispCnt = 0;
    memset(State.BGCnt, 0, 4*2);
    memset(State.BGXPos, 0, 4*2);
    memset(State.BGYPos, 0, 4*2);
    memset(State.BGXRef, 0, 2*4);
    memset(State.BGYRef, 0, 2*4);
    memset(State.BGXRefInternal, 0, 2*4);
    memset(State.BGYRefInternal, 0, 2*4);
    memset(State.BGRotA, 0, 2*2);
    memset(State.BGRotB, 0, 2*2);
    memset(State.BGRotC, 0, 2*2);
    memset(State.BGRotD, 0, 2*2);

    memset(State.Win0Coords, 0, 4);
    memset(State.Win1Coords, 0, 4);
    memset(State.WinCnt, 0, 4);

    State.Win0Active = 0;
    State.Win1Active = 0;

    State.BGMosaicSize[0] = 0;
    State.BGMosaicSize[1] = 0;
    State.OBJMosaicSize[0] = 0;
    State.OBJMosaicSize[1] = 0;
    State.BGMosaicY = 0;
    State.BGMosaicYMax = 0;
    State.OBJMosaicY = 0;
    State.OBJMosaicYMax = 0;
    State.OBJMosaicYCount = 0;

    State.BlendCnt = 0;
    State.EVA = 16;
    State.EVB = 0;
    State.EVY = 0;

    memset(State.DispFIFO, 0, 16*2);
    State.DispFIFOReadPtr = 0;
    State.DispFIFOWritePtr = 0;

    memset(State.DispFIFOBuffer, 0, 256*2);

    State.CaptureCnt = 0;
    State.CaptureLatch = false;

    State.MasterBrightness = 0;
}

void Unit::DoSavestate(Savestate* file)
{
    file->Section((char*)(State.Num ? "GP2B" : "GP2A"));

    file->Var32(&State.DispCnt);
    file->VarArray(State.BGCnt, 4*2);
    file->VarArray(State.BGXPos, 4*2);
    file->VarArray(State.BGYPos, 4*2);
    file->VarArray(State.BGXRef, 2*4);
    file->VarArray(State.BGYRef, 2*4);
    file->VarArray(State.BGXRefInternal, 2*4);
    file->VarArray(State.BGYRefInternal, 2*4);
    file->VarArray(State.BGRotA, 2*2);
    file->VarArray(State.BGRotB, 2*2);
    file->VarArray(State.BGRotC, 2*2);
    file->VarArray(State.BGRotD, 2*2);

    file->VarArray(State.Win0Coords, 4);
    file->VarArray(State.Win1Coords, 4);
    file->VarArray(State.WinCnt, 4);

    file->VarArray(State.BGMosaicSize, 2);
    file->VarArray(State.OBJMosaicSize, 2);
    file->Var8(&State.BGMosaicY);
    file->Var8(&State.BGMosaicYMax);
    file->Var8(&State.OBJMosaicY);
    file->Var8(&State.OBJMosaicYMax);

    file->Var16(&State.BlendCnt);
    file->Var16(&State.BlendAlpha);
    file->Var8(&State.EVA);
    file->Var8(&State.EVB);
    file->Var8(&State.EVY);

    file->Var16(&State.MasterBrightness);

    if (!State.Num)
    {
        file->VarArray(State.DispFIFO, 16*2);
        file->Var32(&State.DispFIFOReadPtr);
        file->Var32(&State.DispFIFOWritePtr);

        file->VarArray(State.DispFIFOBuffer, 256*2);

        file->Var32(&State.CaptureCnt);
    }

    file->Var32(&State.Win0Active);
    file->Var32(&State.Win1Active);
}

u8 Unit::Read8(u32 addr)
{
    switch (addr & 0x00000FFF)
    {
    case 0x000: return State.DispCnt & 0xFF;
    case 0x001: return (State.DispCnt >> 8) & 0xFF;
    case 0x002: return (State.DispCnt >> 16) & 0xFF;
    case 0x003: return State.DispCnt >> 24;

    case 0x008: return State.BGCnt[0] & 0xFF;
    case 0x009: return State.BGCnt[0] >> 8;
    case 0x00A: return State.BGCnt[1] & 0xFF;
    case 0x00B: return State.BGCnt[1] >> 8;
    case 0x00C: return State.BGCnt[2] & 0xFF;
    case 0x00D: return State.BGCnt[2] >> 8;
    case 0x00E: return State.BGCnt[3] & 0xFF;
    case 0x00F: return State.BGCnt[3] >> 8;

    case 0x048: return State.WinCnt[0];
    case 0x049: return State.WinCnt[1];
    case 0x04A: return State.WinCnt[2];
    case 0x04B: return State.WinCnt[3];

    // there are games accidentally trying to read those
    // those are write-only
    case 0x04C:
    case 0x04D: return 0;
    }

    Log(LogLevel::Debug, "unknown GPU read8 %08X\n", addr);
    return 0;
}

u16 Unit::Read16(u32 addr)
{
    switch (addr & 0x00000FFF)
    {
    case 0x000: return State.DispCnt & 0xFFFF;
    case 0x002: return State.DispCnt >> 16;

    case 0x008: return State.BGCnt[0];
    case 0x00A: return State.BGCnt[1];
    case 0x00C: return State.BGCnt[2];
    case 0x00E: return State.BGCnt[3];

    case 0x048: return State.WinCnt[0] | (State.WinCnt[1] << 8);
    case 0x04A: return State.WinCnt[2] | (State.WinCnt[3] << 8);

    case 0x050: return State.BlendCnt;
    case 0x052: return State.BlendAlpha;
    // BLDY is write-only

    case 0x064: return State.CaptureCnt & 0xFFFF;
    case 0x066: return State.CaptureCnt >> 16;

    case 0x06C: return State.MasterBrightness;
    }

    Log(LogLevel::Debug, "unknown GPU read16 %08X\n", addr);
    return 0;
}

u32 Unit::Read32(u32 addr)
{
    switch (addr & 0x00000FFF)
    {
    case 0x000: return State.DispCnt;

    case 0x064: return State.CaptureCnt;
    }

    return Read16(addr) | (Read16(addr+2) << 16);
}

void Unit::Write8(u32 addr, u8 val)
{
    switch (addr & 0x00000FFF)
    {
    case 0x000:
        State.DispCnt = (State.DispCnt & 0xFFFFFF00) | val;
        if (State.Num) State.DispCnt &= 0xC0B1FFF7;
        return;
    case 0x001:
        State.DispCnt = (State.DispCnt & 0xFFFF00FF) | (val << 8);
        if (State.Num) State.DispCnt &= 0xC0B1FFF7;
        return;
    case 0x002:
        State.DispCnt = (State.DispCnt & 0xFF00FFFF) | (val << 16);
        if (State.Num) State.DispCnt &= 0xC0B1FFF7;
        return;
    case 0x003:
        State.DispCnt = (State.DispCnt & 0x00FFFFFF) | (val << 24);
        if (State.Num) State.DispCnt &= 0xC0B1FFF7;
        return;

    case 0x10:
        if (!State.Num) GPU.GPU3D.SetRenderXPos((GPU.GPU3D.GetRenderXPos() & 0xFF00) | val);
        break;
    case 0x11:
        if (!State.Num) GPU.GPU3D.SetRenderXPos((GPU.GPU3D.GetRenderXPos() & 0x00FF) | (val << 8));
        break;
    }

    if (!State.Enabled) return;

    switch (addr & 0x00000FFF)
    {
    case 0x008: State.BGCnt[0] = (State.BGCnt[0] & 0xFF00) | val; return;
    case 0x009: State.BGCnt[0] = (State.BGCnt[0] & 0x00FF) | (val << 8); return;
    case 0x00A: State.BGCnt[1] = (State.BGCnt[1] & 0xFF00) | val; return;
    case 0x00B: State.BGCnt[1] = (State.BGCnt[1] & 0x00FF) | (val << 8); return;
    case 0x00C: State.BGCnt[2] = (State.BGCnt[2] & 0xFF00) | val; return;
    case 0x00D: State.BGCnt[2] = (State.BGCnt[2] & 0x00FF) | (val << 8); return;
    case 0x00E: State.BGCnt[3] = (State.BGCnt[3] & 0xFF00) | val; return;
    case 0x00F: State.BGCnt[3] = (State.BGCnt[3] & 0x00FF) | (val << 8); return;

    case 0x010: State.BGXPos[0] = (State.BGXPos[0] & 0xFF00) | val; return;
    case 0x011: State.BGXPos[0] = (State.BGXPos[0] & 0x00FF) | (val << 8); return;
    case 0x012: State.BGYPos[0] = (State.BGYPos[0] & 0xFF00) | val; return;
    case 0x013: State.BGYPos[0] = (State.BGYPos[0] & 0x00FF) | (val << 8); return;
    case 0x014: State.BGXPos[1] = (State.BGXPos[1] & 0xFF00) | val; return;
    case 0x015: State.BGXPos[1] = (State.BGXPos[1] & 0x00FF) | (val << 8); return;
    case 0x016: State.BGYPos[1] = (State.BGYPos[1] & 0xFF00) | val; return;
    case 0x017: State.BGYPos[1] = (State.BGYPos[1] & 0x00FF) | (val << 8); return;
    case 0x018: State.BGXPos[2] = (State.BGXPos[2] & 0xFF00) | val; return;
    case 0x019: State.BGXPos[2] = (State.BGXPos[2] & 0x00FF) | (val << 8); return;
    case 0x01A: State.BGYPos[2] = (State.BGYPos[2] & 0xFF00) | val; return;
    case 0x01B: State.BGYPos[2] = (State.BGYPos[2] & 0x00FF) | (val << 8); return;
    case 0x01C: State.BGXPos[3] = (State.BGXPos[3] & 0xFF00) | val; return;
    case 0x01D: State.BGXPos[3] = (State.BGXPos[3] & 0x00FF) | (val << 8); return;
    case 0x01E: State.BGYPos[3] = (State.BGYPos[3] & 0xFF00) | val; return;
    case 0x01F: State.BGYPos[3] = (State.BGYPos[3] & 0x00FF) | (val << 8); return;

    case 0x040: State.Win0Coords[1] = val; return;
    case 0x041: State.Win0Coords[0] = val; return;
    case 0x042: State.Win1Coords[1] = val; return;
    case 0x043: State.Win1Coords[0] = val; return;

    case 0x044: State.Win0Coords[3] = val; return;
    case 0x045: State.Win0Coords[2] = val; return;
    case 0x046: State.Win1Coords[3] = val; return;
    case 0x047: State.Win1Coords[2] = val; return;

    case 0x048: State.WinCnt[0] = val; return;
    case 0x049: State.WinCnt[1] = val; return;
    case 0x04A: State.WinCnt[2] = val; return;
    case 0x04B: State.WinCnt[3] = val; return;

    case 0x04C:
        State.BGMosaicSize[0] = val & 0xF;
        State.BGMosaicSize[1] = val >> 4;
        return;
    case 0x04D:
        State.OBJMosaicSize[0] = val & 0xF;
        State.OBJMosaicSize[1] = val >> 4;
        return;

    case 0x050: State.BlendCnt = (State.BlendCnt & 0x3F00) | val; return;
    case 0x051: State.BlendCnt = (State.BlendCnt & 0x00FF) | (val << 8); return;
    case 0x052:
        State.BlendAlpha = (State.BlendAlpha & 0x1F00) | (val & 0x1F);
        State.EVA = val & 0x1F;
        if (State.EVA > 16) State.EVA = 16;
        return;
    case 0x053:
        State.BlendAlpha = (State.BlendAlpha & 0x001F) | ((val & 0x1F) << 8);
        State.EVB = val & 0x1F;
        if (State.EVB > 16) State.EVB = 16;
        return;
    case 0x054:
        State.EVY = val & 0x1F;
        if (State.EVY > 16) State.EVY = 16;
        return;
    }

    Log(LogLevel::Debug, "unknown GPU write8 %08X %02X\n", addr, val);
}

void Unit::Write16(u32 addr, u16 val)
{
    switch (addr & 0x00000FFF)
    {
    case 0x000:
        State.DispCnt = (State.DispCnt & 0xFFFF0000) | val;
        if (State.Num) State.DispCnt &= 0xC0B1FFF7;
        return;
    case 0x002:
        State.DispCnt = (State.DispCnt & 0x0000FFFF) | (val << 16);
        if (State.Num) State.DispCnt &= 0xC0B1FFF7;
        return;

    case 0x010:
        if (!State.Num) GPU.GPU3D.SetRenderXPos(val);
        break;

    case 0x064:
        State.CaptureCnt = (State.CaptureCnt & 0xFFFF0000) | (val & 0xEF3F1F1F);
        return;

    case 0x066:
        State.CaptureCnt = (State.CaptureCnt & 0xFFFF) | ((val << 16) & 0xEF3F1F1F);
        return;

    case 0x068:
        State.DispFIFO[State.DispFIFOWritePtr] = val;
        return;
    case 0x06A:
        State.DispFIFO[State.DispFIFOWritePtr+1] = val;
        State.DispFIFOWritePtr += 2;
        State.DispFIFOWritePtr &= 0xF;
        return;

    case 0x06C: State.MasterBrightness = val; return;
    }

    if (!State.Enabled) return;

    switch (addr & 0x00000FFF)
    {
    case 0x008: State.BGCnt[0] = val; return;
    case 0x00A: State.BGCnt[1] = val; return;
    case 0x00C: State.BGCnt[2] = val; return;
    case 0x00E: State.BGCnt[3] = val; return;

    case 0x010: State.BGXPos[0] = val; return;
    case 0x012: State.BGYPos[0] = val; return;
    case 0x014: State.BGXPos[1] = val; return;
    case 0x016: State.BGYPos[1] = val; return;
    case 0x018: State.BGXPos[2] = val; return;
    case 0x01A: State.BGYPos[2] = val; return;
    case 0x01C: State.BGXPos[3] = val; return;
    case 0x01E: State.BGYPos[3] = val; return;

    case 0x020: State.BGRotA[0] = val; return;
    case 0x022: State.BGRotB[0] = val; return;
    case 0x024: State.BGRotC[0] = val; return;
    case 0x026: State.BGRotD[0] = val; return;
    case 0x028:
        State.BGXRef[0] = (State.BGXRef[0] & 0xFFFF0000) | val;
        if (GPU.VCount < 192) State.BGXRefInternal[0] = State.BGXRef[0];
        return;
    case 0x02A:
        if (val & 0x0800) val |= 0xF000;
        State.BGXRef[0] = (State.BGXRef[0] & 0xFFFF) | (val << 16);
        if (GPU.VCount < 192) State.BGXRefInternal[0] = State.BGXRef[0];
        return;
    case 0x02C:
        State.BGYRef[0] = (State.BGYRef[0] & 0xFFFF0000) | val;
        if (GPU.VCount < 192) State.BGYRefInternal[0] = State.BGYRef[0];
        return;
    case 0x02E:
        if (val & 0x0800) val |= 0xF000;
        State.BGYRef[0] = (State.BGYRef[0] & 0xFFFF) | (val << 16);
        if (GPU.VCount < 192) State.BGYRefInternal[0] = State.BGYRef[0];
        return;

    case 0x030: State.BGRotA[1] = val; return;
    case 0x032: State.BGRotB[1] = val; return;
    case 0x034: State.BGRotC[1] = val; return;
    case 0x036: State.BGRotD[1] = val; return;
    case 0x038:
        State.BGXRef[1] = (State.BGXRef[1] & 0xFFFF0000) | val;
        if (GPU.VCount < 192) State.BGXRefInternal[1] = State.BGXRef[1];
        return;
    case 0x03A:
        if (val & 0x0800) val |= 0xF000;
        State.BGXRef[1] = (State.BGXRef[1] & 0xFFFF) | (val << 16);
        if (GPU.VCount < 192) State.BGXRefInternal[1] = State.BGXRef[1];
        return;
    case 0x03C:
        State.BGYRef[1] = (State.BGYRef[1] & 0xFFFF0000) | val;
        if (GPU.VCount < 192) State.BGYRefInternal[1] = State.BGYRef[1];
        return;
    case 0x03E:
        if (val & 0x0800) val |= 0xF000;
        State.BGYRef[1] = (State.BGYRef[1] & 0xFFFF) | (val << 16);
        if (GPU.VCount < 192) State.BGYRefInternal[1] = State.BGYRef[1];
        return;

    case 0x040:
        State.Win0Coords[1] = val & 0xFF;
        State.Win0Coords[0] = val >> 8;
        return;
    case 0x042:
        State.Win1Coords[1] = val & 0xFF;
        State.Win1Coords[0] = val >> 8;
        return;

    case 0x044:
        State.Win0Coords[3] = val & 0xFF;
        State.Win0Coords[2] = val >> 8;
        return;
    case 0x046:
        State.Win1Coords[3] = val & 0xFF;
        State.Win1Coords[2] = val >> 8;
        return;

    case 0x048:
        State.WinCnt[0] = val & 0xFF;
        State.WinCnt[1] = val >> 8;
        return;
    case 0x04A:
        State.WinCnt[2] = val & 0xFF;
        State.WinCnt[3] = val >> 8;
        return;

    case 0x04C:
        State.BGMosaicSize[0] = val & 0xF;
        State.BGMosaicSize[1] = (val >> 4) & 0xF;
        State.OBJMosaicSize[0] = (val >> 8) & 0xF;
        State.OBJMosaicSize[1] = val >> 12;
        return;

    case 0x050: State.BlendCnt = val & 0x3FFF; return;
    case 0x052:
        State.BlendAlpha = val & 0x1F1F;
        State.EVA = val & 0x1F;
        if (State.EVA > 16) State.EVA = 16;
        State.EVB = (val >> 8) & 0x1F;
        if (State.EVB > 16) State.EVB = 16;
        return;
    case 0x054:
        State.EVY = val & 0x1F;
        if (State.EVY > 16) State.EVY = 16;
        return;
    }

    //printf("unknown GPU write16 %08X %04X\n", addr, val);
}

void Unit::Write32(u32 addr, u32 val)
{
    switch (addr & 0x00000FFF)
    {
    case 0x000:
        State.DispCnt = val;
        if (State.Num) State.DispCnt &= 0xC0B1FFF7;
        return;

    case 0x064:
        State.CaptureCnt = val & 0xEF3F1F1F;
        return;

    case 0x068:
        State.DispFIFO[State.DispFIFOWritePtr] = val & 0xFFFF;
        State.DispFIFO[State.DispFIFOWritePtr+1] = val >> 16;
        State.DispFIFOWritePtr += 2;
        State.DispFIFOWritePtr &= 0xF;
        return;
    }

    if (State.Enabled)
    {
        switch (addr & 0x00000FFF)
        {
        case 0x028:
            if (val & 0x08000000) val |= 0xF0000000;
            State.BGXRef[0] = val;
            if (GPU.VCount < 192) State.BGXRefInternal[0] = State.BGXRef[0];
            return;
        case 0x02C:
            if (val & 0x08000000) val |= 0xF0000000;
            State.BGYRef[0] = val;
            if (GPU.VCount < 192) State.BGYRefInternal[0] = State.BGYRef[0];
            return;

        case 0x038:
            if (val & 0x08000000) val |= 0xF0000000;
            State.BGXRef[1] = val;
            if (GPU.VCount < 192) State.BGXRefInternal[1] = State.BGXRef[1];
            return;
        case 0x03C:
            if (val & 0x08000000) val |= 0xF0000000;
            State.BGYRef[1] = val;
            if (GPU.VCount < 192) State.BGYRefInternal[1] = State.BGYRef[1];
            return;
        }
    }

    Write16(addr, val&0xFFFF);
    Write16(addr+2, val>>16);
}

void Unit::PrepareToDrawScanline(u32 line)
{
    // Copy Palette, OAM
    // TODO: only copy when dirty
    if (State.Num == 1) {
        if (GPU.Is2DRenderingThreaded) {
            if (GPU.PaletteDirty & 0b1100) {
                GPU.PaletteDirty &= ~0b1100;
                memcpy(ShadowPalette, &GPU.Palette[0x400], 1024);
                State.Palette = ShadowPalette;
            }
            if (GPU.OAMDirty & 0b1100) {
                GPU.OAMDirty &= ~0b1100;
                memcpy(ShadowOAM, &GPU.OAM[0x400], 1024);
                State.OAM = ShadowOAM;
            }
        } else {
            State.Palette = &GPU.Palette[0x400];
            State.OAM = &GPU.OAM[0x400];
        }

        auto bgDirty = GPU.VRAMDirty_BBG.DeriveState(GPU.VRAMMap_BBG, GPU);
        GPU.MakeVRAMFlat_BBGCoherent(bgDirty);
        auto bgExtPalDirty = GPU.VRAMDirty_BBGExtPal.DeriveState(GPU.VRAMMap_BBGExtPal, GPU);
        GPU.MakeVRAMFlat_BBGExtPalCoherent(bgExtPalDirty);
        auto objExtPalDirty = GPU.VRAMDirty_BOBJExtPal.DeriveState(&GPU.VRAMMap_BOBJExtPal, GPU);
        GPU.MakeVRAMFlat_BOBJExtPalCoherent(objExtPalDirty);
    } else {
        if (GPU.Is2DRenderingThreaded) {
            if (GPU.PaletteDirty & 0b0011) {
                GPU.PaletteDirty &= ~0b0011;
                memcpy(ShadowPalette, &GPU.Palette[0], 1024);
                State.Palette = ShadowPalette;
            }
            if (GPU.OAMDirty & 0b0011) {
                GPU.OAMDirty &= ~0b0011;
                memcpy(ShadowOAM, &GPU.OAM[0], 1024);
                State.OAM = ShadowOAM;
            }
        } else {
            State.Palette = &GPU.Palette[0];
            State.OAM = &GPU.OAM[0];
        }
        
        auto bgDirty = GPU.VRAMDirty_ABG.DeriveState(GPU.VRAMMap_ABG, GPU);
        GPU.MakeVRAMFlat_ABGCoherent(bgDirty);
        auto bgExtPalDirty = GPU.VRAMDirty_ABGExtPal.DeriveState(GPU.VRAMMap_ABGExtPal, GPU);
        GPU.MakeVRAMFlat_ABGExtPalCoherent(bgExtPalDirty);
        auto objExtPalDirty = GPU.VRAMDirty_AOBJExtPal.DeriveState(&GPU.VRAMMap_AOBJExtPal, GPU);
        GPU.MakeVRAMFlat_AOBJExtPalCoherent(objExtPalDirty);

        // Render the entire VRAM display scanline here
        bool isVRAMDisplayMode = ((State.DispCnt >> 16) & 3) == 2;
        if (isVRAMDisplayMode) {
            u32 vrambank = (State.DispCnt >> 18) & 0x3;
            if (State.GPU_VRAMMap_LCDC & (1<<vrambank))
            {
                u16* vram = (u16*)GPU.VRAM[vrambank];
                vram = &vram[line * 256];

                for (int i = 0; i < 256; i++)
                {
                    u16 color = vram[i];
                    u8 r = (color & 0x001F) << 1;
                    u8 g = (color & 0x03E0) >> 4;
                    u8 b = (color & 0x7C00) >> 9;

                    State.RenderedVRAMDisplay[i] = r | (g << 8) | (b << 16);
                }
            }
            else
            {
                for (int i = 0; i < 256; i++)
                {
                    State.RenderedVRAMDisplay[i] = 0;
                }
            }
        }
    }

    static_assert(VRAMDirtyGranularity == 512);
    
    State.PrevScanlineSpriteBuffer = &SpriteBuffer;
    State.GPU = &GPU;

    if (State.DispCnt & 0xE000) {
        CalculateWindowMask(State.WindowMask, State.PrevScanlineSpriteBuffer->OBJWindow);
    } else {
        memset(State.WindowMask, 0xFF, sizeof(State.WindowMask));
    }

    // Copy variables for the 2D renderer
    State.GPU3D_IsRendererAccelerated = GPU.GPU3D.IsRendererAccelerated();
    State.GPU3D_RenderXPos = GPU.GPU3D.GetRenderXPos();

    State.GPU_VRAMMap_LCDC = GPU.VRAMMap_LCDC;

    State.ForceBlank = false;

    // scanlines that end up outside of the GPU drawing range
    // (as a result of writing to VCount) are filled white
    if (line > 192) State.ForceBlank = true;

    // GPU B can be completely disabled by POWCNT1
    // oddly that's not the case for GPU A
    if (State.Num && !State.Enabled) State.ForceBlank = true;

    if (line == 0 && State.CaptureCnt & (1 << 31) && !State.ForceBlank)
        State.CaptureLatch = true;

    // Y mosaic uses incrementing 4-bit counters
    // the transformed Y position is updated every time the counter matches the MOSAIC register

    if (State.OBJMosaicYCount == State.OBJMosaicSize[1])
    {
        State.OBJMosaicYCount = 0;
        State.OBJMosaicY = line + 1;
    }
    else
    {
        State.OBJMosaicYCount++;
        State.OBJMosaicYCount &= 0xF;
    }
}

void Unit::PrepareToDrawSprites(u32 line)
{
    if (State.Num == 0)
    {
        auto objDirty = GPU.VRAMDirty_AOBJ.DeriveState(GPU.VRAMMap_AOBJ, GPU);
        GPU.MakeVRAMFlat_AOBJCoherent(objDirty);
    }
    else
    {
        auto objDirty = GPU.VRAMDirty_BOBJ.DeriveState(GPU.VRAMMap_BOBJ, GPU);
        GPU.MakeVRAMFlat_BOBJCoherent(objDirty);
    }

    if (line == 0)
    {
        // reset those counters here
        // TODO: find out when those are supposed to be reset
        // it would make sense to reset them at the end of VBlank
        // however, sprites are rendered one scanline in advance
        // so they need to be reset a bit earlier

        State.OBJMosaicY = 0;
        State.OBJMosaicYCount = 0;
    }
}

void Unit::AfterDrawingScanline()
{
    // Increment affine internal registers
    State.BGXRefInternal[0] += State.BGRotB[0];
    State.BGYRefInternal[0] += State.BGRotD[0];
    State.BGXRefInternal[1] += State.BGRotB[1];
    State.BGYRefInternal[1] += State.BGRotD[1];

    if (State.BGMosaicY >= State.BGMosaicYMax)
    {
        State.BGMosaicY = 0;
        State.BGMosaicYMax = State.BGMosaicSize[1];
    }
    else
        State.BGMosaicY++;

    /*if (State.OBJMosaicY >= State.OBJMosaicYMax)
    {
        State.OBJMosaicY = 0;
        State.OBJMosaicYMax = State.OBJMosaicSize[1];
    }
    else
        State.OBJMosaicY++;*/
}

void Unit::VBlank()
{
    if (State.CaptureLatch)
    {
        State.CaptureCnt &= ~(1<<31);
        State.CaptureLatch = false;
    }

    State.DispFIFOReadPtr = 0;
    State.DispFIFOWritePtr = 0;
}

void Unit::VBlankEnd()
{
    // TODO: find out the exact time this happens
    State.BGXRefInternal[0] = State.BGXRef[0];
    State.BGXRefInternal[1] = State.BGXRef[1];
    State.BGYRefInternal[0] = State.BGYRef[0];
    State.BGYRefInternal[1] = State.BGYRef[1];

    State.BGMosaicY = 0;
    State.BGMosaicYMax = State.BGMosaicSize[1];
    //State.OBJMosaicY = 0;
    //State.OBJMosaicYMax = State.OBJMosaicSize[1];
    //State.OBJMosaicY = 0;
    //State.OBJMosaicYCount = 0;
}

void Unit::SampleFIFO(u32 offset, u32 num)
{
    for (u32 i = 0; i < num; i++)
    {
        u16 val = State.DispFIFO[State.DispFIFOReadPtr];
        State.DispFIFOReadPtr++;
        State.DispFIFOReadPtr &= 0xF;

        State.DispFIFOBuffer[offset+i] = val;
    }
}

void Unit::CheckWindows(u32 line) {
    line &= 0xFF;
    if (line == State.Win0Coords[3])      State.Win0Active &= ~0x1;
    else if (line == State.Win0Coords[2]) State.Win0Active |=  0x1;
    if (line == State.Win1Coords[3])      State.Win1Active &= ~0x1;
    else if (line == State.Win1Coords[2]) State.Win1Active |=  0x1;
}

void Unit::CalculateWindowMask(u8* windowMask, const u8* objWindow)
{
    for (u32 i = 0; i < 256; i++)
        windowMask[i] = State.WinCnt[2]; // window outside

    if (State.DispCnt & (1<<15))
    {
        // OBJ window
        for (int i = 0; i < 256; i++)
        {
            if (objWindow[i])
                windowMask[i] = State.WinCnt[3];
        }
    }

    if (State.DispCnt & (1<<14))
    {
        // window 1
        u8 x1 = State.Win1Coords[0];
        u8 x2 = State.Win1Coords[1];

        for (int i = 0; i < 256; i++)
        {
            if (i == x2)      State.Win1Active &= ~0x2;
            else if (i == x1) State.Win1Active |=  0x2;

            if (State.Win1Active == 0x3) windowMask[i] = State.WinCnt[1];
        }
    }

    if (State.DispCnt & (1<<13))
    {
        // window 0
        u8 x1 = State.Win0Coords[0];
        u8 x2 = State.Win0Coords[1];

        for (int i = 0; i < 256; i++)
        {
            if (i == x2)      State.Win0Active &= ~0x2;
            else if (i == x1) State.Win0Active |=  0x2;

            if (State.Win0Active == 0x3) windowMask[i] = State.WinCnt[0];
        }
    }
}

}
}
