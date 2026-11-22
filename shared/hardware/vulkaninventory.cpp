#include "vulkaninventory.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QLibrary>
#include <QScopeGuard>
#include <cstring>
#include <vector>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

namespace SuiteHardware {
namespace {
QString version(uint32_t v) {
    return QStringLiteral("%1.%2.%3").arg(VK_VERSION_MAJOR(v)).arg(VK_VERSION_MINOR(v)).arg(VK_VERSION_PATCH(v));
}
QString deviceType(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
    default: return "other";
    }
}
}

QVariantMap vulkanInventory(const QString &loaderOverride) {
    QVariantMap result{{"schemaVersion", schemaVersion}, {"preferredBackend", "vulkan"},
                       {"processingBackend", "cpu"}, {"cpuFallback", true},
                       {"available", false}, {"devices", QVariantList{}}};
    auto fail = [&](const QString &reason) { result["reason"] = reason; return result; };
#if defined(Q_OS_WIN)
    const QString defaultLoader = "vulkan-1";
#elif defined(Q_OS_MACOS)
    const QString defaultLoader = "libvulkan.1.dylib";
#else
    const QString defaultLoader = "libvulkan.so.1";
#endif
    QLibrary loader(loaderOverride.isEmpty() ? defaultLoader : loaderOverride);
    if (!loader.load()) return fail("Vulkan loader unavailable: " + loader.errorString());
    const auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(loader.resolve("vkGetInstanceProcAddr"));
    if (!get) return fail("Vulkan loader has no instance entry point");
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(get(VK_NULL_HANDLE, "vkCreateInstance"));
    const auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(get(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    if (!create) return fail("Vulkan instance creation is unavailable");
    uint32_t api = VK_API_VERSION_1_0;
    if (enumerateVersion && enumerateVersion(&api) != VK_SUCCESS) api = VK_API_VERSION_1_0;
    result["loaderVersion"] = version(api);
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Suite hardware inventory";
    app.apiVersion = api >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    const auto created = create(&info, nullptr, &instance);
    if (created != VK_SUCCESS) return fail(QStringLiteral("Vulkan instance failed (%1)").arg(created));
    const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(get(instance, "vkDestroyInstance"));
    const auto cleanup = qScopeGuard([&] { if (destroy) destroy(instance, nullptr); });
    const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get(instance, "vkEnumeratePhysicalDevices"));
    const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get(instance, "vkGetPhysicalDeviceProperties"));
    const auto queues = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(get(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    const auto memory = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get(instance, "vkGetPhysicalDeviceMemoryProperties"));
    const auto memory2 = app.apiVersion >= VK_API_VERSION_1_1
        ? reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(get(instance, "vkGetPhysicalDeviceMemoryProperties2")) : nullptr;
    const auto extensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(get(instance, "vkEnumerateDeviceExtensionProperties"));
    if (!destroy || !enumerate || !properties || !queues || !memory || !extensions)
        return fail("Vulkan device inventory entry points are incomplete");
    uint32_t count = 0;
    auto status = enumerate(instance, &count, nullptr);
    if (status != VK_SUCCESS || count > 128) return fail("Could not enumerate Vulkan devices");
    if (!count) return fail("No Vulkan devices detected");
    std::vector<VkPhysicalDevice> devices(count);
    status = enumerate(instance, &count, devices.data());
    if (status != VK_SUCCESS) return fail("Vulkan device inventory changed; refresh to retry");
    QVariantList entries;
    for (uint32_t i = 0; i < count; ++i) {
        const auto device = devices[i];
        VkPhysicalDeviceProperties props{};
        properties(device, &props);
        QVariantMap entry{{"name", QString::fromUtf8(props.deviceName)}, {"type", deviceType(props.deviceType)},
                          {"apiVersion", version(props.apiVersion)}, {"vendorId", props.vendorID},
                          {"deviceId", props.deviceID}, {"driverVersionRaw", props.driverVersion}};
        // Driver-version encoding is vendor-defined. Preserve the raw value.
        uint32_t queueCount = 0;
        queues(device, &queueCount, nullptr);
        if (queueCount > 1024) return fail("Invalid Vulkan queue inventory");
        std::vector<VkQueueFamilyProperties> families(queueCount);
        if (queueCount) queues(device, &queueCount, families.data());
        bool compute = false, graphics = false;
        for (uint32_t q = 0; q < queueCount; ++q) {
            if (!families[q].queueCount) continue;
            compute |= (families[q].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
            graphics |= (families[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        }
        entry["compute"] = compute; entry["graphics"] = graphics;
        entry["maxComputeInvocations"] = props.limits.maxComputeWorkGroupInvocations;
        uint32_t extCount = 0;
        bool budgetAvailable = false;
        if (extensions(device, nullptr, &extCount, nullptr) == VK_SUCCESS && extCount <= 4096) {
            std::vector<VkExtensionProperties> ext(extCount);
            if (extCount && extensions(device, nullptr, &extCount, ext.data()) == VK_SUCCESS)
                for (uint32_t e = 0; e < extCount; ++e)
                    budgetAvailable |= std::strcmp(ext[e].extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0;
        }
        VkPhysicalDeviceMemoryProperties mem{};
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
        budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
        if (budgetAvailable && memory2) {
            VkPhysicalDeviceMemoryProperties2 p{};
            p.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
            p.pNext = &budget;
            memory2(device, &p); mem = p.memoryProperties;
        } else { memory(device, &mem); budgetAvailable = false; }
        if (mem.memoryHeapCount > VK_MAX_MEMORY_HEAPS) return fail("Invalid Vulkan memory inventory");
        QVariantList heaps;
        quint64 local = 0;
        for (uint32_t h = 0; h < mem.memoryHeapCount; ++h) {
            const bool deviceLocal = (mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
            if (deviceLocal) local += mem.memoryHeaps[h].size;
            QVariantMap heap{{"bytes", QVariant::fromValue<qulonglong>(mem.memoryHeaps[h].size)}, {"deviceLocal", deviceLocal}};
            if (budgetAvailable) {
                heap["budgetBytes"] = QVariant::fromValue<qulonglong>(budget.heapBudget[h]);
                heap["usageBytes"] = QVariant::fromValue<qulonglong>(budget.heapUsage[h]);
            }
            heaps.append(heap);
        }
        entry["deviceLocalBytes"] = QVariant::fromValue<qulonglong>(local);
        entry["memoryBudgetAvailable"] = budgetAvailable;
        entry["heaps"] = heaps;
        entries.append(entry);
    }
    result["available"] = true;
    result["devices"] = entries;
    result["reason"] = "Inventory only; processing uses the CPU until operations are implemented and benchmarked";
    return result;
}

int probeMain(int argc, char **argv) {
    if (argc != 2 || std::strcmp(argv[1], "--suite-gpu-probe") != 0) return -1;
    QCoreApplication app(argc, argv);
    QFile output;
    if (!output.open(stdout, QIODevice::WriteOnly)) return 2;
    const auto json = QJsonDocument::fromVariant(vulkanInventory()).toJson(QJsonDocument::Compact) + '\n';
    return output.write(json) == json.size() ? 0 : 2;
}
}
