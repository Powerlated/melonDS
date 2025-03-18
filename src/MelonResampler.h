#pragma once

#include <cstdint>
#include <array>
#include <functional>
#include <deque>

#include "FIFO.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846264338327950288
#endif

class MelonResampler
{
    constexpr static int IR_LEN = 24;
    constexpr static int LUT_PHASES = 1024;
    constexpr static int LUT_LEN = LUT_PHASES * IR_LEN + 8;
    constexpr static int OUTPUT_BUFFER_LEN = 256;

    constexpr static float FS_OUT = 48000.0;
    constexpr static float T = 1.0 / FS_OUT;
    constexpr static float F_CUTOFF = 15360;
    constexpr static float F_SYSTEM = 33513982.0 / 2.0;

    static_assert(F_CUTOFF <= FS_OUT / 2);

public:
    struct Sample {
        float v[2];
        Sample operator+(const Sample &rhs) const {
            return { v[0] + rhs.v[0], v[1] + rhs.v[1] };
        }
        Sample operator-(const Sample &rhs) const {
            return { v[0] - rhs.v[0], v[1] - rhs.v[1] };
        }
        Sample operator/(const Sample &rhs) const {
            return { v[0] / rhs.v[0], v[1] / rhs.v[1] };
        }
        Sample operator*(const float &rhs) const {
            return { v[0] * rhs, v[1] * rhs };
        }
        Sample operator/(const float &rhs) const {
            return { v[0] / rhs, v[1] / rhs };
        }
        Sample operator+=(const Sample &rhs) {
            return { v[0] -= rhs.v[0], v[1] -= rhs.v[1] };
        }
        Sample operator-=(const float &rhs) {
            return { v[0] -= rhs, v[1] -= rhs };
        }
        bool operator==(const Sample &rhs) {
            return v[0] == rhs.v[0] && v[1] == rhs.v[1];
        }
    };

    struct Delta
    {
        uint64_t cycles;
        Sample dV;
    };

    MelonResampler();
    void Reset();
    void WalkBackTime(float t);
    void AddSample(int channel, uint64_t cycles, float vL, float vR);
    void IHaveAddedAllSamplesUpToButNotIncludingCycle(uint64_t cycles);

    bool CanGenerateOutputBuffer();
    const std::array<Sample, OUTPUT_BUFFER_LEN>& GenerateOutputBuffer();

private:
    void GenerateLUT();

    float SamplesToSeconds(float samples);

    std::array<float, LUT_LEN> lut;
    melonDS::ThreadSafeFIFO<Delta, 65536> deltaQueue;
    std::array<Sample, OUTPUT_BUFFER_LEN> outputBuffer;
    
    Sample vLast[16];
    uint64_t cycleThisBufferStart; // buffer position is stored as cycles
    float cycleThisBufferStartFrac; 
    uint64_t cycleUpToDate;
    Sample vOut;
}; 