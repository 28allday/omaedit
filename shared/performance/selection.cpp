#include "selection.h"
#include "benchmark.h"
namespace SuitePerformance {
QVariantMap selectBackend(const QVector<double> &cpu,const QVector<double> &gpu,bool agreement) {
    QVariantMap result{{"schemaVersion",1},{"backend","cpu"},{"requiredSavingFraction",.2},{"requiredSavingMs",.5},{"agreement",agreement}};
    if(!agreement){result["reason"]="CPU/GPU pixel agreement was not verified.";return result;}
    if(cpu.size()<3 || cpu.size()!=gpu.size() || cpu.size()>15){result["reason"]="At least three paired timing samples are required.";return result;}
    const auto a=statistics(cpu),b=statistics(gpu);result["cpu"]=a;result["vulkan"]=b;
    if(a.contains("error") || b.contains("error")){result["reason"]="Invalid timing samples.";return result;}
    const auto fastestCpu=a.value("minMs").toDouble(),slowestGpu=b.value("maxMs").toDouble();
    result["medianSpeedup"]=b.value("medianMs").toDouble()>0 ? a.value("medianMs").toDouble()/b.value("medianMs").toDouble() : 0.;
    if(fastestCpu<=0 || slowestGpu<=0 || slowestGpu>fastestCpu*.8 || fastestCpu-slowestGpu<.5) {
        result["reason"]="Vulkan did not show a consistent measured saving; CPU retained.";return result;
    }
    result["backend"]="vulkan";result["reason"]="Vulkan consistently saved at least 20% and 0.5 ms in the local comparison.";return result;
}
}
