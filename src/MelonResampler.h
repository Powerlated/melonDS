#pragma once

#include <cstdint>
#include <vector>
#include <functional>
#include <deque>

#include "FIFO.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846264338327950288
#endif


class MelonResampler
{
public:
    struct Delta
    {
        float t;
        float dV;
    };

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
        Sample operator/(const float &rhs) const {
            return { v[0] / rhs, v[1] / rhs };
        }
        Sample operator-=(const float &rhs) {
            return { v[0] -= rhs, v[1] -= rhs };
        }
    };

    MelonResampler(uint32_t outputBufferLen,
                   uint32_t irLen,
                   float fsOut,
                   float fCutoff);
    void Reset();
    void WalkBackTime(float t);
    void AddSampleL(int channel, float t, float v);
    void AddSampleR(int channel, float t, float v);
    bool CanGenerateOutputBuffer();
    const std::vector<Sample>& GenerateOutputBuffer();

    uint64_t num_dupes = 0;

private:
    void GenerateLUT();

    float SamplesToSeconds(float samples);
    double CausalScaledWindowedSinc(double t);
    float CausalScaledWindowedSincLUT(float t);

    std::vector<float> lut;
    melonDS::FIFO<Delta, 32768> deltaQueues[2];
    std::vector<Sample> outputBuffer;

    float fsOut;
    float T;
    float fCutoff;
    uint32_t irLen;
    uint32_t outputBufferLen;
    float invWindowedSincArea;
    
    Sample tLastSample;
    Sample vLast[16];
    float tThisBufferStart;
    Sample vOut;
    // A running compensation for lost low-order bits during summation.
    Sample c;
}; 