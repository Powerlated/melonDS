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

#ifndef SPU_H
#define SPU_H

#include "Savestate.h"
#include "Platform.h"
#include "MelonResampler.h"

namespace melonDS
{
class NDS;
class SPU;

enum class AudioBitDepth
{
    Auto,
    _10Bit,
    _16Bit,
};

enum class AudioInterpolation
{
    Clean,    // Band-limited zero-order hold - DS-style crunchiness without the nasty aliasing.
    Faithful, // Aliased zero-order hold - True to the hardware. Crunchiness + nasty aliasing.
    Smooth    // Low-passed at the Fs/2 of the instrument sample - No crunchiness, no aliasing.
};

template <typename T>
struct SPUSample {
    T l, r;
};

class SPUChannel
{
public:
    SPUChannel(u32 num, melonDS::NDS& nds);
    void Reset();
    void DoSavestate(Savestate* file);

    static const s8 ADPCMIndexTable[8];
    static const u16 ADPCMTable[89];
    static const s16 PSGTable[8][8];

    const u32 Num;

    u32 Cnt = 0;
    u32 SrcAddr = 0;
    u16 TimerReload = 0;
    u32 LoopPos = 0;
    u32 Length = 0;

    u8 Volume = 0;
    u8 VolumeShift = 0;
    u8 Pan = 0;

    bool KeyOn = false;
    u32 Timer = 0;
    s32 Pos = 0;
    s16 CurSample = 0;
    s32 CurVal = 0;
    u16 NoiseVal = 0;

    s32 ADPCMVal = 0;
    s32 ADPCMIndex = 0;
    s32 ADPCMValLoop = 0;
    s32 ADPCMIndexLoop = 0;
    u8 ADPCMCurByte = 0;

    u32 FIFO[8] {};
    u32 FIFOReadPos = 0;
    u32 FIFOWritePos = 0;
    u32 FIFOReadOffset = 0;
    u32 FIFOLevel = 0;

    void FIFO_BufferData();
    template<typename T> T FIFO_ReadData();

    void SetCnt(u32 val)
    {
        u32 oldcnt = Cnt;
        Cnt = val & 0xFF7F837F;

        Volume = Cnt & 0x7F;
        if (Volume == 127) Volume++;

        const u8 volshift[4] = {4, 3, 2, 0};
        VolumeShift = volshift[(Cnt >> 8) & 0x3];

        Pan = (Cnt >> 16) & 0x7F;
        if (Pan == 127) Pan++;

        if ((val & (1<<31)) && !(oldcnt & (1<<31)))
        {
            KeyOn = true;
        }
    }

    void SetSrcAddr(u32 val) { SrcAddr = val & 0x07FFFFFC; }
    void SetTimerReload(u32 val) { TimerReload = val & 0xFFFF; }
    void SetLoopPos(u32 val) { LoopPos = (val & 0xFFFF) << 2; }
    void SetLength(u32 val) { Length = (val & 0x001FFFFF) << 2; }

    void Start();

    void NextSample_PCM8();
    void NextSample_PCM16();
    void NextSample_ADPCM();
    void NextSample_PSG();
    void NextSample_Noise();

    void MixIntoSampleWithPan(s32 in, SPUSample<s32> &out);

private:
    melonDS::NDS& NDS;
};

class SPUCaptureUnit
{
public:
    SPUCaptureUnit(u32 num, melonDS::NDS&);
    void Reset();
    void DoSavestate(Savestate* file);

    const u32 Num;

    u8 Cnt = 0;
    u32 DstAddr = 0;
    u16 TimerReload = 0;
    u32 Length = 0;

    u32 Timer = 0;
    s32 Pos = 0;

    u32 FIFO[4] {};
    u32 FIFOReadPos = 0;
    u32 FIFOWritePos = 0;
    u32 FIFOWriteOffset = 0;
    u32 FIFOLevel = 0;

    void FIFO_FlushData();
    template<typename T> void FIFO_WriteData(T val);

    void SetCnt(u8 val)
    {
        if ((val & 0x80) && !(Cnt & 0x80))
            Start();

        val &= 0x8F;
        if (!(val & 0x80)) val &= ~0x01;
        Cnt = val;
    }

    void SetDstAddr(u32 val) { DstAddr = val & 0x07FFFFFC; }
    void SetTimerReload(u32 val) { TimerReload = val & 0xFFFF; }
    void SetLength(u32 val) { Length = val << 2; if (Length == 0) Length = 4; }

    void Start()
    {
        Timer = TimerReload;
        Pos = 0;
        FIFOReadPos = 0;
        FIFOWritePos = 0;
        FIFOWriteOffset = 0;
        FIFOLevel = 0;
    }

    void Run(s32 sample);

private:
    melonDS::NDS& NDS;
};

class SPU
{
public:
    explicit SPU(melonDS::NDS& nds, AudioBitDepth bitdepth, AudioInterpolation interpolation);
    ~SPU();
    void Reset();
    void DoSavestate(Savestate* file);

    void Stop();

    void SetPowerCnt(u32 val);

    void SetInterpolation(AudioInterpolation type);

    void SetBias(u16 bias);
    void SetDegrade10Bit(bool enable);
    void SetDegrade10Bit(AudioBitDepth depth);
    void SetApplyBias(bool enable);

    SPUSample<s32> Mix();
    void Run(u32 dummy);

    void DrainOutput();
    void InitOutput();
    int GetOutputSize() const;
    int ReadOutput(s16* data, int samples);
    void TransferOutput();

    u8 Read8(u32 addr);
    u16 Read16(u32 addr);
    u32 Read32(u32 addr);
    void Write8(u32 addr, u8 val);
    void Write16(u32 addr, u16 val);
    void Write32(u32 addr, u32 val);

private:
    template<u32 type> s32 RunChannelOfType(SPUChannel &c);

    s32 RunChannel(SPUChannel &c)
    {
        s32 val;
        switch ((c.Cnt >> 29) & 0x3)
        {
        case 0: val = RunChannelOfType<0>(c); break;
        case 1: val = RunChannelOfType<1>(c); break;
        case 2: val = RunChannelOfType<2>(c); break;
        case 3:
            if (c.Num >= 14)
            {
                val = RunChannelOfType<4>(c);
                break;
            }
            else if (c.Num >= 8)
            {
                val = RunChannelOfType<3>(c);
                break;
            }
            [[fallthrough]];
        default:
            val = 0;
        }

        c.CurVal = val;
        return val;
    }

    static const u32 OutputBufferLen = 4096;
    melonDS::NDS& NDS;
    SPUSample<s16> OutputBackBuffer[OutputBufferLen] {};
    u32 OutputBackBufferWritePosition = 0;

    SPUSample<s16> OutputFrontBuffer[OutputBufferLen] {};
    u32 OutputFrontBufferWritePosition = 0;
    u32 OutputFrontBufferReadPosition = 0;

    MelonResampler Resampler;

    Platform::Mutex* AudioLock;

    u64 Cycles = 0;

    u16 Cnt = 0;
    u8 MasterVolume = 0;
    u16 Bias = 0;
    bool ApplyBias = true;
    bool Degrade10Bit = false;

    std::array<SPUChannel, 16> Channels;
    std::array<SPUCaptureUnit, 2> Capture;

    AudioInterpolation InterpolationType;
};

}
#endif // SPU_H
