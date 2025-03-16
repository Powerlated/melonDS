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

#ifndef GPU2D_H
#define GPU2D_H

#include "types.h"
#include "Savestate.h"

namespace melonDS
{
class GPU;

namespace GPU2D
{
struct SharedBuffers {
    alignas(8) u32 OBJLine[256];
    alignas(8) u8 OBJWindow[256];
    u8 NumSprites;
};
struct PreparedBuffers {
    u32 RenderedVRAMDisplay[256];
    alignas(u64) u8 WindowMask[256];  
    u16 Line;
    bool GPU3D_IsRendererAccelerated;
    u32 GPU3D_RenderXPos;
    u32 GPU_VRAMMap_LCDC;
    bool ForceBlank;

    u8 *Palette;
    u8 *OAM; 
};
struct UnitState {
    /* Config variables - doesn't change */
    u32 Num;

    /* Affected by registers */
    bool Enabled;

    u16 DispFIFOBuffer[256];

    u32 DispCnt;
    u16 BGCnt[4];

    u16 BGXPos[4];
    u16 BGYPos[4];

    s32 BGXRef[2];
    s32 BGYRef[2];
    s32 BGXRefInternal[2];
    s32 BGYRefInternal[2];
    s16 BGRotA[2];
    s16 BGRotB[2];
    s16 BGRotC[2];
    s16 BGRotD[2];

    u8 Win0Coords[4];
    u8 Win1Coords[4];
    u8 WinCnt[4];
    u32 Win0Active;
    u32 Win1Active;

    u8 BGMosaicSize[2];
    u8 OBJMosaicSize[2];
    u8 BGMosaicY, BGMosaicYMax;
    u8 OBJMosaicYCount, OBJMosaicY, OBJMosaicYMax;

    u16 BlendCnt;
    u16 BlendAlpha;
    u8 EVA, EVB;
    u8 EVY;

    bool CaptureLatch;
    u32 CaptureCnt;

    u16 MasterBrightness;

    const PreparedBuffers *Prepared;
    SharedBuffers *Shared;
    
    /* Pointer to the GPU - TO BE USED FOR FLAT VRAM ACCESS AND DISPLAY CAPTURE ONLY */
    GPU *GPU;

    /* Framebuffer */ 
    u32 *Framebuffer;
};

class Unit
{
public:
    // take a reference to the GPU so we can access its state
    // and ensure that it's not null
    Unit(u32 num, melonDS::GPU& gpu);
    virtual ~Unit() = default;
    Unit(const Unit&) = delete;
    Unit& operator=(const Unit&) = delete;

    void Reset();

    void DoSavestate(Savestate* file);

    void SetEnabled(bool enable) { State.Enabled = enable; }

    
    u8 Read8(u32 addr);
    u16 Read16(u32 addr);
    u32 Read32(u32 addr);
    void Write8(u32 addr, u8 val);
    void Write16(u32 addr, u16 val);
    void Write32(u32 addr, u32 val);
    
    bool UsesFIFO() const
    {
        if (((State.DispCnt >> 16) & 0x3) == 3)
        return true;
        if ((State.CaptureCnt & (1<<25)) && ((State.CaptureCnt >> 29) & 0x3) != 0)
        return true;
        
        return false;
    }
    
    void DequeueDispFIFO(u32 offset, u32 num);

    void CheckWindows(u32 line);
    void CalculateWindowMask(u8* windowMask, const u8* objWindow);
    
    void PrepareToDrawScanline(u32 line);
    void PrepareToDrawSprites(u32 line);
    void AfterDrawingScanline();
    void VBlank();
    virtual void VBlankEnd();

    u16 DispFIFO[16];
    u32 DispFIFOReadPtr;
    u32 DispFIFOWritePtr;

    UnitState State, ShadowState;
    
    /* For threading. These are all prepared by Unit::PrepareToDrawScanline() */
    alignas(u64) u8 ShadowPalette[1024];
    alignas(u64) u8 ShadowOAM[1024];
    
    PreparedBuffers Prepared;
    SharedBuffers Shared;

private:
    melonDS::GPU& GPU;
};

}
}
#endif
