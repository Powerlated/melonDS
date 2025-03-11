#include <cassert>
#include <cmath>
#include <math.h>
#include <stdio.h>
#include "MelonResampler.h"

#define LUT_PHASES 1024

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
      tThisBufferStart(0),
      vOut(Sample{}),
      c(Sample{})
{
  assert(fCutoff <= fsOut / 2);
  outputBuffer.resize(outputBufferLen);

  GenerateLUT();

  // Numerically compute the area of the windowed sinc kernel

  double area = 0;
  double dt = SamplesToSeconds(1.0 / 10);
  double t = 0;
  while (t <= SamplesToSeconds(irLen))
  {
    area += CausalScaledWindowedSinc(t);
    t += dt;
  }

  area *= dt;

  invWindowedSincArea = 1.0 / (area * fsOut);
}

void MelonResampler::Reset()
{
  deltaQueue.Clear();

  tLastSample = 0;
  for (auto v : vLast)
  {
    v = {};
  }
  tThisBufferStart = 0;
  vOut = Sample{};
  c = Sample{};
}

void MelonResampler::WalkBackTime(float t)
{
  uint32_t size = deltaQueue.Level();
  for (uint32_t i = 0; i < size; i++)
  {
    deltaQueue.PeekPtr(i)->t -= t;
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

  Sample dv = (v - vLast[channel]) * invWindowedSincArea;
  vLast[channel] = v;
  deltaQueue.Write({t, dv});
}

bool MelonResampler::CanGenerateOutputBuffer()
{
  float tThisBufferEnd = tThisBufferStart + SamplesToSeconds(outputBufferLen);
  return tLastSample > tThisBufferEnd;
}

const std::vector<MelonResampler::Sample> &MelonResampler::GenerateOutputBuffer()
{
  // assert(CanGenerateOutputBuffer());
  std::fill(outputBuffer.begin(), outputBuffer.end(), Sample{});

  float tThisBufferEnd = tThisBufferStart + SamplesToSeconds(outputBufferLen);

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
      outputBuffer[i] += delta.dV * lut[irLutI];
      irLutI++;
    }
  }

  // Remove deltas that won't affect the next buffer
  while (!deltaQueue.IsEmpty() && deltaQueue.Peek().t + SamplesToSeconds(irLen) < tThisBufferEnd)
  {
    deltaQueue.Read();
  }

  // Integrate the output buffer, using Kahan summation algorithm
  for (uint32_t i = 0; i < outputBufferLen; i++)
  {
    Sample a = outputBuffer[i] - c;
    Sample b = vOut + a;
    c = (b - vOut) - a;
    vOut = b;
    outputBuffer[i] = vOut;
  }

  tThisBufferStart = tThisBufferEnd;
  return outputBuffer;
}

float MelonResampler::SamplesToSeconds(float n)
{
  return n / fsOut;
}

double blackman_window(double x)
{
  if (x < 0 || x > 1)
    return 0;
  return 0.42 - 0.5 * cos(2 * M_PI * x) + 0.08 * cos(4 * M_PI * x);
}

/**
 * @param t the time in seconds, in terms of the output stream
 */
double MelonResampler::CausalScaledWindowedSinc(double t)
{
  // https://www.researchgate.net/figure/Fourier-transform-of-a-rectangle-function-a-and-a-sinc-function-b_fig3_321716019
  // Sinc function
  // In time domain, y = sinc(2πt/T);
  // or alternatively y = sinc(2πt*fsOut/2) because T is inverse of cutoff freq
  // In frequency domain, z = (T/2π)*rect(T*f)

  double sincParam = 2 * M_PI * (t - 0.5 * (irLen / fsOut)) * fCutoff;
  double sinc;
  if (sincParam == 0)
  {
    sinc = 1;
  }
  else
  {
    sinc = sin(sincParam) / (sincParam);
  }

  double windowParam = t * fsOut / irLen;
  double window = blackman_window(windowParam);

  return sinc * window;
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

    for (int n = 0; n < irLen; n++)
    {
      double irT = shift + SamplesToSeconds(n);
      lut.at(i) = CausalScaledWindowedSinc(irT);
      i++;
    }
  }
}