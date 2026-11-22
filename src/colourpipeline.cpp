#include "colourpipeline.h"
#include "bands.h"
#include "imagebands.h"
#include "engine/frame.h"
#include <OpenColorIO/OpenColorIO.h>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <cstring>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <limits>

namespace OCIO = OCIO_NAMESPACE;
namespace {
constexpr auto configId = "oma-video-v2";
// Video 2 holds every transform of Video 1 unchanged: a grade pinned to it means the same here.
constexpr auto earlierConfigId = "oma-video-v1";
struct State {
  OCIO::ConstConfigRcPtr config;
  QString hash, earlierHash, error;
  QMutex mutex;
  QHash<QString, OCIO::ConstCPUProcessorRcPtr> processors;
  QHash<QString, std::shared_ptr<const engine::ColourProgram>> programs;
  quint64 lutGeneration = 0;  // a LUT rescan: programs made before it are not reused
  QHash<QString, QString> lutKeys;
  QHash<QString, OCIO::ConstCPUProcessorRcPtr> luts;
  State() {
    try {
      QFile file(":/colour/oma-video-v2.ocio"), earlier(":/colour/oma-video-v1.ocio");
      if (!file.open(QIODevice::ReadOnly) || !earlier.open(QIODevice::ReadOnly))
        throw std::runtime_error("Bundled OCIO configuration is missing.");
      earlierHash = QString::fromLatin1(
          QCryptographicHash::hash(earlier.readAll(), QCryptographicHash::Sha256).toHex());
      const QByteArray bytes = file.readAll();
      hash = QString::fromLatin1(
          QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
      std::istringstream stream(bytes.toStdString());
      config = OCIO::Config::CreateFromStream(stream);
      config->validate();
    } catch (const std::exception &e) {
      error = QString::fromUtf8(e.what());
    }
  }
};
State &state() {
  static State s;
  return s;
}
// A delivery space may name its view: "rec709/rec709@ACES 2.0" is that display
// through that tone map (see LinearRenderer::outputSpace).
void splitView(QString &to, QString &view) {
  const int at = to.indexOf('@');
  if (at > 0 && view.isEmpty()) { view = to.mid(at + 1); to.truncate(at); }
}
OCIO::BitDepth inputDepth(const QImage &image) {
  if (image.format() == QImage::Format_RGBA64)
    return OCIO::BIT_DEPTH_UINT16;
  if (image.format() == QImage::Format_RGBA8888)
    return OCIO::BIT_DEPTH_UINT8;
  return OCIO::BIT_DEPTH_F32;
}
OCIO::ConstCPUProcessorRcPtr
processor(const QString &from, const QString &space, const QString &asked = {},
          OCIO::BitDepth depth = OCIO::BIT_DEPTH_F32) {
  QString to = space, view = asked;
  splitView(to, view);
  auto &s = state();
  if (!s.error.isEmpty())
    throw std::runtime_error(s.error.toStdString());
  const QString key =
      from + '\n' + to + '\n' + view + '\n' + QString::number(depth);
  QMutexLocker lock(&s.mutex);
  if (s.processors.contains(key))
    return s.processors.value(key);
  OCIO::ConstProcessorRcPtr p;
  if (view.isEmpty())
    p = s.config->getProcessor(from.toUtf8().constData(),
                               to.toUtf8().constData());
  else {
    auto transform = OCIO::DisplayViewTransform::Create();
    transform->setSrc(from.toUtf8().constData());
    transform->setDisplay(to.toUtf8().constData());
    transform->setView(view.toUtf8().constData());
    p = s.config->getProcessor(transform);
  }
  // Preserve reference transfer accuracy for grading and round trips.
  const auto flags = static_cast<OCIO::OptimizationFlags>(
      OCIO::OPTIMIZATION_DEFAULT & ~OCIO::OPTIMIZATION_FAST_LOG_EXP_POW);
  auto cpu = p->getOptimizedCPUProcessor(depth, OCIO::BIT_DEPTH_F32, flags);
  s.processors.insert(key, cpu);
  return cpu;
}
// Rows are transformed in cache-sized bands across the pool. Every band is
// converted, transformed and packed exactly as the whole image would be, so
// the pixels do not depend on the band size or the thread count.
QImage applyPixels(const QImage &image,
                   const OCIO::ConstCPUProcessorRcPtr &cpu,
                   QImage::Format target = QImage::Format_RGBA32FPx4) {
  if (image.isNull())
    return {};
  // Band views carry no colour table and cannot address sub-byte pixels:
  // palette and one-bit pictures are expanded whole first, as before.
  if (image.colorCount() > 0 || image.depth() < 8)
    return applyPixels(image.convertToFormat(QImage::Format_RGBA32FPx4), cpu, target);
  const auto depth = inputDepth(image);
  const int width = image.width(), height = image.height();
  QImage out(width, height, target);
  if (out.isNull())
    throw std::runtime_error("Could not allocate the display image.");
  const int rows = Bands::rowsPerBand(width, 16);
  const int bands = (height + rows - 1) / rows;
  const qsizetype bytes = qsizetype(width) * out.depth() / 8;
  QMutex problems;
  QString problem;
  const auto band = [&](int index) {
    try {
      const int y = index * rows, count = qMin(rows, height - y);
      const QImage source(image.constScanLine(y), width, count,
                          image.bytesPerLine(), image.format());
      QImage pixels = source.convertToFormat(QImage::Format_RGBA32FPx4);
      if (pixels.isNull())
        throw std::runtime_error("Could not convert the band to float.");
      // A three-channel descriptor skips alpha, including on padded scanlines.
      OCIO::PackedImageDesc desc(pixels.bits(), width, count, 3,
                                 OCIO::BIT_DEPTH_F32, sizeof(float),
                                 4 * sizeof(float), pixels.bytesPerLine());
      if (depth == OCIO::BIT_DEPTH_F32)
        cpu->apply(desc);
      else {
        // Native integer input enables OCIO's exact 8/16-bit lookup tables
        // for component curves; do not first promote it and lose that path.
        const int component = depth == OCIO::BIT_DEPTH_UINT16 ? 2 : 1;
        OCIO::PackedImageDesc integer(
            const_cast<uchar *>(source.constBits()), width, count, 3, depth,
            component, component * 4, source.bytesPerLine());
        cpu->apply(integer, desc);
      }
      if (target != QImage::Format_RGBA32FPx4)
        pixels = pixels.convertToFormat(target);
      if (pixels.isNull())
        throw std::runtime_error("Could not pack the display band.");
      for (int r = 0; r < count; ++r)
        std::memcpy(out.scanLine(y + r), pixels.constScanLine(r), bytes);
      return true;
    } catch (const std::exception &e) {
      QMutexLocker lock(&problems);
      if (problem.isEmpty())
        problem = QString::fromUtf8(e.what());
      return false;
    }
  };
  bool ok = true;
  if (bands > 1 && Bands::displayThreads())
    ok = Bands::run(bands, band);
  else
    for (int i = 0; ok && i < bands; ++i)
      ok = band(i);
  if (!ok)
    throw std::runtime_error(problem.isEmpty() ? "Display conversion failed."
                                               : problem.toStdString());
  Bands::copyMetadata(out, image);
  out.setColorSpace(QColorSpace());
  return out;
}
} // namespace

QVariantMap ColourPipeline::info() {
  auto &s = state();
  return {{"engine", "ocio"},
          {"config", configId},
          {"configHash", s.hash},
          {"name", "Oma Colour 1 · Video 2"},
          {"version", OCIO::GetVersion()},
          {"interchange", "Linear Rec.709"},
          {"error", s.error}};
}
QVariantMap ColourPipeline::gpuShader(const QString &from,const QString &to,const QString &view) {
  QVariantMap result{{"available",false},{"from",from},{"to",to},{"view",view},{"config",configId},{"configHash",state().hash}};
  try {
    auto &s=state();QMutexLocker locker(&s.mutex);
    if(!s.error.isEmpty())throw std::runtime_error(s.error.toStdString());
    OCIO::ConstProcessorRcPtr transform;
    if(view.isEmpty())transform=s.config->getProcessor(from.toUtf8().constData(),to.toUtf8().constData());
    else {
      auto display=OCIO::DisplayViewTransform::Create();display->setSrc(from.toUtf8().constData());display->setDisplay(to.toUtf8().constData());display->setView(view.toUtf8().constData());transform=s.config->getProcessor(display);
    }
    const auto flags=static_cast<OCIO::OptimizationFlags>(OCIO::OPTIMIZATION_DEFAULT & ~OCIO::OPTIMIZATION_FAST_LOG_EXP_POW);
    auto gpu=transform->getOptimizedGPUProcessor(flags);auto description=OCIO::GpuShaderDesc::CreateShaderDesc();
    description->setLanguage(OCIO::GPU_LANGUAGE_GLSL_4_0);description->setFunctionName("suiteOcio");description->setResourcePrefix("suite_ocio_");gpu->extractGpuShaderInfo(description);
    result["textures"]=description->getNumTextures();result["textures3d"]=description->getNum3DTextures();result["uniforms"]=description->getNumUniforms();
    if(description->getNumTextures() || description->getNum3DTextures() || description->getNumUniforms()) {
      result["reason"]="This OCIO transform requires GPU textures or uniforms; its CPU implementation remains available.";return result;
    }
    result["source"]=QString::fromUtf8(description->getShaderText());result["processor"]=QString::fromUtf8(gpu->getCacheID());result["available"]=true;
  } catch(const std::exception &e) {result["reason"]=QString::fromUtf8(e.what());}
  return result;
}
namespace {
// An OCIO processor as an engine colour program: OCIO's Vulkan shader with
// its tables and settings for the GPU, its CPU processor for the CPU twin.
// Kept under `key`; `make` builds the processor (with the state locked).
std::shared_ptr<const engine::ColourProgram> programFor(const QString &key,
    const std::function<OCIO::ConstProcessorRcPtr(State &)> &make, QString *error) {
  if(error)error->clear();
  try {
    auto &s=state();
    OCIO::ConstProcessorRcPtr transform;
    {
      QMutexLocker locker(&s.mutex);
      if(!s.error.isEmpty())throw std::runtime_error(s.error.toStdString());
      if(const auto it=s.programs.constFind(key);it!=s.programs.constEnd())return *it;
      transform=make(s);
    }
    // The same accuracy on both: no fast log/exp/pow.
    const auto flags=static_cast<OCIO::OptimizationFlags>(OCIO::OPTIMIZATION_DEFAULT & ~OCIO::OPTIMIZATION_FAST_LOG_EXP_POW);
    const auto cpu=transform->getOptimizedCPUProcessor(OCIO::BIT_DEPTH_F32,OCIO::BIT_DEPTH_F32,flags);
    auto gpu=transform->getOptimizedGPUProcessor(flags);
    auto description=OCIO::GpuShaderDesc::CreateShaderDesc();
    description->setLanguage(OCIO::GPU_LANGUAGE_GLSL_VK_4_6);
    description->setFunctionName("ocioTransform");
    description->setResourcePrefix("ocio_");
    description->setDescriptorSetIndex(1,1);  // set 0 holds the engine's pictures
    gpu->extractGpuShaderInfo(description);
    auto program=std::make_shared<engine::ColourProgram>();
    program->id=std::string(configId)+':'+std::to_string(s.lutGeneration)+':'+gpu->getCacheID();
    program->shader=description->getShaderText();
    program->function="ocioTransform";
    for(unsigned i=0;i<description->getNumTextures();++i) {
      const char *name=nullptr,*sampler=nullptr;unsigned width=0,height=0;
      OCIO::GpuShaderDesc::TextureType channel;OCIO::GpuShaderDesc::TextureDimensions dimensions;OCIO::Interpolation interpolation;
      description->getTexture(i,name,sampler,width,height,channel,dimensions,interpolation);
      const float *values=nullptr;description->getTextureValues(i,values);
      engine::ColourTexture texture;
      texture.binding=description->getTextureShaderBindingIndex(i);
      texture.dims=dimensions==OCIO::GpuShaderDesc::TEXTURE_2D?2:1;
      texture.width=int(width);texture.height=int(std::max(1u,height));
      texture.channels=channel==OCIO::GpuShaderDesc::TEXTURE_RED_CHANNEL?1:3;
      texture.linear=interpolation!=OCIO::INTERP_NEAREST;
      texture.values.assign(values,values+size_t(texture.width)*texture.height*texture.channels);
      program->textures.push_back(std::move(texture));
    }
    for(unsigned i=0;i<description->getNum3DTextures();++i) {
      const char *name=nullptr,*sampler=nullptr;unsigned edge=0;OCIO::Interpolation interpolation;
      description->get3DTexture(i,name,sampler,edge,interpolation);
      const float *values=nullptr;description->get3DTextureValues(i,values);
      engine::ColourTexture texture;
      texture.binding=description->get3DTextureShaderBindingIndex(i);
      texture.dims=3;texture.width=texture.height=texture.depth=int(edge);texture.channels=3;
      texture.linear=interpolation!=OCIO::INTERP_NEAREST;
      texture.values.assign(values,values+size_t(edge)*edge*edge*3);
      program->textures.push_back(std::move(texture));
    }
    // Settings (dynamic properties): written where the uniform block has them.
    if(description->getNumUniforms()) {
      program->uniforms.assign(description->getUniformBufferSize(),0);
      for(unsigned i=0;i<description->getNumUniforms();++i) {
        OCIO::GpuShaderDesc::UniformData data;description->getUniform(i,data);
        auto *at=program->uniforms.data()+data.m_bufferOffset;
        if(data.m_type==OCIO::UNIFORM_DOUBLE){const float v=float(data.m_getDouble());std::memcpy(at,&v,4);}
        else if(data.m_type==OCIO::UNIFORM_BOOL){const int32_t v=data.m_getBool()?1:0;std::memcpy(at,&v,4);}
        else if(data.m_type==OCIO::UNIFORM_FLOAT3){const auto v=data.m_getFloat3();std::memcpy(at,v.data(),12);}
        else throw std::runtime_error("This colour transform has settings the engine cannot pass to the GPU yet.");
      }
    }
    program->cpu=[cpu](float *rgba,size_t pixels) {
      OCIO::PackedImageDesc image(rgba,long(pixels),1,4,OCIO::BIT_DEPTH_F32,sizeof(float),4*sizeof(float),long(pixels)*16);
      cpu->apply(image);
    };
    QMutexLocker locker(&s.mutex);
    if(s.programs.size()>=256)s.programs.clear();
    s.programs.insert(key,program);
    return program;
  } catch(const std::exception &e) {
    if(error)*error=QString::fromUtf8(e.what());
    return nullptr;
  }
}
}  // namespace

std::shared_ptr<const engine::ColourProgram> ColourPipeline::engineProgram(const QString &from,const QString &space,
                                                                           const QString &asked,QString *error) {
  QString to=space,view=asked;
  splitView(to,view);
  return programFor(from+'\n'+to+'\n'+view,[&](State &s) {
    if(view.isEmpty())return s.config->getProcessor(from.toUtf8().constData(),to.toUtf8().constData());
    auto display=OCIO::DisplayViewTransform::Create();display->setSrc(from.toUtf8().constData());
    display->setDisplay(to.toUtf8().constData());display->setView(view.toUtf8().constData());
    return s.config->getProcessor(display);
  },error);
}

std::shared_ptr<const engine::ColourProgram> ColourPipeline::engineCacheProgram(bool encode,QString *error) {
  return programFor(encode?QStringLiteral("cache\nencode"):QStringLiteral("cache\ndecode"),[&](State &s) {
    const double breaks[3]={0.0078125,0.0078125,0.0078125};
    auto log=OCIO::LogCameraTransform::Create(breaks);
    const double slope[3]={1.0/17.52,1.0/17.52,1.0/17.52},offset[3]={9.72/17.52,9.72/17.52,9.72/17.52};
    log->setBase(2.0);
    log->setLogSideSlopeValue(slope);
    log->setLogSideOffsetValue(offset);
    log->setDirection(encode?OCIO::TRANSFORM_DIR_FORWARD:OCIO::TRANSFORM_DIR_INVERSE);
    return s.config->getProcessor(log);
  },error);
}

std::shared_ptr<const engine::ColourProgram> ColourPipeline::engineInputProgram(const QString &lut,const QString &from,
                                                                                const QString &to,QString *error) {
  QString key=QStringLiteral("input\n")+from+'\n'+to;
  QString absolute;
  if(!lut.isEmpty()) {
    const QFileInfo file(lut);
    if(!file.isFile() || !file.isReadable()){if(error)*error=QStringLiteral("The source LUT is unavailable.");return nullptr;}
    absolute=file.absoluteFilePath();
    key+='\n'+absolute+':'+QString::number(file.size())+':'+QString::number(file.lastModified().toMSecsSinceEpoch());
  }
  return programFor(key,[&](State &s) {
    auto group=OCIO::GroupTransform::Create();
    if(!absolute.isEmpty()) {
      // As applyLut: the file as it is now, tetrahedral, on the encoded values.
      OCIO::ClearAllCaches();
      std::const_pointer_cast<OCIO::Config>(s.config)->clearProcessorCache();
      auto file=OCIO::FileTransform::Create();
      file->setSrc(absolute.toUtf8().constData());
      file->setInterpolation(OCIO::INTERP_TETRAHEDRAL);
      group->appendTransform(file);
    }
    auto space=OCIO::ColorSpaceTransform::Create();
    space->setSrc(from.toUtf8().constData());
    space->setDst(to.toUtf8().constData());
    group->appendTransform(space);
    return s.config->getProcessor(group);
  },error);
}

bool ColourPipeline::convertInPlace(float *rgba,qsizetype pixels,const QString &from,const QString &to,const QString &view,QString *error) {
  if(error)error->clear();
  try {
    if(!rgba || pixels<=0 || pixels>std::numeric_limits<long>::max()/16)throw std::runtime_error("Invalid float colour span.");
    auto cpu=processor(from,to,view,OCIO::BIT_DEPTH_F32);
    OCIO::PackedImageDesc image(rgba,long(pixels),1,3,OCIO::BIT_DEPTH_F32,sizeof(float),4*sizeof(float),pixels*16);cpu->apply(image);return true;
  } catch(const std::exception &e) {if(error)*error=QString::fromUtf8(e.what());return false;}
}
quint64 ColourPipeline::lutGeneration() {
  auto &s=state();
  QMutexLocker lock(&s.mutex);
  return s.lutGeneration;
}
void ColourPipeline::forgetLuts() {
  auto &s=state();
  QMutexLocker lock(&s.mutex);
  ++s.lutGeneration;
  s.luts.clear();
  s.lutKeys.clear();
  s.programs.clear();
  // The config keeps processors by file name, whatever the file now holds.
  if (s.config) std::const_pointer_cast<OCIO::Config>(s.config)->clearProcessorCache();
  OCIO::ClearAllCaches();
}
QVariantMap ColourPipeline::effective(const QVariantMap &grade, bool title) {
  QVariantMap own = grade.value("management").toMap();
  if (own.value("enabled").toBool() && own.value("engine") == "ocio") {
    const auto &s = state();
    if (own.value("config") == earlierConfigId && own.value("configHash") == s.earlierHash) {
      own["config"] = configId;
      own["configHash"] = s.hash;
    }
    return own;
  }
  QVariantMap m = defaults();
  m["enabled"] = true;
  if (title) { m["input"] = "srgb"; m["inputGamma"] = "srgb"; }
  return m;
}
QVariantMap ColourPipeline::defaults() {
  auto m = info();
  for (const char *key : {"name", "version", "interchange", "error"})
    m.remove(key);
  m["enabled"] = false;
  for (const char *key : {"input", "working", "output"}) {
    m[key] = "rec709";
    m[QString::fromLatin1(key) + "Gamma"] = "rec709";
  }
  m["workingGamma"] = "linear";
  return m;
}
QString ColourPipeline::spaceName(const QString &gamut, const QString &gamma) {
  const QString g = gamut == "auto" ? "rec709" : gamut;
  return g + "/" +
         (gamma.isEmpty() ? (g == "srgb" || g == "p3" ? "srgb" : "rec709")
                          : gamma);
}
QString ColourPipeline::validate(const QVariantMap &m) {
  if (!m.value("enabled").toBool())
    return {};
  if (m.value("engine") != "ocio")
    return QStringLiteral("Unsupported colour-management engine.");
  auto &s = state();
  if (!s.error.isEmpty())
    return s.error;
  const bool earlier = m.value("config") == earlierConfigId && m.value("configHash").toString() == s.earlierHash;
  if (!earlier && (m.value("config") != configId ||
      m.value("configHash").toString() != s.hash))
    return QStringLiteral("This grade needs a different OCIO configuration. "
                          "Open Colour Management and explicitly select the "
                          "bundled configuration to migrate it.");
  if (m.value("workingGamma") != "linear")
    return QStringLiteral(
        "OpenColorIO grades require a scene-linear working space.");
  if (m.value("working") != "rec709" && m.value("working") != "srgb")
    return QStringLiteral(
        "OpenColorIO grades use the suite's linear Rec.709 working space.");
  try {
    for (const char *key : {"input", "working", "output"}) {
      const QString name = spaceName(
          m.value(key, "rec709").toString(),
          m.value(QString::fromLatin1(key) + "Gamma", "rec709").toString());
      processor(name, "oma_interchange");
    }
    if (const QString grading = m.value("gradeSpace").toString(); !grading.isEmpty())
      processor(grading, "oma_interchange");
  } catch (const std::exception &e) {
    return QString::fromUtf8(e.what());
  }
  return {};
}
namespace {
thread_local const ColourPipeline::ScopedConverter::Function *workerConverter = nullptr;
thread_local bool convertingOnWorker = false;
struct ConversionGuard {
  ConversionGuard() { convertingOnWorker = true; }
  ~ConversionGuard() { convertingOnWorker = false; }
};
}
ColourPipeline::ScopedConverter::ScopedConverter(Function converter)
    : m_converter(std::move(converter)), m_previous(workerConverter) { workerConverter = &m_converter; }
ColourPipeline::ScopedConverter::~ScopedConverter() { workerConverter = m_previous; }
QImage ColourPipeline::convert(const QImage &image, const QString &from,
                               const QString &to, QString *error) {
  if (error)
    error->clear();
  try {
    if (workerConverter && *workerConverter && !convertingOnWorker) {
      ConversionGuard guard;
      return (*workerConverter)(image, from, to, error);
    }
    return applyPixels(image, processor(from, to, {}, inputDepth(image)));
  } catch (const std::exception &e) {
    if (error)
      *error = QString::fromUtf8(e.what());
    return {};
  }
}
QImage ColourPipeline::stage(const QImage &image, const QVariantMap &m,
                             const QString &from, const QString &to,
                             QString *error) {
  const QString problem = validate(m);
  if (!problem.isEmpty()) {
    if (error)
      *error = problem;
    return {};
  }
  return convert(image,
                 spaceName(m.value(from, "rec709").toString(),
                           m.value(from + "Gamma", "rec709").toString()),
                 spaceName(m.value(to, "rec709").toString(),
                           m.value(to + "Gamma", "rec709").toString()),
                 error);
}
QImage ColourPipeline::display(const QImage &image, const QVariantMap &options,
                               QString *error, QImage::Format target) {
  if (error)
    error->clear();
  try {
    return applyPixels(
        image,
        processor(spaceName(options.value("from", "rec709").toString(),
                            options.value("fromGamma", "rec709").toString()),
                  spaceName(options.value("gamut", "srgb").toString(),
                            options.value("gamma", "srgb").toString()),
                  options.value("view", "Standard").toString(),
                  inputDepth(image)),
        target);
  } catch (const std::exception &e) {
    if (error)
      *error = QString::fromUtf8(e.what());
    return {};
  }
}

QImage ColourPipeline::applyLut(const QImage &image, const QString &path,
                                QString *error) {
  if (error)
    error->clear();
  try {
    const QFileInfo file(path);
    if (!file.isFile() || !file.isReadable())
      throw std::runtime_error("The node LUT is unavailable.");
    const QString absolute = file.absoluteFilePath();
    const QString key =
        QString::number(file.size()) + ':' +
        QString::number(file.lastModified().toMSecsSinceEpoch());
    auto &s = state();
    if (!s.error.isEmpty())
      throw std::runtime_error(s.error.toStdString());
    OCIO::ConstCPUProcessorRcPtr cpu;
    {
      QMutexLocker lock(&s.mutex);
      if (s.lutKeys.value(absolute) != key) {
        // OCIO caches file contents separately from processors, and the
        // config keeps processors by file name.
        OCIO::ClearAllCaches();
        std::const_pointer_cast<OCIO::Config>(s.config)->clearProcessorCache();
        auto transform = OCIO::FileTransform::Create();
        transform->setSrc(absolute.toUtf8().constData());
        transform->setInterpolation(OCIO::INTERP_TETRAHEDRAL);
        cpu = s.config->getProcessor(transform)->getDefaultCPUProcessor();
        if (s.luts.size() >= 64) {
          s.luts.clear();
          s.lutKeys.clear();
        }
        s.luts[absolute] = cpu;
        s.lutKeys[absolute] = key;
      } else
        cpu = s.luts.value(absolute);
    }
    // LUTs process floats in the working domain; their own declared
    // domain/transform may intentionally clamp, but coverage is untouched.
    return applyPixels(image.convertToFormat(QImage::Format_RGBA32FPx4), cpu);
  } catch (const std::exception &e) {
    if (error)
      *error = QString::fromUtf8(e.what());
    return {};
  }
}
