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
        double t;
        double dV;
    };

    struct Sample {
        double v[2];
        Sample operator+(const Sample &rhs) const {
            return { v[0] + rhs.v[0], v[1] + rhs.v[1] };
        }
        Sample operator-(const Sample &rhs) const {
            return { v[0] - rhs.v[0], v[1] - rhs.v[1] };
        }
        Sample operator/(const Sample &rhs) const {
            return { v[0] / rhs.v[0], v[1] / rhs.v[1] };
        }
        Sample operator/(const double &rhs) const {
            return { v[0] / rhs, v[1] / rhs };
        }
        Sample operator-=(const double &rhs) {
            return { v[0] -= rhs, v[1] -= rhs };
        }
    };

    MelonResampler(uint32_t outputBufferLen,
                   uint32_t irLen,
                   double fsOut,
                   double fCutoff);
    void Reset();
    void WalkBackTime(double t);
    void AddSampleL(int channel, double t, double v);
    void AddSampleR(int channel, double t, double v);
    bool CanGenerateOutputBuffer();
    const std::vector<Sample>& GenerateOutputBuffer();

    uint64_t num_dupes = 0;

private:
    void GenerateLUT();

    double SamplesToSeconds(double samples);
    double CausalScaledWindowedSinc(double t);
    double CausalScaledWindowedSincLUT(double t);

    std::vector<double> lut;
    melonDS::FIFO<Delta, 100000000> deltaQueues[2];
    std::vector<Sample> outputBuffer;

    double fsOut;
    double T;
    double fCutoff;
    uint32_t irLen;
    uint32_t outputBufferLen;
    double invWindowedSincArea;
    
    Sample tLastSample;
    Sample vLast[16];
    double tThisBufferStart;
    Sample vOut;
    // A running compensation for lost low-order bits during summation.
    Sample c;
};