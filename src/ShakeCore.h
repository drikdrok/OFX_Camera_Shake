// Shake motion model and pixel sampling, kept free of any OFX host types so it
// can be exercised by test/core_test.cpp without a host.
#ifndef SHAKE_CORE_H
#define SHAKE_CORE_H

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace shake {

const double kPi = 3.14159265358979323846;

// Maps normalised amplitude (fraction of frame width) to actual displacement.
// S_Shake's internal scaling is not published; this value was chosen so the
// documented defaults (X Rand Amp 0.2, Y Rand Amp 0.1, Amplitude 1) give a
// strong but usable hand-held shake.
const double kShakeScale = 0.25;

enum StyleEnum { kStyleNormal = 0, kStyleTwitchy, kStyleJumpy };
enum WrapEnum { kWrapNone = 0, kWrapTile, kWrapReflect };

// Distinct lattice ids so each axis draws an independent random sequence.
enum AxisId {
  kAxisX = 1,
  kAxisY = 2,
  kAxisZ = 3,
  kAxisTilt = 4,
  kAxisRgbX = 16,  // + channel
  kAxisRgbY = 32   // + channel
};

////////////////////////////////////////////////////////////////////////////////
// noise

inline uint32_t hashU32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

// Random value in [-1,1] at an integer lattice point.
inline double latticeRand(uint32_t seed, uint32_t axis, int32_t i) {
  const uint32_t h =
      hashU32(seed * 0x9e3779b9U ^ hashU32(axis * 0x85ebca6bU ^ hashU32((uint32_t)i)));
  return (double)h * (2.0 / 4294967295.0) - 1.0;
}

// Smooth (C1) value noise via a Catmull-Rom spline through the lattice.
inline double smoothNoise(uint32_t seed, uint32_t axis, double x) {
  const double fi = std::floor(x);
  const int32_t i = (int32_t)fi;
  const double f = x - fi;
  const double p0 = latticeRand(seed, axis, i - 1);
  const double p1 = latticeRand(seed, axis, i);
  const double p2 = latticeRand(seed, axis, i + 1);
  const double p3 = latticeRand(seed, axis, i + 2);
  const double a = -0.5 * p0 + 1.5 * p1 - 1.5 * p2 + 0.5 * p3;
  const double b = p0 - 2.5 * p1 + 2.0 * p2 - 0.5 * p3;
  const double c = -0.5 * p0 + 0.5 * p2;
  const double v = ((a * f + b) * f + c) * f + p1;
  // Catmull-Rom can overshoot; clamp so amplitude keeps its meaning.
  return v < -1.0 ? -1.0 : (v > 1.0 ? 1.0 : v);
}

// Jumpy style: a random target per step, biased towards the original position.
inline double jumpTarget(uint32_t seed, uint32_t axis, int32_t k, double centerBias) {
  if (centerBias > 0.0) {
    const double prob = centerBias / (1.0 + centerBias);
    const double u = latticeRand(seed, axis + 0x500U, k) * 0.5 + 0.5;  // [0,1]
    if (u < prob) return 0.0;
  }
  return latticeRand(seed, axis, k);
}

// Twitchy style: warp time so it stands still for `stillness` of each period
// and covers the whole period's motion in the remaining fraction.
inline double twitchTime(double t, double twitchFreq, double stillness) {
  if (twitchFreq <= 0.0) return t;
  const double s = t * twitchFreq;
  const double k = std::floor(s);
  const double u = s - k;
  const double still = stillness < 0.0 ? 0.0 : (stillness > 0.999 ? 0.999 : stillness);
  const double v = (u <= still) ? 0.0 : (u - still) / (1.0 - still);
  return (k + v) / twitchFreq;
}

////////////////////////////////////////////////////////////////////////////////
// shake motion

struct AxisParams {
  double randAmp, randFreq, waveAmp, waveFreq, phase;
};

struct ShakeParams {
  int style;
  double amplitude, frequency, phase;
  double stillness, twitchFrequency;
  double drift, centerBias;
  double zDist;
  uint32_t seed;
  AxisParams x, y, z, tilt;
  double chanAmp[3], chanPhase[3];
  double rgbRandomness, rgbFrequency;
};

// Normalised axis displacement, before the global Amplitude is applied.
inline double evalAxis(const ShakeParams& p, const AxisParams& a, uint32_t axisId, double tSec,
                       double chanPhase) {
  const double t = tSec + p.phase + a.phase + chanPhase;
  double v = 0.0;

  if (a.randAmp != 0.0) {
    const double s = t * p.frequency * a.randFreq;
    double r;
    if (p.style == kStyleJumpy) {
      const double fk = std::floor(s);
      const int32_t k = (int32_t)fk;
      const double u = s - fk;
      const double a0 = jumpTarget(p.seed, axisId, k, p.centerBias);
      const double a1 = jumpTarget(p.seed, axisId, k + 1, p.centerBias);
      double blend = 0.0;
      if (p.drift > 1e-6) {
        double q = u / p.drift;
        if (q > 1.0) q = 1.0;
        blend = q * q * (3.0 - 2.0 * q);  // smoothstep
      }
      r = a0 + (a1 - a0) * blend;
    } else {
      r = smoothNoise(p.seed, axisId, s);
    }
    v += a.randAmp * r;
  }

  if (a.waveAmp != 0.0) {
    v += a.waveAmp * std::sin(2.0 * kPi * t * a.waveFreq);
  }
  return v;
}

// Inverse affine warp for one channel at one instant.
struct Xform {
  double dx, dy;      // offset, in square-pixel units
  double scale;       // zoom
  double cosA, sinA;  // rotation
};

struct XformSet {
  Xform ch[3];  // R, G, B
  Xform base;   // alpha, and the no-channel-separation fast path
  bool uniform;
};

// chan < 0 builds the channel-independent base transform.
inline Xform makeXform(const ShakeParams& p, double tSec, double widthSq, int chan) {
  const double chAmp = chan >= 0 ? p.chanAmp[chan] : 1.0;
  const double chPhase = chan >= 0 ? p.chanPhase[chan] : 0.0;
  const double amp = p.amplitude * chAmp;

  double vx = evalAxis(p, p.x, kAxisX, tSec, chPhase);
  double vy = evalAxis(p, p.y, kAxisY, tSec, chPhase);
  const double vz = evalAxis(p, p.z, kAxisZ, tSec, chPhase);
  const double vt = evalAxis(p, p.tilt, kAxisTilt, tSec, chPhase);

  if (chan >= 0 && p.rgbRandomness != 0.0) {
    const double t = (tSec + p.phase + chPhase) * p.rgbFrequency;
    vx += p.rgbRandomness * smoothNoise(p.seed, kAxisRgbX + (uint32_t)chan, t);
    vy += p.rgbRandomness * smoothNoise(p.seed, kAxisRgbY + (uint32_t)chan, t);
  }

  Xform xf;
  xf.dx = amp * vx * kShakeScale * widthSq;
  xf.dy = amp * vy * kShakeScale * widthSq;

  const double zd = p.zDist > 1e-6 ? p.zDist : 1e-6;
  const double zoff = amp * vz * kShakeScale;
  xf.scale = zd / std::max(1e-6, zd + zoff);

  const double ang = amp * vt * kPi / 180.0;
  xf.cosA = std::cos(ang);
  xf.sinA = std::sin(ang);
  return xf;
}

// Builds the per-channel transforms for one instant, applying the Twitchy time
// warp and honouring the no-channel-separation fast path.
inline XformSet makeXformSet(const ShakeParams& p, double tSec, double widthSq) {
  const double tw =
      (p.style == kStyleTwitchy) ? twitchTime(tSec, p.twitchFrequency, p.stillness) : tSec;

  XformSet xs;
  xs.uniform = p.chanAmp[0] == 1.0 && p.chanAmp[1] == 1.0 && p.chanAmp[2] == 1.0 &&
               p.chanPhase[0] == 0.0 && p.chanPhase[1] == 0.0 && p.chanPhase[2] == 0.0 &&
               p.rgbRandomness == 0.0;
  xs.base = makeXform(p, tw, widthSq, -1);
  for (int c = 0; c < 3; ++c) xs.ch[c] = xs.uniform ? xs.base : makeXform(p, tw, widthSq, c);
  return xs;
}

// Scales every transform in a set by a constant overscan zoom. Applied after
// makeXformSet so the zoom is fixed for the whole clip - a time-varying zoom
// would read as the image breathing.
inline void applyOverscan(XformSet& xs, double zoom) {
  xs.base.scale *= zoom;
  for (int c = 0; c < 3; ++c) xs.ch[c].scale *= zoom;
}

// Smallest constant zoom that keeps the frame completely covered on every
// frame, so the shake never drags an empty (or wrapped) edge into view.
//
// Works from the worst case the parameters allow rather than the motion at one
// instant, because the zoom has to be constant over time. Noise and sine are
// both bounded by 1, so the largest displacement is the sum of the random and
// wave amplitudes. `widthSq` is the frame width in square-pixel units and
// `height` the frame height in pixels.
inline double autoOverscan(const ShakeParams& p, double widthSq, double height) {
  // Half extents of the outermost *pixel centre*, leaving a half pixel so the
  // bilinear taps at the very edge still land inside the source.
  const double hx = 0.5 * widthSq - 0.5;
  const double hy = 0.5 * height - 0.5;
  if (hx <= 0.0 || hy <= 0.0) return 1.0;

  // Channel separation can push one channel further than the base transform.
  double maxChan = 1.0;
  for (int c = 0; c < 3; ++c) maxChan = std::max(maxChan, p.chanAmp[c]);
  const double amp = p.amplitude * maxChan;

  const double dx =
      amp * (p.x.randAmp + std::fabs(p.x.waveAmp) + p.rgbRandomness) * kShakeScale * widthSq;
  const double dy =
      amp * (p.y.randAmp + std::fabs(p.y.waveAmp) + p.rgbRandomness) * kShakeScale * widthSq;
  const double maxAngle = amp * (p.tilt.randAmp + std::fabs(p.tilt.waveAmp)) * kPi / 180.0;

  // The most the Z shake can zoom *out*, which shrinks coverage.
  const double zd = p.zDist > 1e-6 ? p.zDist : 1e-6;
  const double zoff = amp * (p.z.randAmp + std::fabs(p.z.waveAmp)) * kShakeScale;
  const double minScaleZ = zd / std::max(1e-6, zd + zoff);

  const double ax = hx + std::fabs(dx);
  const double ay = hy + std::fabs(dy);

  // |r.x| <= (|cos|*ax + |sin|*ay) / scale must stay within hx, and likewise
  // for y. Sweep the reachable angles since the maximum can fall inside.
  double need = 1.0;
  const int kSteps = 64;
  const double a = std::min(maxAngle, 0.5 * kPi);
  for (int i = 0; i <= kSteps; ++i) {
    const double th = a * i / kSteps;
    const double c = std::fabs(std::cos(th));
    const double s = std::fabs(std::sin(th));
    need = std::max(need, (c * ax + s * ay) / hx);
    need = std::max(need, (s * ax + c * ay) / hy);
  }

  const double zoom = need / std::max(1e-9, minScaleZ);
  return zoom < 1.0 ? 1.0 : zoom;
}

// Output pixel centre -> source pixel coordinate.
inline void inverseMap(const Xform& xf, double px, double py, double cx, double cy, double par,
                       double& sx, double& sy) {
  const double qx = ((px - cx) * par - xf.dx) / xf.scale;
  const double qy = ((py - cy) - xf.dy) / xf.scale;
  const double rx = xf.cosA * qx + xf.sinA * qy;
  const double ry = -xf.sinA * qx + xf.cosA * qy;
  sx = rx / par + cx;
  sy = ry + cy;
}

////////////////////////////////////////////////////////////////////////////////
// sampling

// Folds a coordinate into [lo,hi). Returns false when the tap is outside.
inline bool wrapAxis(int& v, int lo, int hi, int mode) {
  if (v >= lo && v < hi) return true;
  const int n = hi - lo;
  if (n <= 0) return false;
  switch (mode) {
    case kWrapNone:
      return false;
    case kWrapTile: {
      int m = (v - lo) % n;
      if (m < 0) m += n;
      v = lo + m;
      return true;
    }
    default: {
      const int p = 2 * n;
      int m = (v - lo) % p;
      if (m < 0) m += p;
      v = (m < n) ? lo + m : lo + (p - 1 - m);
      return true;
    }
  }
}

struct Rect {
  int x1, y1, x2, y2;  // x2/y2 exclusive
};

// Bilinear weights plus wrapped tap coordinates, shared by both samplers.
struct Taps {
  int xi[2], yi[2];
  bool xok[2], yok[2];
  double tx, ty;
};

inline bool computeTaps(double sx, double sy, const Rect& rect, int wrapX, int wrapY, Taps& t) {
  const double fx = sx - 0.5;
  const double fy = sy - 0.5;
  const double ix = std::floor(fx);
  const double iy = std::floor(fy);
  t.tx = fx - ix;
  t.ty = fy - iy;
  // Guard against the int casts below overflowing on absurd transforms.
  if (ix < -2147483000.0 || ix > 2147483000.0 || iy < -2147483000.0 || iy > 2147483000.0)
    return false;
  t.xi[0] = (int)ix;
  t.xi[1] = t.xi[0] + 1;
  t.yi[0] = (int)iy;
  t.yi[1] = t.yi[0] + 1;
  for (int k = 0; k < 2; ++k) {
    t.xok[k] = wrapAxis(t.xi[k], rect.x1, rect.x2, wrapX);
    t.yok[k] = wrapAxis(t.yi[k], rect.y1, rect.y2, wrapY);
  }
  return true;
}

// `fetch` is any callable (int x, int y) -> const PIX* (null when unavailable).
// Taps that fall outside the rect contribute nothing, giving transparent edges
// under Wrap = No.
template <class PIX, int nComponents, class Fetch>
inline void sampleAll(const Fetch& fetch, const Rect& rect, int wrapX, int wrapY, double sx,
                      double sy, double acc[4]) {
  Taps t;
  if (!computeTaps(sx, sy, rect, wrapX, wrapY, t)) return;
  for (int j = 0; j < 2; ++j) {
    if (!t.yok[j]) continue;
    const double wy = j ? t.ty : (1.0 - t.ty);
    for (int i = 0; i < 2; ++i) {
      if (!t.xok[i]) continue;
      const double w = wy * (i ? t.tx : (1.0 - t.tx));
      if (w <= 0.0) continue;
      const PIX* p = fetch(t.xi[i], t.yi[j]);
      if (!p) continue;
      for (int c = 0; c < nComponents; ++c) acc[c] += w * (double)p[c];
    }
  }
}

template <class PIX, class Fetch>
inline double sampleComp(const Fetch& fetch, const Rect& rect, int wrapX, int wrapY, double sx,
                         double sy, int comp) {
  Taps t;
  if (!computeTaps(sx, sy, rect, wrapX, wrapY, t)) return 0.0;
  double v = 0.0;
  for (int j = 0; j < 2; ++j) {
    if (!t.yok[j]) continue;
    const double wy = j ? t.ty : (1.0 - t.ty);
    for (int i = 0; i < 2; ++i) {
      if (!t.xok[i]) continue;
      const double w = wy * (i ? t.tx : (1.0 - t.tx));
      if (w <= 0.0) continue;
      const PIX* p = fetch(t.xi[i], t.yi[j]);
      if (!p) continue;
      v += w * (double)p[comp];
    }
  }
  return v;
}

// Defaults matching the documented S_Shake values, used by tests and as a
// reference for the parameter definitions.
inline ShakeParams defaultParams() {
  ShakeParams p;
  p.style = kStyleNormal;
  p.amplitude = 1.0;
  p.frequency = 8.0;
  p.phase = 0.0;
  p.stillness = 0.7;
  p.twitchFrequency = 2.0;
  p.drift = 0.3;
  p.centerBias = 0.0;
  p.zDist = 1.0;
  p.seed = 0;
  const AxisParams x = {0.2, 1.0, 0.0, 0.5, 0.0};
  const AxisParams y = {0.1, 1.0, 0.0, 0.5, 0.0};
  const AxisParams z = {0.0, 1.0, 0.0, 0.5, 0.0};
  const AxisParams tilt = {0.0, 1.0, 0.0, 0.5, 0.0};
  p.x = x;
  p.y = y;
  p.z = z;
  p.tilt = tilt;
  for (int c = 0; c < 3; ++c) {
    p.chanAmp[c] = 1.0;
    p.chanPhase[c] = 0.0;
  }
  p.rgbRandomness = 0.0;
  p.rgbFrequency = 2.0;
  return p;
}

}  // namespace shake

#endif  // SHAKE_CORE_H
