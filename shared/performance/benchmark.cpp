#include "benchmark.h"
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
#include <exception>
#include <numeric>
namespace SuitePerformance {
QVariantMap statistics(const QVector<double> &values) {
    if (values.isEmpty()) return {{"error","No timing samples."}};
    for (double value : values) if (!std::isfinite(value) || value<0) return {{"error","Timing samples must be finite and non-negative."}};
    auto sorted=values;std::sort(sorted.begin(),sorted.end());
    const auto percentile=[&](double p) {const double at=(sorted.size()-1)*p;const int low=int(at),high=qMin(low+1,int(sorted.size()-1));return sorted[low]+(sorted[high]-sorted[low])*(at-low);};
    const double mean=std::accumulate(values.begin(),values.end(),0.)/values.size();
    double variance=0;for(double value : values) variance+=(value-mean)*(value-mean);variance/=values.size();
    QVariantList samples;for(double value : values)samples.append(value);
    return {{"count",values.size()},{"samplesMs",samples},{"minMs",sorted.first()},{"medianMs",percentile(.5)},{"p95Ms",percentile(.95)},{"maxMs",sorted.last()},{"meanMs",mean},{"populationStdDevMs",std::sqrt(variance)}};
}
QVariantMap measure(const std::function<bool(QString*)> &operation,const Options &options) {
    if (!operation || options.warmups<0 || options.warmups>100 || options.iterations<1 || options.iterations>10000)
        return {{"status","failed"},{"error","Invalid benchmark operation or sample counts."}};
    QVector<double> samples;
    try {
        for(int i=-options.warmups;i<options.iterations;++i) {
            QString error;QElapsedTimer timer;timer.start();
            const bool ok=operation(&error);const double ms=timer.nsecsElapsed()/1000000.;
            if(!ok) return {{"status","failed"},{"error",error.isEmpty() ? QString("Operation failed.") : error},{"completedIterations",samples.size()},{"duringWarmup",i<0}};
            if(i>=0)samples.append(ms);
        }
    } catch(const std::exception &e) {return {{"status","failed"},{"error",QString::fromUtf8(e.what())},{"completedIterations",samples.size()}};}
    auto result=statistics(samples);result["status"]="complete";result["warmups"]=options.warmups;return result;
}
}
