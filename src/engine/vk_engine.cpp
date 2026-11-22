// The Vulkan engine: imports captured frames (DMA-BUF or memory), composites
// them on the GPU with compute shaders, and reads the result back for the
// preview and the encoders. Every public operation that needs the device is
// recorded into one command buffer and flushed before a readback is mapped;
// the frame loop's readbacks are instead submitted without waiting and taken
// a tick later (requestOutput), and anything else waits for them first.
// Usually it works on the Vulkan device FFmpeg makes for its GPU, sharing a
// queue with FFmpeg's encoders under FFmpeg's queue lock, so an encoder there
// takes its pictures without them leaving the GPU (writeVulkanFrames).

#include <vulkan/vulkan.h>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <thread>
#include <unordered_map>
#include <utility>
#include <unistd.h>

#include <QByteArray>
#include <QFile>

#include "engine.h"
#include "glsl.h"
#include "matting.h"
#include "parallel.h"

#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffULL
#endif
#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0ULL
#endif

namespace engine {
QByteArray loadSpirv(const char *file);  // shaders.cpp
namespace {

// One stage's time, told to engine::stageProbe when one is set.
struct Probed {
    const char *stage;
    StageProbe probe = stageProbe.load(std::memory_order_relaxed);
    std::chrono::steady_clock::time_point began;
    explicit Probed(const char *name) : stage(name) { if (probe) began = std::chrono::steady_clock::now(); }
    ~Probed() {
        if (probe) probe(stage, std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - began).count());
    }
};

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return uint32_t(a) | (uint32_t(b) << 8) | (uint32_t(c) << 16) | (uint32_t(d) << 24);
}
constexpr uint32_t kXR24 = fourcc('X', 'R', '2', '4');
constexpr uint32_t kAR24 = fourcc('A', 'R', '2', '4');
constexpr uint32_t kXB24 = fourcc('X', 'B', '2', '4');
constexpr uint32_t kAB24 = fourcc('A', 'B', '2', '4');
constexpr uint32_t kXR30 = fourcc('X', 'R', '3', '0');
constexpr uint32_t kAB30 = fourcc('A', 'B', '3', '0');

struct FormatInfo {
    VkFormat vk = VK_FORMAT_UNDEFINED;
    bool alpha = false;
};

FormatInfo formatForFourcc(uint32_t f) {
    switch (f) {
    case kXR24: return {VK_FORMAT_B8G8R8A8_UNORM, false};
    case kAR24: return {VK_FORMAT_B8G8R8A8_UNORM, true};
    case kXB24: return {VK_FORMAT_R8G8B8A8_UNORM, false};
    case kAB24: return {VK_FORMAT_R8G8B8A8_UNORM, true};
    case kXR30: return {VK_FORMAT_A2R10G10B10_UNORM_PACK32, false};
    case kAB30: return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, false};
    default: return {};
    }
}

FormatInfo formatForPixel(PixelFormat f) {
    switch (f) {
    case PixelFormat::BGRx: return {VK_FORMAT_B8G8R8A8_UNORM, false};
    case PixelFormat::BGRA: return {VK_FORMAT_B8G8R8A8_UNORM, true};
    case PixelFormat::RGBx: return {VK_FORMAT_R8G8B8A8_UNORM, false};
    case PixelFormat::RGBA: return {VK_FORMAT_R8G8B8A8_UNORM, true};
    case PixelFormat::RGBA64: return {VK_FORMAT_R16G16B16A16_UNORM, true};
    case PixelFormat::RGBAF32: return {VK_FORMAT_R32G32B32A32_SFLOAT, true};
    }
    return {};
}

inline uint32_t align(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
// Working pictures: straight linear RGBA in 32-bit floats, as the colour-
// managed pipeline's working frames are. Half floats were not enough: a PQ or
// log curve (or a wide-gamut matrix on HDR light) turns their rounding into
// visible error, and they cannot hold the smallest light a log curve encodes.
constexpr VkFormat kWorkFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
inline uint32_t groups(uint32_t n, uint32_t local) { return (n + local - 1) / local; }


struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    int width = 0;
    int height = 0;
    bool external = false;
    bool initialised = false;  // has had its first layout transition
    std::shared_ptr<const void> owner; // keep cached DMA-BUF identities alive
    bool clearable = false;    // made with TRANSFER_DST, so a composite can clear it
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void *mapped = nullptr;
};

struct Source {
    Image work;                                    // kWorkFormat: straight linear RGBA
    Image previous, fieldFrame;
    DeinterlaceMode deinterlace = DeinterlaceMode::Off;
    bool previousValid = false, resetNext = false, fieldReady = false, pendingProcess = false;
    int field = 0;
    bool firstField = true;
    Image upload;                                  // unorm copy of a memory frame
    std::unordered_map<const void *, Image> imports;  // DMA-BUF imports by buffer token
    bool hasFrame = false;
    // Noise reduction: two cleaned frames used in turn, the other being the
    // history the next frame is averaged with.
    float denoise = 0.f;
    Image clean[2];
    int cleanIndex = 0;
    bool historyValid = false;
    const Image &processed() const { return deinterlace != DeinterlaceMode::Off && fieldReady ? fieldFrame : work; }
    const Image &base() const { return denoise > 0.f && historyValid ? clean[cleanIndex] : processed(); }
    // Video filters, as in cpu_engine.cpp: one picture per step, or a delay's
    // held pictures, oldest first, the shown one at the front; `aux` is the
    // step's LUT or mask on the device, uploaded from `auxOf`.
    struct Step {
        Image out, aux, mid, mid2;  // mid: a two-pass step's first pass; mid2: a third pass's; aux: also RemoveBackground's matte
        Image shaped;               // the step kept within its shape (VideoFilter::within)
        bool within = false;
        matting::Plan plan;   // RemoveBackground's network and its working space,
        Buffer arena;
        Image guide[4], back[2];  // its matte's pictures after the network (see recordGuided)
        std::vector<Image> regs;    // a grade plan's pictures
        std::vector<Step> people;   // its People operations', each a RemoveBackground of its own
        std::vector<std::pair<std::shared_ptr<const std::vector<float>>, Image>> tables;  // its numbers, uploaded
        const void *auxOf = nullptr;
        std::deque<std::pair<Image, double>> held;
        std::vector<Image> spare;
        double origin = 0;
        bool started = false;
        const Image &output() const { return !held.empty() ? held.front().first : within ? shaped : out; }
    };
    std::vector<VideoFilter> filters;
    std::vector<Step> steps;
    bool filtered = false;
    uint64_t generation = 0, filteredGeneration = ~0ull;
    const Image &shown() const { return filtered && !steps.empty() ? steps.back().output() : base(); }
};

struct Pipeline {
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

// Push constants, one struct per shader, laid out as its `Push` block. Each
// pipeline's push range is the struct's size, so the two cannot drift apart.
struct TransitionPush {
    int32_t w, h, type; float progress;
    int32_t side, mask, invert; float softness;
    float colourPoint, pad[3];
    float colour[4];
    float matte[4];
};
struct ConvertPush {
    int32_t w, h, flags;
};
struct CompositePush {
    float x, y, w, h;
    float u0, v0, u1, v1;
    float cosR, sinR;
    int32_t ox, oy;
    int32_t cw, ch;
    float opacity;
    int32_t filter;
    float footprintX, footprintY;
    int32_t blend, blendSpace, pad1, pad2;
};
struct YuvOutPush {
    int32_t w, h, yStride, cStride, uOffset, vOffset;
    int32_t canvasW, canvasH;
    OutputParams q;
};
struct PreviewPush {
    int32_t w, h, encoded = 0;
};
struct YuvInPush {
    int32_t w, h, layout, full;
    float kr, kb;
    int32_t interlaced;
    int32_t alphaRow, premultiplied, linearAlpha;
    int32_t depth, shift, kind, wide;
    float scale, gamma1;
    float gamut[9];
    int32_t chromaLeft;
    int32_t reduce;
};
struct DeinterlacePush { int32_t w, h, mode, field, first, history; };
struct DenoisePush {
    int32_t w, h;
    float sigma, threshold, keepMin;
    int32_t fresh;
};
struct FilterPush {
    int32_t w, h, ow, oh;
    int32_t kind, mode, ix, iy;
    float p[24];
};
static_assert(sizeof(CompositePush) == 88 && sizeof(DenoisePush) == 24 && sizeof(FilterPush) == 128, "push blocks match the shaders");
static_assert(sizeof(YuvOutPush) == 128 && sizeof(YuvInPush) == 108, "push blocks match the shaders, within the 128 bytes every device has");

class VkEngine final : public Engine {
public:
    ~VkEngine() override { shutdown(); }

    bool init(int deviceIndex, std::string *failure, AVBufferRef *shared = nullptr);

    bool isGpu() const override { return true; }
    std::string deviceName() const override { return m_deviceName; }
    std::string renderNode() const override { return m_renderNode; }
    bool lost() const override { return m_lost; }

    int createSource() override {
        const int id = m_nextId++;
        m_sources.emplace(id, Source{});
        return id;
    }
    void destroySource(int id) override {
        auto it = m_sources.find(id);
        if (it == m_sources.end()) return;
        flush();
        m_filterMs.erase(id);
        destroyImage(it->second.work);
        destroyImage(it->second.previous);
        destroyImage(it->second.fieldFrame);
        destroyImage(it->second.upload);
        destroyImage(it->second.clean[0]);
        destroyImage(it->second.clean[1]);
        for (auto &kv : it->second.imports) destroyImage(kv.second);
        releaseSteps(it->second);
        m_sources.erase(it);
    }
    void setSourceDenoise(int id, float strength) override {
        auto it = m_sources.find(id);
        if (it == m_sources.end()) return;
        if ((strength > 0.f) != (it->second.denoise > 0.f)) it->second.historyValid = false;
        it->second.denoise = strength;
        it->second.pendingProcess = true;
    }

    void setSourceDeinterlace(int id, DeinterlaceMode mode) override;
    void renderSourceField(int id, int field, bool firstInFrame) override;
    void setSourceFilters(int id, const std::vector<VideoFilter> &filters) override;
    bool prepareProgram(const ColourProgram &program, std::string *error) override;
    void renderSourceFilters(int id, double seconds) override;
    void resetSourceHistory(int id) override;
    bool updateSource(int id, const CpuFrame &f) override;
    bool updateSource(int id, const DmaBufFrame &f) override;
    bool updateSource(int id, const AVFrame *frame, const YuvInput &shape) override;
    // A YUV picture packed in `packed` (rows as yuvRows says) to the source's
    // working picture: into its upload image, then converted.
    bool convertPacked(Source &s, const YuvInput &f, VkBuffer packed, int alphaRow);
    bool updateSource(int id, const YuvInput &f) override;
    std::vector<uint64_t> importModifiers(uint32_t fourcc) const override;

    void setCanvasSize(int w, int h) override {
        if (w <= 0 || h <= 0 || (m_canvas.width == w && m_canvas.height == h)) return;
        flush();
        destroyImage(m_canvas);
        // No canvas, no engine: taken as lost, and its owner makes another
        // (the CPU twin, when this device cannot make one this size).
        if (!createImage(m_canvas, w, h, kWorkFormat,
                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            fail(VK_ERROR_DEVICE_LOST);
    }
    int canvasWidth() const override { return m_canvas.width; }
    int canvasHeight() const override { return m_canvas.height; }

    void composite(const std::vector<Layer> &layers, const Color &bg) override;
    void compositeInto(int target, int width, int height, const std::vector<Layer> &layers, const Color &bg) override;
    void transitionInto(int target, int width, int height, int from, int to, const TransitionStyle &style, float progress) override;
    bool readPreview(int w, int h, Bgra8Frame &out) override;
    bool readSourcePreview(int source, int w, int h, Bgra8Frame &out) override;
    bool readSourceInput(int source, int w, int h, Bgra8Frame &out) override;
    bool readSourceEncoded(int source, int w, int h, Bgra8Frame &out) override;
    bool readSourceShown(int source, int w, int h, uint8_t *bgra) override;
    bool readLinear(int source, FrameF &out) override;
    bool readLinearInto(int source, const std::function<float *(int, int)> &target) override;
    bool countScopes(int source, const ColourProgram *program, const ScopeLayout &layout, std::vector<uint32_t> &cells) override;
    // The picture's floats into m_linearHost, waited for; its size, or 0×0.
    std::pair<int, int> linearToHost(int source);
    bool readOutput(const OutputColour &colour, YuvFrame &out, int width, int height) override;
    bool readSourceOutput(int source, const OutputColour &colour, YuvFrame &out, int width, int height) override;
    void setHdrLevels(const HdrLevels &levels) override {
        if (levels == m_levels) return;
        m_levels = levels;
        for (auto &kv : m_sources) kv.second.filtered = false;  // tone maps read the white level
    }
    HdrLevels hdrLevels() const override { return m_levels; }
    void flush() override;

    int requestOutput(const OutputColour &colour, int width, int height) override;
    int requestPreview(int width, int height) override;
    void submit() override;
    void collect() override { settle(); }
    bool takeOutput(int request, YuvFrame &out) override;
    bool takePreview(int request, Bgra8Frame &out) override;
    const void *sharedDevice() const override { return m_ffDevice ? m_ffDevice->data : nullptr; }
    bool writeVulkanFrames(const OutputColour &colour, int width, int height, const std::vector<AVFrame *> &frames) override;

private:
    // On FFmpeg's device: its handles, a compute queue of its, and its queue
    // lock around our submissions (its encoders submit to the same queues).
    bool adopt(AVBufferRef *shared, std::string *failure);
    void lockQueue();
    void unlockQueue();
    // Timeline semaphores the next submission waits for and signals (frames
    // handed to FFmpeg's encoders).
    std::vector<VkSemaphore> m_waitSems, m_signalSems;
    std::vector<uint64_t> m_waitValues, m_signalValues;
    std::vector<VkPipelineStageFlags> m_waitStages;
    bool submitRecorded();  // ends m_cmd and submits it with m_fence and the semaphores
    AVBufferRef *m_ffDevice = nullptr;
    uint32_t m_queueIndex = 0;
    // Where a readback's planes sit in its host buffer, and its size.
    struct Planes {
        uint32_t offsets[3] = {};
        uint32_t strides[3] = {};
        VkDeviceSize size = 0;
    };
    // A readback asked for and not yet taken, with buffers of its own.
    struct Readback {
        int id = 0;               // 0: free; -1: feeding a frame handed to an encoder, free once that work is done
        bool preview = false;
        YuvFrame shape;           // the picture's size and format, no samples
        Planes planes;
        Buffer device, host;
    };

    bool previewOf(const Image &picture, int w, int h, Bgra8Frame &out, bool encoded = false);
    // encoded: as preview.comp's.
    bool previewInto(const Image &picture, int w, int h, uint8_t *to, int encoded);
    bool outputOf(const Image &picture, const OutputColour &colour, YuvFrame &out, int width, int height);
    // Records the conversion into `device` and its copy into `host`, sized
    // by `shape`; the samples are there after the work is finished.
    bool recordOutput(const Image &picture, const OutputColour &colour, const YuvFrame &shape, Buffer &device,
                      Buffer &host, Planes *planes, bool toHost = true);
    bool recordReadback(const Image &picture, const OutputColour &colour, const YuvFrame &shape, Buffer &device,
                        Buffer &host, Planes *planes);
    int m_readbackMode = -1;  // -1 not measured yet, 0 copy across, 1 written straight to host memory
    static void copyOutput(const Buffer &host, const Planes &planes, YuvFrame &out);
    bool recordPreview(const Image &picture, int w, int h, Buffer &device, Buffer &host, int encoded = 0);
    Readback &freeReadback();
    // Waits for what submit() sent.
    void settle();
    // Setup
    bool createInstance(std::string *failure);
    bool pickDevice(int index, std::string *failure);
    bool createDevice(std::string *failure);
    bool createPipelines(std::string *failure);
    bool ensureNetworkPipeline();
    bool m_nnTried = false;
    bool createPipeline(Pipeline &p, const char *spv, const std::vector<VkDescriptorType> &types, uint32_t pushSize, std::string *failure);
    bool createPipeline(Pipeline &p, const char *spv, VkDescriptorType binding1, uint32_t pushSize,
                        std::string *failure, bool secondSampler = false);
    void shutdown();

    // Resources
    bool createImage(Image &img, int w, int h, VkFormat fmt, VkImageUsageFlags usage);
    bool importDmaBuf(Image &img, const DmaBufFrame &f, const FormatInfo &fi);
    void destroyImage(Image &img);
    bool createBuffer(Buffer &b, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible);
    void destroyBuffer(Buffer &b);
    bool ensureBuffer(Buffer &b, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible) {
        if (b.size >= size && b.buffer) return true;
        destroyBuffer(b);
        return createBuffer(b, size, usage, hostVisible);
    }
    int32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred);

    // Recording
    bool begin();
    void imageBarrier(Image &img, VkImageLayout from, VkImageLayout to, bool acquireForeign = false);
    void memoryBarrier();
    VkDescriptorSet descriptorSet(const Pipeline &p, VkImageView sampled, VkImageView storage,
                                  VkBuffer buffer, VkDeviceSize bufferSize, VkImageView sampled2 = VK_NULL_HANDLE);
    // Cleans the source's newly converted frame when it asks for noise reduction.
    void beginFrame(Source &s, int w, int h);
    void finishFrame(Source &s);
    bool recordDeinterlace(Source &s, int field, bool first);
    void recordDenoise(Source &s);
    void compositeTo(Image &into, int skip, const std::vector<Layer> &layers, const Color &bg);
    void clearTo(Image &into, const Color &bg);
    void drawLayer(Image &into, const Image &shown, const Layer &l);
    bool wrapLayer(Image &into, const Image &shown, const Layer &l, const VideoFilter &f);
    bool recordMatte(const Image &in, Source::Step &st, const FilterParams &q, bool fresh);
    bool recordGuided(const Image &in, Source::Step &st, const VideoFilter &f, const FilterParams &q);
    bool filterImage(Image &img, int w, int h);
    bool uploadAux(Image &img, VkFormat format, int w, int h, const void *data, size_t bytes);
    void recordFilter(const Image &in, Image &out, const Image *aux, const FilterParams &q);
    void releaseSteps(Source &s);
    void recordConvert(Source &s, const Image &from, bool forceOpaque, bool premultiplied = false, bool limitedRange = false, bool linearAlpha = true, bool yInvert = false, BufferTransform transform = BufferTransform::Normal, bool encoded = false);
    void fail(VkResult r);

    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_family = 0;
    VkCommandPool m_cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    VkFence m_fence = VK_NULL_HANDLE;
    // GPU timestamps around each source's filters: written while recording,
    // read once the fence says the work is done (never waited for).
    static constexpr uint32_t kTimestamps = 128;
    VkQueryPool m_timestamps = VK_NULL_HANDLE;
    double m_tickNs = 0;            // ns per timestamp unit; 0: this queue cannot time
    uint32_t m_timestampsUsed = 0;
    uint64_t m_bufferSerial = 0;    // counts command buffers begun
    struct Timing { int source; uint32_t first; };
    std::vector<Timing> m_timings, m_timingsInFlight;
    std::unordered_map<int, double> m_filterMs;
    void harvestTimings();
public:
    double filterMs(int id) const override {
        const auto it = m_filterMs.find(id);
        return it == m_filterMs.end() ? -1 : it->second;
    }
private:
    VkDescriptorPool m_descPool = VK_NULL_HANDLE;
    // More, made when a frame needs more sets than one pool holds (many
    // sources and filters); reset with the first at each begin().
    std::vector<VkDescriptorPool> m_extraPools;
    size_t m_extraUsed = 0;
    VkDescriptorPool makeDescriptorPool();
    void hostReadBarrier();
    std::vector<Image> m_doomed;  // given up while lost; destroyed in the destructor
    std::vector<Buffer> m_doomedBuffers;
    bool waitForWork();  // waits for what was submitted; false, and lost, when it never finishes
    bool m_tearingDown = false;
    VkSampler m_linear = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties m_memProps{};
    PFN_vkGetMemoryFdPropertiesKHR m_getMemoryFdProperties = nullptr;
    bool m_hasDmaBuf = false;
    bool m_hasForeignQueue = false;
    bool m_recording = false;
    bool m_lost = false;
    std::string m_deviceName;
    std::string m_renderNode;

    Pipeline m_transition, m_convert, m_composite, m_yuv, m_preview, m_yuvIn, m_denoise, m_deinterlace, m_filter, m_linearOut, m_nn;
    // RemoveBackground's network: its weights, uploaded the first time.
    Buffer m_nnWeights;
    // Light wrap's pictures, a set for each scale it works at: as large as
    // the largest region drawn yet, each region worked in their corner.
    struct Wrap {
        int scale = 4;
        Image images[4];
    };
    std::vector<Wrap> m_wraps;
    VkDeviceSize m_maxStorage = 0;  // the largest storage buffer a shader may see
    int m_maxSide = 0;              // the widest or tallest picture the device makes
    // A picture the device cannot take packed (see updateSource): the CPU
    // twin reads it in, and its working floats are uploaded.
    std::unique_ptr<Engine> m_twin;
    int m_twinSource = 0;
    FrameF m_twinFrame;
    bool updateSourceByTwin(int id, const YuvInput &f);
    Buffer m_linearHost;  // readLinear's floats
    Buffer m_gpuPacked;   // a decoder's GPU picture, packed as an upload would be
    Pipeline m_grade;     // grade.comp: a grade plan's operations
    Image m_noPicture, m_noTable;  // bound where an operation has no picture or table
    bool createGradePipeline(std::string *failure);
    bool runPlan(Source::Step &st, const GradePlan &plan, const Image &input);
    void releaseStep(Source::Step &st);
    std::vector<Pipeline> m_effects;  // one per Effect, in effects.def's order
    // Colour programs (OCIO transforms), compiled when first asked for and
    // kept by their id: the pipeline, with their own textures and uniforms.
    struct ProgramPipe {
        Pipeline pipe;
        VkDescriptorSetLayout resources = VK_NULL_HANDLE;  // set 1, when it has any
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        std::vector<Image> textures;
        Buffer uniforms;
        std::string error;  // empty: ready
        uint64_t used = 0;  // when it was last asked for (m_programClock)
        // The scopes' counting pass with this transform in it (countScopes),
        // made when first asked for; `scopeTried` so a refusal is not tried again.
        Pipeline scope;
        bool scopeTried = false;
    };
    // The scopes' counting pass (countScopes): `m_scopePlain` counts a picture
    // as it is. Counted in device memory and copied to `m_scopeHost`. Doubles
    // in a shader are a device feature (m_float64): without it nothing here
    // is made and the caller counts on the CPU.
    Pipeline m_scopePlain;
    bool m_scopePlainTried = false;
    bool m_float64 = false;
    Buffer m_scopeDevice, m_scopeHost;
    bool buildScopePipeline(const ColourProgram *program, const ProgramPipe *with, Pipeline &p);
    void destroyPipeline(Pipeline &p);
    std::unordered_map<std::string, ProgramPipe> m_programs;
    // The transforms kept: the most recently used. Each has a pipeline and
    // tables of its own (megabytes for a 3D table), and an editor trying
    // looks or editing a LUT file makes a new one each time.
    static constexpr size_t kProgramsKept = 64;
    uint64_t m_programClock = 0;
    ProgramPipe &programPipe(const ColourProgram &program);
    bool buildProgram(const ColourProgram &program, ProgramPipe &p, std::string *failure);
    void destroyProgram(ProgramPipe &p);
    bool uploadTexture(Image &img, const ColourTexture &t);
    void recordProgram(const Image &in, Image &out, const ProgramPipe &p);
    VkDescriptorSet allocateSet(VkDescriptorSetLayout layout);
    VkSampler m_nearest = VK_NULL_HANDLE;

    std::unordered_map<int, Source> m_sources;
    HdrLevels m_levels;
    int m_nextId = 1;
    Image m_canvas;
    // Host -> device uploads. A few buffers, each free again once the command
    // buffer that reads it has finished, so a frame's clips upload one after
    // another without each waiting for the GPU to finish the one before.
    struct Staging {
        Buffer buffer;
        uint64_t serial = 0;  // the command buffer (m_bufferSerial) that reads it
    };
    static constexpr size_t kStagings = 4;
    std::vector<Staging> m_stagings;
    Staging *stagingFor(VkDeviceSize size);
    bool stagingBusy(const Staging &st) const { return st.serial == m_bufferSerial && (m_recording || m_submitted); }
    Buffer m_auxStaging;     // LUT and mask uploads
    Buffer m_yuvDevice, m_yuvHost;
    std::vector<Readback> m_readbacks;
    bool m_submitted = false;     // submit() sent work nobody has waited for
    Buffer m_previewDevice, m_previewHost;
    std::vector<VkDescriptorSet> m_usedSets;
};

// ---------------------------------------------------------------- setup

static std::string renderNodeOf(VkPhysicalDevice device);

bool VkEngine::init(int deviceIndex, std::string *failure, AVBufferRef *shared) {
    std::lock_guard<std::recursive_mutex> lock(loaderMutex());
    if (shared) return adopt(shared, failure) && createDevice(failure) && createPipelines(failure);
    return createInstance(failure) && pickDevice(deviceIndex, failure) && createDevice(failure) &&
           createPipelines(failure);
}

bool VkEngine::adopt(AVBufferRef *shared, std::string *failure) {
    auto *dc = reinterpret_cast<AVHWDeviceContext *>(shared->data);
    if (dc->type != AV_HWDEVICE_TYPE_VULKAN) {
        if (failure) *failure = "not a Vulkan device";
        return false;
    }
    const auto *vk = static_cast<const AVVulkanDeviceContext *>(dc->hwctx);
    m_ffDevice = av_buffer_ref(shared);
    m_instance = vk->inst;
    m_physical = vk->phys_dev;
    m_device = vk->act_dev;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(m_physical, &props);
    m_deviceName = props.deviceName;
    m_renderNode = renderNodeOf(m_physical);
    vkGetPhysicalDeviceMemoryProperties(m_physical, &m_memProps);
    return true;
}

void VkEngine::lockQueue() {
    if (!m_ffDevice) return;
    auto *dc = reinterpret_cast<AVHWDeviceContext *>(m_ffDevice->data);
    auto *vk = static_cast<AVVulkanDeviceContext *>(dc->hwctx);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    if (vk->lock_queue) vk->lock_queue(dc, m_family, m_queueIndex);
#pragma GCC diagnostic pop
}

void VkEngine::unlockQueue() {
    if (!m_ffDevice) return;
    auto *dc = reinterpret_cast<AVHWDeviceContext *>(m_ffDevice->data);
    auto *vk = static_cast<AVVulkanDeviceContext *>(dc->hwctx);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    if (vk->unlock_queue) vk->unlock_queue(dc, m_family, m_queueIndex);
#pragma GCC diagnostic pop
}

// The device's DRM render node, from VK_EXT_physical_device_drm when the
// driver offers it.
static std::string renderNodeOf(VkPhysicalDevice device) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, exts.data());
    bool has = false;
    for (const VkExtensionProperties &e : exts)
        if (std::strcmp(e.extensionName, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME) == 0) has = true;
    if (!has) return {};
    VkPhysicalDeviceDrmPropertiesEXT drm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props.pNext = &drm;
    vkGetPhysicalDeviceProperties2(device, &props);
    if (!drm.hasRender) return {};
    return "/dev/dri/renderD" + std::to_string(drm.renderMinor);
}

std::vector<GpuDevice> enumerateGpus() {
    std::lock_guard<std::recursive_mutex> lock(loaderMutex());
    std::vector<GpuDevice> out;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "omaedit";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ci, nullptr, &instance) != VK_SUCCESS) return out;
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceIDProperties ids{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &ids;
        vkGetPhysicalDeviceProperties2(devices[i], &props);
        const VkPhysicalDeviceProperties &p = props.properties;
        GpuDevice g;
        g.index = int(i);
        static const char hex[] = "0123456789abcdef";
        for (uint8_t b : ids.deviceUUID) { g.id += hex[b >> 4]; g.id += hex[b & 15]; }
        g.name = p.deviceName;
        g.vendorId = p.vendorID;
        g.renderNode = renderNodeOf(devices[i]);
        g.kind = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? GpuDevice::Kind::Discrete
               : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? GpuDevice::Kind::Integrated
               : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU    ? GpuDevice::Kind::Virtual
                                                                        : GpuDevice::Kind::Other;
        g.hardware = p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU && p.deviceType != VK_PHYSICAL_DEVICE_TYPE_OTHER;
        g.usable = g.hardware && p.apiVersion >= VK_API_VERSION_1_2;
        out.push_back(g);
    }
    vkDestroyInstance(instance, nullptr);
    return out;
}

bool VkEngine::createInstance(std::string *failure) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "omaedit";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    const VkResult r = vkCreateInstance(&ci, nullptr, &m_instance);
    if (r != VK_SUCCESS) {
        if (failure) *failure = "no Vulkan instance (" + std::to_string(r) + ")";
        return false;
    }
    return true;
}

bool VkEngine::pickDevice(int index, std::string *failure) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0) {
        if (failure) *failure = "no Vulkan device";
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

    auto score = [](VkPhysicalDevice d) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.apiVersion < VK_API_VERSION_1_2) return -1;
        switch (p.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 3;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 2;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 1;
        default: return 0;  // the CPU twin beats a software Vulkan device
        }
    };

    int best = -1;
    if (index >= static_cast<int>(count)) {
        if (failure) *failure = "no Vulkan device at index " + std::to_string(index);
        return false;
    }
    if (index >= 0) {
        best = index;
    } else {
        int bestScore = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const int s = score(devices[i]);
            if (s > bestScore) { bestScore = s; best = static_cast<int>(i); }
        }
    }
    if (best < 0) {
        if (failure) *failure = "no suitable Vulkan device";
        return false;
    }
    m_physical = devices[best];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(m_physical, &props);
    m_deviceName = props.deviceName;
    m_renderNode = renderNodeOf(m_physical);
    vkGetPhysicalDeviceMemoryProperties(m_physical, &m_memProps);
    return true;
}

bool VkEngine::createDevice(std::string *failure) {
    if (m_ffDevice) {
        const auto *vk = static_cast<const AVVulkanDeviceContext *>(reinterpret_cast<AVHWDeviceContext *>(m_ffDevice->data)->hwctx);
        bool found = false;
        for (int i = 0; i < vk->nb_qf && !found; ++i) {
            if (!(vk->qf[i].flags & VK_QUEUE_COMPUTE_BIT)) continue;
            m_family = static_cast<uint32_t>(vk->qf[i].idx);
            m_queueIndex = static_cast<uint32_t>(std::max(0, vk->qf[i].num - 1));
            found = true;
        }
        if (!found) {
            if (failure) *failure = "no compute queue on FFmpeg's device";
            return false;
        }
        auto enabled = [&](const char *name) {
            for (int i = 0; i < vk->nb_enabled_dev_extensions; ++i)
                if (std::strcmp(vk->enabled_dev_extensions[i], name) == 0) return true;
            return false;
        };
        m_hasDmaBuf = enabled(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) && enabled(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) &&
                      enabled(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
        m_hasForeignQueue = m_hasDmaBuf && enabled(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
        m_float64 = vk->device_features.features.shaderFloat64 == VK_TRUE;
        VkDeviceQueueInfo2 qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
        qi.flags = vk->queue_flags;
        qi.queueFamilyIndex = m_family;
        qi.queueIndex = m_queueIndex;
        vkGetDeviceQueue2(m_device, &qi, &m_queue);
    } else {
        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &qCount, families.data());
        bool found = false;
        for (uint32_t i = 0; i < qCount; ++i) {
            if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                m_family = i;
                found = true;
                break;
            }
        }
        if (!found) {
            if (failure) *failure = "no compute queue";
            return false;
        }

        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(m_physical, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(m_physical, nullptr, &extCount, exts.data());
        auto has = [&](const char *name) {
            return std::any_of(exts.begin(), exts.end(), [&](const VkExtensionProperties &e) {
                return std::strcmp(e.extensionName, name) == 0;
            });
        };
        std::vector<const char *> enable;
        if (has(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) && has(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) &&
            has(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME)) {
            enable.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
            enable.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
            enable.push_back(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
            m_hasDmaBuf = true;
            if (has(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)) {
                enable.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
                m_hasForeignQueue = true;
            }
        }

        const float priority = 1.f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = m_family;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = static_cast<uint32_t>(enable.size());
        dci.ppEnabledExtensionNames = enable.data();
        // Doubles in shaders, where the device has them: the scopes' counting
        // pass needs them to count exactly as the CPU does (countScopes).
        VkPhysicalDeviceFeatures has64{}, wanted{};
        vkGetPhysicalDeviceFeatures(m_physical, &has64);
        wanted.shaderFloat64 = has64.shaderFloat64;
        dci.pEnabledFeatures = &wanted;
        VkResult r = vkCreateDevice(m_physical, &dci, nullptr, &m_device);
        m_float64 = r == VK_SUCCESS && wanted.shaderFloat64 == VK_TRUE;
        if (r != VK_SUCCESS) {
            if (failure) *failure = "device creation failed (" + std::to_string(r) + ")";
            return false;
        }
        vkGetDeviceQueue(m_device, m_family, 0, &m_queue);
    }
    if (m_hasDmaBuf) {
        m_getMemoryFdProperties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
            vkGetDeviceProcAddr(m_device, "vkGetMemoryFdPropertiesKHR"));
        if (!m_getMemoryFdProperties) m_hasDmaBuf = false;
    }

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = m_family;
    if (vkCreateCommandPool(m_device, &pci, nullptr, &m_cmdPool) != VK_SUCCESS) {
        if (failure) *failure = "command pool";
        return false;
    }
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = m_cmdPool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkAllocateCommandBuffers(m_device, &cai, &m_cmd) != VK_SUCCESS || vkCreateFence(m_device, &fci, nullptr, &m_fence) != VK_SUCCESS) {
        if (failure) *failure = "command buffer";
        return false;
    }
    {
        // Timestamps, where the queue keeps them (filter costs; nothing else depends on them).
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(m_physical, &props);
        uint32_t n = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &n, nullptr);
        std::vector<VkQueueFamilyProperties> families(n);
        vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &n, families.data());
        if (m_family < n && families[m_family].timestampValidBits > 0 && props.limits.timestampPeriod > 0) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = kTimestamps;
            if (vkCreateQueryPool(m_device, &qi, nullptr, &m_timestamps) == VK_SUCCESS) m_tickNs = props.limits.timestampPeriod;
        }
    }

    {
        VkPhysicalDeviceProperties limits;
        vkGetPhysicalDeviceProperties(m_physical, &limits);
        m_maxStorage = limits.limits.maxStorageBufferRange;
        m_maxSide = int(std::min<uint32_t>(limits.limits.maxImageDimension2D, 1u << 30));
        // Working pictures and float tables are read between their texels.
        for (const VkFormat format : {kWorkFormat, VK_FORMAT_R32_SFLOAT}) {
            VkFormatProperties fp{};
            vkGetPhysicalDeviceFormatProperties(m_physical, format, &fp);
            if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
                if (failure) *failure = "this GPU cannot filter float pictures";
                return false;
            }
        }
    }
    m_descPool = makeDescriptorPool();
    if (!m_descPool) {
        if (failure) *failure = "descriptor pool";
        return false;
    }

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.f;
    if (vkCreateSampler(m_device, &sci, nullptr, &m_linear) != VK_SUCCESS) m_linear = VK_NULL_HANDLE;
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(m_device, &sci, nullptr, &m_nearest) != VK_SUCCESS) m_nearest = VK_NULL_HANDLE;
    if (!m_linear || !m_nearest) {
        if (failure) *failure = "sampler";
        return false;
    }
    return true;
}

bool VkEngine::createPipeline(Pipeline &p, const char *spv, VkDescriptorType binding1, uint32_t pushSize,
                              std::string *failure, bool secondSampler) {
    std::vector<VkDescriptorType> bindings{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, binding1};
    if (secondSampler) bindings.push_back(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    return createPipeline(p, spv, bindings, pushSize, failure);
}

bool VkEngine::createPipeline(Pipeline &p, const char *spv, const std::vector<VkDescriptorType> &types, uint32_t pushSize,
                              std::string *failure) {
    const QByteArray code = loadSpirv(spv);
    if (code.isEmpty() || code.size() % 4 != 0) {
        if (failure) *failure = std::string("shader missing: ") + spv;
        return false;
    }
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = static_cast<size_t>(code.size());
    smci.pCode = reinterpret_cast<const uint32_t *>(code.constData());
    VkShaderModule module;
    if (vkCreateShaderModule(m_device, &smci, nullptr, &module) != VK_SUCCESS) {
        if (failure) *failure = std::string("shader rejected: ") + spv;
        return false;
    }

    std::vector<VkDescriptorSetLayoutBinding> bindings(types.size());
    for (size_t i = 0; i < types.size(); ++i) {
        bindings[i].binding = uint32_t(i);
        bindings[i].descriptorType = types[i];
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dlci.bindingCount = uint32_t(bindings.size());
    dlci.pBindings = bindings.data();
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushSize};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &p.layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    if (vkCreateDescriptorSetLayout(m_device, &dlci, nullptr, &p.layout) != VK_SUCCESS ||
        vkCreatePipelineLayout(m_device, &plci, nullptr, &p.pipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, module, nullptr);
        if (failure) *failure = std::string("no layout for ") + spv;
        return false;
    }

    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = module;
    cpci.stage.pName = "main";
    cpci.layout = p.pipelineLayout;
    const VkResult r = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &cpci, nullptr, &p.pipeline);
    vkDestroyShaderModule(m_device, module, nullptr);
    if (r != VK_SUCCESS) {
        if (failure) *failure = std::string("pipeline failed: ") + spv;
        return false;
    }
    return true;
}

bool VkEngine::createPipelines(std::string *failure) {
    // Each pipeline is an object of its own and the driver compiles it on the
    // thread that asks. Made side by side, a driver with nothing cached (a new
    // install, a driver update) costs the slowest few of them, not the sum of
    // them all. OMAEDIT_PIPELINE_THREADS=1 makes them one after another.
    struct Job {
        Pipeline *pipeline;
        const char *spv;
        std::vector<VkDescriptorType> types;
        uint32_t pushSize;
    };
    const auto sampled = [](VkDescriptorType binding1, bool secondSampler = false) {
        std::vector<VkDescriptorType> types{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, binding1};
        if (secondSampler) types.push_back(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        return types;
    };
    std::vector<Job> jobs{
        {&m_transition, "transition.spv", sampled(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, true), sizeof(TransitionPush)},
        {&m_convert, "convert.spv", sampled(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE), sizeof(ConvertPush)},
        {&m_composite, "composite.spv", sampled(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE), sizeof(CompositePush)},
        {&m_yuv, "yuv.spv", sampled(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER), sizeof(YuvOutPush)},
        {&m_preview, "preview.spv", sampled(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER), sizeof(PreviewPush)},
        {&m_linearOut, "linear.spv", sampled(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER), sizeof(PreviewPush)},
        {&m_yuvIn, "yuv_in.spv", sampled(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE), sizeof(YuvInPush)},
        {&m_deinterlace, "deinterlace.spv", sampled(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, true), sizeof(DeinterlacePush)},
        {&m_denoise, "denoise.spv", sampled(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, true), sizeof(DenoisePush)},
        {&m_filter, "filter.spv", sampled(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, true), sizeof(FilterPush)},
    };
    static const char *const effectShaders[] = {
#define EFFECT(Name, stem, passes) "fx_" #stem ".spv",
#include "effects/effects.def"
#undef EFFECT
    };
    m_effects.resize(sizeof effectShaders / sizeof effectShaders[0]);
    for (size_t i = 0; i < m_effects.size(); ++i)
        jobs.push_back({&m_effects[i], effectShaders[i], sampled(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, true), sizeof(FilterPush)});

    std::vector<std::string> failures(jobs.size());
    std::vector<char> made(jobs.size(), 0);
    std::atomic<size_t> next{0};
    const auto work = [&] {
        for (size_t i = next.fetch_add(1); i < jobs.size(); i = next.fetch_add(1))
            made[i] = createPipeline(*jobs[i].pipeline, jobs[i].spv, jobs[i].types, jobs[i].pushSize, &failures[i]);
    };
    unsigned wanted = std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
    if (const char *asked = std::getenv("OMAEDIT_PIPELINE_THREADS")) {
        char *end = nullptr;
        const long count = std::strtol(asked, &end, 10);
        if (end != asked && count >= 1) wanted = unsigned(std::min(count, 64L));
    }
    std::vector<std::thread> helpers;
    for (unsigned t = 1; t < wanted; ++t) helpers.emplace_back(work);
    std::string gradeFailure;
    const bool grade = createGradePipeline(&gradeFailure);
    work();
    for (std::thread &helper : helpers) helper.join();
    // The first that failed, in the order they are listed.
    for (size_t i = 0; i < jobs.size(); ++i)
        if (!made[i]) {
            if (failure) *failure = failures[i];
            return false;
        }
    if (!grade && failure) *failure = gradeFailure;
    return grade;
}

// The person-finding network's pipeline, made when RemoveBackground first
// runs. It is by far the slowest to compile (on a cold driver cache, more than
// all the others together), and most timelines never use it.
bool VkEngine::ensureNetworkPipeline() {
    if (m_nn.pipeline) return true;
    if (m_nnTried) return false;
    m_nnTried = true;
    return createPipeline(m_nn, "nn.spv",
                          {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                          sizeof(matting::Pass), nullptr);
}

void VkEngine::shutdown() {
    std::lock_guard<std::recursive_mutex> lock(loaderMutex());
    if (!m_device) {
        if (m_instance) vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
        return;
    }
    // Work recorded and never sent goes now, while every picture it names is
    // still there (and with it the signals FFmpeg's frames wait for); after
    // this nothing is recorded, so nothing below can send any.
    flush();
    // On FFmpeg's device its encoders may still be submitting: wait for our
    // own queue only (theirs are theirs), under its lock.
    if (m_ffDevice) {
        // A start that failed before it had a queue has nothing to wait for.
        if (m_queue) {
            lockQueue();
            vkQueueWaitIdle(m_queue);
            unlockQueue();
        }
    } else {
        vkDeviceWaitIdle(m_device);
    }
    m_submitted = false;
    m_tearingDown = true;  // waited for: destroyed for real from here
    for (Image &img : m_doomed) destroyImage(img);
    m_doomed.clear();
    for (Buffer &b : m_doomedBuffers) destroyBuffer(b);
    m_doomedBuffers.clear();
    for (auto &kv : m_sources) {
        destroyImage(kv.second.work);
        destroyImage(kv.second.previous);
        destroyImage(kv.second.fieldFrame);
        destroyImage(kv.second.upload);
        destroyImage(kv.second.clean[0]);
        destroyImage(kv.second.clean[1]);
        for (auto &imp : kv.second.imports) destroyImage(imp.second);
        releaseSteps(kv.second);
    }
    m_sources.clear();
    destroyImage(m_canvas);
    for (Staging &st : m_stagings) destroyBuffer(st.buffer);
    m_stagings.clear();
    destroyBuffer(m_auxStaging);
    destroyBuffer(m_yuvDevice);
    destroyBuffer(m_yuvHost);
    destroyBuffer(m_previewDevice);
    destroyBuffer(m_previewHost);
    destroyBuffer(m_linearHost);
    destroyBuffer(m_gpuPacked);
    destroyBuffer(m_nnWeights);
    for (Wrap &w : m_wraps) for (Image &img : w.images) destroyImage(img);
    m_wraps.clear();
    for (Readback &r : m_readbacks) {
        destroyBuffer(r.device);
        destroyBuffer(r.host);
    }
    m_readbacks.clear();
    for (auto &kv : m_programs) destroyProgram(kv.second);
    m_programs.clear();
    destroyPipeline(m_scopePlain);
    destroyBuffer(m_scopeDevice);
    destroyBuffer(m_scopeHost);
    std::vector<Pipeline *> pipelines{&m_transition, &m_convert, &m_composite, &m_yuv, &m_preview, &m_yuvIn, &m_denoise, &m_deinterlace, &m_filter, &m_linearOut};
    for (Pipeline &e : m_effects) pipelines.push_back(&e);
    pipelines.push_back(&m_grade);
    pipelines.push_back(&m_nn);
    destroyImage(m_noPicture);
    destroyImage(m_noTable);
    for (Pipeline *p : pipelines) {
        if (p->pipeline) vkDestroyPipeline(m_device, p->pipeline, nullptr);
        if (p->pipelineLayout) vkDestroyPipelineLayout(m_device, p->pipelineLayout, nullptr);
        if (p->layout) vkDestroyDescriptorSetLayout(m_device, p->layout, nullptr);
    }
    if (m_linear) vkDestroySampler(m_device, m_linear, nullptr);
    if (m_nearest) vkDestroySampler(m_device, m_nearest, nullptr);
    if (m_descPool) vkDestroyDescriptorPool(m_device, m_descPool, nullptr);
    for (VkDescriptorPool pool : m_extraPools) vkDestroyDescriptorPool(m_device, pool, nullptr);
    if (m_fence) vkDestroyFence(m_device, m_fence, nullptr);
    if (m_timestamps) vkDestroyQueryPool(m_device, m_timestamps, nullptr);
    if (m_cmdPool) vkDestroyCommandPool(m_device, m_cmdPool, nullptr);
    if (m_ffDevice) {
        av_buffer_unref(&m_ffDevice);  // the device and instance are FFmpeg's
    } else {
        vkDestroyDevice(m_device, nullptr);
        vkDestroyInstance(m_instance, nullptr);
    }
    m_device = VK_NULL_HANDLE;
    m_instance = VK_NULL_HANDLE;
}

// ------------------------------------------------------------ resources

int32_t VkEngine::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required,
                                 VkMemoryPropertyFlags preferred) {
    for (int pass = 0; pass < 2; ++pass) {
        const VkMemoryPropertyFlags want = pass == 0 ? (required | preferred) : required;
        for (uint32_t i = 0; i < m_memProps.memoryTypeCount; ++i) {
            if ((typeBits & (1u << i)) && (m_memProps.memoryTypes[i].propertyFlags & want) == want)
                return static_cast<int32_t>(i);
        }
    }
    return -1;
}

bool VkEngine::createImage(Image &img, int w, int h, VkFormat fmt, VkImageUsageFlags usage) {
    // Larger than the device makes: refused here, never asked of the driver.
    if (w <= 0 || h <= 0 || w > m_maxSide || h > m_maxSide) return false;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_device, &ici, nullptr, &img.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(m_device, img.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    const int32_t type = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (type < 0) { destroyImage(img); return false; }
    mai.memoryTypeIndex = static_cast<uint32_t>(type);
    if (vkAllocateMemory(m_device, &mai, nullptr, &img.memory) != VK_SUCCESS) { destroyImage(img); return false; }
    if (vkBindImageMemory(m_device, img.image, img.memory, 0) != VK_SUCCESS) { destroyImage(img); return false; }
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(m_device, &vci, nullptr, &img.view) != VK_SUCCESS) { destroyImage(img); return false; }
    img.format = fmt;
    img.width = w;
    img.height = h;
    img.external = false;
    img.initialised = false;
    return true;
}

bool VkEngine::importDmaBuf(Image &img, const DmaBufFrame &f, const FormatInfo &fi) {
    if (!m_hasDmaBuf || f.planes.size() != 1 || f.modifier == DRM_FORMAT_MOD_INVALID) return false;

    VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkSubresourceLayout plane{};
    plane.offset = f.planes[0].offset;
    plane.rowPitch = f.planes[0].stride;
    VkImageDrmFormatModifierExplicitCreateInfoEXT mod{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
    mod.drmFormatModifier = f.modifier;
    mod.drmFormatModifierPlaneCount = 1;
    mod.pPlaneLayouts = &plane;
    ext.pNext = &mod;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.pNext = &ext;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fi.vk;
    ici.extent = {static_cast<uint32_t>(f.width), static_cast<uint32_t>(f.height), 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_device, &ici, nullptr, &img.image) != VK_SUCCESS) return false;

    const int fd = dup(f.planes[0].fd);
    if (fd < 0) { destroyImage(img); return false; }
    VkMemoryFdPropertiesKHR fdProps{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    if (m_getMemoryFdProperties(m_device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdProps) !=
        VK_SUCCESS) {
        close(fd);
        destroyImage(img);
        return false;
    }
    VkMemoryDedicatedRequirements dedReq{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    req.pNext = &dedReq;
    VkImageMemoryRequirementsInfo2 reqInfo{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
    reqInfo.image = img.image;
    vkGetImageMemoryRequirements2(m_device, &reqInfo, &req);

    const uint32_t bits = req.memoryRequirements.memoryTypeBits & fdProps.memoryTypeBits;
    const int32_t type = findMemoryType(bits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) { close(fd); destroyImage(img); return false; }

    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = img.image;
    VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    imp.pNext = &ded;
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    imp.fd = fd;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &imp;
    mai.allocationSize = req.memoryRequirements.size;
    mai.memoryTypeIndex = static_cast<uint32_t>(type);
    if (vkAllocateMemory(m_device, &mai, nullptr, &img.memory) != VK_SUCCESS) {
        close(fd);  // import failed, the fd is still ours
        destroyImage(img);
        return false;
    }
    if (vkBindImageMemory(m_device, img.image, img.memory, 0) != VK_SUCCESS) { destroyImage(img); return false; }

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fi.vk;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(m_device, &vci, nullptr, &img.view) != VK_SUCCESS) { destroyImage(img); return false; }
    img.format = fi.vk;
    img.width = f.width;
    img.height = f.height;
    img.external = true;
    img.initialised = false;
    return true;
}

void VkEngine::destroyImage(Image &img) {
    // A device that stopped answering (a fence timed out) may still be using
    // it: kept until the engine goes, after its last wait.
    if (m_lost && !m_tearingDown && (img.view || img.image || img.memory)) {
        m_doomed.push_back(img);
        img = Image{};
        return;
    }
    if (img.view) vkDestroyImageView(m_device, img.view, nullptr);
    if (img.image) vkDestroyImage(m_device, img.image, nullptr);
    if (img.memory) vkFreeMemory(m_device, img.memory, nullptr);
    img = Image{};
}

bool VkEngine::createBuffer(Buffer &b, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bci, nullptr, &b.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(m_device, b.buffer, &req);
    const int32_t type = hostVisible
        ? findMemoryType(req.memoryTypeBits,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
        : findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (type < 0) { destroyBuffer(b); return false; }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = static_cast<uint32_t>(type);
    if (vkAllocateMemory(m_device, &mai, nullptr, &b.memory) != VK_SUCCESS) { destroyBuffer(b); return false; }
    if (vkBindBufferMemory(m_device, b.buffer, b.memory, 0) != VK_SUCCESS) { destroyBuffer(b); return false; }
    if (hostVisible && vkMapMemory(m_device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped) != VK_SUCCESS) {
        destroyBuffer(b);
        return false;
    }
    b.size = size;
    return true;
}

void VkEngine::destroyBuffer(Buffer &b) {
    // As destroyImage: a device that stopped answering may still be using it.
    if (m_lost && !m_tearingDown && (b.buffer || b.memory)) {
        m_doomedBuffers.push_back(b);
        b = Buffer{};
        return;
    }
    if (b.mapped) vkUnmapMemory(m_device, b.memory);
    if (b.buffer) vkDestroyBuffer(m_device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(m_device, b.memory, nullptr);
    b = Buffer{};
}

// ------------------------------------------------------------ recording

void VkEngine::fail(VkResult r) {
    if (r == VK_ERROR_DEVICE_LOST) m_lost = true;
}

bool VkEngine::begin() {
    if (m_lost) return false;
    if (m_recording) return true;
    settle();  // the command buffer and descriptor pool may still be in use
    if (m_lost) return false;  // that wait never ended: they are the GPU's still
    vkResetDescriptorPool(m_device, m_descPool, 0);
    for (VkDescriptorPool pool : m_extraPools) vkResetDescriptorPool(m_device, pool, 0);
    m_extraUsed = 0;
    m_usedSets.clear();
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(m_cmd, &bi) != VK_SUCCESS) return false;
    m_recording = true;
    ++m_bufferSerial;
    m_timestampsUsed = 0;
    if (m_tickNs > 0) vkCmdResetQueryPool(m_cmd, m_timestamps, 0, kTimestamps);
    return true;
}

void VkEngine::harvestTimings() {
    for (const Timing &t : m_timingsInFlight) {
        uint64_t ticks[2];
        if (vkGetQueryPoolResults(m_device, m_timestamps, t.first, 2, sizeof ticks, ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS || ticks[1] < ticks[0])
            continue;
        const double ms = double(ticks[1] - ticks[0]) * m_tickNs / 1e6;
        const auto it = m_filterMs.find(t.source);
        if (it == m_filterMs.end()) m_filterMs[t.source] = ms;
        else it->second += (ms - it->second) * 0.1;
    }
    m_timingsInFlight.clear();
}

bool VkEngine::submitRecorded() {
    m_recording = false;
    // A command buffer that would not end holds nothing that may run.
    if (vkEndCommandBuffer(m_cmd) != VK_SUCCESS) fail(VK_ERROR_DEVICE_LOST);
    m_timingsInFlight = std::move(m_timings);
    m_timings.clear();
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &m_cmd;
    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    if (!m_waitSems.empty() || !m_signalSems.empty()) {
        timeline.waitSemaphoreValueCount = static_cast<uint32_t>(m_waitValues.size());
        timeline.pWaitSemaphoreValues = m_waitValues.data();
        timeline.signalSemaphoreValueCount = static_cast<uint32_t>(m_signalValues.size());
        timeline.pSignalSemaphoreValues = m_signalValues.data();
        si.pNext = &timeline;
        si.waitSemaphoreCount = static_cast<uint32_t>(m_waitSems.size());
        si.pWaitSemaphores = m_waitSems.data();
        si.pWaitDstStageMask = m_waitStages.data();
        si.signalSemaphoreCount = static_cast<uint32_t>(m_signalSems.size());
        si.pSignalSemaphores = m_signalSems.data();
    }
    // Lost while recording: nothing it recorded may run (and the fence may
    // still be waiting on work that never finished: left alone).
    VkResult r = VK_ERROR_DEVICE_LOST;
    if (!m_lost) {
        vkResetFences(m_device, 1, &m_fence);
        lockQueue();
        r = vkQueueSubmit(m_queue, 1, &si, m_fence);
        unlockQueue();
    }
    if (r != VK_SUCCESS) {
        // The encoders wait for these values: given from here, so none waits
        // for ever on pictures that were never drawn (they get what was there).
        for (size_t i = 0; i < m_signalSems.size(); ++i) {
            VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
            signal.semaphore = m_signalSems[i];
            signal.value = m_signalValues[i];
            vkSignalSemaphore(m_device, &signal);
        }
    }
    m_waitSems.clear(); m_waitValues.clear(); m_waitStages.clear();
    m_signalSems.clear(); m_signalValues.clear();
    // Any failure here leaves the engine in no state to go on (out of memory
    // as much as a lost device): taken as lost, the session makes a new one.
    if (r != VK_SUCCESS) { fail(VK_ERROR_DEVICE_LOST); return false; }
    return true;
}

// A frame may be slow (a large picture through the people-finding network on
// a small GPU): waited for in turns, half a minute in all, before the device
// is taken as gone.
bool VkEngine::waitForWork() {
    const Probed timed("gpu.wait");
    VkResult r = VK_TIMEOUT;
    for (int turn = 0; turn < 6 && r == VK_TIMEOUT; ++turn)
        r = vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, 5'000'000'000ull);  // 5 s
    if (r == VK_SUCCESS) return true;
    fail(VK_ERROR_DEVICE_LOST);
    return false;
}

void VkEngine::flush() {
    settle();
    if (!m_recording) return;
    if (!submitRecorded()) return;
    if (waitForWork()) harvestTimings();
    for (Readback &rb : m_readbacks) if (rb.id < 0) rb.id = 0;
}

void VkEngine::submit() {
    if (!m_recording) return;
    if (submitRecorded()) m_submitted = true;
}

void VkEngine::settle() {
    if (!m_submitted) return;
    m_submitted = false;
    if (waitForWork()) harvestTimings();
    for (Readback &rb : m_readbacks) if (rb.id < 0) rb.id = 0;
}

void VkEngine::imageBarrier(Image &img, VkImageLayout from, VkImageLayout to, bool acquireForeign) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                      VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = acquireForeign && m_hasForeignQueue ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = acquireForeign && m_hasForeignQueue ? m_family : VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
    img.initialised = true;
}

void VkEngine::memoryBarrier() {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                      VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                         &b, 0, nullptr, 0, nullptr);
}

void VkEngine::hostReadBarrier() {
    // The copy made visible to the host's reads once the fence is waited on.
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
}

VkDescriptorPool VkEngine::makeDescriptorPool() {
    VkDescriptorPoolSize sizes[3] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 512},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 512},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 512},
    };
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 512;
    dpi.poolSizeCount = 3;
    dpi.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    return vkCreateDescriptorPool(m_device, &dpi, nullptr, &pool) == VK_SUCCESS ? pool : VK_NULL_HANDLE;
}

// A set for this frame's work, from the pools reset at each begin().
VkDescriptorSet VkEngine::allocateSet(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = m_descPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    // The first pool full: the next, made if need be.
    while (vkAllocateDescriptorSets(m_device, &ai, &set) != VK_SUCCESS) {
        if (m_extraUsed == m_extraPools.size()) {
            const VkDescriptorPool more = makeDescriptorPool();
            // Out of memory: what is recorded now is never submitted (see
            // submitRecorded), and the session makes the engine again.
            if (!more) { fail(VK_ERROR_DEVICE_LOST); return VK_NULL_HANDLE; }
            m_extraPools.push_back(more);
        }
        ai.descriptorPool = m_extraPools[m_extraUsed++];
    }
    return set;
}

VkDescriptorSet VkEngine::descriptorSet(const Pipeline &p, VkImageView sampled, VkImageView storage,
                                        VkBuffer buffer, VkDeviceSize bufferSize, VkImageView sampled2) {
    const VkDescriptorSet set = allocateSet(p.layout);
    if (!set) return VK_NULL_HANDLE;

    VkDescriptorImageInfo sampledInfo{m_linear, sampled, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo storageInfo{VK_NULL_HANDLE, storage, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bufferInfo{buffer, 0, bufferSize};
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &sampledInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    if (storage) {
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &storageInfo;
    } else {
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &bufferInfo;
    }
    VkWriteDescriptorSet all[3] = {writes[0], writes[1], {}};
    VkDescriptorImageInfo secondInfo{m_linear, sampled2, VK_IMAGE_LAYOUT_GENERAL};
    if (sampled2) {
        all[2] = writes[0];
        all[2].dstBinding = 2;
        all[2].pImageInfo = &secondInfo;
    }
    vkUpdateDescriptorSets(m_device, sampled2 ? 3 : 2, all, 0, nullptr);
    return set;
}

void VkEngine::setSourceDeinterlace(int id, DeinterlaceMode mode) {
    const auto it = m_sources.find(id);
    if (it == m_sources.end() || it->second.deinterlace == mode) return;
    it->second.deinterlace = mode;
    resetSourceHistory(id);
}
void VkEngine::resetSourceHistory(int id) {
    const auto it = m_sources.find(id); if (it == m_sources.end()) return;
    auto &s = it->second;
    s.previousValid = false; s.resetNext = true; s.fieldReady = false;
    s.historyValid = false; s.pendingProcess = true;
}
void VkEngine::beginFrame(Source &s, int w, int h) {
    s.previousValid = s.deinterlace != DeinterlaceMode::Off && s.hasFrame && !s.resetNext && s.work.width == w && s.work.height == h;
    if (s.deinterlace != DeinterlaceMode::Off) std::swap(s.work, s.previous);
    s.resetNext = false; s.fieldReady = false;
}
void VkEngine::finishFrame(Source &s) {
    s.hasFrame = true; s.pendingProcess = true; ++s.generation;
    if (s.deinterlace == DeinterlaceMode::Off) { recordDenoise(s); s.pendingProcess = false; }
}
void VkEngine::renderSourceField(int id, int field, bool firstInFrame) {
    const auto it = m_sources.find(id); if (it == m_sources.end() || m_lost) return;
    auto &s = it->second; if (!s.hasFrame) return;
    field = field == 1 ? 1 : 0;
    if (s.deinterlace == DeinterlaceMode::Off) {
        if (!s.pendingProcess) return;
        recordDenoise(s);
    } else {
        if (s.fieldReady && s.field == field && s.firstField == firstInFrame && !s.pendingProcess) return;
        if (!recordDeinterlace(s, field, firstInFrame)) return;
        recordDenoise(s);
    }
    s.pendingProcess = false; ++s.generation;
}
bool VkEngine::recordDeinterlace(Source &s, int field, bool first) {
    auto &out = s.fieldFrame;
    if (out.width != s.work.width || out.height != s.work.height) {
        flush(); destroyImage(out);
        if (!createImage(out,s.work.width,s.work.height,kWorkFormat,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) return false;
    }
    if (!begin()) return false;
    if (!out.initialised) imageBarrier(out,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);
    const auto &history = s.previousValid ? s.previous : s.work;
    const auto set = descriptorSet(m_deinterlace,s.work.view,out.view,VK_NULL_HANDLE,0,history.view);
    if (!set) return false;
    const DeinterlacePush push{s.work.width,s.work.height,int(s.deinterlace),field,int(first),int(s.previousValid)};
    vkCmdBindPipeline(m_cmd,VK_PIPELINE_BIND_POINT_COMPUTE,m_deinterlace.pipeline);
    vkCmdBindDescriptorSets(m_cmd,VK_PIPELINE_BIND_POINT_COMPUTE,m_deinterlace.pipelineLayout,0,1,&set,0,nullptr);
    vkCmdPushConstants(m_cmd,m_deinterlace.pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof push,&push);
    vkCmdDispatch(m_cmd,groups(s.work.width,16),groups(s.work.height,16),1);
    memoryBarrier();
    s.fieldReady = true; s.field = field; s.firstField = first;
    return true;
}

void VkEngine::recordConvert(Source &s, const Image &from, bool forceOpaque, bool premultiplied, bool limitedRange, bool linearAlpha, bool yInvert, BufferTransform transform, bool encoded) {
    const int width=swapsAxes(transform) ? from.height : from.width;
    const int height=swapsAxes(transform) ? from.width : from.height;
    beginFrame(s, width, height);
    if (s.work.width != width || s.work.height != height) {
        // Resizing while recording is fine: the old image is only destroyed
        // after this flush completes (we flush first).
        flush();
        destroyImage(s.work);
        // Out of memory for it: the source shows nothing rather than an image that is not there.
        if (!createImage(s.work, width, height, kWorkFormat,
                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
            s.hasFrame = false;
            return;
        }
    }
    // Lost in that wait, or with no set to be had: nothing more is recorded.
    if (!begin()) { s.hasFrame = false; return; }
    if (!s.work.initialised) imageBarrier(s.work, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    const VkDescriptorSet set = descriptorSet(m_convert, from.view, s.work.view, VK_NULL_HANDLE, 0);
    if (!set) { s.hasFrame = false; return; }
    const ConvertPush push{width, height, (forceOpaque ? 1 : 0) | (premultiplied ? 2 : 0) | (limitedRange ? 4 : 0) | (linearAlpha ? 0 : 8) | (yInvert ? 16 : 0) | (int(transform) << 5) | (encoded ? 256 : 0)};
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_convert.pipeline);
    vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_convert.pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(m_cmd, m_convert.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(m_cmd, groups(width, 16), groups(height, 16), 1);
    memoryBarrier();
    finishFrame(s);
}

void VkEngine::recordDenoise(Source &s) {
    if (s.denoise <= 0.f) {
        s.historyValid = false;
        return;
    }
    if (!begin()) return;
    const auto &cur = s.processed();
    const int next = 1 - s.cleanIndex;
    for (Image &img : s.clean) {
        if (img.width == cur.width && img.height == cur.height) continue;
        flush();
        destroyImage(img);
        s.historyValid = false;
        if (!createImage(img, cur.width, cur.height, kWorkFormat,
                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
            return;  // shown without noise reduction
    }
    if (!begin()) return;
    for (Image &img : s.clean)
        if (!img.initialised) imageBarrier(img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    const DenoiseParams dp = denoiseParams(s.denoise);
    const DenoisePush push{cur.width, cur.height, dp.sigma, dp.threshold, dp.keepMin, s.historyValid ? 0 : 1};
    const VkDescriptorSet set =
        descriptorSet(m_denoise, cur.view, s.clean[next].view, VK_NULL_HANDLE, 0, s.clean[s.cleanIndex].view);
    if (!set) return;
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_denoise.pipeline);
    vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_denoise.pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(m_cmd, m_denoise.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(m_cmd, groups(static_cast<uint32_t>(cur.width), 16), groups(static_cast<uint32_t>(cur.height), 16), 1);
    memoryBarrier();
    s.cleanIndex = next;
    s.historyValid = true;
}

// ------------------------------------------------------------- public ops

// A staging buffer of at least `size` that no unfinished work reads; null
// without memory for one. The caller stamps it with the command buffer it
// records the copy into.
VkEngine::Staging *VkEngine::stagingFor(VkDeviceSize size) {
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    Staging *free = nullptr;
    for (Staging &st : m_stagings) {
        if (stagingBusy(st)) continue;
        if (st.buffer.size >= size) return &st;
        if (!free) free = &st;
    }
    if (!free && m_stagings.size() < kStagings) free = &m_stagings.emplace_back();
    if (!free) {
        // All in use by this frame's earlier uploads: they finish first.
        flush();
        free = &m_stagings.front();
        for (Staging &st : m_stagings)
            if (st.buffer.size >= size) { free = &st; break; }
    }
    return ensureBuffer(free->buffer, size, usage, true) ? free : nullptr;
}

namespace {
// Rows of a picture copied into a staging buffer. Every track of a timeline
// sends its whole picture up on every frame, whatever size the frame is drawn
// at, and for a UHD picture this copy was the largest part of what a track
// cost: so a large one is copied by rows on several threads (as the
// read-backs are), and rows that lie end to end go as one block.
void copyRowsInto(uint8_t *to, size_t toStride, const uint8_t *from, size_t fromStride, int count, size_t bytes) {
    const auto block = [&](int first, int last) {
        if (toStride == fromStride && bytes == toStride) {
            std::memcpy(to + size_t(first) * toStride, from + size_t(first) * fromStride, size_t(last - first) * bytes);
            return;
        }
        for (int r = first; r < last; ++r) std::memcpy(to + size_t(r) * toStride, from + size_t(r) * fromStride, bytes);
    };
    if (count <= 0) return;
    const Probed timed("upload.copy");
    if (bytes * size_t(count) < (size_t(1) << 22)) block(0, count);
    else parallelRows(0, count, block, 64);
}
}

bool VkEngine::updateSource(int id, const CpuFrame &f) {
    auto it = m_sources.find(id);
    if (it == m_sources.end() || !f.data || f.width <= 0 || f.height <= 0 || m_lost) return false;
    Source &s = it->second;
    const FormatInfo fi = formatForPixel(f.format);

    if (s.upload.width != f.width || s.upload.height != f.height || s.upload.format != fi.vk) {
        flush();  // what is recorded may still read the picture that goes
        destroyImage(s.upload);
        if (!createImage(s.upload, f.width, f.height, fi.vk,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
            return false;
    }
    const VkDeviceSize packed = static_cast<VkDeviceSize>(f.width) * f.height * pixelBytes(f.format);
    Staging *const staging = stagingFor(packed);
    if (!staging) return false;
    uint8_t *dst = static_cast<uint8_t *>(staging->buffer.mapped);
    const size_t rowBytes = static_cast<size_t>(f.width) * pixelBytes(f.format);
    copyRowsInto(dst, rowBytes, f.data, static_cast<size_t>(f.stride), f.height, rowBytes);

    if (!begin()) return false;
    staging->serial = m_bufferSerial;
    imageBarrier(s.upload, s.upload.initialised ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {static_cast<uint32_t>(f.width), static_cast<uint32_t>(f.height), 1};
    vkCmdCopyBufferToImage(m_cmd, staging->buffer.buffer, s.upload.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    imageBarrier(s.upload, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    recordConvert(s, s.upload, !fi.alpha, f.premultiplied, f.limitedRange, f.linearAlpha, false, BufferTransform::Normal, f.encoded);
    // Sent now, not waited for: the GPU takes the picture in while the caller
    // decodes the next clip's (whatever is recorded next waits for it first).
    submit();
    return true;
}

bool VkEngine::updateSource(int id, const YuvInput &f) {
    auto it = m_sources.find(id);
    if (it == m_sources.end() || !f.plane[0] || f.width <= 0 || f.height <= 0 || m_lost) return false;
    if (f.layout != YuvLayout::YUY2 &&
        (!f.plane[1] || (f.layout != YuvLayout::NV12 && !f.plane[2])))
        return false;
    if (f.depth < 8 || f.depth > 16) return false;
    Source &s = it->second;
    const YuvRows rows = yuvRows(f);
    const int sb = sampleBytes(f), cw = (f.width + 1) / 2 * sb, ch = (f.height + 1) / 2, fw = f.width * sb;
    // Its planes one below the other make a picture taller (or, above 8 bits,
    // wider) than this device makes any: the CPU twin reads it in instead.
    if (rows.rowBytes > m_maxSide || rows.rows > m_maxSide) return updateSourceByTwin(id, f);

    if (s.upload.width != rows.rowBytes || s.upload.height != rows.rows || s.upload.format != VK_FORMAT_R8_UNORM) {
        flush();  // what is recorded may still read the picture that goes
        destroyImage(s.upload);
        if (!createImage(s.upload, rows.rowBytes, rows.rows, VK_FORMAT_R8_UNORM,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
            return false;
    }
    const VkDeviceSize packed = static_cast<VkDeviceSize>(rows.rowBytes) * rows.rows;
    Staging *const staging = stagingFor(packed);
    if (!staging) return false;
    uint8_t *dst = static_cast<uint8_t *>(staging->buffer.mapped);
    auto copyRows = [&](int firstRow, const uint8_t *src, int stride, int count, int bytes) {
        copyRowsInto(dst + static_cast<size_t>(firstRow) * rows.rowBytes, static_cast<size_t>(rows.rowBytes), src,
                     static_cast<size_t>(stride), count, static_cast<size_t>(bytes));
    };
    switch (f.layout) {
    case YuvLayout::NV12:
        copyRows(0, f.plane[0], f.stride[0], f.height, fw);
        copyRows(f.height, f.plane[1], f.stride[1], ch, cw * 2);
        break;
    case YuvLayout::I420:
        copyRows(0, f.plane[0], f.stride[0], f.height, fw);
        copyRows(f.height, f.plane[1], f.stride[1], ch, cw);
        copyRows(f.height + ch, f.plane[2], f.stride[2], ch, cw);
        break;
    case YuvLayout::I422:
    case YuvLayout::I444:
        copyRows(0, f.plane[0], f.stride[0], f.height, fw);
        copyRows(f.height, f.plane[1], f.stride[1], f.height, f.layout == YuvLayout::I444 ? fw : cw);
        copyRows(f.height * 2, f.plane[2], f.stride[2], f.height, f.layout == YuvLayout::I444 ? fw : cw);
        break;
    case YuvLayout::YUY2:
        copyRows(0, f.plane[0], f.stride[0], f.height, cw * 4);
        break;
    }
    const int alphaRow = f.alpha ? rows.rows - f.height : -1;
    if (f.alpha) copyRows(alphaRow, f.alpha, f.alphaStride, f.height, fw);
    if (!begin()) return false;
    staging->serial = m_bufferSerial;
    const bool converted = convertPacked(s, f, staging->buffer.buffer, alphaRow);
    submit();  // as a memory frame's upload: taken in while the caller decodes the next
    return converted;
}

// The CPU twin's reading of the picture (the same light, by the parity the
// two are held to), uploaded as the working floats it is. Slow, and only for
// pictures no packed upload fits this device: better than no picture.
bool VkEngine::updateSourceByTwin(int id, const YuvInput &f) {
    const auto it = m_sources.find(id);
    if (it == m_sources.end()) return false;
    if (!m_twin) {
        m_twin = createCpuEngine();
        m_twinSource = m_twin->createSource();
    }
    m_twin->setHdrLevels(m_levels);
    YuvInput in = f;
    // Chroma rows by field for a source being deinterlaced, as convertPacked reads them.
    in.interlaced = f.interlaced || it->second.deinterlace != DeinterlaceMode::Off;
    if (!m_twin->updateSource(m_twinSource, in) || !m_twin->readLinear(m_twinSource, m_twinFrame)) return false;
    CpuFrame floats;
    floats.width = m_twinFrame.width;
    floats.height = m_twinFrame.height;
    floats.stride = m_twinFrame.width * 16;
    floats.format = PixelFormat::RGBAF32;
    floats.data = reinterpret_cast<const uint8_t *>(m_twinFrame.rgba.data());
    floats.encoded = true;  // working light already: the values as they are
    return updateSource(id, floats);
}

bool VkEngine::updateSource(int id, const AVFrame *frame, const YuvInput &f) {
    auto it = m_sources.find(id);
    if (it == m_sources.end() || !frame || frame->format != AV_PIX_FMT_VULKAN || !frame->hw_frames_ctx || !m_ffDevice || m_lost)
        return false;
    auto *fc = reinterpret_cast<AVHWFramesContext *>(frame->hw_frames_ctx->data);
    if (fc->device_ref->data != m_ffDevice->data || f.alpha || f.layout == YuvLayout::YUY2 || f.depth < 8 || f.depth > 16) return false;
    Source &s = it->second;
    const YuvRows rows = yuvRows(f);
    const int b = sampleBytes(f), cw = (f.width + 1) / 2, ch = (f.height + 1) / 2;
    // The planes as the packed layout has them: first row, size in texels,
    // bytes a texel.
    struct Plane { int row, w, h, texel; };
    std::vector<Plane> planes;
    switch (f.layout) {
    case YuvLayout::NV12: planes = {{0, f.width, f.height, b}, {f.height, cw, ch, 2 * b}}; break;
    case YuvLayout::I420: planes = {{0, f.width, f.height, b}, {f.height, cw, ch, b}, {f.height + ch, cw, ch, b}}; break;
    case YuvLayout::I422: planes = {{0, f.width, f.height, b}, {f.height, cw, f.height, b}, {2 * f.height, cw, f.height, b}}; break;
    case YuvLayout::I444: planes = {{0, f.width, f.height, b}, {f.height, f.width, f.height, b}, {2 * f.height, f.width, f.height, b}}; break;
    case YuvLayout::YUY2: return false;
    }
    if (av_pix_fmt_count_planes(fc->sw_format) != int(planes.size())) return false;
    for (const Plane &p : planes)
        if (rows.rowBytes % p.texel || (p.row * rows.rowBytes) % 4) return false;
    auto *vkfc = static_cast<AVVulkanFramesContext *>(fc->hwctx);
    auto *vkf = reinterpret_cast<AVVkFrame *>(frame->data[0]);
    int images = 0;
    while (images < AV_NUM_DATA_POINTERS && vkf->img[images]) ++images;
    const bool multiplane = images == 1 && planes.size() > 1;
    if (!multiplane && images != int(planes.size())) return false;
    if (rows.rowBytes > m_maxSide || rows.rows > m_maxSide) return false;  // too large packed: downloaded, and read in by the twin

    flush();  // the packing buffer may still be read by earlier work
    if (s.upload.width != rows.rowBytes || s.upload.height != rows.rows || s.upload.format != VK_FORMAT_R8_UNORM) {
        destroyImage(s.upload);
        if (!createImage(s.upload, rows.rowBytes, rows.rows, VK_FORMAT_R8_UNORM,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
            return false;
    }
    const VkDeviceSize packed = static_cast<VkDeviceSize>(rows.rowBytes) * rows.rows;
    if (!ensureBuffer(m_gpuPacked, packed, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false) || !begin())
        return false;
    // The decoder's planes into the packed layout, on the GPU; the decoder
    // waits on the frame's semaphore before it writes it again.
    vkfc->lock_frame(fc, vkf);
    for (int i = 0; i < images; ++i) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = vkf->layout[i];
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = vkf->img[i];
        // The whole image, as writeVulkanFrames moves an encoder's: the colour
        // aspect is right for one image of several planes whether or not its
        // planes have memory of their own; only the copies below name planes.
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &barrier);
    }
    for (size_t p = 0; p < planes.size(); ++p) {
        VkBufferImageCopy region{};
        region.bufferOffset = static_cast<VkDeviceSize>(planes[p].row) * rows.rowBytes;
        region.bufferRowLength = static_cast<uint32_t>(rows.rowBytes / planes[p].texel);
        region.imageSubresource.aspectMask =
            multiplane ? static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_PLANE_0_BIT << p) : VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {static_cast<uint32_t>(planes[p].w), static_cast<uint32_t>(planes[p].h), 1};
        vkCmdCopyImageToBuffer(m_cmd, vkf->img[multiplane ? 0 : p], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_gpuPacked.buffer, 1, &region);
    }
    for (int i = 0; i < images; ++i) {
        vkf->layout[i] = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkf->access[i] = VK_ACCESS_2_TRANSFER_READ_BIT;
        m_waitSems.push_back(vkf->sem[i]);
        m_waitValues.push_back(vkf->sem_value[i]);
        m_waitStages.push_back(VK_PIPELINE_STAGE_TRANSFER_BIT);
        m_signalSems.push_back(vkf->sem[i]);
        m_signalValues.push_back(++vkf->sem_value[i]);
    }
    vkfc->unlock_frame(fc, vkf);
    VkBufferMemoryBarrier written{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    written.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    written.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    written.srcQueueFamilyIndex = written.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    written.buffer = m_gpuPacked.buffer;
    written.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &written, 0, nullptr);
    const bool converted = convertPacked(s, f, m_gpuPacked.buffer, -1);
    // Sent now, its semaphore signal with it: the decoder reads this frame
    // again as a reference, waiting for that signal, before the engine's
    // next submission might come.
    submit();
    return converted;
}

bool VkEngine::convertPacked(Source &s, const YuvInput &f, VkBuffer packedRows, int alphaRow) {
    const YuvRows rows = yuvRows(f);
    if (!begin()) return false;
    imageBarrier(s.upload, s.upload.initialised ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {static_cast<uint32_t>(rows.rowBytes), static_cast<uint32_t>(rows.rows), 1};
    vkCmdCopyBufferToImage(m_cmd, packedRows, s.upload.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    imageBarrier(s.upload, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);

    // (Reduced, the working picture is that much smaller: every step after
    // this one works on the pixels that are drawn and no more.)
    const int reduce = yuvReduce(f, s.deinterlace != DeinterlaceMode::Off);
    const int workW = (f.width + reduce - 1) / reduce, workH = (f.height + reduce - 1) / reduce;
    beginFrame(s, workW, workH);
    if (s.work.width != workW || s.work.height != workH) {
        flush();
        destroyImage(s.work);
        if (!createImage(s.work, workW, workH, kWorkFormat,
                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
            s.hasFrame = false;
            return false;
        }
        if (!begin()) return false;
    }
    if (!s.work.initialised) imageBarrier(s.work, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    float kr, kb;
    yuvCoefficients(f.matrix, &kr, &kb);
    const VkDescriptorSet set = descriptorSet(m_yuvIn, s.upload.view, s.work.view, VK_NULL_HANDLE, 0);
    if (!set) { s.hasFrame = false; return false; }
    const InputParams light = inputParams(f, m_levels);
    YuvInPush push{f.width, f.height, static_cast<int32_t>(f.layout), f.fullRange ? 1 : 0, kr, kb,
        (f.interlaced || s.deinterlace != DeinterlaceMode::Off) ? 1 : 0, alphaRow, f.premultiplied ? 1 : 0, f.linearAlpha ? 1 : 0,
        f.depth, f.depth > 8 && f.msb ? 16 - f.depth : 0, light.kind, light.wide, light.scale, light.gamma1, {},
        f.chromaLeft ? 1 : 0, reduce};
    std::copy_n(light.gamut, 9, push.gamut);
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_yuvIn.pipeline);
    vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_yuvIn.pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(m_cmd, m_yuvIn.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(m_cmd, groups(static_cast<uint32_t>(workW), 16), groups(static_cast<uint32_t>(workH), 16), 1);
    memoryBarrier();
    finishFrame(s);
    return true;
}

bool VkEngine::updateSource(int id, const DmaBufFrame &f) {
    auto it = m_sources.find(id);
    if (it == m_sources.end() || m_lost || !m_hasDmaBuf || uint32_t(f.transform)>7) return false;
    Source &s = it->second;
    const FormatInfo fi = formatForFourcc(f.fourcc);
    if (fi.vk == VK_FORMAT_UNDEFINED) return false;

    Image *img = nullptr;
    auto cached = f.token ? s.imports.find(f.token) : s.imports.end();
    if (cached != s.imports.end() && cached->second.width == f.width && cached->second.height == f.height &&
        cached->second.format == fi.vk) {
        img = &cached->second;
    } else {
        if (cached != s.imports.end()) {
            flush();
            destroyImage(cached->second);
            s.imports.erase(cached);
        }
        // Resize/reconnect must not grow the import cache indefinitely.
        if (s.imports.size() >= 8) {
            flush();
            for (auto &entry : s.imports) destroyImage(entry.second);
            s.imports.clear();
        }
        Image fresh;
        if (!importDmaBuf(fresh, f, fi)) return false;
        if (!f.token) {
            // No stable identity: import, use once, throw away after the flush.
            if (!begin()) { destroyImage(fresh); return false; }
            imageBarrier(fresh, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, true);
            recordConvert(s, fresh, !fi.alpha, f.premultiplied, false, true, f.yInvert, f.transform);
            flush();
            destroyImage(fresh);
            return !m_lost;
        }
        fresh.owner = f.owner;
        img = &s.imports.emplace(f.token, fresh).first->second;
    }
    if (!begin()) return false;
    // The compositor wrote this buffer since we last read it: acquire it again.
    imageBarrier(*img, img->initialised ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_GENERAL, true);
    recordConvert(s, *img, !fi.alpha, f.premultiplied, false, true, f.yInvert, f.transform);
    return true;
}

std::vector<uint64_t> VkEngine::importModifiers(uint32_t fourcc) const {
    std::vector<uint64_t> out;
    if (!m_hasDmaBuf) return out;
    const FormatInfo fi = formatForFourcc(fourcc);
    if (fi.vk == VK_FORMAT_UNDEFINED) return out;

    VkDrmFormatModifierPropertiesListEXT list{VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkFormatProperties2 props{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
    props.pNext = &list;
    vkGetPhysicalDeviceFormatProperties2(m_physical, fi.vk, &props);
    std::vector<VkDrmFormatModifierPropertiesEXT> mods(list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = mods.data();
    vkGetPhysicalDeviceFormatProperties2(m_physical, fi.vk, &props);

    for (const auto &m : mods) {
        if (m.drmFormatModifierPlaneCount != 1) continue;
        if (!(m.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) continue;

        VkPhysicalDeviceExternalImageFormatInfo extInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        extInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT modInfo{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
        modInfo.pNext = &extInfo;
        modInfo.drmFormatModifier = m.drmFormatModifier;
        modInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        info.pNext = &modInfo;
        info.format = fi.vk;
        info.type = VK_IMAGE_TYPE_2D;
        info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        VkExternalImageFormatProperties extProps{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 outProps{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        outProps.pNext = &extProps;
        if (vkGetPhysicalDeviceImageFormatProperties2(m_physical, &info, &outProps) != VK_SUCCESS) continue;
        if (!(extProps.externalMemoryProperties.externalMemoryFeatures &
              VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
            continue;
        out.push_back(m.drmFormatModifier);
    }
    return out;
}

void VkEngine::composite(const std::vector<Layer> &layers, const Color &bg) { compositeTo(m_canvas, -1, layers, bg); }

void VkEngine::compositeInto(int target, int width, int height, const std::vector<Layer> &layers, const Color &bg) {
    auto it = m_sources.find(target);
    if (it == m_sources.end() || width <= 0 || height <= 0 || m_lost) return;
    Source &s = it->second;
    if (s.work.width != width || s.work.height != height || !s.work.clearable) {
        flush();
        destroyImage(s.work);
        if (!createImage(s.work, width, height, kWorkFormat,
                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            s.hasFrame = false;
            return;
        }
        s.work.clearable = true;
    }
    s.denoise = 0.f;  // a drawn studio is not noisy
    compositeTo(s.work, target, layers, bg);
    s.hasFrame = true; ++s.generation;
}

void VkEngine::transitionInto(int target, int width, int height, int from, int to, const TransitionStyle &style, float progress) {
    if (target == from || target == to || width <= 0 || height <= 0 || m_lost) return;
    auto dst = m_sources.find(target), a = m_sources.find(from), b = m_sources.find(to);
    if (dst == m_sources.end() || a == m_sources.end() || b == m_sources.end() || !a->second.hasFrame || !b->second.hasFrame) return;
    Source &s = dst->second;
    if (s.work.width != width || s.work.height != height) {
        flush(); destroyImage(s.work);
        if (!createImage(s.work, width, height, kWorkFormat,
                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            // No memory for it: the source shows nothing rather than a picture that is not there.
            s.hasFrame = false;
            s.filtered = false;
            return;
        }
        s.work.clearable = true;
    }
    // An end with no picture to give (it could not be made) mixes nothing.
    if (!a->second.shown().view || !b->second.shown().view || !begin()) return;
    imageBarrier(s.work, s.work.initialised ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    const auto set = descriptorSet(m_transition, a->second.shown().view, s.work.view, VK_NULL_HANDLE, 0, b->second.shown().view);
    if (!set) return;
    const TransitionPush push{width, height, int32_t(style.type), std::clamp(progress, 0.f, 1.f),
                              int32_t(style.side), int32_t(style.mask), style.invert ? 1 : 0, style.softness,
                              style.colourPoint, {0, 0, 0}, {style.colour.r, style.colour.g, style.colour.b, style.colour.a},
                              {style.matteRect[0], style.matteRect[1], style.matteRect[2], style.matteRect[3]}};
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_transition.pipeline);
    vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_transition.pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(m_cmd, m_transition.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(m_cmd, (width + 15) / 16, (height + 15) / 16, 1);
    imageBarrier(s.work, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    s.hasFrame = true; s.denoise = 0.f; ++s.generation;
}

void VkEngine::compositeTo(Image &into, int skip, const std::vector<Layer> &layers, const Color &bg) {
    if (!into.image || !begin()) return;
    clearTo(into, bg);
    for (const Layer &l : layers) {
        auto it = m_sources.find(l.source);
        if (l.source == skip || it == m_sources.end() || !it->second.hasFrame || !it->second.shown().view || l.w <= 0 || l.h <= 0) continue;
        const VideoFilter *wrap = it->second.filtered ? lightWrapOf(it->second.filters) : nullptr;
        if (!wrap || !wrapLayer(into, it->second.shown(), l, *wrap)) drawLayer(into, it->second.shown(), l);
    }
}

// A layer with light wrap (lightWrapParams in frame.h): in its region, drawn
// alone on the picture's grid (unless it is its own picture, whole), the
// light under it brought on, then drawn in as it would have been. False,
// with nothing drawn, when there is no memory for its pictures.
bool VkEngine::wrapLayer(Image &into, const Image &shown, const Layer &l, const VideoFilter &f) {
    const PixelBounds b = lightWrapRegion(f, l, into.width, into.height);
    if (b.empty()) return true;
    const int scale = lightWrapScale(f, into.height);
    const int bw = b.x1 - b.x0, bh = b.y1 - b.y0, hw = (bw + scale - 1) / scale, hh = (bh + scale - 1) / scale;
    // The set for this scale. A layer that moves or grows changes its
    // region's size every frame, so the pictures are not the region's size:
    // they are made in steps of 256 (within what is drawn into) and only
    // ever grow, and each pass works in their corner as in a picture of the
    // region's size. Playing such a layer then makes no pictures.
    auto it = std::find_if(m_wraps.begin(), m_wraps.end(), [&](const Wrap &w) { return w.scale == scale; });
    if (it == m_wraps.end()) {
        m_wraps.push_back({scale, {}});
        it = m_wraps.end() - 1;
    }
    const int roomW = std::min(into.width, int(align(uint32_t(bw), 256))), roomH = std::min(into.height, int(align(uint32_t(bh), 256)));
    const auto room = [&](Image &img, int w, int h) {
        return (img.image && img.width >= w && img.height >= h) || filterImage(img, std::max(img.width, w), std::max(img.height, h));
    };
    const bool whole = layerIsWhole(l, shown.width, shown.height, into.width, into.height);
    if ((!whole && !room(it->images[0], roomW, roomH)) || !room(it->images[1], roomW, roomH) ||
        !room(it->images[2], (roomW + scale - 1) / scale, (roomH + scale - 1) / scale) ||
        !room(it->images[3], (roomW + scale - 1) / scale, (roomH + scale - 1) / scale) || !begin())
        return false;
    const auto corner = [](const Image &img, int w, int h) {
        Image part = img;
        part.width = w;
        part.height = h;
        return part;
    };
    Image drawn = corner(it->images[0], bw, bh), lit = corner(it->images[1], bw, bh), light = corner(it->images[2], hw, hh),
          across = corner(it->images[3], hw, hh);
    if (!whole) {
        clearTo(drawn, {0, 0, 0, 0});
        Layer plain = l;
        plain.x -= float(b.x0); plain.y -= float(b.y0);
        plain.blend = BlendMode::Normal;
        plain.blendSpace = BlendSpace::Linear;
        drawLayer(drawn, shown, plain);
    }
    const Image &alone = whole ? shown : drawn;
    FilterParams under = lightWrapParams(f, 0, into.height, b.x0, b.y0, hw, hh);
    // The source alone ends where the region does, not where its picture does.
    if (!whole) { under.p[1] = float(bw); under.p[2] = float(bh); }
    recordFilter(into, light, &alone, under);
    recordFilter(light, across, nullptr, lightWrapParams(f, 1, into.height, b.x0, b.y0, hw, hh));
    recordFilter(across, light, nullptr, lightWrapParams(f, 2, into.height, b.x0, b.y0, hw, hh));
    recordFilter(alone, lit, &light, lightWrapParams(f, 3, into.height, b.x0, b.y0, hw, hh));
    // The region's corner of the lit picture, drawn in where the region is.
    Layer region;
    region.x = float(b.x0); region.y = float(b.y0); region.w = float(bw); region.h = float(bh);
    region.u1 = float(bw) / float(it->images[1].width);
    region.v1 = float(bh) / float(it->images[1].height);
    region.filter = ScaleFilter::Point;
    region.blend = l.blend;
    region.blendSpace = l.blendSpace;
    drawLayer(into, it->images[1], region);
    return true;
}

void VkEngine::clearTo(Image &into, const Color &bg) {
    imageBarrier(into, into.initialised ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue clear{};
    clear.float32[0] = bg.r;
    clear.float32[1] = bg.g;
    clear.float32[2] = bg.b;
    clear.float32[3] = bg.a;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(m_cmd, into.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    imageBarrier(into, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
}

void VkEngine::drawLayer(Image &into, const Image &shown, const Layer &l) {
    // Whoever calls may have waited for the GPU since it last recorded (a
    // light wrap that found no memory for its pictures has): recording again.
    if (!begin()) return;
    {
        const PixelBounds b = layerBounds(l, into.width, into.height);
        if (b.empty()) return;
        const VkDescriptorSet set = descriptorSet(m_composite, shown.view, into.view, VK_NULL_HANDLE, 0);
        if (!set) return;
        vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_composite.pipeline);
        const float fx = std::fabs(l.u1 - l.u0) * shown.width / l.w, fy = std::fabs(l.v1 - l.v0) * shown.height / l.h;
        const CompositePush push{l.x, l.y, l.w, l.h, l.u0, l.v0, l.u1, l.v1, std::cos(l.rotation), std::sin(l.rotation),
                                 b.x0, b.y0, into.width, into.height, l.opacity,
                                 static_cast<int32_t>(layerFilter(l, shown.width, shown.height)), fx, fy,
                                 static_cast<int32_t>(l.blend), static_cast<int32_t>(l.blendSpace), 0, 0};
        vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_composite.pipelineLayout, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(m_cmd, m_composite.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
        vkCmdDispatch(m_cmd, groups(static_cast<uint32_t>(b.x1 - b.x0), 16),
                      groups(static_cast<uint32_t>(b.y1 - b.y0), 16), 1);
        memoryBarrier();
    }
}

// ------------------------------------------------------------ video filters

bool VkEngine::filterImage(Image &img, int w, int h) {
    if (img.image && img.width == w && img.height == h) return true;
    flush();
    destroyImage(img);
    if (!createImage(img, w, h, kWorkFormat,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return false;
    img.clearable = true;
    if (!begin()) return false;
    imageBarrier(img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    return true;
}

// A LUT or mask picture, rarely: flushed straight away so the staging buffer is free again.
bool VkEngine::uploadAux(Image &img, VkFormat format, int w, int h, const void *data, size_t bytes) {
    flush();
    destroyImage(img);
    if (!createImage(img, w, h, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
        !ensureBuffer(m_auxStaging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true) || !begin())
        return false;
    std::memcpy(m_auxStaging.mapped, data, bytes);
    imageBarrier(img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {uint32_t(w), uint32_t(h), 1};
    vkCmdCopyBufferToImage(m_cmd, m_auxStaging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    imageBarrier(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    flush();
    return !m_lost;
}

// ------------------------------------------------------------ grade plans

bool VkEngine::createGradePipeline(std::string *failure) {
    const QByteArray code = loadSpirv("grade.spv");
    if (code.isEmpty()) { if (failure) *failure = "shader missing: grade.spv"; return false; }
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = size_t(code.size());
    smci.pCode = reinterpret_cast<const uint32_t *>(code.constData());
    VkShaderModule module;
    if (vkCreateShaderModule(m_device, &smci, nullptr, &module) != VK_SUCCESS) { if (failure) *failure = "shader rejected: grade.spv"; return false; }
    VkDescriptorSetLayoutBinding bindings[5]{};
    for (uint32_t i = 0; i < 5; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = i == 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dlci.bindingCount = 5;
    dlci.pBindings = bindings;
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 7 * 4};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &m_grade.layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    if (vkCreateDescriptorSetLayout(m_device, &dlci, nullptr, &m_grade.layout) != VK_SUCCESS ||
        vkCreatePipelineLayout(m_device, &plci, nullptr, &m_grade.pipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, module, nullptr);
        if (failure) *failure = "no layout for grade.spv";
        return false;
    }
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = module;
    cpci.stage.pName = "main";
    cpci.layout = m_grade.pipelineLayout;
    const VkResult r = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &cpci, nullptr, &m_grade.pipeline);
    vkDestroyShaderModule(m_device, module, nullptr);
    if (r != VK_SUCCESS) { if (failure) *failure = "pipeline failed: grade.spv"; return false; }
    return true;
}

// A grade plan's operations in turn, between the step's own pictures; the
// last one's picture copied to the step's output. False when it could not
// all be drawn (no memory for a picture or a table, a program that is not
// ready): the step's output is then not the plan's picture.
bool VkEngine::runPlan(Source::Step &st, const GradePlan &plan, const Image &input) {
    const int w = input.width, h = input.height;
    // What an operation without a picture or a table is bound to, made once.
    if (!m_noPicture.image && !filterImage(m_noPicture, 1, 1)) return false;
    const float zero = 0.f;
    if (!m_noTable.image && !uploadAux(m_noTable, VK_FORMAT_R32_SFLOAT, 1, 1, &zero, sizeof zero)) return false;
    if (int(st.regs.size()) < plan.registers) st.regs.resize(size_t(plan.registers));
    for (int r = 1; r < plan.registers; ++r)
        if (!filterImage(st.regs[size_t(r)], w, h)) return false;
    // Its tables, uploaded once while the plan keeps them.
    std::vector<std::pair<std::shared_ptr<const std::vector<float>>, Image>> kept;
    auto tableFor = [&](const std::shared_ptr<const std::vector<float>> &table) -> const Image * {
        for (auto &k : kept) if (k.first == table) return &k.second;
        for (auto it = st.tables.begin(); it != st.tables.end(); ++it)
            if (it->first == table) { kept.push_back(std::move(*it)); st.tables.erase(it); return &kept.back().second; }
        Image img;
        if (!uploadAux(img, VK_FORMAT_R32_SFLOAT, int(table->size()), 1, table->data(), table->size() * sizeof(float))) return nullptr;
        kept.emplace_back(table, img);
        return &kept.back().second;
    };
    for (const GradeOp &op : plan.ops)
        if (op.table && !tableFor(op.table)) { for (auto &k : kept) destroyImage(k.second); return false; }
    // Tables the plan no longer uses go once the GPU has finished with them.
    // With none to drop nothing waits: the clip's passes stay one piece of
    // work with what was recorded before them.
    if (!st.tables.empty()) {
        flush();
        for (auto &old : st.tables) destroyImage(old.second);
    }
    st.tables = std::move(kept);
    auto picture = [&](int r) -> const Image & { return r == 0 ? input : st.regs[size_t(r)]; };
    int last = 0;
    size_t people = 0;
    for (const GradeOp &op : plan.ops) {
        if (!begin()) return false;
        Image &to = st.regs[size_t(op.target)];
        int kind = int(op.kind), has = 0;
        const Image *a = &m_noPicture, *b = &m_noPicture, *m = &m_noPicture, *table = &m_noTable;
        if (op.a >= 0) { a = &picture(op.a); has |= 1; }
        if (op.b >= 0) { b = &picture(op.b); has |= 2; }
        if (op.mask >= 0) { m = &picture(op.mask); has |= 4; }
        if (op.table) for (auto &k : st.tables) if (k.first == op.table) table = &k.second;
        if (op.kind == GradeOp::Kind::People) {
            // The network and its edges as RemoveBackground; a missing model, or a
            // device too small for the network, keys everything (has stays without a).
            if (st.people.size() <= people) { flush(); st.people.resize(people + 1); }
            Source::Step &found = st.people[people++];
            VideoFilter f;
            f.type = VideoFilterType::RemoveBackground;
            f.quality = op.channel;
            f.tighten = op.amount;
            const FilterParams q = filterParams(f, w, h, 0);
            const bool matte = recordMatte(*a, found, q, true);
            if (m_lost) return false;
            has = 0;
            if (matte) {
                if (!filterImage(found.out, w, h) || !recordGuided(*a, found, f, q) || !begin()) return false;
                a = &found.out;
                has = 1;
            }
        }
        if (op.kind == GradeOp::Kind::Program) {
            // The program into the target, then the target mixed back towards
            // its input by the amount (the mix reads and writes each texel once).
            if (!op.program) return false;
            const ProgramPipe &program = programPipe(*op.program);
            if (!program.error.empty()) return false;
            recordProgram(*a, to, program);
            b = &to;
            has |= 2;
        }
        const VkDescriptorSet set = allocateSet(m_grade.layout);
        if (!set) return false;
        VkDescriptorImageInfo infos[5] = {{VK_NULL_HANDLE, a->view, VK_IMAGE_LAYOUT_GENERAL},
                                          {VK_NULL_HANDLE, b->view, VK_IMAGE_LAYOUT_GENERAL},
                                          {VK_NULL_HANDLE, m->view, VK_IMAGE_LAYOUT_GENERAL},
                                          {m_nearest, table->view, VK_IMAGE_LAYOUT_GENERAL},
                                          {VK_NULL_HANDLE, to.view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet writes[5]{};
        for (uint32_t i = 0; i < 5; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = i == 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &infos[i];
        }
        vkUpdateDescriptorSets(m_device, 5, writes, 0, nullptr);
        struct { int32_t w, h, kind, channel; float amount; int32_t has, pad; } push{w, h, kind, op.channel, op.amount, has, 0};
        vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_grade.pipeline);
        vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_grade.pipelineLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(m_cmd, m_grade.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
        vkCmdDispatch(m_cmd, groups(uint32_t(w), 16), groups(uint32_t(h), 16), 1);
        memoryBarrier();
        last = op.target;
    }
    // The step's output: the plan's last picture.
    if (!filterImage(st.out, w, h) || !begin()) return false;
    const VkDescriptorSet set = allocateSet(m_grade.layout);
    if (!set) return false;
    const Image &result = picture(last);
    VkDescriptorImageInfo infos[5] = {{VK_NULL_HANDLE, result.view, VK_IMAGE_LAYOUT_GENERAL},
                                      {VK_NULL_HANDLE, m_noPicture.view, VK_IMAGE_LAYOUT_GENERAL},
                                      {VK_NULL_HANDLE, m_noPicture.view, VK_IMAGE_LAYOUT_GENERAL},
                                      {m_nearest, m_noTable.view, VK_IMAGE_LAYOUT_GENERAL},
                                      {VK_NULL_HANDLE, st.out.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[5]{};
    for (uint32_t i = 0; i < 5; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i == 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(m_device, 5, writes, 0, nullptr);
    struct { int32_t w, h, kind, channel; float amount; int32_t has, pad; } push{w, h, int(GradeOp::Kind::Copy), 0, 1.f, 1, 0};
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_grade.pipeline);
    vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_grade.pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(m_cmd, m_grade.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(m_cmd, groups(uint32_t(w), 16), groups(uint32_t(h), 16), 1);
    memoryBarrier();
    return true;
}

// ------------------------------------------------------------ colour programs

bool VkEngine::prepareProgram(const ColourProgram &program, std::string *error) {
    const ProgramPipe &p = programPipe(program);
    if (error) *error = p.error;
    return p.error.empty();
}

VkEngine::ProgramPipe &VkEngine::programPipe(const ColourProgram &program) {
    const auto it = m_programs.find(program.id);
    if (it != m_programs.end()) { it->second.used = ++m_programClock; return it->second; }
    // Room for it: the one longest unused goes (a chain that still names it
    // has it made again when it next runs).
    while (m_programs.size() >= kProgramsKept) {
        auto oldest = m_programs.begin();
        for (auto other = m_programs.begin(); other != m_programs.end(); ++other)
            if (other->second.used < oldest->second.used) oldest = other;
        destroyProgram(oldest->second);
        m_programs.erase(oldest);
    }
    ProgramPipe &p = m_programs[program.id];
    p.used = ++m_programClock;
    std::string why;
    if (!buildProgram(program, p, &why)) {
        destroyProgram(p);
        p.error = why.empty() ? std::string("the colour transform could not be set up") : why;
    }
    return p;
}

void VkEngine::destroyProgram(ProgramPipe &p) {
    flush();
    for (Image &img : p.textures) destroyImage(img);
    p.textures.clear();
    destroyBuffer(p.uniforms);
    destroyPipeline(p.scope);
    p.scopeTried = false;
    if (p.pool) vkDestroyDescriptorPool(m_device, p.pool, nullptr);
    if (p.resources) vkDestroyDescriptorSetLayout(m_device, p.resources, nullptr);
    if (p.pipe.pipeline) vkDestroyPipeline(m_device, p.pipe.pipeline, nullptr);
    if (p.pipe.pipelineLayout) vkDestroyPipelineLayout(m_device, p.pipe.pipelineLayout, nullptr);
    if (p.pipe.layout) vkDestroyDescriptorSetLayout(m_device, p.pipe.layout, nullptr);
    p.pool = VK_NULL_HANDLE;
    p.resources = VK_NULL_HANDLE;
    p.set = VK_NULL_HANDLE;
    p.pipe = Pipeline{};
}

// One of a program's lookup tables as a 1D, 2D or 3D float texture (RGB
// tables padded to RGBA, which every device can sample).
bool VkEngine::uploadTexture(Image &img, const ColourTexture &t) {
    const VkFormat format = t.channels == 1 ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R32G32B32A32_SFLOAT;
    const int w = std::max(1, t.width), h = t.dims >= 2 ? std::max(1, t.height) : 1, d = t.dims == 3 ? std::max(1, t.depth) : 1;
    const size_t texels = size_t(w) * h * d, in = size_t(t.channels == 1 ? 1 : 3), out = t.channels == 1 ? 1 : 4;
    if (t.values.size() < texels * in) return false;
    std::vector<float> data(texels * out, 1.f);
    for (size_t i = 0; i < texels; ++i)
        for (size_t c = 0; c < in; ++c) data[i * out + c] = t.values[i * in + c];
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = t.dims == 3 ? VK_IMAGE_TYPE_3D : t.dims == 2 ? VK_IMAGE_TYPE_2D : VK_IMAGE_TYPE_1D;
    ici.format = format;
    ici.extent = {uint32_t(w), uint32_t(h), uint32_t(d)};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_device, &ici, nullptr, &img.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(m_device, img.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    const int32_t type = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (type < 0 || (mai.memoryTypeIndex = uint32_t(type), vkAllocateMemory(m_device, &mai, nullptr, &img.memory) != VK_SUCCESS))
        return false;
    if (vkBindImageMemory(m_device, img.image, img.memory, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = t.dims == 3 ? VK_IMAGE_VIEW_TYPE_3D : t.dims == 2 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_1D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(m_device, &vci, nullptr, &img.view) != VK_SUCCESS) return false;
    img.format = format;
    img.width = w;
    img.height = h;
    const size_t bytes = data.size() * sizeof(float);
    flush();
    if (!ensureBuffer(m_auxStaging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true) || !begin()) return false;
    std::memcpy(m_auxStaging.mapped, data.data(), bytes);
    imageBarrier(img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {uint32_t(w), uint32_t(h), uint32_t(d)};
    vkCmdCopyBufferToImage(m_cmd, m_auxStaging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    imageBarrier(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    flush();
    return !m_lost;
}

bool VkEngine::buildProgram(const ColourProgram &program, ProgramPipe &p, std::string *failure) {
    if (program.function.empty() || program.shader.empty()) { *failure = "the colour transform has no GPU form"; return false; }
    // The program's function on each working pixel's RGB; alpha passes by.
    const std::string source = "#version 460\n"
        "layout(local_size_x = 16, local_size_y = 16) in;\n"
        "layout(set = 0, binding = 0, rgba32f) uniform readonly image2D sourcePicture;\n"
        "layout(set = 0, binding = 1, rgba32f) uniform writeonly image2D targetPicture;\n"
        "layout(push_constant) uniform ProgramPush { ivec2 size; } programPush;\n"
        + program.shader +
        "\nvoid main() {\n"
        "    ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
        "    if (any(greaterThanEqual(p, programPush.size))) return;\n"
        "    vec4 c = imageLoad(sourcePicture, p);\n"
        "    imageStore(targetPicture, p, vec4(" + program.function + "(vec4(c.rgb, 1.0)).rgb, c.a));\n"
        "}\n";
    std::vector<uint32_t> spirv;
    if (!compileCompute(source, spirv, failure)) return false;
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = spirv.size() * sizeof(uint32_t);
    smci.pCode = spirv.data();
    VkShaderModule module;
    if (vkCreateShaderModule(m_device, &smci, nullptr, &module) != VK_SUCCESS) { *failure = "the GPU refused the colour transform"; return false; }

    VkDescriptorSetLayoutBinding pictures[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        pictures[i].binding = i;
        pictures[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        pictures[i].descriptorCount = 1;
        pictures[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dlci.bindingCount = 2;
    dlci.pBindings = pictures;
    if (vkCreateDescriptorSetLayout(m_device, &dlci, nullptr, &p.pipe.layout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, module, nullptr);
        *failure = "no memory for the colour transform";
        return false;
    }

    // Set 1: the program's uniform block and its lookup tables.
    std::vector<VkDescriptorSetLayoutBinding> resources;
    if (!program.uniforms.empty())
        resources.push_back({0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    for (const ColourTexture &tex : program.textures)
        resources.push_back({tex.binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    VkDescriptorSetLayout layouts[2] = {p.pipe.layout, VK_NULL_HANDLE};
    if (!resources.empty()) {
        VkDescriptorSetLayoutCreateInfo rci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        rci.bindingCount = uint32_t(resources.size());
        rci.pBindings = resources.data();
        if (vkCreateDescriptorSetLayout(m_device, &rci, nullptr, &p.resources) != VK_SUCCESS) {
            vkDestroyShaderModule(m_device, module, nullptr);
            *failure = "no memory for the colour transform";
            return false;
        }
        layouts[1] = p.resources;
    }
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 2 * sizeof(int32_t)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = resources.empty() ? 1 : 2;
    plci.pSetLayouts = layouts;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(m_device, &plci, nullptr, &p.pipe.pipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, module, nullptr);
        *failure = "no memory for the colour transform";
        return false;
    }
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = module;
    cpci.stage.pName = "main";
    cpci.layout = p.pipe.pipelineLayout;
    const VkResult made = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &cpci, nullptr, &p.pipe.pipeline);
    vkDestroyShaderModule(m_device, module, nullptr);
    if (made != VK_SUCCESS) { *failure = "the GPU could not make the colour transform's pipeline"; return false; }
    if (resources.empty()) return true;

    // Its own resources, kept with it: they never change.
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                                     {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, uint32_t(std::max<size_t>(1, program.textures.size()))}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = sizes;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &p.resources;
    if (vkCreateDescriptorPool(m_device, &dpi, nullptr, &p.pool) != VK_SUCCESS || (ai.descriptorPool = p.pool,
        vkAllocateDescriptorSets(m_device, &ai, &p.set) != VK_SUCCESS)) {
        *failure = "no room for the colour transform's tables";
        return false;
    }
    std::vector<VkWriteDescriptorSet> writes;
    VkDescriptorBufferInfo bufferInfo{};
    if (!program.uniforms.empty()) {
        if (!createBuffer(p.uniforms, program.uniforms.size(), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true)) {
            *failure = "no memory for the colour transform's settings";
            return false;
        }
        std::memcpy(p.uniforms.mapped, program.uniforms.data(), program.uniforms.size());
        bufferInfo = {p.uniforms.buffer, 0, program.uniforms.size()};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = p.set;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &bufferInfo;
        writes.push_back(w);
    }
    p.textures.resize(program.textures.size());
    std::vector<VkDescriptorImageInfo> images(program.textures.size());
    for (size_t i = 0; i < program.textures.size(); ++i) {
        if (!uploadTexture(p.textures[i], program.textures[i])) { *failure = "the colour transform's tables could not be loaded"; return false; }
        images[i] = {program.textures[i].linear ? m_linear : m_nearest, p.textures[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = p.set;
        w.dstBinding = program.textures[i].binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &images[i];
        writes.push_back(w);
    }
    vkUpdateDescriptorSets(m_device, uint32_t(writes.size()), writes.data(), 0, nullptr);
    return true;
}

void VkEngine::recordProgram(const Image &in, Image &out, const ProgramPipe &p) {
    if (!begin()) return;
    const VkDescriptorSet pictures = allocateSet(p.pipe.layout);
    if (!pictures) return;
    VkDescriptorImageInfo infos[2] = {{VK_NULL_HANDLE, in.view, VK_IMAGE_LAYOUT_GENERAL},
                                      {VK_NULL_HANDLE, out.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = pictures;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);
    const VkDescriptorSet sets[2] = {pictures, p.set};
    const int32_t push[2] = {out.width, out.height};
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipe.pipeline);
    vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipe.pipelineLayout, 0, p.set ? 2 : 1, sets, 0, nullptr);
    vkCmdPushConstants(m_cmd, p.pipe.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
    vkCmdDispatch(m_cmd, groups(uint32_t(out.width), 16), groups(uint32_t(out.height), 16), 1);
    memoryBarrier();
}

// ------------------------------------------------------------ scopes

namespace {
// The counting pass's push constants, as its `ScopePush` block.
struct ScopePush {
    int32_t w, h;
    int32_t width, height, third, vector;
    int32_t flags, pad = 0;
    double low, high, inverseRange, kr, kb, cbGain, crGain, steps;
};
// The side of the tile of pixels one invocation of the counting pass measures.
// (8, with 16 cells held for each plane: the quickest of those tried on an
// Arc B580 at 1920x1080 and 3840x2160; larger tiles leave a small picture
// too few invocations to fill the GPU.)
constexpr uint32_t kScopeTile = 8;
constexpr int kScopeWays = 4;  // times four cells held
// What a counting pass does with each pixel, word for word with
// Analysis::accumulateRows in src/scopes.cpp: the same doubles in the same
// order (`precise`: nothing joined or reordered), so the counts are the same.
const char *const kScopeBody = R"(
// Counts are added to the table with atomicAdd, the one thing here that is
// slow on a GPU: so each invocation measures a tile of pixels and holds, for
// each plane, the last few cells it hit and how often, and only adds to the
// table when a cell gives up its place (and at the end). Neighbouring pixels
// mostly hit the same cells. The sums are the same whatever the order.
const int kStreams = 12;
uvec4 heldCells[kStreams * SCOPE_WAYS];
uvec4 heldCounts[kStreams * SCOPE_WAYS];
uint heldTurn[kStreams];
void scopeAdd(int stream, uint cell) {
    for (int way = 0; way < SCOPE_WAYS; ++way) {
        const bvec4 same = equal(heldCells[stream * SCOPE_WAYS + way], uvec4(cell));
        if (any(same)) { heldCounts[stream * SCOPE_WAYS + way] += uvec4(same); return; }
    }
    const uint turn = heldTurn[stream];
    heldTurn[stream] = (turn + 1u) % uint(4 * SCOPE_WAYS);
    const int at = stream * SCOPE_WAYS + int(turn >> 2u);
    const uint slot = turn & 3u;
    if (heldCounts[at][slot] != 0u) atomicAdd(cells[heldCells[at][slot]], heldCounts[at][slot]);
    heldCells[at][slot] = cell;
    heldCounts[at][slot] = 1u;
}
int scopeBin(double value, int bins) {
    precise double t = (value - q.low) * q.inverseRange;
    t = max(0.0lf, min(1.0lf, t));
    precise double at = t * double(bins - 1) + 0.5lf;
    return int(at);
}
bool scopeOutside(double value) { return value < q.low || value > q.high; }
void scopeHit(int stream, uint plane, int width, int x, int y, bool clipped) {
    const uint i = plane + uint(y) * uint(width) + uint(x);
    scopeAdd(stream, i);
    if (clipped) scopeAdd(stream, i + uint(width) * uint(q.height));
}
void scopePixel(ivec2 p) {
    vec3 rgb = imageLoad(sourcePicture, p).rgb;
    // As a working frame is mended: a colour that is not a number is black.
    if (any(isnan(rgb)) || any(isinf(rgb))) rgb = vec3(0.0);
    rgb = SCOPE_SIGNAL;
    const uint wavePlane = uint(q.width) * uint(q.height) * 2u, paradePlane = uint(q.third) * uint(q.height) * 2u;
    const uint vectorAt = 4u * wavePlane + 3u * paradePlane;
    const uint histogramAt = vectorAt + uint(q.vector) * uint(q.vector) * 2u;
    // (Left out, and counted as such where the pixels counted will be.)
    if (any(isnan(rgb)) || any(isinf(rgb))) { atomicAdd(cells[histogramAt + 8u * uint(q.width)], 1u); return; }
    precise dvec4 v = dvec4(dvec3(rgb), 0.0lf);
    if (q.steps != 0.0lf) {
        for (int c = 0; c < 3; ++c) {
            // std::round: halves away from nought. (The product is exact: 24 bits by 12.)
            precise double scaled = v[c] * q.steps;
            precise double whole = abs(scaled) < 4503599627370496.0lf ? sign(scaled) * floor(abs(scaled) + 0.5lf) : scaled;
            v[c] = whole / q.steps;
        }
    }
    precise double redPart = q.kr * (v.r - v.g);
    precise double bluePart = q.kb * (v.b - v.g);
    precise double luma = v.g + redPart;
    luma = luma + bluePart;
    v.a = luma;
    ivec4 rows, bins;
    bvec4 clipped;
    for (int c = 0; c < 4; ++c) {
        rows[c] = q.height - 1 - scopeBin(v[c], q.height);
        bins[c] = scopeBin(v[c], q.width);
        clipped[c] = scopeOutside(v[c]);
    }
    const int first = int(uint(p.x) * uint(q.width) / uint(q.size.x));
    const int last = max(first, int(uint(p.x + 1) * uint(q.width) / uint(q.size.x)) - 1);
    const int pFirst = int(uint(p.x) * uint(q.third) / uint(q.size.x));
    const int pLast = max(pFirst, int(uint(p.x + 1) * uint(q.third) / uint(q.size.x)) - 1);
    const bool lumaOnly = (q.flags & 16) != 0;
    if ((q.flags & 1) != 0) {
        if (lumaOnly) { for (int x = first; x <= last; ++x) scopeHit(3, 3u * wavePlane, q.width, x, rows[3], clipped[3]); }
        else for (int c = 0; c < 3; ++c) if ((q.flags & (32 << c)) != 0)
            for (int x = first; x <= last; ++x) scopeHit(c, uint(c) * wavePlane, q.width, x, rows[c], clipped[c]);
    }
    if ((q.flags & 2) != 0)
        for (int c = 0; c < 3; ++c)
            for (int x = pFirst; x <= pLast; ++x) scopeHit(4 + c, 4u * wavePlane + uint(c) * paradePlane, q.third, x, rows[c], clipped[c]);
    if ((q.flags & 4) != 0)
        for (int c = 0; c < 4; ++c) if ((q.flags & (32 << c)) != 0) {
            const uint i = histogramAt + uint(c) * uint(q.width) * 2u + uint(bins[c]);
            scopeAdd(7 + c, i);
            if (clipped[c]) scopeAdd(7 + c, i + uint(q.width));
        }
    if ((q.flags & 8) != 0) {
        precise double cb = (v.b - luma) * q.cbGain;
        precise double cr = (v.r - luma) * q.crGain;
        precise double across = max(0.0lf, min(1.0lf, 0.5lf + cb)) * double(q.vector - 1) + 0.5lf;
        precise double down = max(0.0lf, min(1.0lf, 0.5lf - cr)) * double(q.vector - 1) + 0.5lf;
        const uint i = vectorAt + uint(int(down)) * uint(q.vector) + uint(int(across));
        scopeAdd(11, i);
        if (abs(cb) > 0.5lf || abs(cr) > 0.5lf) scopeAdd(11, i + uint(q.vector) * uint(q.vector));
    }
}
void main() {
    const ivec2 corner = ivec2(gl_GlobalInvocationID.xy) * SCOPE_TILE;
    if (any(greaterThanEqual(corner, q.size))) return;
    for (int s = 0; s < kStreams * SCOPE_WAYS; ++s) { heldCells[s] = uvec4(0xFFFFFFFFu); heldCounts[s] = uvec4(0u); }
    for (int s = 0; s < kStreams; ++s) heldTurn[s] = 0u;
    const ivec2 end = min(corner + ivec2(SCOPE_TILE), q.size);
    for (int y = corner.y; y < end.y; ++y)
        for (int x = corner.x; x < end.x; ++x) scopePixel(ivec2(x, y));
    for (int s = 0; s < kStreams * SCOPE_WAYS; ++s)
        for (int n = 0; n < 4; ++n)
            if (heldCounts[s][n] != 0u) atomicAdd(cells[heldCells[s][n]], heldCounts[s][n]);
}
)";
}  // namespace

void VkEngine::destroyPipeline(Pipeline &p) {
    if (p.pipeline) vkDestroyPipeline(m_device, p.pipeline, nullptr);
    if (p.pipelineLayout) vkDestroyPipelineLayout(m_device, p.pipelineLayout, nullptr);
    if (p.layout) vkDestroyDescriptorSetLayout(m_device, p.layout, nullptr);
    p = Pipeline{};
}

// The counting pass for one transform (`with` holds its tables, as set 1), or
// for none. Left empty when the device will not make it.
bool VkEngine::buildScopePipeline(const ColourProgram *program, const ProgramPipe *with, Pipeline &p) {
    std::string source = "#version 460\n"
        "layout(local_size_x = 8, local_size_y = 8) in;\n"
        "#define SCOPE_TILE " + std::to_string(kScopeTile) + "\n"
        "#define SCOPE_WAYS " + std::to_string(kScopeWays) + "\n"
        "layout(set = 0, binding = 0, rgba32f) uniform readonly image2D sourcePicture;\n"
        "layout(set = 0, binding = 1, std430) buffer ScopeCells { uint cells[]; };\n"
        "layout(push_constant, std430) uniform ScopePush {\n"
        "    ivec2 size; int width; int height; int third; int vector; int flags; int pad;\n"
        "    double low; double high; double inverseRange; double kr; double kb; double cbGain; double crGain; double steps;\n"
        "} q;\n";
    if (program) source += program->shader + "\n#define SCOPE_SIGNAL " + program->function + "(vec4(rgb, 1.0)).rgb\n";
    else source += "#define SCOPE_SIGNAL rgb\n";
    source += kScopeBody;
    std::vector<uint32_t> spirv;
    std::string why;
    if (!compileCompute(source, spirv, &why)) return false;
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = spirv.size() * sizeof(uint32_t);
    smci.pCode = spirv.data();
    VkShaderModule module;
    if (vkCreateShaderModule(m_device, &smci, nullptr, &module) != VK_SUCCESS) return false;
    const VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dlci.bindingCount = 2;
    dlci.pBindings = bindings;
    bool ok = vkCreateDescriptorSetLayout(m_device, &dlci, nullptr, &p.layout) == VK_SUCCESS;
    const VkDescriptorSetLayout layouts[2] = {p.layout, with ? with->resources : VK_NULL_HANDLE};
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ScopePush)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = layouts[1] ? 2 : 1;
    plci.pSetLayouts = layouts;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    ok = ok && vkCreatePipelineLayout(m_device, &plci, nullptr, &p.pipelineLayout) == VK_SUCCESS;
    if (ok) {
        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = module;
        cpci.stage.pName = "main";
        cpci.layout = p.pipelineLayout;
        ok = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &cpci, nullptr, &p.pipeline) == VK_SUCCESS;
    }
    vkDestroyShaderModule(m_device, module, nullptr);
    if (!ok) destroyPipeline(p);
    return ok;
}

bool VkEngine::countScopes(int source, const ColourProgram *program, const ScopeLayout &layout, std::vector<uint32_t> &cells) {
    if (!m_float64 || m_lost) return false;
    const Image *picture = &m_canvas;
    if (source >= 0) {
        const auto it = m_sources.find(source);
        if (it == m_sources.end() || !it->second.hasFrame) return false;
        picture = &it->second.base();
    }
    if (!picture->image || !picture->initialised) return false;
    // Sizes the pass's 32-bit sums hold, and a table worth reading back
    // (the scopes' own rasters are 256 by 200: about 2 MB).
    if (layout.width < 2 || layout.height < 2 || layout.third < 1 || layout.vector < 2 || layout.width > 2048
        || layout.height > 2048 || layout.vector > 2048 || picture->width > 32768)
        return false;
    const VkDeviceSize size = VkDeviceSize(layout.cells()) * sizeof(uint32_t);
    if (size > m_maxStorage || size > (VkDeviceSize(32) << 20)) return false;
    const ProgramPipe *with = nullptr;
    Pipeline *pipe = &m_scopePlain;
    if (program) {
        ProgramPipe &p = programPipe(*program);
        if (!p.error.empty()) return false;
        if (!p.scopeTried) { p.scopeTried = true; buildScopePipeline(program, &p, p.scope); }
        with = &p;
        pipe = &p.scope;
    } else if (!m_scopePlainTried) {
        m_scopePlainTried = true;
        buildScopePipeline(nullptr, nullptr, m_scopePlain);
    }
    if (!pipe->pipeline) return false;
    if (m_scopeDevice.size < size || m_scopeHost.size < size) flush();
    if (!ensureBuffer(m_scopeDevice, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false)
        || !ensureBuffer(m_scopeHost, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true) || !begin())
        return false;
    const VkDescriptorSet set = allocateSet(pipe->layout);
    if (!set) return false;
    const VkDescriptorImageInfo image{VK_NULL_HANDLE, picture->view, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorBufferInfo buffer{m_scopeDevice.buffer, 0, size};
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &image;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &buffer;
    vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);
    ScopePush push{};
    push.w = picture->width;
    push.h = picture->height;
    push.width = layout.width;
    push.height = layout.height;
    push.third = layout.third;
    push.vector = layout.vector;
    push.flags = (layout.wave ? 1 : 0) | (layout.parade ? 2 : 0) | (layout.histogram ? 4 : 0) | (layout.vectorscope ? 8 : 0)
                 | (layout.lumaOnly ? 16 : 0);
    for (int c = 0; c < 4; ++c) if (layout.selected[c]) push.flags |= 32 << c;
    push.low = layout.low; push.high = layout.high; push.inverseRange = layout.inverseRange;
    push.kr = layout.kr; push.kb = layout.kb; push.cbGain = layout.cbGain; push.crGain = layout.crGain; push.steps = layout.steps;
    // Only the planes this layout counts are cleared and brought back: the
    // copy to memory the host can read is the slow part of a small picture.
    std::vector<VkBufferCopy> counted;
    const auto plane = [&counted](size_t at, size_t length) {
        const VkDeviceSize offset = VkDeviceSize(at) * sizeof(uint32_t), bytes = VkDeviceSize(length) * sizeof(uint32_t);
        if (!counted.empty() && counted.back().srcOffset + counted.back().size == offset) counted.back().size += bytes;
        else counted.push_back({offset, offset, bytes});
    };
    if (layout.wave) {
        if (layout.lumaOnly) plane(layout.waveAt(3), layout.wavePlane());
        else for (int c = 0; c < 3; ++c) if (layout.selected[c]) plane(layout.waveAt(c), layout.wavePlane());
    }
    if (layout.parade) plane(layout.paradeAt(0), 3 * layout.paradePlane());
    if (layout.vectorscope) plane(layout.vectorAt(), size_t(layout.vector) * size_t(layout.vector) * 2);
    if (layout.histogram) for (int c = 0; c < 4; ++c) if (layout.selected[c]) plane(layout.histogramAt(c), size_t(layout.width) * 2);
    plane(layout.pixelsAt(), 1);
    for (const VkBufferCopy &part : counted) vkCmdFillBuffer(m_cmd, m_scopeDevice.buffer, part.srcOffset, part.size, 0);
    memoryBarrier();
    const VkDescriptorSet sets[2] = {set, with ? with->set : VK_NULL_HANDLE};
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe->pipeline);
    vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe->pipelineLayout, 0, sets[1] ? 2 : 1, sets, 0, nullptr);
    vkCmdPushConstants(m_cmd, pipe->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(m_cmd, groups(groups(uint32_t(picture->width), kScopeTile), 8), groups(groups(uint32_t(picture->height), kScopeTile), 8), 1);
    memoryBarrier();
    vkCmdCopyBuffer(m_cmd, m_scopeDevice.buffer, m_scopeHost.buffer, uint32_t(counted.size()), counted.data());
    hostReadBarrier();
    flush();
    if (m_lost) return false;
    cells.assign(layout.cells(), 0);
    for (const VkBufferCopy &part : counted)
        std::memcpy(reinterpret_cast<uint8_t *>(cells.data()) + part.srcOffset, static_cast<const uint8_t *>(m_scopeHost.mapped) + part.srcOffset, size_t(part.size));
    // The pass counted the pixels it left out (rare) where the count of
    // those it measured belongs: one cell every pixel added to would be slow.
    cells[layout.pixelsAt()] = uint32_t(picture->width) * uint32_t(picture->height) - cells[layout.pixelsAt()];
    return true;
}

// RemoveBackground: the network run on `in` into the step's matte (its aux
// picture), on each new picture.
bool VkEngine::recordMatte(const Image &in, Source::Step &st, const FilterParams &q, bool fresh) {
    const matting::Model &m = matting::model();
    if (!m.ok) return false;
    if (!ensureNetworkPipeline()) return false;
    const bool samePlan = st.plan.ok && st.plan.width == q.ix && st.plan.height == q.iy &&
                          st.plan.passes.front().inW == in.width && st.plan.passes.front().inH == in.height;
    if (!samePlan) {
        st.plan = matting::plan(m, q.ix, q.iy, in.width, in.height, 1);
        st.auxOf = nullptr;
    }
    const VkDeviceSize weights = m.values.size() * sizeof(float), arena = st.plan.arena * sizeof(float);
    if (!st.plan.ok || weights > m_maxStorage || arena > m_maxStorage) return false;
    if (!m_nnWeights.buffer) {
        // Once: through a buffer of its own, so nothing else waits on the staging one.
        flush();
        Buffer host;
        const bool made = createBuffer(m_nnWeights, weights, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false) &&
                          createBuffer(host, weights, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true) && begin();
        if (made) {
            std::memcpy(host.mapped, m.values.data(), weights);
            const VkBufferCopy region{0, 0, weights};
            vkCmdCopyBuffer(m_cmd, host.buffer, m_nnWeights.buffer, 1, &region);
            memoryBarrier();
            flush();
        }
        destroyBuffer(host);
        if (!made || m_lost) { destroyBuffer(m_nnWeights); return false; }
    }
    if (st.arena.size < arena) {
        flush();  // the smaller one may still be in use
        if (!ensureBuffer(st.arena, arena, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false)) return false;
    }
    if (!filterImage(st.aux, q.ix, q.iy)) return false;
    if (st.auxOf == &m && !fresh) return true;
    if (!begin()) return false;
    const VkDescriptorSet set = allocateSet(m_nn.layout);
    if (!set) return false;
    const VkDescriptorImageInfo source{m_linear, in.view, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo matte{VK_NULL_HANDLE, st.aux.view, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorBufferInfo buffers[2] = {{st.arena.buffer, 0, arena}, {m_nnWeights.buffer, 0, weights}};
    VkWriteDescriptorSet writes[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &source;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &buffers[0];
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &buffers[1];
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[3].pImageInfo = &matte;
    vkUpdateDescriptorSets(m_device, 4, writes, 0, nullptr);
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_nn.pipeline);
    vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_nn.pipelineLayout, 0, 1, &set, 0, nullptr);
    for (const matting::Pass &pass : st.plan.passes) {
        // A thread a pixel (64 a workgroup), or a workgroup a channel.
        uint32_t g[3] = {groups(uint32_t(pass.outW * pass.outH), 64), 1, 1};
        switch (pass.op) {
        case matting::NormStats: g[0] = uint32_t(pass.splits); g[1] = uint32_t(pass.cout - pass.actUntil); break;
        case matting::NormMerge: g[0] = groups(uint32_t(pass.cout - pass.actUntil), 64); break;
        case matting::Pool: g[0] = uint32_t(pass.c0); break;
        case matting::Conv:
            // Eight channels a row of workgroups; a full one: four pixels in a
            // row a thread (PX in nn.comp), and with fewer runs than 64 the
            // eights in x instead.
            g[1] = groups(uint32_t(pass.cout), 8);
            if (!pass.depthwise) {
                const uint32_t runs = uint32_t((pass.outW + 3) / 4 * pass.outH);
                g[0] = runs < 64 ? groups(runs * g[1], 64) : groups(runs, 64);
                if (runs < 64) g[1] = 1;
            }
            g[2] = uint32_t(pass.splits);
            break;
        case matting::Sum: g[1] = uint32_t(pass.cout); break;
        case matting::Resize:
        case matting::Scale: g[1] = uint32_t(pass.c0); break;
        case matting::NormApply: g[1] = uint32_t(pass.cout - pass.actUntil); break;
        default: break;
        }
        vkCmdPushConstants(m_cmd, m_nn.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pass, &pass);
        vkCmdDispatch(m_cmd, g[0], g[1], g[2]);
        memoryBarrier();
    }
    st.auxOf = &m;
    return true;
}

// RemoveBackground after its network (the matte in st.aux): brought to the
// picture's edges (matteParams in frame.h), the picture cut out by it, and
// with blurBackground put over itself blurred.
bool VkEngine::recordGuided(const Image &in, Source::Step &st, const VideoFilter &f, const FilterParams &q) {
    const int w = in.width, h = in.height;
    for (Image &img : st.guide)
        if (!filterImage(img, q.ix, q.iy)) return false;
    if (f.blurBackground && (!filterImage(st.back[0], (w + 3) / 4, (h + 3) / 4) || !filterImage(st.back[1], (w + 3) / 4, (h + 3) / 4) ||
                             !filterImage(st.mid, w, h)))
        return false;
    if (!begin()) return false;
    Image &now = st.guide[0], &spare = st.guide[1], &blurred = st.guide[2], &fit = st.guide[3];
    auto pass = [&](MattePass p) { return matteParams(f, p, w, h); };
    recordFilter(in, now, &st.aux, pass(MattePass::Stats));
    recordFilter(now, spare, nullptr, pass(MattePass::Across));
    recordFilter(spare, blurred, nullptr, pass(MattePass::Down));
    recordFilter(blurred, fit, nullptr, pass(MattePass::Coefficients));
    recordFilter(fit, spare, nullptr, pass(MattePass::Across));
    recordFilter(spare, blurred, nullptr, pass(MattePass::Down));
    recordFilter(blurred, fit, &now, pass(MattePass::Pack));
    Image &cut = f.blurBackground ? st.mid : st.out;
    recordFilter(in, cut, &fit, q);
    if (f.blurBackground) {
        recordFilter(in, st.back[0], nullptr, pass(MattePass::BackDown));
        recordFilter(st.back[0], st.back[1], nullptr, pass(MattePass::BackAcross));
        recordFilter(st.back[1], st.back[0], nullptr, pass(MattePass::BackDownBlur));
        recordFilter(cut, st.out, &st.back[0], pass(MattePass::BackOver));
    }
    return true;
}

void VkEngine::recordFilter(const Image &in, Image &out, const Image *aux, const FilterParams &q) {
    if (!begin()) return;
    // Effects have a pipeline each; every other step is one of filter.comp's kinds.
    const bool effect = q.kind >= kEffectKind && size_t(q.kind - kEffectKind) < m_effects.size();
    const Pipeline &pipe = effect ? m_effects[size_t(q.kind - kEffectKind)] : m_filter;
    const VkDescriptorSet set = descriptorSet(pipe, in.view, out.view, VK_NULL_HANDLE, 0, aux ? aux->view : in.view);
    if (!set) return;
    FilterPush push{in.width, in.height, out.width, out.height, q.kind, q.mode, q.ix, q.iy, {}};
    std::memcpy(push.p, q.p, sizeof push.p);
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.pipeline);
    vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(m_cmd, pipe.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(m_cmd, groups(uint32_t(out.width), 16), groups(uint32_t(out.height), 16), 1);
    memoryBarrier();
}

void VkEngine::releaseStep(Source::Step &st) {
    {
        destroyImage(st.out);
        destroyImage(st.shaped);
        destroyImage(st.aux);
        destroyImage(st.mid);
        destroyImage(st.mid2);
        for (auto &h : st.held) destroyImage(h.first);
        for (auto &img : st.regs) destroyImage(img);
        for (auto &table : st.tables) destroyImage(table.second);
        for (auto &img : st.spare) destroyImage(img);
        for (auto &img : st.guide) destroyImage(img);
        for (auto &img : st.back) destroyImage(img);
        destroyBuffer(st.arena);
        for (auto &inner : st.people) releaseStep(inner);
    }
}

void VkEngine::releaseSteps(Source &s) {
    for (auto &st : s.steps) releaseStep(st);
    s.steps.clear();
}

void VkEngine::setSourceFilters(int id, const std::vector<VideoFilter> &filters) {
    const auto it = m_sources.find(id);
    if (it == m_sources.end() || it->second.filters == filters) return;
    Source &s = it->second;
    // A step keeps its pictures while it stays the same kind of step: an
    // effect's frame number, a keyframed amount or a grade that moves change
    // what is drawn into them, not the pictures (filterImage makes one again
    // only when its size changes). What a step holds of its past (a delay's
    // pictures, where a scroll began) it keeps only as the same step
    // (filterStepKept). Playing a clip with an effect therefore makes no
    // picture and waits for nothing here.
    enum class Keep { Nothing, Pictures, All };
    std::vector<Keep> keep(filters.size(), Keep::Nothing);
    bool sameKinds = filters.size() == s.filters.size(), releases = filters.size() < s.steps.size();
    for (size_t i = 0; i < filters.size(); ++i) {
        if (i < s.filters.size() && s.filters[i].type == filters[i].type)
            keep[i] = filterStepKept(s.filters, filters, i) ? Keep::All : Keep::Pictures;
        else
            sameKinds = false;
        if (i < s.steps.size() && (keep[i] == Keep::Nothing || (keep[i] == Keep::Pictures && !(s.steps[i].held.empty() && s.steps[i].spare.empty()))))
            releases = true;
    }
    if (releases) flush();  // what is recorded may still read what goes
    if (!sameKinds) m_filterMs.erase(id);  // another chain: measured afresh
    std::vector<Source::Step> next(filters.size());
    for (size_t i = 0; i < filters.size(); ++i) {
        if (keep[i] == Keep::Nothing) continue;
        Source::Step &st = next[i] = std::exchange(s.steps[i], {});
        if (keep[i] == Keep::All) continue;
        for (auto &held : st.held) destroyImage(held.first);
        for (Image &img : st.spare) destroyImage(img);
        st.held.clear();
        st.spare.clear();
        st.started = false;
        // Its table or mask on the device is of what the old filter named:
        // kept only when the new one names the very same (another may come
        // to sit at the old one's address).
        const VideoFilter &was = s.filters[i], &now = filters[i];
        if (was.lut != now.lut || was.image != now.image || was.grade != now.grade) st.auxOf = nullptr;
    }
    releaseSteps(s);  // the rest
    s.filters = filters;
    s.steps = std::move(next);
    s.filtered = false;
}

void VkEngine::renderSourceFilters(int id, double seconds) {
    const auto it = m_sources.find(id);
    if (it == m_sources.end() || m_lost) return;
    Source &s = it->second;
    if (s.filters.empty() || !s.hasFrame || !s.base().image) { s.filtered = false; return; }
    bool timed = false;
    for (const VideoFilter &f : s.filters) timed |= filterIsTimed(f);
    if (s.filtered && !timed && s.filteredGeneration == s.generation) return;
    const Image *cur = &s.base();
    bool fresh = s.generation != s.filteredGeneration || !s.filtered;  // the step's input is new
    // Until the chain is through: a step that fails (no memory for its
    // picture) leaves the source unfiltered rather than half drawn.
    s.filtered = false;
    // Its GPU time, when it all goes into one command buffer.
    uint64_t timedBuffer = 0;
    uint32_t timedFirst = 0;
    if (m_tickNs > 0 && begin() && m_timestampsUsed + 2 <= kTimestamps) {
        timedFirst = m_timestampsUsed;
        m_timestampsUsed += 2;
        timedBuffer = m_bufferSerial;
        vkCmdWriteTimestamp(m_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_timestamps, timedFirst);
    }
    for (size_t i = 0; i < s.filters.size(); ++i) {
        const VideoFilter &f = s.filters[i];
        Source::Step &st = s.steps[i];
        st.within = false;
        if (!st.started) { st.started = true; st.origin = seconds; }
        if (filterIsDelay(f)) {
            const double due = seconds - f.delayMs / 1000.0;
            auto retire = [&] { st.spare.push_back(st.held.front().first); st.held.pop_front(); };
            if (f.type == VideoFilterType::RenderDelay || fresh || st.held.empty()) {
                while (st.held.size() >= 2 && st.held[1].second <= due) retire();
                while (st.held.size() >= filterDelayCapacity(cur->width, cur->height, 16)) retire();
                Image img;
                if (!st.spare.empty()) { img = st.spare.back(); st.spare.pop_back(); }
                if (!filterImage(img, cur->width, cur->height)) { destroyImage(img); return; }
                recordFilter(*cur, img, nullptr, FilterParams{});
                st.held.emplace_back(img, seconds);
            }
            while (st.held.size() >= 2 && st.held[1].second <= due) retire();
            if (st.spare.size() > 2) {
                flush();
                while (st.spare.size() > 2) { destroyImage(st.spare.back()); st.spare.pop_back(); }
            }
            cur = &st.held.front().first;
            fresh = true;
            continue;
        }
        int ow, oh;
        filterSize(f, cur->width, cur->height, &ow, &oh);
        if (!filterImage(st.out, ow, oh)) return;
        if (f.type == VideoFilterType::Scale) {
            if (!begin()) return;
            clearTo(st.out, {0, 0, 0, 0});
            drawLayer(st.out, *cur, scaleLayer(f, cur->width, cur->height, ow, oh));
        } else if (f.type == VideoFilterType::Effect) {
            // Its passes in turn, each between the step's spare pictures,
            // the last into the step's output; every pass also sees the input.
            const int passes = effectPasses(f.effect);
            const Image *from = cur;
            for (int pass = 0; pass < passes; ++pass) {
                Image *to = &st.out;
                if (pass + 1 < passes) {
                    to = pass % 2 ? &st.mid2 : &st.mid;
                    if (!filterImage(*to, cur->width, cur->height)) return;
                }
                recordFilter(*from, *to, cur, filterParams(f, cur->width, cur->height, 0, pass));
                from = to;
            }
        } else if (f.type == VideoFilterType::GradePlan) {
            if (!f.plan || !runPlan(st, *f.plan, *cur) || m_lost) return;
        } else if (f.type == VideoFilterType::Program) {
            // A program that would not compile leaves the source unfiltered;
            // prepareProgram says so before it is used.
            if (!f.program) return;
            const ProgramPipe &program = programPipe(*f.program);
            if (!program.error.empty()) return;
            recordProgram(*cur, st.out, program);
        } else if (f.type == VideoFilterType::RemoveBackground) {
            // A missing model, or a device too small for the network, leaves the picture as it was.
            const FilterParams q = filterParams(f, cur->width, cur->height, 0);
            const bool matte = recordMatte(*cur, st, q, fresh);
            if (m_lost) return;
            if (!matte) recordFilter(*cur, st.out, nullptr, FilterParams{});
            else if (!recordGuided(*cur, st, f, q)) return;
        } else if (f.type == VideoFilterType::Blur || f.type == VideoFilterType::Shadow) {
            const bool blur = f.type == VideoFilterType::Blur;
            if (!filterImage(st.mid, cur->width, cur->height)) return;
            // A wide one reads the picture averaged over its taps' spacing.
            const Image *first = cur;
            if (blurStep(f.radius) > 1) {
                if (!filterImage(st.mid2, cur->width, cur->height)) return;
                recordFilter(*cur, st.mid2, nullptr, filterParams(f, cur->width, cur->height, 0, 2));
                first = &st.mid2;
            }
            recordFilter(*first, st.mid, nullptr, filterParams(f, cur->width, cur->height, 0, 0));
            recordFilter(blur ? st.mid : *cur, st.out, blur ? nullptr : &st.mid, filterParams(f, cur->width, cur->height, 0, 1));
        } else {
            const FilterParams q = filterParams(f, cur->width, cur->height, seconds - st.origin, 0, m_levels.sdrWhite);
            const void *auxOf = q.kind == 6 ? static_cast<const void *>(f.lut.get()) : q.kind == 7 ? static_cast<const void *>(f.image.get())
                              : q.kind == 15 && (q.mode & 1) ? static_cast<const void *>(f.grade->channels.get()) : nullptr;
            if (auxOf && st.auxOf != auxOf) {
                bool ok;
                if (q.kind == 6) {
                    const LookupTable &t = *f.lut;
                    const int texels = t.cube ? t.size * t.size * t.size : t.size;
                    std::vector<float> rgba(size_t(texels) * 4, 1.f);
                    for (int n = 0; n < texels; ++n) std::copy_n(&t.rgb[size_t(n) * 3], 3, &rgba[size_t(n) * 4]);
                    ok = uploadAux(st.aux, VK_FORMAT_R32G32B32A32_SFLOAT, t.size, t.cube ? t.size * t.size : 1, rgba.data(), rgba.size() * sizeof(float));
                } else if (q.kind == 15) {
                    // The three tables, 256 entries a row, one channel below the other.
                    std::vector<float> rows(size_t(3) * 65536);
                    for (int ch = 0; ch < 3; ++ch) std::copy((*f.grade->channels)[size_t(ch)].begin(), (*f.grade->channels)[size_t(ch)].end(), rows.begin() + ch * 65536);
                    ok = uploadAux(st.aux, VK_FORMAT_R32_SFLOAT, 256, 768, rows.data(), rows.size() * sizeof(float));
                } else {
                    ok = uploadAux(st.aux, VK_FORMAT_R8G8B8A8_UNORM, f.image->width, f.image->height, f.image->rgba.data(), f.image->rgba.size());
                }
                if (!ok) { destroyImage(st.aux); st.auxOf = nullptr; return; }
                st.auxOf = auxOf;
            }
            recordFilter(*cur, st.out, auxOf ? &st.aux : nullptr, q);
        }
        // Kept within its shape: outside it, the picture the step was given.
        st.within = f.within.kind != 0 && ow == cur->width && oh == cur->height;
        if (st.within) {
            if (!filterImage(st.shaped, ow, oh)) return;
            recordFilter(st.out, st.shaped, cur, shapeParams(f.within, ow, oh));
            cur = &st.shaped;
            fresh |= filterIsTimed(f);
            continue;
        }
        cur = &st.out;
        fresh |= filterIsTimed(f);
    }
    if (timedBuffer && timedBuffer == m_bufferSerial && m_recording) {
        vkCmdWriteTimestamp(m_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_timestamps, timedFirst + 1);
        m_timings.push_back({id, timedFirst});
    }
    s.filtered = true;
    s.filteredGeneration = s.generation;
}

bool VkEngine::readPreview(int w, int h, Bgra8Frame &out) { return previewOf(m_canvas, w, h, out); }
bool VkEngine::readSourcePreview(int source, int w, int h, Bgra8Frame &out) {
    const auto it = m_sources.find(source);
    return it != m_sources.end() && previewOf(it->second.shown(), w, h, out);
}

bool VkEngine::readLinear(int source, FrameF &out) {
    return readLinearInto(source, [&out](int w, int h) {
        if (out.width != w || out.height != h) out.resize(w, h);
        return out.rgba.data();
    });
}

bool VkEngine::readLinearInto(int source, const std::function<float *(int, int)> &target) {
    const auto [w, h] = linearToHost(source);
    if (w <= 0 || h <= 0) return false;
    float *to = target(w, h);
    if (!to) return false;
    // Rows copied out of the mapped memory on every core: one thread alone
    // reads it at a fraction of the bus's speed.
    const size_t row = size_t(w) * 4;
    const float *from = static_cast<const float *>(m_linearHost.mapped);
    RowPool::instance().run(0, h, 16, [&](int first, int last) {
        std::memcpy(to + size_t(first) * row, from + size_t(first) * row, size_t(last - first) * row * sizeof(float));
    });
    return true;
}

std::pair<int, int> VkEngine::linearToHost(int source) {
    const Image *picture = &m_canvas;
    if (source >= 0) {
        const auto it = m_sources.find(source);
        if (it == m_sources.end() || !it->second.hasFrame) return {};
        picture = &it->second.shown();
    }
    if (!picture->image || !picture->initialised || m_lost) return {};
    const int w = picture->width, h = picture->height;
    const VkDeviceSize size = VkDeviceSize(w) * h * 16;
    if (size > m_maxStorage) return {};  // more than a shader may see of one buffer
    if (m_linearHost.size < size) flush();
    if (!ensureBuffer(m_linearHost, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true) || !begin()) return {};
    const VkDescriptorSet set = descriptorSet(m_linearOut, picture->view, VK_NULL_HANDLE, m_linearHost.buffer, size);
    if (!set) return {};
    const PreviewPush push{w, h};
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_linearOut.pipeline);
    vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_linearOut.pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(m_cmd, m_linearOut.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(m_cmd, groups(uint32_t(w), 16), groups(uint32_t(h), 16), 1);
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
    flush();
    if (m_lost) return {};
    return {w, h};
}

bool VkEngine::readSourceEncoded(int source, int w, int h, Bgra8Frame &out) {
    const auto it = m_sources.find(source);
    return it != m_sources.end() && previewOf(it->second.shown(), w, h, out, true);
}

bool VkEngine::readSourceInput(int source, int w, int h, Bgra8Frame &out) {
    const auto it = m_sources.find(source);
    return it != m_sources.end() && it->second.hasFrame && previewOf(it->second.base(), w, h, out);
}

bool VkEngine::recordPreview(const Image &picture, int w, int h, Buffer &device, Buffer &host, int encoded) {
    const VkDeviceSize size = static_cast<VkDeviceSize>(w) * h * 4;
    // As the output readback measured (recordReadback): straight into host
    // memory, or converted on the GPU and copied across.
    const bool direct = m_readbackMode == 1;
    if (device.size < size || host.size < size) flush();
    if ((!direct && !ensureBuffer(device, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false)) ||
        !ensureBuffer(host, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true))
        return false;
    if (!begin()) return false;
    const VkDescriptorSet set = descriptorSet(m_preview, picture.view, VK_NULL_HANDLE, direct ? host.buffer : device.buffer, size);
    if (!set) return false;
    const PreviewPush push{w, h, encoded};
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_preview.pipeline);
    vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_preview.pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(m_cmd, m_preview.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(m_cmd, groups(w, 16), groups(h, 16), 1);
    memoryBarrier();
    if (!direct) {
        VkBufferCopy copy{0, 0, size};
        vkCmdCopyBuffer(m_cmd, device.buffer, host.buffer, 1, &copy);
    }
    hostReadBarrier();
    return true;
}

bool VkEngine::previewOf(const Image &picture, int w, int h, Bgra8Frame &out, bool encoded) {
    if (!picture.image || !picture.initialised || w <= 0 || h <= 0 || m_lost) return false;
    if (out.width != w || out.height != h) out.resize(w, h);
    return previewInto(picture, w, h, out.data.data(), encoded ? 1 : 0);
}

bool VkEngine::previewInto(const Image &picture, int w, int h, uint8_t *to, int encoded) {
    if (!picture.image || !picture.initialised || w <= 0 || h <= 0 || m_lost) return false;
    if (!recordPreview(picture, w, h, m_previewDevice, m_previewHost, encoded)) return false;
    flush();
    if (m_lost) return false;
    const size_t bytes = static_cast<size_t>(w) * h * 4;
    const auto *from = static_cast<const uint8_t *>(m_previewHost.mapped);
    // A large picture is copied out by rows on several threads.
    if (bytes < (size_t(1) << 22)) std::memcpy(to, from, bytes);
    else parallelRows(0, h, [&](int first, int last) {
        std::memcpy(to + static_cast<size_t>(first) * w * 4, from + static_cast<size_t>(first) * w * 4,
                    static_cast<size_t>(last - first) * w * 4);
    });
    return true;
}

bool VkEngine::readSourceShown(int source, int w, int h, uint8_t *bgra) {
    const auto it = m_sources.find(source);
    return it != m_sources.end() && bgra && previewInto(it->second.shown(), w, h, bgra, 2);
}

bool VkEngine::readOutput(const OutputColour &colour, YuvFrame &out, int width, int height) {
    return outputOf(m_canvas, colour, out, width, height);
}
bool VkEngine::readSourceOutput(int source, const OutputColour &colour, YuvFrame &out, int width, int height) {
    const auto it = m_sources.find(source);
    return it != m_sources.end() && it->second.hasFrame && outputOf(it->second.shown(), colour, out, width, height);
}

bool VkEngine::recordOutput(const Image &picture, const OutputColour &colour, const YuvFrame &shape, Buffer &device,
                            Buffer &host, Planes *planes, bool toHost) {
    const int w = shape.width, h = shape.height;
    // Each 8x2 block writes whole uints: rows padded to the block's samples.
    const uint32_t bytes = static_cast<uint32_t>(shape.sampleBytes());
    const uint32_t blockW = align(static_cast<uint32_t>(w), 8);
    const bool rgb = formatIsRgb(colour.format);
    const uint32_t chromaSamples = chromaHalfWidth(colour.format) ? blockW / 2 : blockW;
    const uint32_t yStride = rgb ? blockW * 4 : blockW * bytes;
    const uint32_t cStride = rgb ? 0 : chromaSamples * bytes * (chromaInterleaved(colour.format) ? 2 : 1);
    const uint32_t cRows = static_cast<uint32_t>(shape.rows(1)) + 1;  // an odd height's last block row
    const uint32_t uOffset = yStride * align(static_cast<uint32_t>(h), 2);
    const uint32_t vOffset = uOffset + cStride * cRows;
    const VkDeviceSize size = rgb ? uOffset : shape.rowBytes(2) ? vOffset + cStride * cRows : vOffset;
    *planes = Planes{{0, uOffset, vOffset}, {yStride, cStride, cStride}, size};

    if (device.size < size || (toHost && host.size < size)) flush();
    if (!ensureBuffer(device, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, &device == &host) ||
        (toHost && !ensureBuffer(host, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)))
        return false;
    if (!begin()) return false;
    const VkDescriptorSet set = descriptorSet(m_yuv, picture.view, VK_NULL_HANDLE, device.buffer, size);
    if (!set) return false;
    const YuvOutPush push{w, h, static_cast<int32_t>(yStride), static_cast<int32_t>(cStride),
           static_cast<int32_t>(uOffset), static_cast<int32_t>(vOffset), picture.width, picture.height, outputParams(colour)};
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_yuv.pipeline);
    vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_yuv.pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(m_cmd, m_yuv.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(m_cmd, groups(static_cast<uint32_t>(w), 128), groups(static_cast<uint32_t>(h), 8), 1);
    memoryBarrier();
    if (!toHost) return true;
    VkBufferCopy copy{0, 0, size};
    vkCmdCopyBuffer(m_cmd, device.buffer, host.buffer, 1, &copy);
    hostReadBarrier();
    return true;
}

// An output picture into `host` for the CPU: converted on the GPU and copied
// across, or written by the conversion straight into host memory. Which is
// faster depends on the machine (on an Arc B580, copying some layouts into its
// cached host memory ran at a tenth of the speed of writing them there
// directly; elsewhere the copy engine may win), so outputOf measures both once.
bool VkEngine::recordReadback(const Image &picture, const OutputColour &colour, const YuvFrame &shape, Buffer &device,
                              Buffer &host, Planes *planes) {
    if (m_readbackMode == 1) {
        if (!recordOutput(picture, colour, shape, host, host, planes, false)) return false;
        hostReadBarrier();
        return true;
    }
    return recordOutput(picture, colour, shape, device, host, planes, true);
}

void VkEngine::copyOutput(const Buffer &host, const Planes &planes, YuvFrame &out) {
    const uint8_t *src = static_cast<const uint8_t *>(host.mapped);
    std::vector<uint8_t> *dst[3] = {&out.y, &out.u, &out.v};
    // Rows on every core: one thread alone reads the mapped memory at a
    // fraction of the bus's speed.
    for (int p = 0; p < 3; ++p) {
        const int rowBytes = out.rowBytes(p);
        if (rowBytes <= 0) continue;
        RowPool::instance().run(0, out.rows(p), 16, [&](int first, int last) {
            for (int y = first; y < last; ++y)
                std::memcpy(dst[p]->data() + static_cast<size_t>(y) * rowBytes,
                            src + planes.offsets[p] + static_cast<size_t>(y) * planes.strides[p], rowBytes);
        });
    }
}

bool VkEngine::outputOf(const Image &picture, const OutputColour &colour, YuvFrame &out, int width, int height) {
    if (!picture.image || !picture.initialised || m_lost) return false;
    const int w = width > 0 ? width : picture.width, h = height > 0 ? height : picture.height;
    if (out.width != w || out.height != h || out.format != colour.format) out.resize(w, h, colour.format);
    Planes planes;
    // The first large readback tries both ways and keeps the faster (see
    // recordReadback); both give the same bytes.
    const bool calibrate = m_readbackMode < 0 && qint64(w) * h >= 1000000;
    double took[2] = {0, 0};
    for (int mode = calibrate ? 0 : -1; mode < (calibrate ? 2 : 0); ++mode) {
        if (calibrate) m_readbackMode = mode;
        const auto started = std::chrono::steady_clock::now();
        if (!recordReadback(picture, colour, out, m_yuvDevice, m_yuvHost, &planes)) { if (calibrate) m_readbackMode = -1; return false; }
        flush();
        if (m_lost) return false;
        took[mode < 0 ? 0 : mode] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    }
    if (calibrate) m_readbackMode = took[1] < took[0] ? 1 : 0;
    if (!calibrate) {
        if (!recordReadback(picture, colour, out, m_yuvDevice, m_yuvHost, &planes)) return false;
        flush();
        if (m_lost) return false;
    }
    copyOutput(m_yuvHost, planes, out);
    return true;
}

// Each request keeps its buffers until taken; a free slot's buffers are in no
// recorded or submitted work, so they may be resized at will.
VkEngine::Readback &VkEngine::freeReadback() {
    for (Readback &r : m_readbacks)
        if (r.id == 0) return r;
    return m_readbacks.emplace_back();
}

int VkEngine::requestOutput(const OutputColour &colour, int width, int height) {
    if (!m_canvas.image || !m_canvas.initialised || m_lost) return 0;
    Readback &r = freeReadback();
    r.shape = YuvFrame{};
    r.shape.width = width > 0 ? width : m_canvas.width;
    r.shape.height = height > 0 ? height : m_canvas.height;
    r.shape.format = colour.format;
    r.shape.nv12 = colour.format == ColourFormat::NV12;
    if (!recordReadback(m_canvas, colour, r.shape, r.device, r.host, &r.planes)) return 0;
    r.preview = false;
    r.id = m_nextRequest++;
    return r.id;
}

int VkEngine::requestPreview(int width, int height) {
    if (!m_canvas.image || !m_canvas.initialised || width <= 0 || height <= 0 || m_lost) return 0;
    Readback &r = freeReadback();
    if (!recordPreview(m_canvas, width, height, r.device, r.host)) return 0;
    r.shape = YuvFrame{};
    r.shape.width = width;
    r.shape.height = height;
    r.preview = true;
    r.id = m_nextRequest++;
    return r.id;
}

bool VkEngine::takeOutput(int request, YuvFrame &out) {
    if (m_recording) flush();  // asked for and never submitted
    settle();
    for (Readback &r : m_readbacks) {
        if (r.id != request || r.preview) continue;
        r.id = 0;
        if (m_lost) return false;
        if (out.width != r.shape.width || out.height != r.shape.height || out.format != r.shape.format)
            out.resize(r.shape.width, r.shape.height, r.shape.format);
        copyOutput(r.host, r.planes, out);
        return true;
    }
    return false;
}

// What FFmpeg calls each of our colour formats.
static AVPixelFormat pixelFormatFor(ColourFormat format) {
    switch (format) {
    case ColourFormat::NV12: return AV_PIX_FMT_NV12;
    case ColourFormat::I420: return AV_PIX_FMT_YUV420P;
    case ColourFormat::I444: return AV_PIX_FMT_YUV444P;
    case ColourFormat::P010: return AV_PIX_FMT_P010LE;
    case ColourFormat::I010: return AV_PIX_FMT_YUV420P10LE;
    case ColourFormat::P216: return AV_PIX_FMT_P216LE;
    case ColourFormat::P416: return AV_PIX_FMT_P416LE;
    case ColourFormat::BGRA: return AV_PIX_FMT_BGRA;
    case ColourFormat::I210: return AV_PIX_FMT_YUV422P10LE;
    }
    return AV_PIX_FMT_NONE;
}

bool VkEngine::writeVulkanFrames(const OutputColour &colour, int width, int height, const std::vector<AVFrame *> &frames) {
    if (!m_ffDevice || frames.empty() || !m_canvas.image || !m_canvas.initialised || m_lost) return false;
    const int w = width > 0 ? width : m_canvas.width, h = height > 0 ? height : m_canvas.height;
    YuvFrame shape;
    shape.width = w;
    shape.height = h;
    shape.format = colour.format;
    shape.nv12 = colour.format == ColourFormat::NV12;
    const int planes = shape.rowBytes(2) ? 3 : shape.rowBytes(1) ? 2 : 1;
    // Every frame on this device, in this format and size, or none is written.
    for (const AVFrame *frame : frames) {
        if (!frame || frame->format != AV_PIX_FMT_VULKAN || !frame->hw_frames_ctx) return false;
        const auto *fc = reinterpret_cast<const AVHWFramesContext *>(frame->hw_frames_ctx->data);
        if (fc->device_ref->data != m_ffDevice->data || fc->sw_format != pixelFormatFor(colour.format) ||
            fc->width != w || fc->height != h || av_pix_fmt_count_planes(fc->sw_format) != planes)
            return false;
    }

    // One conversion into a buffer of the GPU's own, copied into each frame.
    Readback &r = freeReadback();
    r.shape = shape;
    if (!recordOutput(m_canvas, colour, r.shape, r.device, r.host, &r.planes, false)) return false;
    r.preview = false;
    r.id = -1;
    for (AVFrame *frame : frames) {
        auto *fc = reinterpret_cast<AVHWFramesContext *>(frame->hw_frames_ctx->data);
        auto *vkfc = static_cast<AVVulkanFramesContext *>(fc->hwctx);
        auto *vkf = reinterpret_cast<AVVkFrame *>(frame->data[0]);
        int images = 0;
        while (images < AV_NUM_DATA_POINTERS && vkf->img[images]) ++images;
        const bool multiplane = images == 1 && planes > 1;
        vkfc->lock_frame(fc, vkf);
        // From wherever the encoder left it; it waits on the semaphore below
        // before it reads it again.
        for (int i = 0; i < images; ++i) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = vkf->layout[i];
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = vkf->img[i];
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                 0, nullptr, 1, &b);
        }
        for (int p = 0; p < planes; ++p) {
            const int texels = p ? shape.chromaWidth() : w;
            VkBufferImageCopy region{};
            region.bufferOffset = r.planes.offsets[p];
            region.bufferRowLength = r.planes.strides[p] / static_cast<uint32_t>(shape.rowBytes(p) / texels);
            region.imageSubresource.aspectMask =
                multiplane ? static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_PLANE_0_BIT << p) : VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {static_cast<uint32_t>(texels), static_cast<uint32_t>(p ? shape.chromaHeight() : h), 1};
            vkCmdCopyBufferToImage(m_cmd, r.device.buffer, vkf->img[multiplane ? 0 : p],
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        for (int i = 0; i < images; ++i) {
            vkf->layout[i] = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            vkf->access[i] = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            m_waitSems.push_back(vkf->sem[i]);
            m_waitValues.push_back(vkf->sem_value[i]);
            m_waitStages.push_back(VK_PIPELINE_STAGE_TRANSFER_BIT);
            m_signalSems.push_back(vkf->sem[i]);
            m_signalValues.push_back(++vkf->sem_value[i]);
        }
        vkfc->unlock_frame(fc, vkf);
    }
    return true;
}

bool VkEngine::takePreview(int request, Bgra8Frame &out) {
    if (m_recording) flush();
    settle();
    for (Readback &r : m_readbacks) {
        if (r.id != request || !r.preview) continue;
        r.id = 0;
        if (m_lost) return false;
        if (out.width != r.shape.width || out.height != r.shape.height) out.resize(r.shape.width, r.shape.height);
        std::memcpy(out.data.data(), r.host.mapped, static_cast<size_t>(r.shape.width) * r.shape.height * 4);
        return true;
    }
    return false;
}

}  // namespace

std::vector<GpuDevice> listGpus() { return enumerateGpus(); }

std::vector<int> rankGpus(const std::vector<GpuDevice> &gpus, const std::string &chosen) {
    const auto rank = [](GpuDevice::Kind kind) {
        switch (kind) {
        case GpuDevice::Kind::Discrete: return 3;
        case GpuDevice::Kind::Integrated: return 2;
        case GpuDevice::Kind::Virtual: return 1;
        default: return 0;  // the CPU twin beats a software Vulkan device
        }
    };
    std::vector<const GpuDevice *> order;
    for (const GpuDevice &g : gpus)
        if (g.usable && rank(g.kind) > 0) order.push_back(&g);
    // The one chosen first; the rest by kind, ties in the order the driver lists them.
    std::stable_sort(order.begin(), order.end(), [&](const GpuDevice *a, const GpuDevice *b) {
        const bool pickedA = !chosen.empty() && a->id == chosen, pickedB = !chosen.empty() && b->id == chosen;
        if (pickedA != pickedB) return pickedA;
        return rank(a->kind) > rank(b->kind);
    });
    std::vector<int> indices(order.size());
    std::transform(order.begin(), order.end(), indices.begin(), [](const GpuDevice *g) { return g->index; });
    return indices;
}

GpuPick chooseGpu(const std::vector<GpuDevice> &gpus, const std::string &chosen) {
    GpuPick pick;
    const std::vector<int> order = rankGpus(gpus, chosen);
    if (order.empty()) return pick;
    const auto picked = std::find_if(gpus.begin(), gpus.end(), [&](const GpuDevice &g) { return g.index == order.front(); });
    if (!chosen.empty() && picked->id == chosen) return {picked->index, GpuPick::Why::Chosen};
    pick = {picked->index, GpuPick::Why::Automatic};
    if (!chosen.empty()) pick.why = GpuPick::Why::ChosenMissing;
    return pick;
}

std::recursive_mutex &loaderMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

std::unique_ptr<Engine> createVulkanEngine(int deviceIndex, std::string *failure, AVBufferRef *shared) {
    auto e = std::make_unique<VkEngine>();
    if (!e->init(deviceIndex, failure, shared)) return nullptr;
    return e;
}

}  // namespace engine
