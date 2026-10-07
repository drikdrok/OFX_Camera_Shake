/*
  Camera Shake - an OpenFX plug-in that reproduces the behaviour of Sapphire's
  S_Shake for hosts that do not have Sapphire installed (built for VEGAS Pro 2026).

  Parameter set and defaults follow the documented S_Shake controls, with two
  deliberate omissions and one addition:

    * The Mocha tracking controls (Mocha Project, Blur Mocha, Dilate Mocha, ...)
      are not reproduced - Mocha is proprietary and not available to us.
    * The separate Mask *input clip* (and its Mask Use / Blur Mask / Invert Mask
      controls) is not reproduced, because VEGAS has no way to route a second
      clip into a Video FX chain, so the input would be unusable there.
    * "Mo Blur Samples" is added, because Sapphire decides the motion-blur sample
      count internally and OFX gives us no equivalent quality hint.

  The motion model and pixel sampling live in ShakeCore.h so they can be tested
  without a host; this file is the OFX plumbing around them.
*/

#include "ShakeCore.h"

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace {

const char* const kParamStyle = "style";
const char* const kParamAmplitude = "amplitude";
const char* const kParamFrequency = "frequency";
const char* const kParamPhase = "phase";
const char* const kParamStillness = "stillness";
const char* const kParamTwitchFreq = "twitchFrequency";
const char* const kParamDrift = "drift";
const char* const kParamCenterBias = "centerBias";
const char* const kParamZDist = "zDist";
const char* const kParamAutoZoom = "autoZoom";
const char* const kParamZoom = "zoom";
const char* const kParamMotionBlur = "motionBlur";
const char* const kParamMoBlurLength = "moBlurLength";
const char* const kParamMoBlurSamples = "moBlurSamples";
const char* const kParamSeed = "seed";
const char* const kParamWrapX = "wrapX";
const char* const kParamWrapY = "wrapY";
const char* const kParamRgbRandomness = "rgbRandomness";
const char* const kParamRgbFrequency = "rgbFrequency";
const char* const kParamCropTop = "cropTop";
const char* const kParamCropBottom = "cropBottom";
const char* const kParamCropLeft = "cropLeft";
const char* const kParamCropRight = "cropRight";

const char* const kAxisPrefix[4] = {"x", "y", "z", "tilt"};
const char* const kAxisSuffix[5] = {"RandAmp", "RandFreq", "WaveAmp", "WaveFreq", "Phase"};
const char* const kChanPrefix[3] = {"red", "green", "blue"};

////////////////////////////////////////////////////////////////////////////////
// processing

class ShakeProcessorBase : public OFX::ImageProcessor {
 protected:
  const OFX::Image* _srcImg;
  std::vector<shake::XformSet> _samples;
  shake::Rect _srcRect;  // effective (cropped) source rect
  double _cx, _cy;       // shake centre, in pixel coordinates
  double _par;           // pixel aspect ratio
  int _wrapX, _wrapY;

 public:
  explicit ShakeProcessorBase(OFX::ImageEffect& instance)
      : OFX::ImageProcessor(instance),
        _srcImg(0),
        _cx(0.0),
        _cy(0.0),
        _par(1.0),
        _wrapX(shake::kWrapReflect),
        _wrapY(shake::kWrapReflect) {
    _srcRect.x1 = _srcRect.y1 = _srcRect.x2 = _srcRect.y2 = 0;
  }

  void setSrcImg(const OFX::Image* v) { _srcImg = v; }

  void setValues(const std::vector<shake::XformSet>& samples, const shake::Rect& srcRect, double cx,
                 double cy, double par, int wrapX, int wrapY) {
    _samples = samples;
    _srcRect = srcRect;
    _cx = cx;
    _cy = cy;
    _par = par;
    _wrapX = wrapX;
    _wrapY = wrapY;
  }
};

template <class PIX, int nComponents, int maxValue>
class ShakeProcessor : public ShakeProcessorBase {
 public:
  explicit ShakeProcessor(OFX::ImageEffect& instance) : ShakeProcessorBase(instance) {}

  void multiThreadProcessImages(OfxRectI window) {
    const int nSamples = (int)_samples.size();
    const double norm = nSamples > 0 ? 1.0 / nSamples : 1.0;
    // Colour channels present; component 3 (alpha) follows the base transform.
    const int nColour = nComponents >= 3 ? 3 : 0;

    const OFX::Image* srcImg = _srcImg;
    const Fetch fetch = {srcImg};

    for (int y = window.y1; y < window.y2; ++y) {
      if (_effect.abort()) break;
      PIX* dst = (PIX*)_dstImg->getPixelAddress(window.x1, y);
      if (!dst) continue;
      const double py = y + 0.5;

      for (int x = window.x1; x < window.x2; ++x, dst += nComponents) {
        const double px = x + 0.5;
        double acc[4] = {0.0, 0.0, 0.0, 0.0};

        for (int s = 0; s < nSamples; ++s) {
          const shake::XformSet& xs = _samples[s];
          double sx, sy;
          if (xs.uniform || nColour == 0) {
            shake::inverseMap(xs.base, px, py, _cx, _cy, _par, sx, sy);
            shake::sampleAll<PIX, nComponents>(fetch, _srcRect, _wrapX, _wrapY, sx, sy, acc);
          } else {
            for (int c = 0; c < nColour; ++c) {
              shake::inverseMap(xs.ch[c], px, py, _cx, _cy, _par, sx, sy);
              acc[c] += shake::sampleComp<PIX>(fetch, _srcRect, _wrapX, _wrapY, sx, sy, c);
            }
            if (nComponents == 4) {
              shake::inverseMap(xs.base, px, py, _cx, _cy, _par, sx, sy);
              acc[3] += shake::sampleComp<PIX>(fetch, _srcRect, _wrapX, _wrapY, sx, sy, 3);
            }
          }
        }

        for (int c = 0; c < nComponents; ++c) dst[c] = toPix(acc[c] * norm);
      }
    }
  }

 private:
  struct Fetch {
    const OFX::Image* img;
    const PIX* operator()(int x, int y) const {
      return (const PIX*)img->getPixelAddress(x, y);
    }
  };

  inline PIX toPix(double v) const {
    if (maxValue == 1) return (PIX)v;  // float: leave headroom alone
    const double r = v + 0.5;
    if (r <= 0.0) return (PIX)0;
    if (r >= (double)maxValue) return (PIX)maxValue;
    return (PIX)r;
  }
};

////////////////////////////////////////////////////////////////////////////////
// the effect

class CameraShakePlugin : public OFX::ImageEffect {
 public:
  explicit CameraShakePlugin(OfxImageEffectHandle handle);

  virtual void render(const OFX::RenderArguments& args);
  virtual bool isIdentity(const OFX::IsIdentityArguments& args, OFX::Clip*& identityClip,
                          double& identityTime);
  virtual void getRegionsOfInterest(const OFX::RegionsOfInterestArguments& args,
                                    OFX::RegionOfInterestSetter& rois);
  virtual void getClipPreferences(OFX::ClipPreferencesSetter& clipPreferences);

 private:
  void setupAndProcess(ShakeProcessorBase& proc, const OFX::RenderArguments& args);
  shake::ShakeParams readParams(double time) const;

  OFX::Clip* _dstClip;
  OFX::Clip* _srcClip;

  OFX::ChoiceParam* _style;
  OFX::DoubleParam* _amplitude;
  OFX::DoubleParam* _frequency;
  OFX::DoubleParam* _phase;
  OFX::DoubleParam* _stillness;
  OFX::DoubleParam* _twitchFrequency;
  OFX::DoubleParam* _drift;
  OFX::DoubleParam* _centerBias;
  OFX::DoubleParam* _zDist;
  OFX::BooleanParam* _autoZoom;
  OFX::DoubleParam* _zoom;
  OFX::BooleanParam* _motionBlur;
  OFX::DoubleParam* _moBlurLength;
  OFX::IntParam* _moBlurSamples;
  OFX::DoubleParam* _seed;
  OFX::ChoiceParam* _wrapX;
  OFX::ChoiceParam* _wrapY;
  OFX::DoubleParam* _rgbRandomness;
  OFX::DoubleParam* _rgbFrequency;
  OFX::DoubleParam* _cropTop;
  OFX::DoubleParam* _cropBottom;
  OFX::DoubleParam* _cropLeft;
  OFX::DoubleParam* _cropRight;

  // [axis][0..4] = randAmp, randFreq, waveAmp, waveFreq, phase
  OFX::DoubleParam* _axis[4][5];
  OFX::DoubleParam* _chanAmp[3];
  OFX::DoubleParam* _chanPhase[3];
};

CameraShakePlugin::CameraShakePlugin(OfxImageEffectHandle handle) : OFX::ImageEffect(handle) {
  _dstClip = fetchClip(kOfxImageEffectOutputClipName);
  _srcClip = fetchClip(kOfxImageEffectSimpleSourceClipName);

  _style = fetchChoiceParam(kParamStyle);
  _amplitude = fetchDoubleParam(kParamAmplitude);
  _frequency = fetchDoubleParam(kParamFrequency);
  _phase = fetchDoubleParam(kParamPhase);
  _stillness = fetchDoubleParam(kParamStillness);
  _twitchFrequency = fetchDoubleParam(kParamTwitchFreq);
  _drift = fetchDoubleParam(kParamDrift);
  _centerBias = fetchDoubleParam(kParamCenterBias);
  _zDist = fetchDoubleParam(kParamZDist);
  _autoZoom = fetchBooleanParam(kParamAutoZoom);
  _zoom = fetchDoubleParam(kParamZoom);
  _motionBlur = fetchBooleanParam(kParamMotionBlur);
  _moBlurLength = fetchDoubleParam(kParamMoBlurLength);
  _moBlurSamples = fetchIntParam(kParamMoBlurSamples);
  _seed = fetchDoubleParam(kParamSeed);
  _wrapX = fetchChoiceParam(kParamWrapX);
  _wrapY = fetchChoiceParam(kParamWrapY);
  _rgbRandomness = fetchDoubleParam(kParamRgbRandomness);
  _rgbFrequency = fetchDoubleParam(kParamRgbFrequency);
  _cropTop = fetchDoubleParam(kParamCropTop);
  _cropBottom = fetchDoubleParam(kParamCropBottom);
  _cropLeft = fetchDoubleParam(kParamCropLeft);
  _cropRight = fetchDoubleParam(kParamCropRight);

  for (int a = 0; a < 4; ++a)
    for (int k = 0; k < 5; ++k)
      _axis[a][k] = fetchDoubleParam(std::string(kAxisPrefix[a]) + kAxisSuffix[k]);

  for (int c = 0; c < 3; ++c) {
    _chanAmp[c] = fetchDoubleParam(std::string(kChanPrefix[c]) + "Amplitude");
    _chanPhase[c] = fetchDoubleParam(std::string(kChanPrefix[c]) + "Phase");
  }
}

shake::ShakeParams CameraShakePlugin::readParams(double time) const {
  shake::ShakeParams p;
  int style = shake::kStyleNormal;
  _style->getValueAtTime(time, style);
  p.style = style;
  p.amplitude = _amplitude->getValueAtTime(time);
  p.frequency = _frequency->getValueAtTime(time);
  p.phase = _phase->getValueAtTime(time);
  p.stillness = _stillness->getValueAtTime(time);
  p.twitchFrequency = _twitchFrequency->getValueAtTime(time);
  p.drift = _drift->getValueAtTime(time);
  p.centerBias = _centerBias->getValueAtTime(time);
  p.zDist = _zDist->getValueAtTime(time);
  p.seed = (uint32_t)std::max(0.0, _seed->getValueAtTime(time) * 1000.0 + 0.5);
  p.rgbRandomness = _rgbRandomness->getValueAtTime(time);
  p.rgbFrequency = _rgbFrequency->getValueAtTime(time);

  shake::AxisParams* axes[4] = {&p.x, &p.y, &p.z, &p.tilt};
  for (int a = 0; a < 4; ++a) {
    axes[a]->randAmp = _axis[a][0]->getValueAtTime(time);
    axes[a]->randFreq = _axis[a][1]->getValueAtTime(time);
    axes[a]->waveAmp = _axis[a][2]->getValueAtTime(time);
    axes[a]->waveFreq = _axis[a][3]->getValueAtTime(time);
    axes[a]->phase = _axis[a][4]->getValueAtTime(time);
  }
  for (int c = 0; c < 3; ++c) {
    p.chanAmp[c] = _chanAmp[c]->getValueAtTime(time);
    p.chanPhase[c] = _chanPhase[c]->getValueAtTime(time);
  }
  return p;
}

bool CameraShakePlugin::isIdentity(const OFX::IsIdentityArguments& args, OFX::Clip*& identityClip,
                                   double& identityTime) {
  const shake::ShakeParams p = readParams(args.time);
  const bool anyMotion = p.x.randAmp != 0.0 || p.x.waveAmp != 0.0 || p.y.randAmp != 0.0 ||
                         p.y.waveAmp != 0.0 || p.z.randAmp != 0.0 || p.z.waveAmp != 0.0 ||
                         p.tilt.randAmp != 0.0 || p.tilt.waveAmp != 0.0 || p.rgbRandomness != 0.0;
  // A manual Zoom still scales the image even with no shake at all, so it has
  // to block the identity shortcut. Auto Zoom resolves to 1 when nothing moves.
  const bool scaling = _zoom->getValueAtTime(args.time) != 1.0;
  if ((p.amplitude == 0.0 || !anyMotion) && !scaling) {
    identityClip = _srcClip;
    identityTime = args.time;
    return true;
  }
  return false;
}

void CameraShakePlugin::getClipPreferences(OFX::ClipPreferencesSetter& clipPreferences) {
  // The shake produces a different image on every frame even when no parameter
  // is keyframed. Without this the host is entitled to render one frame and
  // reuse it for the whole event, which looks like a frozen shake on playback.
  clipPreferences.setOutputFrameVarying(true);
}

void CameraShakePlugin::getRegionsOfInterest(const OFX::RegionsOfInterestArguments& args,
                                             OFX::RegionOfInterestSetter& rois) {
  // The warp reads well outside the output window, so ask for the whole frame.
  rois.setRegionOfInterest(*_srcClip, _srcClip->getRegionOfDefinition(args.time));
}

void CameraShakePlugin::setupAndProcess(ShakeProcessorBase& proc,
                                        const OFX::RenderArguments& args) {
  std::unique_ptr<OFX::Image> dst(_dstClip->fetchImage(args.time));
  if (!dst.get()) OFX::throwSuiteStatusException(kOfxStatFailed);
  std::unique_ptr<const OFX::Image> src(_srcClip->fetchImage(args.time));
  if (!src.get()) OFX::throwSuiteStatusException(kOfxStatFailed);

  if (src->getPixelDepth() != dst->getPixelDepth() ||
      src->getPixelComponents() != dst->getPixelComponents())
    OFX::throwSuiteStatusException(kOfxStatErrImageFormat);

  const shake::ShakeParams p = readParams(args.time);

  // Effective source rect after Crop Input. OFX y is bottom-up, so "top"
  // trims y2 and "bottom" trims y1.
  const OfxRectI sb = src->getBounds();
  const int sw = sb.x2 - sb.x1;
  const int sh = sb.y2 - sb.y1;
  shake::Rect r;
  r.x1 = sb.x1 + (int)std::floor(_cropLeft->getValueAtTime(args.time) * sw + 0.5);
  r.x2 = sb.x2 - (int)std::floor(_cropRight->getValueAtTime(args.time) * sw + 0.5);
  r.y1 = sb.y1 + (int)std::floor(_cropBottom->getValueAtTime(args.time) * sh + 0.5);
  r.y2 = sb.y2 - (int)std::floor(_cropTop->getValueAtTime(args.time) * sh + 0.5);
  r.x1 = std::max(r.x1, sb.x1);
  r.y1 = std::max(r.y1, sb.y1);
  r.x2 = std::min(r.x2, sb.x2);
  r.y2 = std::min(r.y2, sb.y2);
  if (r.x2 <= r.x1) r.x2 = r.x1 + 1;
  if (r.y2 <= r.y1) r.y2 = r.y1 + 1;

  double par = _srcClip->getPixelAspectRatio();
  if (!(par > 0.0)) par = 1.0;
  const double cx = 0.5 * (r.x1 + r.x2);
  const double cy = 0.5 * (r.y1 + r.y2);
  const double widthSq = (double)(r.x2 - r.x1) * par;

  double fps = _srcClip->getFrameRate();
  if (!(fps > 0.0)) fps = 25.0;
  const double tSec = args.time / fps;

  // Motion blur: average the warp over the shutter interval.
  int nSamples = 1;
  double blurLength = 0.0;
  if (_motionBlur->getValueAtTime(args.time)) {
    blurLength = _moBlurLength->getValueAtTime(args.time);
    if (blurLength > 0.0) nSamples = std::max(1, _moBlurSamples->getValueAtTime(args.time));
  }

  // Overscan: zoom in enough that the shake never drags an edge into frame.
  // Constant for the whole clip, so it cannot read as the image breathing.
  double overscan = _zoom->getValueAtTime(args.time);
  if (_autoZoom->getValueAtTime(args.time))
    overscan *= shake::autoOverscan(p, widthSq, (double)(r.y2 - r.y1));

  std::vector<shake::XformSet> samples;
  samples.reserve(nSamples);
  for (int i = 0; i < nSamples; ++i) {
    double ts = tSec;
    if (nSamples > 1) ts += ((i + 0.5) / nSamples - 0.5) * blurLength / fps;
    shake::XformSet xs = shake::makeXformSet(p, ts, widthSq);
    if (overscan != 1.0) shake::applyOverscan(xs, overscan);
    samples.push_back(xs);
  }

  int wrapX = shake::kWrapReflect, wrapY = shake::kWrapReflect;
  _wrapX->getValueAtTime(args.time, wrapX);
  _wrapY->getValueAtTime(args.time, wrapY);

  proc.setDstImg(dst.get());
  proc.setSrcImg(src.get());
  proc.setRenderWindow(args.renderWindow);
  proc.setValues(samples, r, cx, cy, par, wrapX, wrapY);
  proc.process();
}

void CameraShakePlugin::render(const OFX::RenderArguments& args) {
  const OFX::BitDepthEnum depth = _dstClip->getPixelDepth();
  const OFX::PixelComponentEnum comps = _dstClip->getPixelComponents();

#define SHAKE_DISPATCH(NCOMP)                                 \
  switch (depth) {                                            \
    case OFX::eBitDepthUByte: {                               \
      ShakeProcessor<unsigned char, NCOMP, 255> f(*this);     \
      setupAndProcess(f, args);                               \
      break;                                                  \
    }                                                         \
    case OFX::eBitDepthUShort: {                              \
      ShakeProcessor<unsigned short, NCOMP, 65535> f(*this);  \
      setupAndProcess(f, args);                               \
      break;                                                  \
    }                                                         \
    case OFX::eBitDepthFloat: {                               \
      ShakeProcessor<float, NCOMP, 1> f(*this);               \
      setupAndProcess(f, args);                                \
      break;                                                  \
    }                                                         \
    default:                                                  \
      OFX::throwSuiteStatusException(kOfxStatErrUnsupported);  \
  }

  if (comps == OFX::ePixelComponentRGBA) {
    SHAKE_DISPATCH(4)
  } else if (comps == OFX::ePixelComponentRGB) {
    SHAKE_DISPATCH(3)
  } else if (comps == OFX::ePixelComponentAlpha) {
    SHAKE_DISPATCH(1)
  } else {
    OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
  }
#undef SHAKE_DISPATCH
}

////////////////////////////////////////////////////////////////////////////////
// description

mDeclarePluginFactory(CameraShakeFactory, {}, {});

OFX::DoubleParamDescriptor* defDouble(OFX::ImageEffectDescriptor& desc,
                                      OFX::PageParamDescriptor* page,
                                      OFX::GroupParamDescriptor* group, const std::string& name,
                                      const std::string& label, const std::string& hint,
                                      double def, double mn, double mx, double dmn, double dmx) {
  OFX::DoubleParamDescriptor* p = desc.defineDoubleParam(name);
  p->setLabels(label, label, label);
  p->setHint(hint);
  p->setDefault(def);
  p->setRange(mn, mx);
  p->setDisplayRange(dmn, dmx);
  if (group) p->setParent(*group);
  if (page) page->addChild(*p);
  return p;
}

OFX::GroupParamDescriptor* defGroup(OFX::ImageEffectDescriptor& desc,
                                    OFX::PageParamDescriptor* page, const std::string& name,
                                    const std::string& label) {
  OFX::GroupParamDescriptor* g = desc.defineGroupParam(name);
  g->setLabels(label, label, label);
  g->setOpen(false);
  if (page) page->addChild(*g);
  return g;
}

void CameraShakeFactory::describe(OFX::ImageEffectDescriptor& desc) {
  desc.setLabels("Camera Shake", "Camera Shake", "Camera Shake");
  desc.setPluginGrouping("Distort");
  desc.setPluginDescription(
      "Shakes the image with random and/or regular wave motion on the X, Y, zoom and rotation "
      "axes, with optional per-channel separation and motion blur. Parameters and defaults "
      "follow Sapphire's S_Shake; Mocha tracking and the separate Mask input are not included.");

  desc.addSupportedContext(OFX::eContextFilter);
  desc.addSupportedContext(OFX::eContextGeneral);
  desc.addSupportedBitDepth(OFX::eBitDepthUByte);
  desc.addSupportedBitDepth(OFX::eBitDepthUShort);
  desc.addSupportedBitDepth(OFX::eBitDepthFloat);

  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(true);
  // The warp reads the whole frame, so take un-tiled images and avoid seams.
  desc.setSupportsTiles(false);
  desc.setTemporalClipAccess(false);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(false);
  desc.setRenderThreadSafety(OFX::eRenderFullySafe);
}

void CameraShakeFactory::describeInContext(OFX::ImageEffectDescriptor& desc,
                                           OFX::ContextEnum /*context*/) {
  OFX::ClipDescriptor* src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  src->addSupportedComponent(OFX::ePixelComponentRGBA);
  src->addSupportedComponent(OFX::ePixelComponentRGB);
  src->addSupportedComponent(OFX::ePixelComponentAlpha);
  src->setTemporalClipAccess(false);
  src->setSupportsTiles(false);
  src->setIsMask(false);

  OFX::ClipDescriptor* dst = desc.defineClip(kOfxImageEffectOutputClipName);
  dst->addSupportedComponent(OFX::ePixelComponentRGBA);
  dst->addSupportedComponent(OFX::ePixelComponentRGB);
  dst->addSupportedComponent(OFX::ePixelComponentAlpha);
  dst->setSupportsTiles(false);

  OFX::PageParamDescriptor* page = desc.definePageParam("Controls");
  const shake::ShakeParams d = shake::defaultParams();

  // --- main shake ---------------------------------------------------------
  {
    OFX::ChoiceParamDescriptor* style = desc.defineChoiceParam(kParamStyle);
    style->setLabels("Style", "Style", "Style");
    style->setHint(
        "Normal shakes continuously; Twitchy holds still then twitches; Jumpy jumps between "
        "positions.");
    style->appendOption("Normal");
    style->appendOption("Twitchy");
    style->appendOption("Jumpy");
    style->setDefault(shake::kStyleNormal);
    page->addChild(*style);
  }

  defDouble(desc, page, 0, kParamAmplitude, "Amplitude",
            "Scales the amplitude of the shaking motion.", d.amplitude, 0.0, 1e6, 0.0, 4.0);
  defDouble(desc, page, 0, kParamFrequency, "Frequency",
            "Increase for faster shaking, decrease for slower shaking.", d.frequency, 0.0, 1e6,
            0.0, 30.0);
  defDouble(desc, page, 0, kParamPhase, "Phase", "Time shift of the shaking motions, in seconds.",
            d.phase, -1e6, 1e6, -10.0, 10.0);
  defDouble(desc, page, 0, kParamStillness, "Stillness",
            "Twitchy style: fraction of the time the image remains still.", d.stillness, 0.0, 1.0,
            0.0, 1.0);
  defDouble(desc, page, 0, kParamTwitchFreq, "Twitch Frequency",
            "Twitchy style: length of the period of movement and stillness.", d.twitchFrequency,
            0.0, 1e6, 0.0, 20.0);
  defDouble(desc, page, 0, kParamDrift, "Drift", "Jumpy style: speed of motion between jumps.",
            d.drift, 0.0, 1.0, 0.0, 1.0);
  defDouble(desc, page, 0, kParamCenterBias, "Center Bias",
            "Jumpy style: likelihood of resetting to the original position.", d.centerBias, 0.0,
            1e6, 0.0, 4.0);
  defDouble(desc, page, 0, kParamZDist, "Z Dist", "Scales the 'distance' of the image.", d.zDist,
            0.001, 1e6, 0.001, 10.0);

  {
    OFX::BooleanParamDescriptor* az = desc.defineBooleanParam(kParamAutoZoom);
    az->setLabels("Auto Zoom", "Auto Zoom", "Auto Zoom");
    az->setHint(
        "Zoom in just enough that the shake never pulls an empty or mirrored edge into frame. "
        "Turn off to keep the image at its original size and control the framing yourself.");
    az->setDefault(true);
    page->addChild(*az);
  }
  defDouble(desc, page, 0, kParamZoom, "Zoom",
            "Extra zoom applied on top of Auto Zoom. Raise it if you still see an edge, or use "
            "it on its own with Auto Zoom off.",
            1.0, 0.01, 100.0, 1.0, 2.0);

  {
    OFX::BooleanParamDescriptor* mb = desc.defineBooleanParam(kParamMotionBlur);
    mb->setLabels("Motion Blur", "Motion Blur", "Motion Blur");
    mb->setHint("Enable motion blur of the shaking motion.");
    mb->setDefault(false);
    page->addChild(*mb);
  }
  defDouble(desc, page, 0, kParamMoBlurLength, "Mo Blur Length",
            "Scales the amount of motion blur.", 1.0, 0.0, 1e6, 0.0, 4.0);
  {
    OFX::IntParamDescriptor* ms = desc.defineIntParam(kParamMoBlurSamples);
    ms->setLabels("Mo Blur Samples", "Mo Blur Samples", "Mo Blur Samples");
    ms->setHint("Number of motion blur samples. Higher is smoother but slower.");
    ms->setDefault(8);
    ms->setRange(1, 256);
    ms->setDisplayRange(1, 32);
    page->addChild(*ms);
  }

  defDouble(desc, page, 0, kParamSeed, "Seed", "Used to initialize the random number generator.",
            0.0, 0.0, 1e6, 0.0, 100.0);

  const char* const wrapNames[2] = {kParamWrapX, kParamWrapY};
  const char* const wrapLabels[2] = {"Wrap X", "Wrap Y"};
  for (int i = 0; i < 2; ++i) {
    OFX::ChoiceParamDescriptor* w = desc.defineChoiceParam(wrapNames[i]);
    w->setLabels(wrapLabels[i], wrapLabels[i], wrapLabels[i]);
    w->setHint("How to access pixels outside the frame when the image is shaken into view.");
    w->appendOption("No");
    w->appendOption("Tile");
    w->appendOption("Reflect");
    w->setDefault(shake::kWrapReflect);
    page->addChild(*w);
  }

  // --- per-axis groups ---------------------------------------------------
  struct AxisDefaults {
    const char* label;        // group label
    const char* labelPrefix;  // prefix for each param label, e.g. "X "
    const char* unit;         // wording used in the hints
    const shake::AxisParams* defs;
  };
  const AxisDefaults axisDefs[4] = {
      {"X Shake", "X ", "horizontal", &d.x},
      {"Y Shake", "Y ", "vertical", &d.y},
      {"Z Shake", "Z ", "zoom", &d.z},
      {"Tilt Shake", "Tilt ", "rotational", &d.tilt},
  };
  const char* const axisLabels[5] = {"Rand Amp", "Rand Freq", "Wave Amp", "Wave Freq", "Phase"};

  for (int a = 0; a < 4; ++a) {
    OFX::GroupParamDescriptor* g =
        defGroup(desc, page, std::string(kAxisPrefix[a]) + "Group", axisDefs[a].label);
    const std::string unit = axisDefs[a].unit;
    const bool degrees = (a == 3);  // tilt amplitudes are in degrees
    const std::string ampUnit = degrees ? ", in degrees" : "";
    const shake::AxisParams& ad = *axisDefs[a].defs;
    const double defs[5] = {ad.randAmp, ad.randFreq, ad.waveAmp, ad.waveFreq, ad.phase};

    for (int k = 0; k < 5; ++k) {
      const std::string name = std::string(kAxisPrefix[a]) + kAxisSuffix[k];
      const std::string label = std::string(axisDefs[a].labelPrefix) + axisLabels[k];
      std::string hint;
      switch (k) {
        case 0: hint = "Amplitude of the " + unit + " random shaking" + ampUnit + "."; break;
        case 1: hint = "Frequency of the " + unit + " random shaking."; break;
        case 2: hint = "Amplitude of the " + unit + " regular wave shaking" + ampUnit + "."; break;
        case 3:
          hint = "Frequency of the " + unit + " regular wave shaking, in cycles per second.";
          break;
        default: hint = "Time shift of the " + unit + " shaking, in seconds."; break;
      }
      const bool isPhase = (k == 4);
      const bool isAmp = (k == 0 || k == 2);
      const double dmax = isAmp ? (degrees ? 45.0 : 1.0) : 20.0;
      defDouble(desc, page, g, name, label, hint, defs[k], isPhase ? -1e6 : 0.0, 1e6,
                isPhase ? -10.0 : 0.0, isPhase ? 10.0 : dmax);
    }
  }

  // --- channel separation ------------------------------------------------
  {
    OFX::GroupParamDescriptor* g = defGroup(desc, page, "channelGroup", "Channels");
    const char* const chanLabels[3] = {"Red", "Green", "Blue"};
    for (int c = 0; c < 3; ++c)
      defDouble(desc, page, g, std::string(kChanPrefix[c]) + "Amplitude",
                std::string(chanLabels[c]) + " Amplitude",
                "The relative amount of shaking in the " + std::string(chanLabels[c]) + " channel.",
                1.0, 0.0, 1e6, 0.0, 4.0);
    for (int c = 0; c < 3; ++c)
      defDouble(desc, page, g, std::string(kChanPrefix[c]) + "Phase",
                std::string(chanLabels[c]) + " Phase",
                "The relative phase of the " + std::string(chanLabels[c]) + " channel, in seconds.",
                0.0, -1e6, 1e6, -1.0, 1.0);
    defDouble(desc, page, g, kParamRgbRandomness, "RGB Randomness",
              "The amount of random motion in each color channel.", d.rgbRandomness, 0.0, 1e6, 0.0,
              1.0);
    defDouble(desc, page, g, kParamRgbFrequency, "RGB Frequency",
              "The frequency of the random color channel shaking.", d.rgbFrequency, 0.0, 1e6, 0.0,
              20.0);
  }

  // --- crop input --------------------------------------------------------
  {
    OFX::GroupParamDescriptor* g = defGroup(desc, page, "cropGroup", "Crop Input");
    const char* const names[4] = {kParamCropTop, kParamCropBottom, kParamCropLeft,
                                  kParamCropRight};
    const char* const labels[4] = {"Crop Top", "Crop Bottom", "Crop Left", "Crop Right"};
    for (int i = 0; i < 4; ++i)
      defDouble(desc, page, g, names[i], labels[i],
                "Selects a rectangular subsection of the input to use.", 0.0, 0.0, 1.0, 0.0, 0.5);
  }
}

OFX::ImageEffect* CameraShakeFactory::createInstance(OfxImageEffectHandle handle,
                                                     OFX::ContextEnum /*context*/) {
  return new CameraShakePlugin(handle);
}

}  // namespace

namespace OFX {
namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray& ids) {
  static CameraShakeFactory factory("net.drikdrok.CameraShake", 1, 0);
  ids.push_back(&factory);
}
}  // namespace Plugin
}  // namespace OFX
