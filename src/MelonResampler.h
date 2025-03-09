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
    void AddSample(float t, float v);
    void Flush();

private:
    void GenerateOutputBuffer();
    void GenerateLUT();
    float SamplesToSeconds(float samples);
    float CausalScaledWindowedSinc(float t);
    float CausalScaledWindowedSincLUT(float t);

    float fsOut;
    float fCutoff;
    uint32_t irLen;
    uint32_t outputBufferLen;
    float windowedSincArea;
    std::vector<float> lut;

    std::function<void(std::vector<float> &)> audioReadyCallback;
    std::deque<Delta> deltaDeque;
    std::vector<float> outputBuffer;
    float lastT;
    float lastV;
    float thisBufferStartT;

    float outV;
};