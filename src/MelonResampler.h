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
        float t;
        Sample dV;
    };

    MelonResampler(uint32_t outputBufferLen,
                   uint32_t irLen,
                   float fsOut,
                   float fCutoff);
    void Reset();
    void WalkBackTime(float t);
    void AddSample(int channel, float t, float vL, float vR);
    bool CanGenerateOutputBuffer();
    const std::vector<Sample>& GenerateOutputBuffer();

private:
    void GenerateLUT();

    float SamplesToSeconds(float samples);

    std::vector<float> lut;
    melonDS::FIFO<Delta, 4096> deltaQueues[16];
    std::vector<Sample> outputBuffer;
    std::vector<Sample> intermediateBuffer;

    float fsOut;
    float T;
    float fCutoff;
    uint32_t irLen;
    uint32_t outputBufferLen;
    float invWindowedSincArea;
    
    float tLastSample;
    Sample vLast[16];
    float tThisBufferStart;
    Sample vOut[16];
    // A running compensation for lost low-order bits during summation.
    Sample c;
}; 