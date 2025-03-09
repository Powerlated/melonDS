#include <cassert>
#include <cmath>
#include <print>
#include <math.h>
#include <stdio.h>
#include "MelonResampler.h"

MelonResampler::MelonResampler(
    uint32_t outputBufferLen,
    uint32_t irLen,
    double fsOut,
    double fCutoff,
    std::function<void(std::vector<double> &)> audioReadyCallback)
    : audioReadyCallback(audioReadyCallback),
      fsOut(fsOut),
      fCutoff(fCutoff),
      irLen(irLen),
      outputBufferLen(outputBufferLen),
      lastT(0),
      lastV(0),
      thisBufferStartT(0),
      outV(0)
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

void MelonResampler::AddSample(double t, double v)
{
  assert(t >= lastT);

  deltaDeque.push_back({t, v - lastV});
  lastT = t;
  lastV = v;
}

bool MelonResampler::CanGenerateOutputBuffer() {
  return lastT > thisBufferStartT + SamplesToSeconds(outputBufferLen);
}

const std::vector<double>& MelonResampler::GenerateOutputBuffer()
{
  assert(CanGenerateOutputBuffer());
  std::fill(outputBuffer.begin(), outputBuffer.end(), 0.0);

  double thisBufferEndT = thisBufferStartT + SamplesToSeconds(outputBufferLen);
  for (const auto &delta : deltaDeque)
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
      double bufT = thisBufferStartT + i * SamplesToSeconds(1);
      double irT = bufT - delta.t;
      outputBuffer.at(i) += delta.dV * CausalScaledWindowedSinc(irT);
    }
  }

  // Remove deltas that won't affect the next buffer
  while (!deltaDeque.empty() && deltaDeque.front().t + SamplesToSeconds(irLen) < thisBufferEndT)
  {
    deltaDeque.pop_front();
  }

  // Integrate the output buffer, using Kahan summation algorithm
  for (uint32_t i = 0; i < outputBufferLen; i++)
  {
    double a = outputBuffer[i] - c;
    double b = outV + a;
    c = (b - outV) - a;
    outV = b; 
    outputBuffer[i] = outV / windowedSincArea;
  }

  thisBufferStartT = thisBufferEndT;
  return outputBuffer;
}

double MelonResampler::SamplesToSeconds(double n)
{
  return n / fsOut;
}

double blackman_window(double x)
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

double MelonResampler::CausalScaledWindowedSincLUT(double t)
{
  double i = t * (LUT_SIZE - 1) / LUT_T_END;
  // lerp
  int i0 = (int) i;
  int i1 = i0 + 1;
  double f = i - i0;
  return (1 - f) * lut.at(i0) + f * lut.at(i1);
}

void MelonResampler::GenerateLUT() {
  lut.resize(LUT_SIZE + 2);
  for (int i = 0; i < LUT_SIZE; i++) {
    double t = i * LUT_T_END / (LUT_SIZE - 1);
    // printf("%f\n", t);
    lut.at(i) = CausalScaledWindowedSinc(t);
  }
}