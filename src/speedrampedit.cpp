#include "speedrampedit.h"
#include "retimemap.h"
#include <QCoreApplication>
#include <algorithm>
#include <cmath>

namespace {
QString message(const char *s){return QCoreApplication::translate("SpeedRamp",s);}
QVariantMap fail(const QString &s){return {{"valid",false},{"error",s}};}
double number(const QVariant &v) {
    switch(v.metaType().id()) {
    case QMetaType::Double:case QMetaType::Float:case QMetaType::Int:case QMetaType::UInt:
    case QMetaType::LongLong:case QMetaType::ULongLong:return v.toDouble();
    default:return NAN;
    }
}
double rate(const QVariantMap &s,double phase) {
    if(s.value("easing")=="smooth")phase=phase*phase*(3-2*phase);
    return s.value("speed").toDouble()+(s.value("endSpeed").toDouble()-s.value("speed").toDouble())*phase;
}
void append(QVariantList &to,const QVariantMap &part) {
    auto list=part.value("segments").toList();if(list.isEmpty())return;
    auto first=list[0].toMap();first["source"]=part.value("sourceIn");list[0]=first;to+=list;
}
QVariantMap reversed(const RetimeMap &map) {
    auto result=map.definition();QVariantList list;
    const auto segments=result.value("segments").toList();int end=map.frames();
    for(int i=segments.size()-1;i>=0;--i) {
        const auto old=segments[i].toMap();auto s=old;
        s["speed"]=-old.value("endSpeed").toDouble();s["endSpeed"]=-old.value("speed").toDouble();
        s["curveStart"]=1-old.value("curveEnd").toDouble();s["curveEnd"]=1-old.value("curveStart").toDouble();
        s["source"]=map.sample(end-1).nextSource;list<<s;end-=old.value("frames").toInt();
    }
    result["sourceIn"]=list[0].toMap().value("source");result["segments"]=list;return result;
}
}

QVariantMap SpeedRampEdit::inspect(const QVariantMap &definition) {
    RetimeMap map;QString error;if(!map.set(definition,&error))return fail(error);
    QVariantList rows;int start=0;
    for(const auto &v:map.definition().value("segments").toList()) {
        auto s=v.toMap();const int count=s.value("frames").toInt();
        s["start"]=start;s["sourceStart"]=map.sample(start).source;s["sourceEnd"]=map.sample(start+count-1).nextSource;
        s["fromRate"]=rate(s,s.value("curveStart").toDouble());s["toRate"]=rate(s,s.value("curveEnd").toDouble());
        rows<<s;start+=count;
    }
    return {{"valid",true},{"error",""},{"definition",map.definition()},{"segments",rows},{"frames",map.frames()},
        {"sourceIn",map.sample(0).source},{"sourceEnd",map.sample(map.frames()-1).nextSource}};
}

QVariantMap SpeedRampEdit::preset(const QVariantMap &entry) {
    if(entry.contains("speedPattern")) {
        const auto pattern=entry.value("speedPattern").toMap();
        for(auto it=pattern.begin();it!=pattern.end();++it)if(!QStringList{"version","sourceSpan","definition"}.contains(it.key()))
            return fail(message(QT_TRANSLATE_NOOP("SpeedRamp","Unknown speed preset option.")));
        const double version=number(pattern.value("version")),span=number(pattern.value("sourceSpan"));
        RetimeMap map;QString error;
        if(version!=1 || !std::isfinite(span) || span<0 || !map.set(pattern.value("definition").toMap(),&error))
            return fail(error.isEmpty()?message(QT_TRANSLATE_NOOP("SpeedRamp","The speed preset is invalid.")):error);
        const auto bounds=map.sourceBounds();
        if(std::abs(map.sourceLength()-std::max(1.,span))>1e-6 || std::abs(bounds.first)>1e-6 || std::abs(bounds.second-span)>1e-6)
            return fail(message(QT_TRANSLATE_NOOP("SpeedRamp","The speed preset has invalid source bounds.")));
        return {{"valid",true},{"pattern",QVariantMap{{"version",1},{"sourceSpan",span},{"definition",map.definition()}}}};
    }
    RetimeMap parent;QString error;
    if(!parent.set(entry.value("retime").toMap(),&error))return fail(error);
    const auto visible=prepare(entry,parent.sourceLength());if(!visible.value("valid").toBool())return visible;
    RetimeMap map;if(!map.set(visible.value("definition").toMap(),&error))return fail(error);
    const auto bounds=map.sourceBounds();const double span=bounds.second-bounds.first;
    auto definition=map.definition();definition["sourceIn"]=definition.value("sourceIn").toDouble()-bounds.first;
    definition["sourceLength"]=std::max(1.,span);
    auto list=definition.value("segments").toList();
    for(auto &v:list){auto s=v.toMap();if(s.contains("source"))s["source"]=s.value("source").toDouble()-bounds.first;v=s;}
    definition["segments"]=list;
    return preset({{"speedPattern",QVariantMap{{"version",1},{"sourceSpan",span},{"definition",definition}}}});
}

QVariantMap SpeedRampEdit::fitPreset(const QVariantMap &input,const QVariantMap &destination) {
    const auto checked=preset({{"speedPattern",input}});if(!checked.value("valid").toBool())return checked;
    const auto pattern=checked.value("pattern").toMap();const double span=pattern.value("sourceSpan").toDouble();
    RetimeMap source,target;QString error;
    if(!source.set(pattern.value("definition").toMap(),&error) || !target.set(destination,&error))return fail(error);
    const auto bounds=target.sourceBounds();const auto point=target.sample(0);
    const double scale=span>1e-9?(bounds.second-bounds.first)/span:double(target.frames())/source.frames();
    if(!std::isfinite(scale) || scale<=0 || source.frames()*scale>100000000.)
        return fail(message(QT_TRANSLATE_NOOP("SpeedRamp","This speed preset cannot fit the destination source range.")));
    auto definition=source.definition();const double origin=span>1e-9?bounds.first:point.source;
    definition["sourceIn"]=origin+definition.value("sourceIn").toDouble()*scale;
    definition["sourceLength"]=target.sourceLength();
    QVariantList list;double end=0;int roundedEnd=0;
    for(const auto &v:source.definition().value("segments").toList()) {
        auto s=v.toMap();const int frames=s.value("frames").toInt();end+=frames*scale;
        const int newEnd=std::max(roundedEnd+1,int(std::round(end))),duration=newEnd-roundedEnd;
        const double correction=frames*scale/duration;
        s["frames"]=duration;s["speed"]=s.value("speed").toDouble()*correction;s["endSpeed"]=s.value("endSpeed").toDouble()*correction;
        if(s.contains("source"))s["source"]=origin+s.value("source").toDouble()*scale;
        if(span<=1e-9)s["holdBefore"]=point.nextSource<point.source || point.holdBefore;
        list<<s;roundedEnd=newEnd;
    }
    definition["segments"]=list;return inspect(definition);
}

QVariantMap SpeedRampEdit::prepare(const QVariantMap &entry,double sourceLength) {
    if(entry.isEmpty() || entry.value("gap").toBool() || entry.contains("compoundJson") || !entry.value("title").toString().isEmpty())
        return fail(message(QT_TRANSLATE_NOOP("SpeedRamp","Choose a media clip to edit its speed segments.")));
    const int in=entry.value("in",-1).toInt(),out=entry.value("out",-1).toInt();
    RetimeMap parent;QString error;
    if(entry.contains("retime")) {
        if(!parent.set(entry.value("retime").toMap(),&error))return fail(error);
        if(parent.sourceLength()>sourceLength+1e-6)return fail(message(QT_TRANSLATE_NOOP("SpeedRamp","The source is shorter than this clip's timing map.")));
        auto definition=parent.definition();definition["sourceLength"]=sourceLength;
        if(!parent.set(definition,&error))return fail(error);
    } else {
        const double speed=entry.value("speed",1.).toDouble();const int length=entry.value("length").toInt();
        if(!std::isfinite(speed) || std::abs(speed)<.05 || std::abs(speed)>20 || length<1 || length>100000000 || !std::isfinite(sourceLength) || sourceLength<1 || sourceLength>100000000)
            return fail(message(QT_TRANSLATE_NOOP("SpeedRamp","The clip has invalid source bounds for speed editing.")));
        // A constant timewarp can include a rounded tail frame. Keep that
        // frame as a hold, rather than claiming media beyond the real end.
        const int moving=std::min(length,int(std::floor(sourceLength/std::abs(speed)+1e-7)));
        QVariantList segments;if(moving>0)segments<<QVariantMap{{"frames",moving},{"speed",speed}};
        if(moving<length)segments<<QVariantMap{{"frames",length-moving},{"speed",0.},{"source",speed<0?0.:std::max(0.,sourceLength-1e-7)}};
        if(!parent.set({{"sourceIn",speed<0?sourceLength:0.},{"sourceLength",sourceLength},{"segments",segments}},&error))return fail(error);
    }
    auto visible=parent.window(in,out-in+1,&error);if(visible.isEmpty())return fail(error);
    if(entry.contains("freeze")) {
        const auto point=parent.sample(entry.value("freeze").toInt());
        if(!point.valid)return fail(message(QT_TRANSLATE_NOOP("SpeedRamp","The held frame is outside the source.")));
        visible["sourceIn"]=point.source;
        visible["segments"]=QVariantList{QVariantMap{{"frames",out-in+1},{"speed",0.},{"holdBefore",point.nextSource<point.source || point.holdBefore}}};
    }
    auto result=inspect(visible);result["parent"]=parent.definition();return result;
}

QVariantMap SpeedRampEdit::splice(const QVariantMap &entry,const QVariantMap &parentDefinition,const QVariantMap &definition,bool normal,double sourceRate) {
    RetimeMap parent,draft;QString error;
    if(!parent.set(parentDefinition,&error) || !draft.set(definition,&error))return fail(error);
    if(std::abs(parent.sourceLength()-draft.sourceLength())>1e-6)
        return fail(message(QT_TRANSLATE_NOOP("SpeedRamp","The draft does not match this source. Reopen the speed editor.")));
    const int in=entry.value("in").toInt(),out=entry.value("out").toInt();
    if(in<0 || out<in || out>=parent.frames())return fail(message(QT_TRANSLATE_NOOP("SpeedRamp","The clip trim changed. Reopen the speed editor.")));
    const int delta=draft.frames()-(out-in+1);
    if(draft.frames()<=entry.value("mixIn").toInt()+entry.value("mixOut").toInt())
        return fail(message(QT_TRANSLATE_NOOP("SpeedRamp","The new duration is shorter than the clip's transitions. Lengthen it or shorten the transitions.")));
    if(normal) {
        const auto reset=edit(definition,"normal",0);
        if(!reset.value("valid").toBool() || reset.value("definition").toMap()!=draft.definition())
            return fail(message(QT_TRANSLATE_NOOP("SpeedRamp","Choose Normal speed before clearing the speed segments.")));
        const double boundary=draft.definition().value("sourceIn").toDouble();
        const int first=int(std::floor(boundary+1e-9));
        const double phase=std::max(0.,boundary-first);
        QVariantMap result{{"valid",true},{"error",""},{"in",first},{"out",first+draft.frames()-1},{"frames",draft.frames()},
            {"length",int(std::ceil(parent.sourceLength()-phase-1e-9))},{"delta",delta},{"ratio",double(draft.frames())/(out-in+1)}};
        if(entry.contains("sourceOrigin") || phase>0. || parent.sourceLength()!=std::floor(parent.sourceLength())) {
            if(!std::isfinite(sourceRate) || sourceRate<=0)
                return fail(message(QT_TRANSLATE_NOOP("SpeedRamp","The clip has invalid source bounds for speed editing.")));
            result["sourceOrigin"]=phase/sourceRate;
        }
        return result;
    }
    QVariantList joined;
    if(in>0)append(joined,parent.window(0,in));append(joined,draft.definition());
    if(out+1<parent.frames())append(joined,parent.window(out+1,parent.frames()-out-1));
    RetimeMap result;
    if(!result.set({{"sourceIn",joined[0].toMap().value("source")},{"sourceLength",parent.sourceLength()},{"segments",joined}},&error))return fail(error);
    return {{"valid",true},{"error",""},{"retime",result.definition()},{"in",in},{"out",in+draft.frames()-1},
        {"frames",draft.frames()},{"length",result.frames()},{"delta",delta},{"ratio",double(draft.frames())/(out-in+1)}};
}

QVariantMap SpeedRampEdit::edit(const QVariantMap &definition,const QString &action,int index,const QVariantMap &options) {
    RetimeMap map;QString error;if(!map.set(definition,&error))return fail(error);
    auto result=map.definition();auto segments=result.value("segments").toList();
    const auto rows=inspect(result).value("segments").toList();
    auto invalid=[&]{return fail(message(QT_TRANSLATE_NOOP("SpeedRamp","Choose a valid speed segment and settings.")));};
    const QStringList allowed=action=="set"?QStringList{"speed","endSpeed","frames","easing","keepSource"}
        :action=="split"?QStringList{"frame"}:action=="hold"?QStringList{"frame","frames"}:QStringList{};
    for(auto it=options.begin();it!=options.end();++it)if(!allowed.contains(it.key()))return invalid();
    if(action=="split") {
        const double frame=number(options.value("frame"));
        if(!std::isfinite(frame) || frame!=std::floor(frame) || frame<1 || frame>=map.frames())return invalid();
        result=map.blade(int(frame),&error);if(result.isEmpty())return fail(error);
        return inspect(result);
    }
    if(action=="reverse")return inspect(reversed(map));
    if(action=="normal") {
        const auto bounds=map.sourceBounds();
        double first=bounds.first;
        int count=std::max(1,int(std::llrint(bounds.second-bounds.first)));
        if(bounds.second-bounds.first<1e-9)first=map.sourceFrame(0,1.,1.);
        const double available=map.sourceLength()-first;
        count=std::min(count,std::max(1,int(std::ceil(available-1e-9))));
        const int whole=std::min(count,int(std::floor(available+1e-9)));
        QVariantList normal;
        if(whole>0)normal<<QVariantMap{{"frames",whole},{"speed",1.}};
        if(whole<count)normal<<QVariantMap{{"frames",count-whole},{"speed",std::max(0.,available-whole)}};
        result["sourceIn"]=first;result["segments"]=normal;
        return inspect(result);
    }
    if(action=="replay" || action=="rewind") {
        QVariantList list;append(list,result);
        if(action=="rewind")append(list,reversed(map));
        append(list,result);result["segments"]=list;return inspect(result);
    }
    if(action=="hold") {
        const double at=number(options.value("frame")),count=number(options.value("frames"));
        if(!std::isfinite(at) || at!=std::floor(at) || at<0 || at>=map.frames() || !std::isfinite(count) || count!=std::floor(count) || count<1 || count>100000000)return invalid();
        const auto point=map.sample(int(at));const double source=point.source;
        QVariantList list;if(at>0)append(list,map.window(0,int(at)));
        list<<QVariantMap{{"frames",int(count)},{"speed",0.},{"source",source},{"holdBefore",point.nextSource<point.source || point.holdBefore}};
        append(list,map.window(int(at),map.frames()-int(at)));result["segments"]=list;
        if(at==0)result["sourceIn"]=source;
        return inspect(result);
    }
    if(index<0 || index>=segments.size())return invalid();
    if(action=="join") {
        if(index==0)return invalid();
        const auto left=rows[index-1].toMap(),right=rows[index].toMap();
        if(std::abs(left.value("sourceEnd").toDouble()-right.value("sourceStart").toDouble())>1e-6)
            return fail(message(QT_TRANSLATE_NOOP("SpeedRamp","These segments have a source jump. Keep their boundary or remove the replay first.")));
        const int frames=left.value("frames").toInt()+right.value("frames").toInt();
        segments[index-1]=QVariantMap{{"frames",frames},{"speed",(right.value("sourceEnd").toDouble()-left.value("sourceStart").toDouble())/frames},
            {"source",left.value("sourceStart")}};segments.removeAt(index);
    } else if(action=="set") {
        auto s=segments[index].toMap();const auto old=rows[index].toMap();
        const bool curve=options.contains("speed") || options.contains("endSpeed") || options.contains("easing");
        if(curve) {s["speed"]=old.value("fromRate");s["endSpeed"]=old.value("toRate");s["curveStart"]=0.;s["curveEnd"]=1.;}
        for(const auto &key:QStringList{"speed","endSpeed","frames","easing"})if(options.contains(key))s[key]=options.value(key);
        if(options.contains("keepSource") && options.value("keepSource").metaType().id()!=QMetaType::Bool)return invalid();
        const double first=number(s.value("speed")),last=number(s.value("endSpeed"));
        if(!std::isfinite(first) || !std::isfinite(last) || std::abs(first)>20 || std::abs(last)>20)return invalid();
        if(first!=0 || last!=0)s.remove("holdBefore");
        const double average=(first+last)/2,span=old.value("sourceEnd").toDouble()-old.value("sourceStart").toDouble();
        if(curve && !options.contains("frames") && options.value("keepSource",true).toBool() && std::abs(average)>.000001 && span/average>0) {
            const double count=std::floor(span/average+1e-7);
            if(count<1 || count>100000000)return invalid();s["frames"]=int(count);
        }
        // Keep neighbouring sections on their existing material when a
        // whole-frame duration rounds the edited range by a fraction.
        s["source"]=old.value("sourceStart");segments[index]=s;
        if(index+1<segments.size()){auto next=segments[index+1].toMap();next["source"]=rows[index+1].toMap().value("sourceStart");segments[index+1]=next;}
    } else return invalid();
    result["segments"]=segments;return inspect(result);
}
