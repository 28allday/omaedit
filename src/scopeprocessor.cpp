#include "scopeprocessor.h"
#include "scopes.h"
#include "workingframe.h"
#include <QMutex>

QVariantMap ScopeProcessing::measured(const QVariantMap &options,const QString &outputSpace) {
    auto all=options;all["width"]=256;all["height"]=200;all["outputSpace"]=outputSpace;
    return all;
}

namespace {
struct Kept { qint64 key; ScopeProcessing::Counted counted; };
QMutex keptLock;
QList<Kept> &kept() { static QList<Kept> list; return list; }
// More than playback queues ahead; each is a megabyte or two.
constexpr int kKept=12;
}
void ScopeProcessing::attach(const QImage &shown,const Counted &counted) {
    if(shown.isNull() || !counted.cells)return;
    QMutexLocker lock(&keptLock);
    while(kept().size()>=kKept)kept().removeFirst();
    kept().append({shown.cacheKey(),counted});
}
ScopeProcessing::Counted ScopeProcessing::counted(const QImage &shown) {
    if(shown.isNull())return {};
    QMutexLocker lock(&keptLock);
    for(const auto &one:kept())if(one.key==shown.cacheKey())return one.counted;
    return {};
}

ScopeProcessing::Result ScopeProcessing::render(const Request &request,const std::atomic<bool> &cancelled) {
    Result result;QElapsedTimer clock;clock.start();result.frameKey=request.frame.cacheKey();
    if(cancelled || request.frame.isNull())return result;
    if(request.counts) {
        // Counted where the frame was drawn: painted from the counts.
        result.images=Scopes::renderCounted(*request.counts,measured(request.options,request.outputSpace),200);
        if(result.images.size()!=4){result.error="Could not render scopes.";result.images.clear();}
        result.milliseconds=clock.nsecsElapsed()/1000000.;return result;
    }
    // Measure the entire delivered frame. A nearest-neighbour thumbnail can
    // alternate between colours on fine moving detail and miss peak levels.
    auto signal=request.frame;
    if(WorkingFrame::isTagged(signal)) {
        const auto working=WorkingFrame::fromTagged(signal,&result.error);
        if(!working.isNull())signal=working.output(request.outputSpace,&result.error);
        else signal={};
    }
    if(cancelled || signal.isNull() || !result.error.isEmpty())return result;
    result.images=Scopes::renderAll(signal,measured(request.options,request.outputSpace),200,&cancelled,request.maximumParts);
    if(cancelled)return {};
    for(const auto &image:result.images)if(image.isNull()){result.error="Could not render scopes.";result.images.clear();break;}
    result.milliseconds=clock.nsecsElapsed()/1000000.;return result;
}

int ScopeProcessor::playbackParts() {
    static const int parts=[] {
        bool ok=false;const int value=qEnvironmentVariableIntValue("OMAEDIT_SCOPE_PLAYBACK_PARTS",&ok);
        return ok && value>0 ? qMin(value,16) : 4;
    }();
    return parts;
}
ScopeProcessor::ScopeProcessor(QObject *parent,Render render):QObject(parent),m_render(std::move(render)) {
    m_timer.setSingleShot(true);m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer,&QTimer::timeout,this,&ScopeProcessor::schedule);
    // Cancelled jobs do not invoke completion, but still release the worker.
    connect(&m_queue,&SuiteProcessing::LatestQueue::changed,this,[this] {
        if(!m_queue.information().value("busy").toBool()){m_runningBytes=0;schedule();}
    });
}
ScopeProcessor::~ScopeProcessor(){m_stopping=true;cancel();}
void ScopeProcessor::cancel() {
    ++m_epoch;m_timer.stop();m_pending={};m_hasPending=false;m_hasContext=false;
    m_queue.cancel(1);
}
void ScopeProcessor::submit(const ScopeProcessing::Request &request,bool continuous) {
    if(m_stopping)return;
    if(request.frame.isNull()){cancel();return;}
    if(!continuous || !m_hasContext || request.options!=m_options || request.outputSpace!=m_outputSpace)cancel();
    m_hasContext=true;m_options=request.options;m_outputSpace=request.outputSpace;
    if(m_hasPending)++m_coalesced;
    m_pending=request;m_hasPending=true;m_pendingClock.start();++m_submitted;
    // During playback the scopes share the machine with decoding and the
    // viewer: a few threads keep them live without starving the picture.
    if(continuous && m_pending.maximumParts==0)m_pending.maximumParts=playbackParts();
    schedule();
}
void ScopeProcessor::schedule() {
    if(m_stopping || !m_hasPending || m_queue.information().value("busy").toBool())return;
    const int remaining=m_clock.isValid()?qMax(0,40-int(qMin<qint64>(m_clock.elapsed(),40))):0;
    if(remaining>0){if(!m_timer.isActive())m_timer.start(remaining);return;}
    m_timer.stop();auto request=std::move(m_pending);m_pending={};m_hasPending=false;
    const auto epoch=m_epoch;const auto requested=m_pendingClock;auto render=m_render;
    m_runningBytes=request.frame.sizeInBytes();m_clock.start();
    m_queue.submit(1,m_runningBytes,[request,render](const std::atomic<bool> &cancelled) {
        return QVariant::fromValue(render(request,cancelled));
    },[this,epoch,requested](const QVariant &value,const QString &error) {
        if(epoch!=m_epoch || m_stopping)return;
        auto result=value.value<ScopeProcessing::Result>();if(!error.isEmpty())result.error=error;
        ++m_delivered;m_lastProcessingMs=result.milliseconds;m_lastLatencyMs=requested.nsecsElapsed()/1000000.;
        emit ready(result);
    });
}
QVariantMap ScopeProcessor::information() const {
    return {{"backend","cpu"},{"workerThreads",1},{"busy",m_queue.information().value("busy")},
        {"pendingFrames",int(m_hasPending)},{"maximumPendingFrames",1},
        {"pendingBytes",m_pending.frame.sizeInBytes()},{"runningInputBytes",m_runningBytes},
        {"submitted",m_submitted},{"coalesced",m_coalesced},{"delivered",m_delivered},
        {"lastProcessingMs",m_lastProcessingMs},{"lastRequestToResultMs",m_lastLatencyMs}};
}
