#pragma once

#include <cstdint>
#include <vector>
#include <functional>
#include <deque>


#ifndef M_PI
#define M_PI 3.14159265358979323846264338327950288
#endif

struct Delta
{
    float t;
    float dV;
};

class MelonResampler
{
public:
    MelonResampler(uint32_t outputBufferLen,
                   uint32_t irLen,
                   float fsOut,
                   float fCutoff,
                   std::function<void(std::vector<float> &)> audioReadyCallback);
    void Reset();
    void WalkBackTime(float t);
    void AddSample(float t, float v);
    const std::vector<float>& GenerateOutputBuffer();
    bool CanGenerateOutputBuffer();

private:
    void GenerateLUT();
    float SamplesToSeconds(float samples);
    double CausalScaledWindowedSinc(double t);
    float CausalScaledWindowedSincLUT(float t);

    std::function<void(std::vector<float> &)> audioReadyCallback;
    std::vector<float> lut;
    std::deque<Delta> deltaDeque;
    std::vector<float> outputBuffer;

    float fsOut;
    float fCutoff;
    uint32_t irLen;
    uint32_t outputBufferLen;
    float windowedSincArea;

    float lastT;
    float lastV;
    float thisBufferStartT;
    float outV;
    // A running compensation for lost low-order bits during summation.
    float c;
};