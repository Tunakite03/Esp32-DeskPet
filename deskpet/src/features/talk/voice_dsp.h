#pragma once
#include <math.h>
#include <stddef.h>
#include <stdint.h>

// Signal helpers for talk mode. They depend only on the C library, so the host
// tests can compile them unchanged.

// Coefficients of a second-order Butterworth high-pass (RBJ form, Q = 1/sqrt(2)).
struct Biquad {
  float b0, b1, b2, a1, a2;
};

// Memory of one filtered stream. Start from zero, and reset it when the input is interrupted.
struct BiquadState {
  float z1, z2;
};

inline Biquad highPassCoefficients(float sampleRate, float cutoff) {
  const double w0 = 6.28318530717958647692 * cutoff / sampleRate;  // 2 * pi * cutoff / fs
  const double cosW0 = cos(w0);
  const double alpha = sin(w0) * 0.70710678118654752440;          // sin(w0) / (2 * Q)
  const double a0 = 1.0 + alpha;
  const double b0 = (1.0 + cosW0) / 2.0 / a0;
  return {static_cast<float>(b0), static_cast<float>(-2.0 * b0), static_cast<float>(b0),
          static_cast<float>(-2.0 * cosW0 / a0), static_cast<float>((1.0 - alpha) / a0)};
}

// Filters count samples in place (transposed direct form II). The state carries over between calls.
inline void filterBlock(const Biquad &c, BiquadState &s, int16_t *samples, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    const float x = samples[i];
    const float y = c.b0 * x + s.z1;
    s.z1 = c.b1 * x - c.a1 * y + s.z2;
    s.z2 = c.b2 * x - c.a2 * y;
    samples[i] = static_cast<int16_t>(y > 32767.0F ? 32767.0F : y < -32768.0F ? -32768.0F : y);
  }
}

// Limits for voiceGain. The floor is the older peak-based rule (peak at floorPeak, gain at most
// floorMaxGain). The RMS lift is applied on top of it, so no phrase ends up quieter than before.
struct VoiceGainLimits {
  float targetRms;     // average loudness that quiet phrases are lifted to
  float peakCeiling;   // no sample is raised above this
  float maxGain;       // no phrase is raised by more than this
  float floorPeak;     // peak that the older rule scaled to
  float floorMaxGain;  // gain cap of the older rule
};

// Gain for one repeated phrase. An empty phrase has no gain.
inline float voiceGain(const float *samples, size_t count, VoiceGainLimits limits) {
  if (count == 0) return 0.0F;
  float peak = 1, squares = 0;
  for (size_t i = 0; i < count; ++i) {
    const float magnitude = fabsf(samples[i]);
    if (magnitude > peak) peak = magnitude;
    squares += samples[i] * samples[i];
  }
  const float rms = sqrtf(squares / static_cast<float>(count));
  const float peakFloorGain = limits.floorPeak / peak;
  const float floorGain = peakFloorGain < limits.floorMaxGain ? peakFloorGain : limits.floorMaxGain;
  const float liftGain = limits.targetRms / (rms > 1.0F ? rms : 1.0F);
  const float wanted = floorGain > liftGain ? floorGain : liftGain;
  const float ceilingGain = limits.peakCeiling / peak;
  const float capped = ceilingGain < wanted ? ceilingGain : wanted;
  return capped < limits.maxGain ? capped : limits.maxGain;
}
