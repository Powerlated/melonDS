#include <cassert>
#include <cmath>
#include <math.h>
#include <stdio.h>
#include "MelonResampler.h"

double CausalScaledWindowedSincBlackman(double irLen, double fsOut, double fCutoff, double t);
double CausalScaledSinc(double irLen, double fsOut, double fCutoff, double t);

#define ImpulseResponse CausalScaledWindowedSincBlackman 

MelonResampler::MelonResampler() :
      vLast{},
      cycleThisBufferStart(0),
      cycleUpToDate(0),
      vOut(Sample{})
{
  GenerateLUT();
}

void MelonResampler::Reset()
{
  deltaQueue.Clear();

  for (auto v : vLast)
  {
    v = {};
  }
  cycleThisBufferStart = 0;
  cycleUpToDate = 0;
  vOut = Sample{};
}

void MelonResampler::AddSample(int channel, uint64_t cycle, float vL, float vR)
{
  assert(cycle >= cycleThisBufferStart);
  
  Sample v = Sample {vL, vR};
  if (vLast[channel] == v)
  {
    return;
  }

  if (!deltaQueue.IsFull()) {
    Sample dv = (v - vLast[channel]);
    vLast[channel] = v;
    deltaQueue.Write({cycle, dv});
  }
}

void MelonResampler::IHaveAddedAllSamplesUpToButNotIncludingCycle(uint64_t cycle) {
  cycleUpToDate = cycle;
}

bool MelonResampler::CanGenerateOutputBuffer()
{
  return cycleUpToDate > cycleThisBufferStart;
}


const std::array<MelonResampler::Sample, MelonResampler::OUTPUT_BUFFER_LEN> &MelonResampler::GenerateOutputBuffer()
{
  // assert(CanGenerateOutputBuffer());
  std::fill(outputBuffer.begin(), outputBuffer.end(), Sample{});

  float tThisBufferEnd = OUTPUT_BUFFER_LEN / FS_OUT;

  uint32_t size = deltaQueue.Level();
  for (uint32_t qi = 0; qi < size; qi++)
  {
    auto delta = deltaQueue.Peek(qi);

    // Relative to start of buffer
    float cycleDelta = (float)(delta.cycles - cycleThisBufferStart) - cycleThisBufferStartFrac;   
    float tDelta = cycleDelta / F_SYSTEM;

    // This delta is past the end of the buffer, we are done for now
    if (tDelta > tThisBufferEnd)
    {
      break;
    }

    // when does this delta's influence start?
    int32_t i = floor(tDelta * FS_OUT);

    // what is the sample after this delta's influence ends?
    int32_t iEnd = i + IR_LEN;
    if (i < 0)
    {
      i = 0;
    }

    if (iEnd > OUTPUT_BUFFER_LEN)
    {
      iEnd = OUTPUT_BUFFER_LEN;
    }

    float irN = (i * T - tDelta) * FS_OUT;
    // break into fractional and integer part for LUT access
    int32_t irLutN = (int32_t)floor(irN);
    float irLutFrac = irN - irLutN;
    int32_t irLutPhase = irLutFrac * LUT_PHASES; 
    int32_t irLutI = irLutPhase * IR_LEN + irLutN + 4; // 4 entries of padding in case some calculation is off and gives us a negative index
    for (; i < iEnd; i++)
    {
      outputBuffer[i] += delta.dV * lut[irLutI];
      irLutI++;
    }
  }

  // Remove deltas that won't affect the next buffer
  while (!deltaQueue.IsEmpty())
  {
    float cycleDelta = (float)(deltaQueue.Peek().cycles - cycleThisBufferStart) - cycleThisBufferStartFrac;   
    float tDelta = cycleDelta / F_SYSTEM;
    if (tDelta > tThisBufferEnd) {
      break;
    }
    deltaQueue.Read();
  }

  // Integrate the output buffer
  for (uint32_t i = 0; i < OUTPUT_BUFFER_LEN; i++)
  {
    vOut += outputBuffer[i];
    outputBuffer[i] = vOut;
  }

  cycleThisBufferStart += floor(OUTPUT_BUFFER_LEN / FS_OUT * F_SYSTEM);
  cycleThisBufferStartFrac += fmod(OUTPUT_BUFFER_LEN / FS_OUT * F_SYSTEM, 1.0f);
  if (cycleThisBufferStartFrac > 1) {
    cycleThisBufferStartFrac -= 1;
    cycleThisBufferStart++;
  }

  return outputBuffer;
}

float MelonResampler::SamplesToSeconds(float n)
{
  return n / FS_OUT;
}

void MelonResampler::GenerateLUT()
{
  // Give it 8 entries of padding, 4 on each side of the IR
  std::fill(lut.begin(), lut.end(), 0.0);
  int i = 4;
  for (int p = 0; p < LUT_PHASES; p++)
  {
    double shift = ((float)T * p) / LUT_PHASES;

    double phaseSum = 0.0;
    for (int n = 0; n < IR_LEN; n++)
    {
      double irT = shift + SamplesToSeconds(n);
      double ir = ImpulseResponse(IR_LEN, FS_OUT, F_CUTOFF, irT);
      lut.at(i) = ir;
      phaseSum += ir;
      i++;
    }

    /* Normalize phase */
    for (int n = 0; n < IR_LEN; n++) {
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
