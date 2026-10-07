// Headless tests for the shake motion model and pixel sampling in ShakeCore.h.
// These do not need an OFX host - they drive the same functions the plug-in's
// render loop calls.

#include "ShakeCore.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int gFailures = 0;
int gChecks = 0;

void check(bool ok, const std::string& what) {
  ++gChecks;
  if (!ok) {
    ++gFailures;
    std::printf("  FAIL: %s\n", what.c_str());
  }
}

void checkNear(double got, double want, double tol, const std::string& what) {
  ++gChecks;
  const double d = got - want;
  if (!(d <= tol && d >= -tol)) {
    ++gFailures;
    std::printf("  FAIL: %s (got %.9g, want %.9g, tol %g)\n", what.c_str(), got, want, tol);
  }
}

const double kWidthSq = 1920.0;

// ---------------------------------------------------------------------------
// motion model

void testDeterminismAndSeed() {
  const shake::ShakeParams p = shake::defaultParams();
  const shake::Xform a = shake::makeXform(p, 1.234, kWidthSq, -1);
  const shake::Xform b = shake::makeXform(p, 1.234, kWidthSq, -1);
  check(a.dx == b.dx && a.dy == b.dy, "same params and time give identical transforms");

  shake::ShakeParams q = p;
  q.seed = 7000;
  const shake::Xform c = shake::makeXform(q, 1.234, kWidthSq, -1);
  check(c.dx != a.dx, "changing Seed changes the motion");
}

void testAmplitudeZeroIsIdentity() {
  shake::ShakeParams p = shake::defaultParams();
  p.amplitude = 0.0;
  bool allIdentity = true;
  for (int f = 0; f < 120; ++f) {
    const shake::Xform x = shake::makeXform(p, f / 30.0, kWidthSq, -1);
    if (x.dx != 0.0 || x.dy != 0.0 || x.scale != 1.0 || x.cosA != 1.0 || x.sinA != 0.0)
      allIdentity = false;
  }
  check(allIdentity, "Amplitude 0 produces an identity transform at every frame");
}

void testDefaultsAreBoundedAndMoving() {
  const shake::ShakeParams p = shake::defaultParams();
  // X Rand Amp 0.2 and Y Rand Amp 0.1 with Amplitude 1, noise clamped to [-1,1].
  const double boundX = 0.2 * shake::kShakeScale * kWidthSq;  // 96 px
  const double boundY = 0.1 * shake::kShakeScale * kWidthSq;  // 48 px
  double maxX = 0.0, maxY = 0.0;
  bool withinBounds = true;
  for (int f = 0; f < 600; ++f) {
    const shake::Xform x = shake::makeXform(p, f / 30.0, kWidthSq, -1);
    const double ax = x.dx < 0 ? -x.dx : x.dx;
    const double ay = x.dy < 0 ? -x.dy : x.dy;
    if (ax > boundX + 1e-9 || ay > boundY + 1e-9) withinBounds = false;
    if (ax > maxX) maxX = ax;
    if (ay > maxY) maxY = ay;
  }
  check(withinBounds, "default motion never exceeds the amplitude bound");
  check(maxX > 0.2 * boundX, "default X motion is actually substantial");
  check(maxY > 0.2 * boundY, "default Y motion is actually substantial");
  std::printf("  (defaults on 1920px: peak dx %.1f px of max %.1f, peak dy %.1f of max %.1f)\n",
              maxX, boundX, maxY, boundY);
}

void testNormalStyleIsContinuous() {
  const shake::ShakeParams p = shake::defaultParams();
  double maxStep = 0.0;
  shake::Xform prev = shake::makeXform(p, 0.0, kWidthSq, -1);
  for (int f = 1; f < 600; ++f) {
    const shake::Xform cur = shake::makeXform(p, f / 60.0, kWidthSq, -1);
    const double d = cur.dx - prev.dx;
    const double ad = d < 0 ? -d : d;
    if (ad > maxStep) maxStep = ad;
    prev = cur;
  }
  // At 8 Hz the noise advances 0.133 lattice units per 1/60 s, so the per-frame
  // step must stay well below the full amplitude - no discontinuities.
  check(maxStep < 40.0, "Normal style moves smoothly between adjacent frames");
  std::printf("  (max per-frame dx step at 60fps: %.2f px)\n", maxStep);
}

void testTwitchyHoldsStill() {
  shake::ShakeParams p = shake::defaultParams();
  p.style = shake::kStyleTwitchy;
  p.stillness = 0.7;
  p.twitchFrequency = 2.0;  // period 0.5 s, still for the first 0.35 s

  // Everything inside the still fraction of period 0 must be identical.
  const shake::XformSet a = shake::makeXformSet(p, 0.02, kWidthSq);
  const shake::XformSet b = shake::makeXformSet(p, 0.30, kWidthSq);
  check(a.base.dx == b.base.dx && a.base.dy == b.base.dy,
        "Twitchy is perfectly still during the stillness fraction");

  // And it must actually move during the remaining fraction.
  const shake::XformSet c = shake::makeXformSet(p, 0.38, kWidthSq);
  const shake::XformSet e = shake::makeXformSet(p, 0.49, kWidthSq);
  check(c.base.dx != e.base.dx, "Twitchy moves during the twitch fraction");
}

void testJumpySteps() {
  shake::ShakeParams p = shake::defaultParams();
  p.style = shake::kStyleJumpy;
  p.drift = 0.0;  // instant jumps, held between steps
  p.frequency = 4.0;
  p.x.randFreq = 1.0;  // one jump every 0.25 s

  const shake::Xform a = shake::makeXform(p, 0.05, kWidthSq, -1);
  const shake::Xform b = shake::makeXform(p, 0.20, kWidthSq, -1);
  check(a.dx == b.dx, "Jumpy with Drift 0 holds position between jumps");

  const shake::Xform c = shake::makeXform(p, 0.30, kWidthSq, -1);
  check(c.dx != a.dx, "Jumpy changes position at the next jump");
}

void testCenterBiasPullsToZero() {
  // With a large Center Bias most jump targets should be exactly the original
  // position, so count how many land on zero.
  int zeros = 0;
  const int n = 400;
  for (int k = 0; k < n; ++k)
    if (shake::jumpTarget(0, shake::kAxisX, k, 9.0) == 0.0) ++zeros;
  check(zeros > n / 2, "high Center Bias resets to the original position most of the time");

  int zerosNone = 0;
  for (int k = 0; k < n; ++k)
    if (shake::jumpTarget(0, shake::kAxisX, k, 0.0) == 0.0) ++zerosNone;
  check(zerosNone == 0, "Center Bias 0 never forces the original position");
}

void testWaveIsExactSine() {
  shake::ShakeParams p = shake::defaultParams();
  p.x.randAmp = 0.0;
  p.y.randAmp = 0.0;
  p.x.waveAmp = 1.0;
  p.x.waveFreq = 1.0;

  checkNear(shake::makeXform(p, 0.00, kWidthSq, -1).dx, 0.0, 1e-9, "X wave is 0 at t=0");
  checkNear(shake::makeXform(p, 0.25, kWidthSq, -1).dx, shake::kShakeScale * kWidthSq, 1e-9,
            "X wave peaks at t=1/4 period");
  checkNear(shake::makeXform(p, 0.50, kWidthSq, -1).dx, 0.0, 1e-6, "X wave is 0 at t=1/2 period");
  checkNear(shake::makeXform(p, 0.75, kWidthSq, -1).dx, -shake::kShakeScale * kWidthSq, 1e-9,
            "X wave troughs at t=3/4 period");
}

void testZoomAndTilt() {
  shake::ShakeParams p = shake::defaultParams();
  p.x.randAmp = 0.0;
  p.y.randAmp = 0.0;

  // Z: a positive offset moves the image away, so it scales down.
  p.z.waveAmp = 1.0;
  p.z.waveFreq = 1.0;
  p.zDist = 1.0;
  const double zoff = shake::kShakeScale;  // amp 1 * sin(peak) * kShakeScale
  checkNear(shake::makeXform(p, 0.25, kWidthSq, -1).scale, 1.0 / (1.0 + zoff), 1e-9,
            "Z shake zooms out by zDist/(zDist+offset)");
  p.z.waveAmp = 0.0;

  // Tilt amplitudes are in degrees.
  p.tilt.waveAmp = 90.0;
  p.tilt.waveFreq = 1.0;
  const shake::Xform t = shake::makeXform(p, 0.25, kWidthSq, -1);
  checkNear(t.cosA, 0.0, 1e-9, "Tilt Wave Amp 90 rotates by 90 degrees (cos)");
  checkNear(t.sinA, 1.0, 1e-9, "Tilt Wave Amp 90 rotates by 90 degrees (sin)");
}

void testChannelSeparation() {
  shake::ShakeParams p = shake::defaultParams();
  shake::XformSet u = shake::makeXformSet(p, 0.4, kWidthSq);
  check(u.uniform, "no channel separation by default");
  check(u.ch[0].dx == u.ch[2].dx, "channels share one transform by default");

  p.chanAmp[0] = 2.0;  // red shakes twice as far
  shake::XformSet s = shake::makeXformSet(p, 0.4, kWidthSq);
  check(!s.uniform, "a non-default Red Amplitude enables channel separation");
  checkNear(s.ch[0].dx, 2.0 * s.ch[1].dx, 1e-9, "Red Amplitude 2 doubles the red displacement");

  shake::ShakeParams q = shake::defaultParams();
  q.rgbRandomness = 0.5;
  shake::XformSet r = shake::makeXformSet(q, 0.4, kWidthSq);
  check(!r.uniform, "RGB Randomness enables channel separation");
  check(r.ch[0].dx != r.ch[1].dx, "RGB Randomness moves channels independently");
}

void testInverseMapRoundTrip() {
  shake::ShakeParams p = shake::defaultParams();
  p.z.waveAmp = 0.3;  // exercise scale
  p.tilt.waveAmp = 12.0;  // and rotation
  const shake::Xform xf = shake::makeXform(p, 0.37, kWidthSq, -1);
  const double cx = 960.0, cy = 540.0, par = 1.0;

  // Forward model: p_o = centre + scale * R(angle) * (p_s - centre) + offset.
  const double sx0 = 1234.0, sy0 = 321.0;
  const double vx = (sx0 - cx) * par, vy = (sy0 - cy);
  const double rx = xf.cosA * vx - xf.sinA * vy;
  const double ry = xf.sinA * vx + xf.cosA * vy;
  const double ox = cx + (rx * xf.scale + xf.dx) / par;
  const double oy = cy + ry * xf.scale + xf.dy;

  double bx, by;
  shake::inverseMap(xf, ox, oy, cx, cy, par, bx, by);
  checkNear(bx, sx0, 1e-6, "inverseMap undoes the forward warp (x)");
  checkNear(by, sy0, 1e-6, "inverseMap undoes the forward warp (y)");
}

// ---------------------------------------------------------------------------
// wrapping and sampling

void testWrapModes() {
  const int lo = 0, hi = 10;
  int v;

  v = 5;
  check(shake::wrapAxis(v, lo, hi, shake::kWrapNone) && v == 5, "Wrap No keeps in-range values");
  v = -1;
  check(!shake::wrapAxis(v, lo, hi, shake::kWrapNone), "Wrap No rejects values below the range");
  v = 10;
  check(!shake::wrapAxis(v, lo, hi, shake::kWrapNone), "Wrap No rejects values above the range");

  v = -1;
  check(shake::wrapAxis(v, lo, hi, shake::kWrapTile) && v == 9, "Wrap Tile repeats (-1 -> 9)");
  v = 10;
  check(shake::wrapAxis(v, lo, hi, shake::kWrapTile) && v == 0, "Wrap Tile repeats (10 -> 0)");
  v = -11;
  check(shake::wrapAxis(v, lo, hi, shake::kWrapTile) && v == 9, "Wrap Tile repeats (-11 -> 9)");

  v = -1;
  check(shake::wrapAxis(v, lo, hi, shake::kWrapReflect) && v == 0, "Wrap Reflect mirrors (-1 -> 0)");
  v = -2;
  check(shake::wrapAxis(v, lo, hi, shake::kWrapReflect) && v == 1, "Wrap Reflect mirrors (-2 -> 1)");
  v = 10;
  check(shake::wrapAxis(v, lo, hi, shake::kWrapReflect) && v == 9, "Wrap Reflect mirrors (10 -> 9)");
  v = 11;
  check(shake::wrapAxis(v, lo, hi, shake::kWrapReflect) && v == 8, "Wrap Reflect mirrors (11 -> 8)");
}

// A tiny single-channel float image to sample from.
struct Img {
  int w, h;
  std::vector<float> px;
  Img(int ww, int hh) : w(ww), h(hh), px((size_t)ww * hh, 0.0f) {}
  float& at(int x, int y) { return px[(size_t)y * w + x]; }
  float get(int x, int y) const { return px[(size_t)y * w + x]; }
};

struct ImgFetch {
  const Img* img;
  const float* operator()(int x, int y) const {
    if (x < 0 || y < 0 || x >= img->w || y >= img->h) return 0;
    return &img->px[(size_t)y * img->w + x];
  }
};

void testBilinearSampling() {
  Img im(4, 1);
  im.at(0, 0) = 0.0f;
  im.at(1, 0) = 10.0f;
  im.at(2, 0) = 20.0f;
  im.at(3, 0) = 30.0f;
  const shake::Rect rect = {0, 0, 4, 1};
  const ImgFetch fetch = {&im};

  double acc[4] = {0, 0, 0, 0};
  shake::sampleAll<float, 1>(fetch, rect, shake::kWrapReflect, shake::kWrapReflect, 1.5, 0.5, acc);
  checkNear(acc[0], 10.0, 1e-9, "sampling a pixel centre returns that pixel");

  acc[0] = 0.0;
  shake::sampleAll<float, 1>(fetch, rect, shake::kWrapReflect, shake::kWrapReflect, 2.0, 0.5, acc);
  checkNear(acc[0], 15.0, 1e-9, "sampling between two pixels averages them");

  // Wrap No: outside taps contribute nothing, giving a transparent edge.
  acc[0] = 0.0;
  shake::sampleAll<float, 1>(fetch, rect, shake::kWrapNone, shake::kWrapNone, -5.0, 0.5, acc);
  checkNear(acc[0], 0.0, 1e-9, "Wrap No returns nothing well outside the frame");

  const double v =
      shake::sampleComp<float>(fetch, rect, shake::kWrapReflect, shake::kWrapReflect, 2.0, 0.5, 0);
  checkNear(v, 15.0, 1e-9, "sampleComp matches sampleAll");
}

// Runs the same per-pixel loop the plug-in uses, on a single-channel image.
void renderOnce(const Img& src, Img& dst, const shake::Xform& xf, int wrapX, int wrapY) {
  const shake::Rect rect = {0, 0, src.w, src.h};
  const ImgFetch fetch = {&src};
  const double cx = 0.5 * src.w, cy = 0.5 * src.h, par = 1.0;
  for (int y = 0; y < dst.h; ++y) {
    for (int x = 0; x < dst.w; ++x) {
      double sx, sy;
      shake::inverseMap(xf, x + 0.5, y + 0.5, cx, cy, par, sx, sy);
      double acc[4] = {0, 0, 0, 0};
      shake::sampleAll<float, 1>(fetch, rect, wrapX, wrapY, sx, sy, acc);
      dst.at(x, y) = (float)acc[0];
    }
  }
}

void testIdentityRenderReproducesInput() {
  Img src(32, 24), dst(32, 24);
  for (int y = 0; y < src.h; ++y)
    for (int x = 0; x < src.w; ++x) src.at(x, y) = (float)(x * 7 + y * 13);

  shake::ShakeParams p = shake::defaultParams();
  p.amplitude = 0.0;
  const shake::Xform xf = shake::makeXform(p, 0.5, (double)src.w, -1);
  renderOnce(src, dst, xf, shake::kWrapReflect, shake::kWrapReflect);

  bool same = true;
  for (int y = 0; y < src.h && same; ++y)
    for (int x = 0; x < src.w; ++x)
      if (dst.get(x, y) != src.get(x, y)) {
        same = false;
        break;
      }
  check(same, "Amplitude 0 renders the input unchanged, pixel for pixel");
}

void testPureTranslationShiftsByExactPixels() {
  Img src(64, 16), dst(64, 16);
  for (int y = 0; y < src.h; ++y)
    for (int x = 0; x < src.w; ++x) src.at(x, y) = (float)(x * 3 + y);

  // Wave-only X motion: dx = amplitude * waveAmp * kShakeScale * widthSq at the
  // sine peak. With widthSq 64 and waveAmp 0.625 that is exactly 10 px.
  shake::ShakeParams p = shake::defaultParams();
  p.x.randAmp = 0.0;
  p.y.randAmp = 0.0;
  p.x.waveAmp = 0.625;
  p.x.waveFreq = 1.0;
  const shake::Xform xf = shake::makeXform(p, 0.25, (double)src.w, -1);
  checkNear(xf.dx, 10.0, 1e-9, "test setup produces exactly a 10 px offset");
  checkNear(xf.dy, 0.0, 1e-9, "test setup has no vertical offset");
  checkNear(xf.scale, 1.0, 1e-12, "test setup has no zoom");

  renderOnce(src, dst, xf, shake::kWrapReflect, shake::kWrapReflect);

  // A positive offset shifts the image right, so output(x) == input(x - 10).
  bool shifted = true;
  for (int y = 0; y < src.h && shifted; ++y)
    for (int x = 10; x < src.w; ++x)
      if (dst.get(x, y) != src.get(x - 10, y)) {
        shifted = false;
        break;
      }
  check(shifted, "a +10 px offset shifts the image right by exactly 10 px");
}

void testWrapNoLeavesEdgesEmpty() {
  Img src(64, 16), dst(64, 16);
  for (int y = 0; y < src.h; ++y)
    for (int x = 0; x < src.w; ++x) src.at(x, y) = 100.0f;

  shake::ShakeParams p = shake::defaultParams();
  p.x.randAmp = 0.0;
  p.y.randAmp = 0.0;
  p.x.waveAmp = 0.625;  // +10 px
  p.x.waveFreq = 1.0;
  const shake::Xform xf = shake::makeXform(p, 0.25, (double)src.w, -1);

  renderOnce(src, dst, xf, shake::kWrapNone, shake::kWrapNone);
  bool leftEmpty = true;
  for (int y = 0; y < dst.h; ++y)
    for (int x = 0; x < 9; ++x)
      if (dst.get(x, y) != 0.0f) leftEmpty = false;
  check(leftEmpty, "Wrap No leaves the uncovered edge empty");

  renderOnce(src, dst, xf, shake::kWrapReflect, shake::kWrapReflect);
  bool leftFilled = true;
  for (int y = 0; y < dst.h; ++y)
    for (int x = 0; x < 9; ++x)
      if (dst.get(x, y) != 100.0f) leftFilled = false;
  check(leftFilled, "Wrap Reflect fills the uncovered edge");
}

// The reported bug: with the shake alone, pixels near the edge are pulled from
// outside the source and come back empty (or mirrored). Auto Zoom must make the
// frame fully covered at every frame, so rendering with Wrap = No - which
// contributes nothing outside the source - must leave no pixel short.
void testAutoOverscanLeavesNoUncoveredPixel() {
  struct Case {
    const char* name;
    shake::ShakeParams p;
  };
  std::vector<Case> cases;
  {
    Case c = {"defaults", shake::defaultParams()};
    cases.push_back(c);
  }
  {
    Case c = {"large amplitude", shake::defaultParams()};
    c.p.amplitude = 3.0;
    cases.push_back(c);
  }
  {
    Case c = {"with tilt", shake::defaultParams()};
    c.p.tilt.randAmp = 8.0;
    c.p.tilt.waveAmp = 4.0;
    cases.push_back(c);
  }
  {
    Case c = {"with zoom shake", shake::defaultParams()};
    c.p.z.randAmp = 0.5;
    c.p.z.waveAmp = 0.3;
    cases.push_back(c);
  }
  {
    Case c = {"wave only", shake::defaultParams()};
    c.p.x.randAmp = 0.0;
    c.p.y.randAmp = 0.0;
    c.p.x.waveAmp = 0.8;
    c.p.y.waveAmp = 0.5;
    cases.push_back(c);
  }
  {
    Case c = {"channel separation", shake::defaultParams()};
    c.p.chanAmp[0] = 2.5;
    c.p.rgbRandomness = 0.4;
    cases.push_back(c);
  }
  {
    Case c = {"twitchy", shake::defaultParams()};
    c.p.style = shake::kStyleTwitchy;
    cases.push_back(c);
  }
  {
    Case c = {"jumpy", shake::defaultParams()};
    c.p.style = shake::kStyleJumpy;
    cases.push_back(c);
  }
  {
    Case c = {"everything at once", shake::defaultParams()};
    c.p.amplitude = 2.0;
    c.p.tilt.randAmp = 6.0;
    c.p.z.randAmp = 0.4;
    c.p.x.waveAmp = 0.3;
    c.p.y.waveAmp = 0.3;
    c.p.chanAmp[2] = 1.8;
    c.p.rgbRandomness = 0.2;
    cases.push_back(c);
  }

  Img src(96, 54), dst(96, 54);
  for (int y = 0; y < src.h; ++y)
    for (int x = 0; x < src.w; ++x) src.at(x, y) = 100.0f;

  for (size_t ci = 0; ci < cases.size(); ++ci) {
    const shake::ShakeParams& p = cases[ci].p;
    const double zoom = shake::autoOverscan(p, (double)src.w, (double)src.h);
    check(zoom >= 1.0, std::string("auto zoom never zooms out (") + cases[ci].name + ")");

    double worst = 1e30;
    for (int f = 0; f < 150; ++f) {
      shake::XformSet xs = shake::makeXformSet(p, f / 30.0, (double)src.w);
      shake::applyOverscan(xs, zoom);
      // Channel separation moves channels independently; the widest one decides.
      const shake::Xform& xf = xs.uniform ? xs.base : xs.ch[0];
      renderOnce(src, dst, xf, shake::kWrapNone, shake::kWrapNone);
      for (int y = 0; y < dst.h; ++y)
        for (int x = 0; x < dst.w; ++x) worst = std::min(worst, (double)dst.get(x, y));
    }
    // Every pixel must be fully sourced from inside the image.
    checkNear(worst, 100.0, 1e-4,
              std::string("auto zoom leaves no uncovered pixel (") + cases[ci].name + ")");
  }
}

void testAutoOverscanIsNeutralWhenStill() {
  shake::ShakeParams p = shake::defaultParams();
  p.amplitude = 0.0;
  checkNear(shake::autoOverscan(p, 1920.0, 1080.0), 1.0, 1e-12,
            "auto zoom is exactly 1 when there is no motion");

  shake::ShakeParams q = shake::defaultParams();
  // Defaults shift up to 5% of width, so expect roughly a 10% zoom.
  const double z = shake::autoOverscan(q, 1920.0, 1080.0);
  check(z > 1.05 && z < 1.25, "auto zoom for the defaults is a modest zoom");
  std::printf("  (auto zoom at defaults on 1920x1080: %.3f)\n", z);
}

void testManualZoomStacksOnAuto() {
  const shake::ShakeParams p = shake::defaultParams();
  shake::XformSet a = shake::makeXformSet(p, 0.4, 1920.0);
  shake::XformSet b = a;
  shake::applyOverscan(b, 2.0);
  checkNear(b.base.scale, a.base.scale * 2.0, 1e-12, "overscan multiplies the existing scale");
  for (int c = 0; c < 3; ++c)
    checkNear(b.ch[c].scale, a.ch[c].scale * 2.0, 1e-12,
              "overscan applies to every channel transform");
}

void testMotionBlurAveraging() {
  // Averaging several instants must land between the extremes of the motion,
  // which is what the plug-in's motion-blur loop relies on.
  const shake::ShakeParams p = shake::defaultParams();
  const double fps = 30.0, t = 1.0, len = 1.0;
  const int n = 8;
  double sum = 0.0, lo = 1e30, hi = -1e30;
  for (int i = 0; i < n; ++i) {
    const double ts = t + ((i + 0.5) / n - 0.5) * len / fps;
    const double dx = shake::makeXform(p, ts, kWidthSq, -1).dx;
    sum += dx;
    if (dx < lo) lo = dx;
    if (dx > hi) hi = dx;
  }
  const double avg = sum / n;
  check(avg >= lo && avg <= hi, "motion-blur average lies within the sampled range");
  check(hi > lo, "the shutter interval actually spans some motion");
}

}  // namespace

int main() {
  std::printf("ShakeCore tests\n");
  testDeterminismAndSeed();
  testAmplitudeZeroIsIdentity();
  testDefaultsAreBoundedAndMoving();
  testNormalStyleIsContinuous();
  testTwitchyHoldsStill();
  testJumpySteps();
  testCenterBiasPullsToZero();
  testWaveIsExactSine();
  testZoomAndTilt();
  testChannelSeparation();
  testInverseMapRoundTrip();
  testWrapModes();
  testBilinearSampling();
  testIdentityRenderReproducesInput();
  testPureTranslationShiftsByExactPixels();
  testWrapNoLeavesEdgesEmpty();
  testAutoOverscanLeavesNoUncoveredPixel();
  testAutoOverscanIsNeutralWhenStill();
  testManualZoomStacksOnAuto();
  testMotionBlurAveraging();

  std::printf("%d checks, %d failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
