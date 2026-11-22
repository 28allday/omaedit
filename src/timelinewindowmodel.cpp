#include "timelinewindowmodel.h"
#include "clipentry.h"
#include "mixshape.h"
#include "audiotimeline.h"
#include <QCoreApplication>
#include <QQmlEngine>
#include <QTimer>
#include <cmath>

static void registerTimelineWindow() { qmlRegisterType<TimelineWindowModel>("omaedit",1,0,"TimelineWindowModel"); }
Q_COREAPP_STARTUP_FUNCTION(registerTimelineWindow)

QVariant TimelineWindowModel::data(const QModelIndex &index,int role) const {
    if(!index.isValid() || index.row()<0 || index.row()>=m_rows.size())return {};
    const int sourceIndex=m_rows[index.row()];
    if(role==SourceIndexRole)return sourceIndex;
    if(role==EntryRole)return m_source.value(sourceIndex);
    if(role==TimelineStartRole)return m_spans[sourceIndex].start;
    if(role==TimelineFramesRole)return m_spans[sourceIndex].frames;
    if(role==BlockLeadRole)return m_spans[sourceIndex].lead;
    if(role==BlockTailRole)return m_spans[sourceIndex].tail;
    if(role==BlockBeforeRole)return m_spans[sourceIndex].before;
    if(role==MostBeforeRole)return m_spans[sourceIndex].mostBefore;
    if(role==MostAfterRole)return m_spans[sourceIndex].mostAfter;
    return {};
}
void TimelineWindowModel::indexSpans() {
    m_spans.clear();m_spans.reserve(m_source.size());
    const auto blocks=MixShape::blocks(m_source);
    for(int i=0;i<m_source.size();++i){
        const auto e=m_source[i].toMap();
        // The block meets its neighbour at the cut a transition sits across.
        auto block=blocks[i];
        double start=e.value(ClipKey::start).toDouble()-block.lead,frames=e.value(ClipKey::frames).toDouble()+block.lead-block.tail;
        const bool audio=e.value(ClipKey::audioOnly).toBool();
        if(e.contains(ClipKey::audioTiming)) {
            block={};
            AudioTimeline::Time at,duration;
            if(audio && AudioTimeline::readSpan(e,&at,&duration)) {
                start=double(static_cast<long double>(at.ticks)/at.rate*m_fps);
                frames=double(static_cast<long double>(duration.ticks)/duration.rate*m_fps);
            } else frames=0; // do not draw malformed precise cuts at rounded bounds
        }
        bool eligible=std::isfinite(start) && std::isfinite(frames) && frames>0;
        if(m_kind=="audio")eligible &= audio;
        else if(m_kind=="video")eligible &= !audio;
        else if(m_kind=="attached")eligible &= !e.value(ClipKey::gap).toBool() && !e.value(ClipKey::videoOnly).toBool() && e.value(ClipKey::title).toString().isEmpty();
        m_spans.append({start,start+frames,frames,eligible,block.lead,block.tail,block.before,block.mostBefore,block.mostAfter});
    }
}
void TimelineWindowModel::setSource(const QVariantList &value) {
    if(m_source==value)return;
    // Decorations and parameter edits retain their objects. Structural edits
    // can change source addresses, so only those reset the presentation.
    bool structural=value.size()!=m_source.size();
    if(!structural)for(int i=0;i<value.size();++i){
        const auto a=m_source[i].toMap(),b=value[i].toMap();
        for(const auto *key:{"track","index","start","path","in","out","clipId"})if(a.value(key)!=b.value(key)){structural=true;break;}
        if(structural)break;
    }
    if(structural){beginResetModel();m_rows.clear();m_source=value;indexSpans();endResetModel();}
    else {
        QList<int> changed;for(int row=0;row<m_rows.size();++row)if(m_source[m_rows[row]]!=value[m_rows[row]])changed.append(row);
        m_source=value;indexSpans();
        for(int row:changed)emit dataChanged(index(row),index(row),{EntryRole,TimelineStartRole,TimelineFramesRole,BlockLeadRole,BlockTailRole,BlockBeforeRole,MostBeforeRole,MostAfterRole});
    }
    emit sourceChanged();schedule();
}
void TimelineWindowModel::setFps(double v) {
    if(!std::isfinite(v) || v<=0 || m_fps==v)return;
    m_fps=v;indexSpans();emit fpsChanged();
    if(!m_rows.isEmpty())emit dataChanged(index(0),index(m_rows.size()-1),{TimelineStartRole,TimelineFramesRole});
    schedule();
}
void TimelineWindowModel::setFirstFrame(double v){if(!std::isfinite(v)||m_first==v)return;m_first=v;emit windowChanged();schedule();}
void TimelineWindowModel::setLastFrame(double v){if(!std::isfinite(v)||m_last==v)return;m_last=v;emit windowChanged();schedule();}
void TimelineWindowModel::setMinimumFrames(double v){if(!std::isfinite(v)||v<0||m_minimum==v)return;m_minimum=v;emit windowChanged();schedule();}
void TimelineWindowModel::setNarrowFrames(double v){if(!std::isfinite(v)||v<0||m_narrow==v)return;m_narrow=v;emit windowChanged();schedule();}
void TimelineWindowModel::setKind(const QString &v){if(m_kind==v)return;m_kind=v;indexSpans();emit windowChanged();schedule();}
void TimelineWindowModel::setActive(bool v){if(m_active==v)return;m_active=v;emit windowChanged();schedule();}
void TimelineWindowModel::setPinned(const QVariantList &v){if(m_pinned==v)return;m_pinned=v;m_pinSet.clear();for(const auto &i:v){bool ok=false;const int n=i.toInt(&ok);if(ok&&n>=0)m_pinSet.insert(n);}emit windowChanged();schedule();}
void TimelineWindowModel::schedule(){if(m_pending)return;m_pending=true;QTimer::singleShot(0,this,[this]{m_pending=false;refresh();});}
void TimelineWindowModel::refresh() {
    QList<int> wanted;
    if(m_active && m_last>=m_first)for(int i=0;i<m_spans.size();++i){const auto &s=m_spans[i];if(s.eligible && ((s.start<=m_last && qMax(s.end,s.start+m_minimum)>=m_first && s.frames>=m_narrow)||m_pinSet.contains(i)))wanted.append(i);}
    // Both sequences are sorted source addresses. Merge in linear time and
    // batch adjacent removals/insertions, avoiding whole-view reconstruction.
    int row=0,next=0;
    while(row<m_rows.size() || next<wanted.size()){
        if(row<m_rows.size() && next<wanted.size() && m_rows[row]==wanted[next]){++row;++next;continue;}
        if(row<m_rows.size() && (next==wanted.size() || m_rows[row]<wanted[next])){
            int end=row;while(end+1<m_rows.size() && (next==wanted.size() || m_rows[end+1]<wanted[next]))++end;
            beginRemoveRows({},row,end);m_rows.remove(row,end-row+1);endRemoveRows();
        }else{
            int end=next;while(end+1<wanted.size() && (row==m_rows.size() || wanted[end+1]<m_rows[row]))++end;
            beginInsertRows({},row,row+end-next);for(int i=next;i<=end;++i)m_rows.insert(row+i-next,wanted[i]);endInsertRows();row+=end-next+1;next=end+1;
        }
    }
}
