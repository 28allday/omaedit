#include "lineargrade.h"
#include "affine.h"
#include "colourpipeline.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace {
double number(const QVariantMap &map, const char *key, double fallback = 0) {
  const double v = map.value(key, fallback).toDouble();
  return std::isfinite(v) ? v : fallback;
}
double wheel(const QVariantMap &map, const char *key, int channel,
             double fallback = 0) {
  const auto values = map.value(key).toList();
  return values.isEmpty() ? fallback : values.value(channel).toDouble();
}
double signedPower(double v, double p) {
  return std::copysign(std::pow(std::abs(v), p), v);
}
using Points = std::vector<std::array<double, 2>>;
Points points(const QVariant &value) {
  Points out;
  for (const auto &v : value.toList()) {
    const auto p = v.toList();
    if (p.size() >= 2 && std::isfinite(p[0].toDouble()) &&
        std::isfinite(p[1].toDouble()))
      out.push_back({p[0].toDouble(), p[1].toDouble()});
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const auto &a, const auto &b) { return a[0] < b[0]; });
  return out;
}
// Extend endpoint slopes rather than clipping extended scene values.
double sample(const Points &p, double x, double fallback) {
  if (p.empty())
    return fallback;
  if (p.size() == 1)
    return p.front()[1];
  size_t right = 1;
  while (right < p.size() - 1 && p[right][0] < x)
    ++right;
  const auto &a = p[right - 1], &b = p[right];
  if (b[0] <= a[0])
    return b[1];
  return a[1] + (x - a[0]) * (b[1] - a[1]) / (b[0] - a[0]);
}
bool changed(const Points &p, bool flat = false) {
  for (const auto &v : p)
    if (std::abs(v[1] - (flat ? .5 : v[0])) > 1e-12)
      return true;
  return false;
}
double luminance(const double *rgb) {
  return .2126 * rgb[0] + .7152 * rgb[1] + .0722 * rgb[2];
}
// The hue and saturation coordinates the curve and warper editors are drawn
// in: the colour-wheel hue (red 0, green 1/3, blue 2/3) and a 0..1 saturation
// where a primary is 1 and neutral is 0. Scene values above 1 keep their
// ratio; a negative (out-of-gamut) component saturates at 1. Callers pass
// display-like values (see encoded()), so a colour sits where it looks.
void hueSaturation(const double *rgb, double *hue, double *saturation) {
  const double high = std::max({rgb[0], rgb[1], rgb[2]}),
               low = std::min({rgb[0], rgb[1], rgb[2]}), range = high - low;
  *saturation = high > 1e-8 ? std::min(1., range / high) : 0;
  if (range <= 0) {
    *hue = 0;
    return;
  }
  double h = high == rgb[0]   ? (rgb[1] - rgb[2]) / range
             : high == rgb[1] ? 2 + (rgb[2] - rgb[0]) / range
                              : 4 + (rgb[0] - rgb[1]) / range;
  h /= 6;
  *hue = h - std::floor(h);
}
// Linear light carried to display-like code values with a 2.4 power, the
// space the display-referred engine measures hue and saturation in. Without
// it a skin tone reads as far more saturated in linear light than it looks,
// and the two engines would disagree about which warper ring touches it.
void encoded(const double *rgb, double *out) {
  for (int c = 0; c < 3; ++c)
    out[c] = signedPower(rgb[c], 1 / 2.4);
}
// How much a hue-indexed curve applies. Neutral pixels have no hue, and a
// grain-level wobble around neutral has a random one, so the curves fade in
// over absolute chroma of the encoded values: nothing below about 8 codes of
// 255 (camera noise on grey), fully from about 25, the same thresholds as
// the display-referred engine. Chroma on linear values would call every
// dark noisy pixel neutral-free.
double hueWeight(const double *encodedRgb) {
  const double *e = encodedRgb;
  const double chroma = std::max({e[0], e[1], e[2]}) - std::min({e[0], e[1], e[2]});
  const double t = std::clamp((chroma - .03) / .07, 0., 1.);
  return t * t * (3 - 2 * t);
}
// Rotate chroma around neutral in a luminance/chroma basis. Unlike QColor
// HSL, this has no [0,1] RGB storage limit and retains negative components.
void chroma(double *rgb, double angle, double scale, double newLuma) {
  const double y = luminance(rgb), u = rgb[2] - y, v = rgb[0] - y;
  const double c = std::cos(angle), s = std::sin(angle);
  rgb[2] = newLuma + scale * (u * c - v * s);
  rgb[0] = newLuma + scale * (u * s + v * c);
  rgb[1] = (newLuma - .2126 * rgb[0] - .0722 * rgb[2]) / .7152;
}
void detail(QImage &image, double amount) {
  if (std::abs(amount) < 1e-6)
    return;
  const int w = image.width(), h = image.height();
  const int radius =
      std::max(1, int(std::round(8 * std::abs(amount) * w / 1920.)));
  QImage horizontal(image.size(), QImage::Format_RGBA32FPx4);
  // Separable box blur on RGB only, with constant-edge extension.
  for (int y = 0; y < h; ++y) {
    const float *row = reinterpret_cast<const float *>(image.constScanLine(y));
    float *out = reinterpret_cast<float *>(horizontal.scanLine(y));
    double sum[3] = {};
    for (int k = -radius; k <= radius; ++k)
      for (int c = 0; c < 3; ++c)
        sum[c] += row[std::clamp(k, 0, w - 1) * 4 + c];
    for (int x = 0; x < w; ++x) {
      for (int c = 0; c < 3; ++c) {
        out[x * 4 + c] = sum[c] / (2 * radius + 1);
        sum[c] += row[std::min(w - 1, x + radius + 1) * 4 + c] -
                  row[std::max(0, x - radius) * 4 + c];
      }
    }
  }
  for (int x = 0; x < w; ++x) {
    double sum[3] = {};
    for (int k = -radius; k <= radius; ++k) {
      const auto *row = reinterpret_cast<const float *>(
          horizontal.constScanLine(std::clamp(k, 0, h - 1)));
      for (int c = 0; c < 3; ++c)
        sum[c] += row[x * 4 + c];
    }
    for (int y = 0; y < h; ++y) {
      auto *row = reinterpret_cast<float *>(image.scanLine(y));
      const auto *add = reinterpret_cast<const float *>(
          horizontal.constScanLine(std::min(h - 1, y + radius + 1)));
      const auto *remove = reinterpret_cast<const float *>(
          horizontal.constScanLine(std::max(0, y - radius)));
      for (int c = 0; c < 3; ++c) {
        const double blurred = sum[c] / (2 * radius + 1);
        row[x * 4 + c] += amount * (row[x * 4 + c] - blurred);
        sum[c] += add[x * 4 + c] - remove[x * 4 + c];
      }
    }
  }
}
} // namespace

static QImage correctImpl(const QImage &image, const QVariantMap &node,
                          QString *error, bool allowAffine, bool *affineEligible = nullptr) {
  QImage out = image.convertToFormat(QImage::Format_RGBA32FPx4);
  const auto p = node.value("primaries").toMap(),
             curves = node.value("curves").toMap(),
             hdr = node.value("hdr").toMap();
  const auto master = points(curves.value("master"));
  const Points channels[] = {points(curves.value("red")),
                             points(curves.value("green")),
                             points(curves.value("blue"))};
  const auto hueHue = points(curves.value("hueHue")),
             hueSat = points(curves.value("hueSat")),
             hueLum = points(curves.value("hueLum")),
             lumaCurve = points(curves.value("lumaLuma")),
             satCurve = points(curves.value("satSat"));
  const double exposure = std::exp2(number(p, "exposure")),
               contrast = number(p, "contrast", 1),
               pivot = number(p, "pivot", .435);
  const double black = number(p, "blacks") * .25,
               white = number(p, "whites") * .25;
  const double baseAngle = number(p, "hue") * M_PI / 180,
               baseSaturation = number(p, "saturation", 1);
  struct Warp {
    double hue, sat, dh, ds;
  };
  std::vector<Warp> warps;
  for (const auto &v : node.value("warper").toList()) {
    const auto m = v.toMap();
    if (number(m, "dh") != 0 || number(m, "ds") != 0)
      warps.push_back({number(m, "hue"), number(m, "sat"), number(m, "dh"),
                       number(m, "ds")});
  }
  double lift[3], gamma[3], gain[3], offset[3];
  const double temperature = std::clamp(number(p, "temp", 6500), 1000., 40000.);
  const double tint = number(p, "tint") / 100;
  for (int c = 0; c < 3; ++c) {
    lift[c] = wheel(p, "lift", 0) + wheel(p, "lift", c + 1) +
              number(p, "shadows") * .3;
    gamma[c] =
        1 /
        std::clamp(1 + wheel(p, "gamma", 0) + wheel(p, "gamma", c + 1), .1, 5.);
    gain[c] = wheel(p, "gain", 0, 1) * (1 + wheel(p, "gain", c + 1)) *
              (1 + number(p, "highlights") * .5);
    offset[c] = wheel(p, "offset", 0) + wheel(p, "offset", c + 1);
    // A smooth white-balance control referenced to neutral 6500 K.
    gain[c] *= std::pow(temperature / 6500., c == 0 ? -.5 : c == 2 ? .5 : 0.);
    gain[c] *= std::exp2(c == 1 ? -tint * .5 : tint * .25);
  }
  const char *zones[] = {"black", "dark",      "shadow",
                         "light", "highlight", "specular"};
  const double centres[] = {.04, .14, .3, .525, .75, .925};
  double pushes[6][4] = {};
  bool hdrActive = false;
  for (int z = 0; z < 6; ++z)
    for (int c = 0; c < 4; ++c) {
      pushes[z][c] = hdr.value(zones[z]).toList().value(c).toDouble();
      hdrActive |= pushes[z][c] != 0;
    }
  const bool chromaActive = baseAngle != 0 || baseSaturation != 1 ||
                            !warps.empty() || changed(hueHue, true) ||
                            changed(hueSat, true) || changed(hueLum, true) ||
                            changed(lumaCurve) || changed(satCurve);
  bool active = chromaActive || hdrActive || exposure != 1 || contrast != 1 ||
                black != 0 || white != 0 || changed(master);
  for (int c = 0; c < 3; ++c)
    active |= lift[c] != 0 || gamma[c] != 1 || gain[c] != 1 || offset[c] != 0 ||
              changed(channels[c]);
  // The general chroma path computes hue/hypot per pixel even when only
  // global hue/saturation are changed. Those controls are a fixed RGB matrix.
  // Identity curves must describe an actual identity function: a single or
  // duplicate-x point can be constant in sample(), despite lying on y=x.
  const auto identity=[](const Points &curve) {
    if (curve.empty()) return true;
    if (curve.size()<2) return false;
    for (size_t i=0;i<curve.size();++i)
      if (curve[i][0]!=curve[i][1] || (i && curve[i][0]<=curve[i-1][0])) return false;
    return true;
  };
  const auto neutral=[](const Points &curve) {
    for (const auto &point:curve) if (point[1]!=.5) return false;
    return true;
  };
  bool affine=!hdrActive && warps.empty() && identity(master)
      && neutral(hueHue) && neutral(hueSat) && neutral(hueLum)
      && identity(lumaCurve) && identity(satCurve);
  for (int c=0;c<3;++c) affine &= gamma[c]==1 && identity(channels[c]);
  SuiteProcessing::RgbAffine matrix;
  if (affine) {
    double scale[3],shift[3];
    const double denominator=std::max(.01,1-std::max(0.,white)-std::max(0.,black));
    const double range=1+std::min(0.,white)-std::max(0.,-black);
    for (int c=0;c<3;++c) {
      scale[c]=(1-lift[c])*gain[c]*contrast*exposure/denominator*range;
      double value=lift[c]*gain[c];
      value=(value-pivot)*contrast+pivot+offset[c];
      value*=exposure;
      value=(value-std::max(0.,black))/denominator;
      shift[c]=value*range+std::max(0.,-black);
    }
    double colour[3][3]={{1,0,0},{0,1,0},{0,0,1}};
    if (chromaActive) {
      const double weights[3]={.2126,.7152,.0722};
      const double cs=std::cos(baseAngle)*baseSaturation,sn=std::sin(baseAngle)*baseSaturation;
      for (int c=0;c<3;++c) {
        colour[0][c]=weights[c]*(1-sn-cs)+(c==0 ? cs : c==2 ? sn : 0);
        colour[2][c]=weights[c]*(1-cs+sn)+(c==2 ? cs : c==0 ? -sn : 0);
        colour[1][c]=(weights[c]-.2126*colour[0][c]-.0722*colour[2][c])/.7152;
      }
    }
    for (int r=0;r<3;++r) {
      matrix.values[r*4+3]=0;
      for (int c=0;c<3;++c) {
        matrix.values[r*4+c]=colour[r][c]*scale[c];
        matrix.values[r*4+3]+=colour[r][c]*shift[c];
      }
    }
    affine=matrix.valid();
  }
  if (affineEligible) { *affineEligible=affine; return {}; }
  if (active && affine && allowAffine) {
    for (int y=0;y<out.height();++y)
      SuiteProcessing::applyAffineCpu(reinterpret_cast<float*>(out.scanLine(y)),out.width(),matrix);
  }
  for (int y = 0; active && !(affine && allowAffine) && y < out.height(); ++y) {

    auto *row = reinterpret_cast<float *>(out.scanLine(y));
    for (int x = 0; x < out.width(); ++x) {
      double rgb[3];
      for (int c = 0; c < 3; ++c) {
        double v = row[x * 4 + c];
        v += lift[c] * (1 - v);
        if (gamma[c] != 1)
          v = signedPower(v, gamma[c]);
        v *= gain[c];
        v = (v - pivot) * contrast + pivot + offset[c];
        v *= exposure;
        v = (v - std::max(0., black)) /
            std::max(.01, 1 - std::max(0., white) - std::max(0., black));
        v = v * (1 + std::min(0., white) - std::max(0., -black)) +
            std::max(0., -black);
        rgb[c] = sample(master, sample(channels[c], v, v), v);
      }
      const double lum = luminance(rgb);
      if (chromaActive) {
        double hue, saturation, display[3];
        encoded(rgb, display);
        hueSaturation(display, &hue, &saturation);
        const double amount = hueWeight(display);
        double angle =
            baseAngle + amount * (sample(hueHue, hue, .5) - .5) * 2 * M_PI;
        double sat = baseSaturation *
                     std::max(0., 1 + amount * (sample(hueSat, hue, .5) - .5) * 2);
        if (saturation > 1e-8)
          sat *= std::max(0., sample(satCurve, saturation, saturation)) /
                 saturation;
        // The mesh's tent weights sum to one between neighbouring points and
        // rings, so a lone moved point fades to nothing at its neighbours and
        // towards neutral, and grain on a grey stays put, exactly as on the
        // display-referred engine.
        double weights = 0, meshHue = 0, meshSat = 0;
        for (const auto &point : warps) {
          double dh = std::abs(hue - point.hue);
          dh = std::min(dh, 1 - dh);
          const double weight =
              std::max(0., 1 - dh * 12) *
              std::max(0., 1 - std::abs(saturation - point.sat) * 3);
          weights += weight;
          meshHue += weight * point.dh;
          meshSat += weight * point.ds;
        }
        const double scale = amount / std::max(1., weights);
        meshHue *= scale;
        meshSat *= scale;
        angle += meshHue * 2 * M_PI;
        sat *= std::max(0., 1 + meshSat);
        chroma(rgb, angle, sat,
               sample(lumaCurve, lum, lum) +
                   amount * (sample(hueLum, hue, .5) - .5));
      }
      double weights = 0, push[4] = {};
      for (int z = 0; hdrActive && z < 6; ++z) {
        const double width = z == 0   ? .1
                             : z == 5 ? .15
                                      : (centres[z + 1] - centres[z - 1]) * .7;
        const double weight =
            z == 5 && lum >= centres[z]
                ? 1
                : std::max(0., 1 - std::abs(lum - centres[z]) / width);
        weights += weight;
        for (int c = 0; c < 4; ++c)
          push[c] += weight * pushes[z][c];
      }
      for (int c = 0; c < 3; ++c) {
        if (weights > 0)
          rgb[c] *= std::exp2((push[0] + push[c + 1]) / weights);
        row[x * 4 + c] = rgb[c];
      }
    }
  }
  detail(out, std::clamp(number(p, "midDetail"), -1., 1.));
  const auto lut = node.value("lut").toMap();
  const double amount = std::clamp(number(lut, "intensity", 1), 0., 1.);
  if (!lut.value("path").toString().isEmpty() && amount > 0) {
    // A node LUT works on Rec.709-encoded values, the domain video LUTs are
    // made for: the light encoded, looked up, and decoded again.
    QImage encoded = ColourPipeline::convert(out, "oma_interchange", "rec709/rec709", error);
    if (encoded.isNull())
      return {};
    const QImage looked = ColourPipeline::applyLut(encoded, lut.value("path").toString(), error);
    if (looked.isNull())
      return {};
    // Its strength mixes there too, between the encoded values and the look.
    for (int y = 0; y < encoded.height(); ++y) {
      auto *a = reinterpret_cast<float *>(encoded.scanLine(y));
      const auto *b = reinterpret_cast<const float *>(looked.constScanLine(y));
      for (int x = 0; x < encoded.width(); ++x)
        for (int c = 0; c < 3; ++c)
          a[x * 4 + c] += (b[x * 4 + c] - a[x * 4 + c]) * amount;
    }
    out = ColourPipeline::convert(encoded, "rec709/rec709", "oma_interchange", error);
    if (out.isNull())
      return {};
  }
  return out;
}

QImage LinearGrade::correct(const QImage &image,const QVariantMap &node,QString *error) {
  return correctImpl(image,node,error,true);
}
QImage LinearGrade::correctReference(const QImage &image,const QVariantMap &node,QString *error) {
  return correctImpl(image,node,error,false);
}
bool LinearGrade::usesAffine(const QVariantMap &node) {
  bool eligible=false;correctImpl({},node,nullptr,true,&eligible);return eligible;
}
