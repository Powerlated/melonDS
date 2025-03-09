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
    double t;
    double dV;
};

class MelonResampler
{
public:
    MelonResampler(uint32_t outputBufferLen,
                   uint32_t irLen,
                   double fsOut,
                   double fCutoff,
                   std::function<void(std::vector<double> &)> audioReadyCallback);
    void AddSample(double t, double v);
    const std::vector<double>& GenerateOutputBuffer();
    bool CanGenerateOutputBuffer();

private:
    void GenerateLUT();
    double SamplesToSeconds(double samples);
    double CausalScaledWindowedSinc(double t);
    double CausalScaledWindowedSincLUT(double t);

    std::function<void(std::vector<double> &)> audioReadyCallback;
    std::vector<double> lut;
    std::deque<Delta> deltaDeque;
    std::vector<double> outputBuffer;

    double fsOut;
    double fCutoff;
    uint32_t irLen;
    uint32_t outputBufferLen;
    double windowedSincArea;

    double lastT;
    double lastV;
    double thisBufferStartT;
    double outV;
    // A running compensation for lost low-order bits.
    double c = 0.0;
};