#include <cassert>
#include <cmath>
#include <math.h>
#include <stdio.h>
#include "MelonResampler.h"

#define LUT_PHASES 1024

double CausalScaledWindowedSincBlackman(double irLen, double fsOut, double fCutoff, double t);
double CausalScaledSinc(double irLen, double fsOut, double fCutoff, double t);

#define ImpulseResponse CausalScaledWindowedSincBlackman 

MelonResampler::MelonResampler(
    uint32_t outputBufferLen,
    uint32_t irLen,
    float fsOut,
    float fCutoff)
    : fsOut(fsOut),
      T(1.0 / fsOut),
      fCutoff(fCutoff),
      irLen(irLen),
      outputBufferLen(outputBufferLen),
      tLastSample(0),
      vLast{},
      tThisBufferStart(0)
{
  assert(fCutoff <= fsOut / 2);

  memset(vOut, 0, sizeof(vOut));

  outputBuffer.resize(outputBufferLen);
  intermediateBuffer.resize(outputBufferLen);

  GenerateLUT();

  // Numerically compute the area of the windowed sinc kernel

  double area = 0;
  double dt = SamplesToSeconds(1.0 / 10);
  double t = 0;
  while (t <= SamplesToSeconds(irLen))
  {
    area += ImpulseResponse(irLen, fsOut, fCutoff, t);
    t += dt;
  }

  area *= dt;

  invWindowedSincArea = 1.0 / (area * fsOut);
}

void MelonResampler::Reset()
{
  for (auto &deltaQueue : deltaQueues) {
    deltaQueue.Clear();
  }

  tLastSample = 0;
  for (auto v : vLast)
  {
    v = {};
  }
  tThisBufferStart = 0;
  memset(vOut, 0, sizeof(vOut));
}

void MelonResampler::WalkBackTime(float t)
{
  for (auto &deltaQueue : deltaQueues) {
    uint32_t size = deltaQueue.Level();
    for (uint32_t i = 0; i < size; i++)
    {
      deltaQueue.PeekPtr(i)->t -= t;
    }
  }

  tLastSample -= t;
  tThisBufferStart -= t;
}

void MelonResampler::AddSample(int channel, float t, float vL, float vR)
{
  assert(t >= tThisBufferStart);

  tLastSample = t;
  
  Sample v = Sample {vL, vR};
  if (vLast[channel] == v)
  {
    return;
  }

  if (!deltaQueues[channel].IsFull()) {
    Sample dv = (v - vLast[channel]);
    vLast[channel] = v;
    deltaQueues[channel].Write({t, dv});
  }
}

bool MelonResampler::CanGenerateOutputBuffer()
{
  float tThisBufferEnd = tThisBufferStart + SamplesToSeconds(outputBufferLen);
  return tLastSample > tThisBufferEnd;
}

const std::vector<MelonResampler::Sample> &MelonResampler::GenerateOutputBuffer()
{
  // assert(CanGenerateOutputBuffer());
  float tThisBufferEnd = tThisBufferStart + SamplesToSeconds(outputBufferLen);

  for (int c = 0; c < 16; c++) {
    auto &deltaQueue = deltaQueues[c];
    std::fill(intermediateBuffer.begin(), intermediateBuffer.end(), Sample{});

    uint32_t size = deltaQueue.Level();
    for (uint32_t qi = 0; qi < size; qi++)
    {
      auto delta = deltaQueue.Peek(qi);
      // This delta is past the end of the buffer, we are done for now
      if (delta.t > tThisBufferEnd)
      {
        break;
      }

      // when does this delta's influence start?
      int32_t i = floor((delta.t - tThisBufferStart) * fsOut);

      // when does this delta's influence end?
      int32_t iEnd = i + irLen;
      if (i < 0)
      {
        i = 0;
      }

      if (iEnd > (int)outputBufferLen)
      {
        iEnd = outputBufferLen;
      }

      float irN = (tThisBufferStart + i * T - delta.t) * fsOut;
      // break into fractional and integer part for LUT access
      int32_t irLutN = (int32_t)floor(irN);
      float irLutFrac = irN - irLutN;
      int32_t irLutPhase = irLutFrac * LUT_PHASES; 
      int32_t irLutI = irLutPhase * irLen + irLutN + 4; // 4 entries of padding in case some calculation is off and gives us a negative index
      for (; i < iEnd; i++)
      {
        intermediateBuffer[i] += delta.dV * lut[irLutI];
        irLutI++;
      }
    }

    // Remove deltas that won't affect the next buffer
    while (!deltaQueue.IsEmpty() && deltaQueue.Peek().t + SamplesToSeconds(irLen) < tThisBufferEnd)
    {
      deltaQueue.Read();
    }

    // Integrate & filter the intermediate buffer
    for (int i = 0; i < outputBufferLen; i++)
    {
      vOut[c] += intermediateBuffer[i];
      intermediateBuffer[i] = vOut[c];
    }

    // Sum up intermediate into output
    if (c == 0) {
      // If channel is 0 overwrite output
      for (int i = 0; i < outputBufferLen; i++) {
        outputBuffer[i] = intermediateBuffer[i];
      }
    } else {
      // Otherwise sum into output
      for (int i = 0; i < outputBufferLen; i++) {
        outputBuffer[i] += intermediateBuffer[i];
      }
    }
  }

  tThisBufferStart = tThisBufferEnd;
  return outputBuffer;
}

float MelonResampler::SamplesToSeconds(float n)
{
  return n / fsOut;
}

void MelonResampler::GenerateLUT()
{
  // Give it 8 entries of padding, 4 on each side of the IR
  lut.resize(LUT_PHASES * irLen + 8);
  std::fill(lut.begin(), lut.end(), 0.0);
  int i = 4;
  for (int p = 0; p < LUT_PHASES; p++)
  {
    double shift = ((float)T * p) / LUT_PHASES;

    double phaseSum = 0.0;
    for (int n = 0; n < irLen; n++)
    {
      double irT = shift + SamplesToSeconds(n);
      double ir = ImpulseResponse(irLen, fsOut, fCutoff, irT);
      lut.at(i) = ir;
      phaseSum += ir;
      i++;
    }

    /* Normalize phase */
    for (int n = 0; n < irLen; n++) {
      lut.at(i) /= phaseSum;
    }
  }
}

double CausalScaledSinc(double irLen, double fsOut, double fCutoff, double t) {
  double sincParam = 2 * M_PI * (t - 0.5 * (irLen / fsOut)) * fCutoff;
  double sinc;
  if (sincParam == 0)
      sinc = 1;
  else
      sinc = sin(sincParam) / sincParam;
  
  return sinc;
}

double BlackmanWindow(double x)
{
  if (x < 0 || x > 1)
    return 0;
  return 0.42 - 0.5 * cos(2 * M_PI * x) + 0.08 * cos(4 * M_PI * x);
}

/**
 * @param t the time in seconds, in terms of the output stream
 */
double CausalScaledWindowedSincBlackman(double irLen, double fsOut, double fCutoff, double t)
{
  double sinc = CausalScaledSinc(irLen, fsOut, fCutoff, t);

  double windowParam = t * fsOut / irLen;
  double window = BlackmanWindow(windowParam);

  return sinc * window;
}
