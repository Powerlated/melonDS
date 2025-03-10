#include <cassert>
#include <cmath>
#include <print>
#include <math.h>
#include <stdio.h>
#include "MelonResampler.h"

MelonResampler::MelonResampler(
    uint32_t outputBufferLen,
    uint32_t irLen,
    float fsOut,
    float fCutoff,
    std::function<void(std::vector<float> &)> audioReadyCallback)
    : audioReadyCallback(audioReadyCallback),
      fsOut(fsOut),
      fCutoff(fCutoff),
      irLen(irLen),
      outputBufferLen(outputBufferLen),
      lastT(Sample{}),
      lastV{},
      thisBufferStartT(0),
      outV(Sample{}),
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

  windowedSincArea = area * fsOut;
}

void MelonResampler::Reset()
{
  for (auto &deque : deltaDeques)
  {
    deque.clear();
  }

  lastT = Sample{};
  for (auto v : lastV) {
    v = {};
  }
  thisBufferStartT = 0;
  outV = Sample{};
  c = Sample{};
}

void MelonResampler::WalkBackTime(float t)
{
  for (auto &deque : deltaDeques)
  {
    for (auto &delta : deque)
    {
      delta.t -= t;
    }
  }
  lastT -= t;
  thisBufferStartT -= t;
}

void MelonResampler::AddSampleL(int channel, float t, float v)
{
  assert(t >= thisBufferStartT);

  lastT.l = t;
  if (lastV[channel].l == v) return;

  deltaDeques[0].push_back({t, v - lastV[channel].l});
  lastV[channel].l = v;
}

void MelonResampler::AddSampleR(int channel, float t, float v)
{
  assert(t >= thisBufferStartT);

  lastT.r = t;
  if (lastV[channel].r == v) return;

  deltaDeques[1].push_back({t, v - lastV[channel].r});
  lastV[channel].r = v;
}

void MelonResampler::AddDeltaL(int channel, float t, float dV)
{
  assert(t >= thisBufferStartT);

  deltaDeques[0].push_back({t, dV});
  lastT.l = t;
  lastV[channel].l += dV;
}

void MelonResampler::AddDeltaR(int channel, float t, float dV)
{
  assert(t >= thisBufferStartT);

  deltaDeques[1].push_back({t, dV});
  lastT.r = t;
  lastV[channel].r += dV;
}

bool MelonResampler::CanGenerateOutputBuffer()
{
  float endOfThisBuffer = thisBufferStartT + SamplesToSeconds(outputBufferLen);
  return lastT.l > endOfThisBuffer && lastT.r > endOfThisBuffer;
}

const std::vector<MelonResampler::Sample> &MelonResampler::GenerateOutputBuffer()
{
  assert(CanGenerateOutputBuffer());
  std::fill(outputBuffer.begin(), outputBuffer.end(), Sample{});

  float thisBufferEndT = thisBufferStartT + SamplesToSeconds(outputBufferLen);

  for (const auto &deque : deltaDeques)
  {
    for (const auto &delta : deque)
    {
      // The next delta is past the end of the buffer, we are done for now
      if (delta.t > thisBufferEndT)
      {
        break;
      }

      // when does this delta's influence start?
      int32_t i = (delta.t - thisBufferStartT) / SamplesToSeconds(1);

      // when does this delta's influence end?
      int32_t iEnd = i + irLen;
      if (iEnd > outputBufferLen)
      {
        iEnd = outputBufferLen;
      }

      if (i < 0)
      {
        i = 0;
      }

      for (; i < iEnd; i++)
      {
        float bufT = thisBufferStartT + i * SamplesToSeconds(1);
        float irT = bufT - delta.t;
        float value = delta.dV * CausalScaledWindowedSincLUT(irT);
        if (&deque == &deltaDeques[0])
        {
          outputBuffer.at(i).l += value;
        }
        else
        {
          outputBuffer.at(i).r += value;
        }
      }
    }
  }

  // Remove deltas that won't affect the next buffer
  for (auto &deque : deltaDeques)
  {
    while (!deque.empty() && deque.front().t + SamplesToSeconds(irLen) < thisBufferEndT)
    {
      deque.pop_front();
    }
  }

  // Integrate the output buffer, using Kahan summation algorithm
  for (uint32_t i = 0; i < outputBufferLen; i++)
  {
    Sample a = outputBuffer[i] - c;
    Sample b = outV + a;
    c = (b - outV) - a;
    outV = b;
    outputBuffer[i] = outV / windowedSincArea;
  }

  thisBufferStartT = thisBufferEndT;
  return outputBuffer;
}

float MelonResampler::SamplesToSeconds(float n)
{
  return n / fsOut;
}

float blackman_window(float x)
{
  if (x < 0 || x > 1)
    return 0;
  return 0.42 - 0.5 * cos(2 * M_PI * x) + 0.08 * cos(4 * M_PI * x);
}

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

#define LUT_SIZE 32768
#define LUT_T_END (irLen / fsOut)

float MelonResampler::CausalScaledWindowedSincLUT(float t)
{
  float i = t * (LUT_SIZE - 1) / LUT_T_END;
  // lerp
  int i0 = (int)i;
  int i1 = i0 + 1;
  float f = i - i0;
  if (i0 < 0 || i1 >= lut.size())
    return 0;
  return (1 - f) * lut.at(i0) + f * lut.at(i1);
}

void MelonResampler::GenerateLUT()
{
  lut.resize(LUT_SIZE);
  for (int i = 0; i < LUT_SIZE; i++)
  {
    double t = i * LUT_T_END / (LUT_SIZE - 1);
    // printf("%f\n", t);
    lut.at(i) = CausalScaledWindowedSinc(t);
  }
}