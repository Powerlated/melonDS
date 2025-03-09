#pragma once

#include <cstdint>
#include <vector>
#include <functional>
#include <deque>


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
        float l, r;
        Sample operator+(const Sample &rhs) const {
            return { l + rhs.l, r + rhs.r };
        }
        Sample operator-(const Sample &rhs) const {
            return { l - rhs.l, r - rhs.r };
        }
        Sample operator/(const Sample &rhs) const {
            return { l / rhs.l, r / rhs.r };
        }
        Sample operator/(const float &rhs) const {
            return { l / rhs, r / rhs };
        }
        Sample operator-=(const float &rhs) {
            return { l -= rhs, r -= rhs };
        }
    };

    MelonResampler(uint32_t outputBufferLen,
                   uint32_t irLen,
                   float fsOut,
                   float fCutoff,
                   std::function<void(std::vector<float> &)> audioReadyCallback);
    void Reset();
    void WalkBackTime(float t);
    void AddSampleL(float t, float v);
    void AddSampleR(float t, float v);
    const std::vector<Sample>& GenerateOutputBuffer();
    bool CanGenerateOutputBuffer();

private:
    void GenerateLUT();
    float SamplesToSeconds(float samples);
    double CausalScaledWindowedSinc(double t);
    float CausalScaledWindowedSincLUT(float t);

    std::function<void(std::vector<float> &)> audioReadyCallback;
    std::vector<float> lut;
    std::deque<Delta> deltaDeques[2];
    std::vector<Sample> outputBuffer;

    float fsOut;
    float fCutoff;
    uint32_t irLen;
    uint32_t outputBufferLen;
    float windowedSincArea;

    Sample lastT;
    Sample lastV;
    float thisBufferStartT;
    Sample outV;
    // A running compensation for lost low-order bits during summation.
    Sample c;
};