#include "compute.h"
#include <QCryptographicHash>
#include <QHash>
#include <QElapsedTimer>
#include <QLibrary>
#include <QSet>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <vector>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

namespace SuiteProcessing {
namespace {
std::atomic<bool> submissionQuarantined{false};
QString validatePixels(const QByteArray &pixels,qsizetype bytes,const QByteArray *coverage=nullptr) {
    if(pixels.size()!=bytes)return "A point stage returned the wrong byte count.";
    if(quintptr(pixels.constData())%alignof(float))return "RGBA32F data is not float-aligned.";
    const auto *p=reinterpret_cast<const float*>(pixels.constData());
    const auto *original=coverage ? reinterpret_cast<const float*>(coverage->constData()) : nullptr;
    for(qsizetype i=0;i<bytes/16;++i,p+=4) {
        if(!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]) || !std::isfinite(p[3]) || p[3]<0 || p[3]>1)
            return "A point stage contains nonfinite RGB or invalid coverage.";
        if(original && p[3]!=original[i*4+3])return "A colour point stage changed coverage.";
    }
    return {};
}
QString resultText(const char *operation,VkResult result) {return QString("%1 failed (%2)").arg(QLatin1String(operation)).arg(result);}
}
struct ComputeContext::State {
    Options options;
    QLibrary loader;
    bool attempted=false,ready=false,quarantined=false,float64=false,inFlight=false;
    QString reason,deviceName;
    QVariantMap lastTimings;
    int selectedIndex=-1;
    quint32 queueFamily=0;
    VkInstance instance=VK_NULL_HANDLE;
    VkPhysicalDevice physical=VK_NULL_HANDLE;
    VkDevice device=VK_NULL_HANDLE;
    VkQueue queue=VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkDescriptorSetLayout descriptorLayout=VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout=VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool=VK_NULL_HANDLE;
    VkDescriptorSet descriptors[2]={VK_NULL_HANDLE,VK_NULL_HANDLE};
    VkCommandPool commandPool=VK_NULL_HANDLE;
    VkCommandBuffer command=VK_NULL_HANDLE;
    VkFence fence=VK_NULL_HANDLE;
    struct Buffer {VkBuffer handle=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;VkMemoryPropertyFlags properties=0;};
    Buffer pixels[2],flags,staging;
    void *mapped=nullptr;
    qsizetype capacity=0;
    QHash<QByteArray,VkPipeline> pipelines;
    QSet<QByteArray> rejectedShaders;
    QList<QByteArray> readbackPool;
    quint64 hostAllocations=0,hostReuses=0;
    quint64 runs=0,dispatches=0,uploads=0,downloads=0,allocations=0,reuses=0,cpuFallbacks=0;
    PFN_vkGetInstanceProcAddr getInstance=nullptr;
    PFN_vkGetDeviceProcAddr getDevice=nullptr;
    PFN_vkDestroyInstance destroyInstance=nullptr;
#define DEVICE_PROCS(X) \
 X(DestroyDevice) X(GetDeviceQueue) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) \
 X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) \
 X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
 X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) \
 X(CreateShaderModule) X(DestroyShaderModule) X(CreateComputePipelines) X(DestroyPipeline) \
 X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(ResetCommandPool) \
 X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdCopyBuffer) X(CmdPipelineBarrier) X(CmdBindPipeline) \
 X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdDispatch) X(CreateFence) X(DestroyFence) \
 X(ResetFences) X(QueueSubmit) X(WaitForFences)
#define DECLARE_PROC(name) PFN_vk##name name=nullptr;
    DEVICE_PROCS(DECLARE_PROC)
#undef DECLARE_PROC
    explicit State(const Options &o):options(o) {
        options.waitMilliseconds=qBound(100,options.waitMilliseconds,30000);
        options.maximumImageBytes=qBound(qsizetype(16),options.maximumImageBytes,qsizetype(1024)*1024*1024);
    }
    ~State() {
        // A timed-out/failed submission may still reference every native object.
        // Keep that one context's native resources alive until process exit;
        // disable further GPU contexts in this process and continue on the CPU.
        // Never wait indefinitely or free storage that a driver may still use.
        if(quarantined)return;
        std::unique_lock<std::recursive_mutex> loaderHeld;
        if(options.loaderLock)loaderHeld=std::unique_lock<std::recursive_mutex>(*options.loaderLock);
        if(device) {
            clearBuffers();
            for(auto pipeline:pipelines)if(DestroyPipeline)DestroyPipeline(device,pipeline,nullptr);
            if(fence && DestroyFence)DestroyFence(device,fence,nullptr);
            if(commandPool && DestroyCommandPool)DestroyCommandPool(device,commandPool,nullptr);
            if(descriptorPool && DestroyDescriptorPool)DestroyDescriptorPool(device,descriptorPool,nullptr);
            if(pipelineLayout && DestroyPipelineLayout)DestroyPipelineLayout(device,pipelineLayout,nullptr);
            if(descriptorLayout && DestroyDescriptorSetLayout)DestroyDescriptorSetLayout(device,descriptorLayout,nullptr);
            if(DestroyDevice)DestroyDevice(device,nullptr);
        }
        if(instance && destroyInstance)destroyInstance(instance,nullptr);
    }
    bool fail(const QString &message) {reason=message;return false;}
    bool check(VkResult result,const char *operation) {return result==VK_SUCCESS || fail(resultText(operation,result));}
    void quarantine(const QString &message) {quarantined=true;ready=false;submissionQuarantined=true;reason=message+"; Vulkan disabled until restart; CPU fallback remains active";}
    void destroyBuffer(Buffer &buffer) {
        if(buffer.handle && DestroyBuffer)DestroyBuffer(device,buffer.handle,nullptr);
        if(buffer.memory && FreeMemory)FreeMemory(device,buffer.memory,nullptr);
        buffer={};
    }
    void clearBuffers() {
        if(mapped && UnmapMemory)UnmapMemory(device,staging.memory);
        mapped=nullptr;
        destroyBuffer(pixels[0]);destroyBuffer(pixels[1]);destroyBuffer(flags);destroyBuffer(staging);capacity=0;
    }
    bool initialize() {
        if(submissionQuarantined)return fail("Vulkan was disabled after a submission failure; CPU fallback active");
        if(attempted)return ready;
        attempted=true;
        std::unique_lock<std::recursive_mutex> loaderHeld;
        if(options.loaderLock)loaderHeld=std::unique_lock<std::recursive_mutex>(*options.loaderLock);
#if defined(Q_OS_WIN)
        const QString defaultLoader="vulkan-1";
#elif defined(Q_OS_MACOS)
        const QString defaultLoader="libvulkan.1.dylib";
#else
        const QString defaultLoader="libvulkan.so.1";
#endif
        loader.setFileName(options.loader.isEmpty() ? defaultLoader : options.loader);
        // Pin the loader so a quarantined driver's objects never outlive it.
        loader.setLoadHints(QLibrary::PreventUnloadHint);
        if(!loader.load())return fail("Vulkan loader unavailable: "+loader.errorString());
        getInstance=reinterpret_cast<PFN_vkGetInstanceProcAddr>(loader.resolve("vkGetInstanceProcAddr"));
        if(!getInstance)return fail("Vulkan instance entry point unavailable");
        auto createInstance=reinterpret_cast<PFN_vkCreateInstance>(getInstance(VK_NULL_HANDLE,"vkCreateInstance"));
        if(!createInstance)return fail("Vulkan instance creation unavailable");
        VkApplicationInfo app{};app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;app.pApplicationName="Suite point processing";app.apiVersion=VK_API_VERSION_1_0;
        VkInstanceCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;info.pApplicationInfo=&app;
        if(!check(createInstance(&info,nullptr,&instance),"vkCreateInstance"))return false;
        destroyInstance=reinterpret_cast<PFN_vkDestroyInstance>(getInstance(instance,"vkDestroyInstance"));
        auto enumerate=reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(getInstance(instance,"vkEnumeratePhysicalDevices"));
        auto getProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(getInstance(instance,"vkGetPhysicalDeviceProperties"));
        auto getMemory=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(getInstance(instance,"vkGetPhysicalDeviceMemoryProperties"));
        auto getQueues=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(getInstance(instance,"vkGetPhysicalDeviceQueueFamilyProperties"));
        auto getFeatures=reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(getInstance(instance,"vkGetPhysicalDeviceFeatures"));
        auto createDevice=reinterpret_cast<PFN_vkCreateDevice>(getInstance(instance,"vkCreateDevice"));
        getDevice=reinterpret_cast<PFN_vkGetDeviceProcAddr>(getInstance(instance,"vkGetDeviceProcAddr"));
        if(!destroyInstance || !enumerate || !getProperties || !getMemory || !getQueues || !getFeatures || !createDevice || !getDevice)return fail("Vulkan device entry points incomplete");
        uint32_t count=0;if(!check(enumerate(instance,&count,nullptr),"vkEnumeratePhysicalDevices") || !count || count>128)return fail("No usable Vulkan device inventory");
        std::vector<VkPhysicalDevice> devices(count);if(!check(enumerate(instance,&count,devices.data()),"vkEnumeratePhysicalDevices"))return false;
        struct Candidate {int index;int score;VkPhysicalDeviceProperties props;};std::vector<Candidate> candidates;
        for(uint32_t i=0;i<count;++i) {
            if(options.deviceIndex>=0 && options.deviceIndex!=int(i))continue;
            VkPhysicalDeviceProperties props{};getProperties(devices[i],&props);
            if(props.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU)continue;
            const int score=props.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 : props.deviceType==VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 90 : props.deviceType==VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? 50 : 10;
            candidates.push_back({int(i),score,props});
        }
        std::stable_sort(candidates.begin(),candidates.end(),[](const auto &a,const auto &b){return a.score>b.score;});
        QStringList rejected;
        for(const auto &candidate:candidates) {
            const auto &limit=candidate.props.limits;
            if(limit.maxComputeWorkGroupInvocations<256 || limit.maxComputeWorkGroupSize[0]<256 || limit.maxPushConstantsSize<4 || limit.maxPerStageDescriptorStorageBuffers<3 || limit.maxStorageBufferRange<16)continue;
            uint32_t families=0;getQueues(devices[candidate.index],&families,nullptr);if(!families || families>1024)continue;
            std::vector<VkQueueFamilyProperties> queues(families);getQueues(devices[candidate.index],&families,queues.data());
            int selected=-1;
            for(uint32_t q=0;q<families;++q)if(queues[q].queueCount && (queues[q].queueFlags&VK_QUEUE_COMPUTE_BIT)) {selected=int(q);if(!(queues[q].queueFlags&VK_QUEUE_GRAPHICS_BIT))break;}
            if(selected<0)continue;
            float priority=1;VkDeviceQueueCreateInfo qi{};qi.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;qi.queueFamilyIndex=selected;qi.queueCount=1;qi.pQueuePriorities=&priority;
            VkPhysicalDeviceFeatures supported{},enabled{};getFeatures(devices[candidate.index],&supported);enabled.robustBufferAccess=supported.robustBufferAccess;enabled.shaderFloat64=supported.shaderFloat64;
            VkDeviceCreateInfo di{};di.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;di.pEnabledFeatures=&enabled;
            const auto result=createDevice(devices[candidate.index],&di,nullptr,&device);
            if(result!=VK_SUCCESS) {rejected.append(QString::fromUtf8(candidate.props.deviceName)+": "+resultText("vkCreateDevice",result));device=VK_NULL_HANDLE;continue;}
            float64=bool(enabled.shaderFloat64);physical=devices[candidate.index];properties=candidate.props;queueFamily=selected;selectedIndex=candidate.index;deviceName=QString::fromUtf8(properties.deviceName);getMemory(physical,&memory);break;
        }
        if(!device)return fail("No suitable Vulkan compute device"+(rejected.isEmpty() ? QString() : ": "+rejected.join("; ")));
#define LOAD_PROC(name) (name)=reinterpret_cast<PFN_vk##name>(getDevice(device,"vk" #name));if(!(name))return fail("Missing Vulkan device function: vk" #name);
        DEVICE_PROCS(LOAD_PROC)
#undef LOAD_PROC
        GetDeviceQueue(device,queueFamily,0,&queue);if(!queue)return fail("Vulkan compute queue unavailable");
        VkDescriptorSetLayoutBinding bindings[3]{};
        for(uint32_t i=0;i<3;++i){bindings[i].binding=i;bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;bindings[i].descriptorCount=1;bindings[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;}
        VkDescriptorSetLayoutCreateInfo li{};li.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;li.bindingCount=3;li.pBindings=bindings;
        if(!check(CreateDescriptorSetLayout(device,&li,nullptr,&descriptorLayout),"vkCreateDescriptorSetLayout"))return false;
        VkPushConstantRange push{};push.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;push.size=4;
        VkPipelineLayoutCreateInfo pi{};pi.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;pi.setLayoutCount=1;pi.pSetLayouts=&descriptorLayout;pi.pushConstantRangeCount=1;pi.pPushConstantRanges=&push;
        if(!check(CreatePipelineLayout(device,&pi,nullptr,&pipelineLayout),"vkCreatePipelineLayout"))return false;
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,6};VkDescriptorPoolCreateInfo dp{};dp.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;dp.maxSets=2;dp.poolSizeCount=1;dp.pPoolSizes=&poolSize;
        if(!check(CreateDescriptorPool(device,&dp,nullptr,&descriptorPool),"vkCreateDescriptorPool"))return false;
        VkDescriptorSetLayout layouts[2]={descriptorLayout,descriptorLayout};VkDescriptorSetAllocateInfo ds{};ds.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;ds.descriptorPool=descriptorPool;ds.descriptorSetCount=2;ds.pSetLayouts=layouts;
        if(!check(AllocateDescriptorSets(device,&ds,descriptors),"vkAllocateDescriptorSets"))return false;
        VkCommandPoolCreateInfo cp{};cp.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;cp.queueFamilyIndex=queueFamily;
        if(!check(CreateCommandPool(device,&cp,nullptr,&commandPool),"vkCreateCommandPool"))return false;
        VkCommandBufferAllocateInfo cb{};cb.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;cb.commandPool=commandPool;cb.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cb.commandBufferCount=1;
        if(!check(AllocateCommandBuffers(device,&cb,&command),"vkAllocateCommandBuffers"))return false;
        VkFenceCreateInfo fi{};fi.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if(!check(CreateFence(device,&fi,nullptr,&fence),"vkCreateFence"))return false;
        ready=true;reason.clear();return true;
    }
    bool buffer(Buffer &out,VkDeviceSize bytes,VkBufferUsageFlags usage,VkMemoryPropertyFlags wanted,VkMemoryPropertyFlags preferred=0) {
        VkBufferCreateInfo bi{};bi.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;bi.size=bytes;bi.usage=usage;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        if(!check(CreateBuffer(device,&bi,nullptr,&out.handle),"vkCreateBuffer"))return false;
        VkMemoryRequirements requirements{};GetBufferMemoryRequirements(device,out.handle,&requirements);
        int type=-1;
        for(uint32_t i=0;i<memory.memoryTypeCount && i<VK_MAX_MEMORY_TYPES;++i) {
            if(!(requirements.memoryTypeBits&(1u<<i)) || (memory.memoryTypes[i].propertyFlags&wanted)!=wanted)continue;
            if(type<0)type=int(i);
            if((memory.memoryTypes[i].propertyFlags&preferred)==preferred){type=int(i);break;}
        }
        if(type<0)return fail("Required Vulkan buffer memory type unavailable");
        out.properties=memory.memoryTypes[type].propertyFlags;
        VkMemoryAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;ai.allocationSize=requirements.size;ai.memoryTypeIndex=type;
        return check(AllocateMemory(device,&ai,nullptr,&out.memory),"vkAllocateMemory") && check(BindBufferMemory(device,out.handle,out.memory,0),"vkBindBufferMemory");
    }
    bool ensureBuffers(qsizetype bytes) {
        if(capacity>=bytes){++reuses;return true;}
        clearBuffers();const qsizetype next=(bytes+4095)&~qsizetype(4095);
        const auto storage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if(!buffer(pixels[0],next,storage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            || !buffer(pixels[1],next,storage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            || !buffer(flags,4,storage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            || !buffer(staging,next+16,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,options.preferCachedStaging ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0)
            || !check(MapMemory(device,staging.memory,0,VK_WHOLE_SIZE,0,&mapped),"vkMapMemory")){clearBuffers();return false;}
        capacity=next;++allocations;return true;
    }
    bool prepare(const QList<ComputeStage> &stages,std::vector<VkPipeline> &prepared) {
        QSet<QByteArray> needed;
        for(const auto &stage:stages)needed.insert(QCryptographicHash::hash(stage.spirv,QCryptographicHash::Sha256));
        for(auto it=pipelines.begin();pipelines.size()+needed.size()>64 && it!=pipelines.end();) {
            if(!needed.contains(it.key())){DestroyPipeline(device,it.value(),nullptr);it=pipelines.erase(it);}else ++it;
        }
        for(const auto &stage:stages) {
            const auto key=QCryptographicHash::hash(stage.spirv,QCryptographicHash::Sha256);
            if(rejectedShaders.contains(key))return fail("Shader previously failed pixel validation: "+stage.name);
            if(pipelines.contains(key)){prepared.push_back(pipelines.value(key));continue;}
            quint32 magic=0;if(stage.spirv.size()>=4)std::memcpy(&magic,stage.spirv.constData(),4);
            if(stage.spirv.size()<20 || stage.spirv.size()%4 || stage.spirv.size()>4*1024*1024 || magic!=0x07230203)return fail(stage.unavailableReason.isEmpty() ? "Vulkan shader unavailable or invalid: "+stage.name : stage.unavailableReason);
            std::vector<uint32_t> words(stage.spirv.size()/4);std::memcpy(words.data(),stage.spirv.constData(),stage.spirv.size());
            if(words[1]!=0x00010000)return fail("Point shaders must target SPIR-V 1.0: "+stage.name);
            for(size_t offset=5;offset<words.size();) {
                const uint32_t count=words[offset]>>16,opcode=words[offset]&65535;
                if(!count || offset+count>words.size())return fail("Malformed SPIR-V instruction: "+stage.name);
                if(opcode==17) { // OpCapability: this point engine enables Shader and optional Float64 only.
                    if(count!=2 || (words[offset+1]!=1 && !(words[offset+1]==10 && float64)))
                        return fail("Unsupported SPIR-V capability for this device: "+stage.name);
                }
                offset+=count;
            }
            VkShaderModuleCreateInfo si{};si.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;si.codeSize=stage.spirv.size();si.pCode=words.data();VkShaderModule shader=VK_NULL_HANDLE;
            if(!check(CreateShaderModule(device,&si,nullptr,&shader),"vkCreateShaderModule"))return false;
            VkComputePipelineCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;ci.layout=pipelineLayout;ci.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=shader;ci.stage.pName="main";
            VkPipeline pipeline=VK_NULL_HANDLE;const auto result=CreateComputePipelines(device,VK_NULL_HANDLE,1,&ci,nullptr,&pipeline);DestroyShaderModule(device,shader,nullptr);
            if(result!=VK_SUCCESS){if(pipeline)DestroyPipeline(device,pipeline,nullptr);return fail(resultText("vkCreateComputePipelines",result));}
            pipelines.insert(key,pipeline);prepared.push_back(pipeline);
        }
        return true;
    }
    QByteArray readback(qsizetype bytes) {
        // Only reuse a buffer when this context is its sole owner. Previously
        // returned frames can outlive the context and must never be overwritten.
        for(auto &entry:readbackPool) {
            if(!entry.isDetached())continue;
            if(entry.size()!=bytes){entry.resize(bytes);++hostAllocations;}else ++hostReuses;
            std::memcpy(entry.data(),mapped,bytes);return entry;
        }
        QByteArray fresh;fresh.resize(bytes);++hostAllocations;std::memcpy(fresh.data(),mapped,bytes);
        if(readbackPool.size()<2)readbackPool.append(fresh);
        return fresh;
    }
    void barrier(VkPipelineStageFlags source,VkPipelineStageFlags target,VkAccessFlags read,VkAccessFlags write) {
        VkMemoryBarrier memoryBarrier{};memoryBarrier.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;memoryBarrier.srcAccessMask=read;memoryBarrier.dstAccessMask=write;
        CmdPipelineBarrier(command,source,target,0,1,&memoryBarrier,0,nullptr,0,nullptr);
    }
    bool execute(const QByteArray &input,quint32 count,const QList<ComputeStage> &stages,QByteArray &output) {
        lastTimings.clear();QElapsedTimer timer;timer.start();
        if(input.size()>options.maximumImageBytes)return fail("Image exceeds the Vulkan buffer budget; CPU fallback active");
        if(!initialize())return false;
        if(quint64(input.size())>properties.limits.maxStorageBufferRange || (quint64(count)+255)/256>properties.limits.maxComputeWorkGroupCount[0])return fail("Image exceeds this Vulkan device's storage or dispatch limits");
        std::vector<VkPipeline> prepared;if(!prepare(stages,prepared) || !ensureBuffers(input.size()))return false;
        lastTimings["setupMs"]=timer.nsecsElapsed()/1000000.;timer.restart();
        std::memcpy(mapped,input.constData(),input.size());std::memset(static_cast<char*>(mapped)+capacity,0,4);
        lastTimings["hostWriteMs"]=timer.nsecsElapsed()/1000000.;timer.restart();
        VkDescriptorBufferInfo bi[2][3]{};VkWriteDescriptorSet writes[6]{};
        for(int set=0;set<2;++set)for(int binding=0;binding<3;++binding){
            bi[set][binding].buffer=binding==2 ? flags.handle : pixels[(set+binding)%2].handle;bi[set][binding].range=binding==2 ? 4 : input.size();
            auto &w=writes[set*3+binding];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=descriptors[set];w.dstBinding=binding;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w.pBufferInfo=&bi[set][binding];
        }
        UpdateDescriptorSets(device,6,writes,0,nullptr);
        if(!check(ResetCommandPool(device,commandPool,0),"vkResetCommandPool"))return false;
        VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if(!check(BeginCommandBuffer(command,&begin),"vkBeginCommandBuffer"))return false;
        VkBufferCopy imageCopy{0,0,VkDeviceSize(input.size())},flagUpload{VkDeviceSize(capacity),0,4};
        CmdCopyBuffer(command,staging.handle,pixels[0].handle,1,&imageCopy);CmdCopyBuffer(command,staging.handle,flags.handle,1,&flagUpload);
        barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
        for(size_t i=0;i<prepared.size();++i){
            CmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,prepared[i]);CmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&descriptors[i%2],0,nullptr);CmdPushConstants(command,pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&count);CmdDispatch(command,(count+255)/256,1,1);
            if(i+1<prepared.size())barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
        }
        barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferCopy flagDownload{0,VkDeviceSize(capacity),4};CmdCopyBuffer(command,pixels[prepared.size()%2].handle,staging.handle,1,&imageCopy);CmdCopyBuffer(command,flags.handle,staging.handle,1,&flagDownload);
        barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
        if(!check(EndCommandBuffer(command),"vkEndCommandBuffer") || !check(ResetFences(device,1,&fence),"vkResetFences"))return false;
        VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&command;
        lastTimings["recordCommandsMs"]=timer.nsecsElapsed()/1000000.;timer.restart();
        inFlight=true;
        auto result=QueueSubmit(queue,1,&submit,fence);
        if(result!=VK_SUCCESS){quarantine(resultText("vkQueueSubmit",result));return false;}
        result=WaitForFences(device,1,&fence,VK_TRUE,quint64(options.waitMilliseconds)*1000000);
        if(result!=VK_SUCCESS){quarantine(resultText("vkWaitForFences",result));return false;}
        inFlight=false;
        lastTimings["submitAndWaitMs"]=timer.nsecsElapsed()/1000000.;timer.restart();
        ++runs;dispatches+=prepared.size();++uploads;++downloads;
        quint32 invalid=0;std::memcpy(&invalid,static_cast<char*>(mapped)+capacity,4);
        if(invalid){for(const auto &stage:stages)rejectedShaders.insert(QCryptographicHash::hash(stage.spirv,QCryptographicHash::Sha256));return fail("Vulkan stage returned nonfinite pixels; replaying on the CPU");}
        output=readback(input.size());
        lastTimings["hostReadMs"]=timer.nsecsElapsed()/1000000.;timer.restart();
        const auto error=validatePixels(output,input.size(),&input);if(!error.isEmpty())return fail(error);
        lastTimings["outputValidationMs"]=timer.nsecsElapsed()/1000000.;
        reason.clear();return true;
    }
};
#undef DEVICE_PROCS
ComputeContext::ComputeContext():ComputeContext(Options{}){}
ComputeContext::ComputeContext(const Options &options):d(std::make_unique<State>(options)){}
ComputeContext::~ComputeContext()=default;
ComputeResult ComputeContext::process(const QByteArray &input,quint32 width,quint32 height,const QList<ComputeStage> &stages,Preference preference) {
    ComputeResult result;
    try {
        const quint64 count=quint64(width)*height;
        if(!width || !height || count>std::numeric_limits<quint32>::max() || count>quint64(std::numeric_limits<qsizetype>::max())/16 || input.size()!=qsizetype(count*16)){result.error="Invalid or oversized tightly packed RGBA32F input.";return result;}
        if(stages.size()>32){result.error="Point processing chains support at most 32 stages.";return result;}
        for(const auto &stage:stages)if(!stage.cpu){result.error="Every point stage requires a CPU implementation: "+stage.name;return result;}
        result.error=validatePixels(input,input.size());if(!result.error.isEmpty())return result;
        if(!stages.isEmpty() && preference==Preference::Vulkan) {
            try {
                if(d->execute(input,quint32(count),stages,result.pixels)){result.backend="vulkan";return result;}
            } catch(const std::exception &e) {
                if(d->inFlight)d->quarantine("Vulkan execution exception: "+QString::fromUtf8(e.what()));
                else {d->clearBuffers();d->readbackPool.clear();d->reason="Vulkan allocation/execution exception: "+QString::fromUtf8(e.what());}
            } catch(...) {d->quarantine("Unknown Vulkan execution exception");}
            result.fallbackReason=d->reason;++d->cpuFallbacks;
        }
        auto current=input;
        for(const auto &stage:stages){
            QString error;auto next=stage.cpu(current,&error);if(error.isEmpty())error=validatePixels(next,input.size(),&current);
            if(!error.isEmpty()){result.error=stage.name+": "+error;result.pixels.clear();return result;}
            current=std::move(next);
        }
        current.data(); // detach any borrowed source storage before returning it
        result.pixels=std::move(current);
    } catch(const std::exception &e) {result.error=QString::fromUtf8(e.what());result.pixels.clear();}
    catch(...) {result.error="Unknown CPU point-processing exception.";result.pixels.clear();}
    return result;
}
QVariantMap ComputeContext::information() const {
    return {{"schemaVersion",1},{"preferredBackend","vulkan"},{"cpuFallback",true},{"initialized",d->attempted},{"available",d->ready && !submissionQuarantined},{"device",d->deviceName},{"deviceIndex",d->selectedIndex},{"float64",d->float64},{"queueFamily",d->queueFamily},{"reason",d->reason},{"quarantined",d->quarantined},{"bufferCapacityBytes",d->capacity},{"stagingHostCached",bool(d->staging.properties&VK_MEMORY_PROPERTY_HOST_CACHED_BIT)},{"stagingMemoryFlags",quint32(d->staging.properties)},{"bufferAllocations",d->allocations},{"bufferReuses",d->reuses},{"pipelineCount",d->pipelines.size()},{"hostOutputAllocations",d->hostAllocations},{"hostOutputReuses",d->hostReuses},{"hostOutputPoolSize",d->readbackPool.size()},{"gpuRuns",d->runs},{"dispatches",d->dispatches},{"imageUploads",d->uploads},{"imageDownloads",d->downloads},{"cpuFallbacks",d->cpuFallbacks},{"lastGpuCallWallMs",d->lastTimings}};
}
}
