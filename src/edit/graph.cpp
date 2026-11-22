#include "graph.h"
#include <atomic>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace edit {

namespace {

// A value as it was set.
struct Value {
    enum Type { None, Int, Int64, Double, Text, Data } type = None;
    int i = 0;
    int64_t l = 0;
    double d = 0;
    QByteArray text;
    mutable QByteArray shown;  // its text form, for get()
    std::shared_ptr<void> data;
    int size = 0;
};

// The leading number of `text` (strtod/strtol reading: digits up to the
// first character that cannot belong to a number).
QByteArray leadingNumber(const QByteArray &text) {
    int i = 0;
    const int n = int(text.size());
    while (i < n && (text[i] == ' ' || text[i] == '\t')) ++i;
    const int start = i;
    if (i < n && (text[i] == '+' || text[i] == '-')) ++i;
    while (i < n && std::isdigit(uchar(text[i]))) ++i;
    if (i < n && text[i] == '.') {
        ++i;
        while (i < n && std::isdigit(uchar(text[i]))) ++i;
    }
    if (i < n && (text[i] == 'e' || text[i] == 'E')) {
        int j = i + 1;
        if (j < n && (text[j] == '+' || text[j] == '-')) ++j;
        if (j < n && std::isdigit(uchar(text[j]))) {
            while (j < n && std::isdigit(uchar(text[j]))) ++j;
            i = j;
        }
    }
    return text.mid(start, i - start);
}

int textToInt(const QByteArray &text) {
    if (text.startsWith('#')) {
        // A colour, #RRGGBB or #AARRGGBB, as RRGGBBAA.
        const unsigned rgb = text.mid(1).toUInt(nullptr, 16);
        const unsigned alpha = text.size() > 7 ? (rgb >> 24) : 0xff;
        return int((rgb << 8) | alpha);
    }
    if (text.startsWith("0x")) return int(text.mid(2).toUInt(nullptr, 16));
    const QByteArray digits = leadingNumber(text);
    const int dot = int(digits.indexOf('.')), exp = int(digits.indexOf('e'));
    QByteArray whole = digits;
    if (dot >= 0) whole = digits.left(dot);
    else if (exp >= 0) whole = digits.left(exp);
    bool ok = false;
    const long long v = whole.toLongLong(&ok);
    return ok ? int(v) : 0;
}

double textToDouble(const QByteArray &text) {
    const QByteArray digits = leadingNumber(text);
    double v = digits.toDouble();
    const int after = int(text.indexOf(digits)) + int(digits.size());
    if (!digits.isEmpty() && after < text.size() && text.at(after) == '%') v /= 100.0;  // a ratio
    return v;
}

}  // namespace

struct PlaylistEntry {
    std::shared_ptr<Bag> cut;
    int in = 0, out = -1, count = 0, repeat = 1, producerLength = 0;
    bool preserve = false;  // preserved: removed without being unmixed or decoupled
    // What refresh() last found in step: this cut, at this revision, with
    // these points. While all three hold, the cut is not read again.
    const Bag *syncedCut = nullptr;
    quint64 synced = 0;
    int syncedIn = 0, syncedOut = 0;
};

struct PlaylistState {
    std::vector<PlaylistEntry> list;
    std::shared_ptr<Bag> blank;
};

struct TractorState {
    std::vector<std::shared_ptr<Bag>> tracks, transitions;
};

// Counts every change to any graph object (see revision() in graph.h).
static std::atomic<quint64> graphRevision{1};
static inline quint64 touch() { return graphRevision.fetch_add(1, std::memory_order_relaxed) + 1; }

struct Bag {
    std::vector<std::pair<QByteArray, Value>> values;
    std::vector<std::shared_ptr<Bag>> filters;
    std::shared_ptr<Bag> parent;  // a cut's source
    std::shared_ptr<PlaylistState> playlist;
    std::shared_ptr<TractorState> tractor;
    bool mix = false;                       // a mix's tractor
    std::weak_ptr<Bag> mixIn, mixOut;       // the mix before/after a cut; a mix's left/right cut
    quint64 written = 0;                    // the revision of the last write to a value

    Value *find(const char *name) {
        for (auto &[key, value] : values)
            if (key == name) return &value;
        return nullptr;
    }
    const Value *find(const char *name) const { return const_cast<Bag *>(this)->find(name); }
    Value &slot(const char *name) {
        // Handed out to be written. A playback cursor is not part of what
        // the graph holds, and moves with every picture shown.
        if (qstrcmp(name, "_position") != 0) written = touch();
        if (Value *v = find(name)) return *v;
        values.emplace_back(QByteArray(name), Value{});
        return values.back().second;
    }
    int number(const char *name) const {
        const Value *v = find(name);
        if (!v) return 0;
        switch (v->type) {
        case Value::Int: return v->i;
        case Value::Int64: return int(v->l);
        case Value::Double: return int(v->d);
        case Value::Text: return textToInt(v->text);
        default: return 0;
        }
    }
    void setNumber(const char *name, int value) {
        Value &v = slot(name);
        v = Value{};
        v.type = Value::Int;
        v.i = value;
    }
    std::shared_ptr<Bag> source() { return parent ? parent : nullptr; }
};

namespace {

std::shared_ptr<Bag> newBag() { return std::make_shared<Bag>(); }

std::shared_ptr<Bag> cutParentOf(const std::shared_ptr<Bag> &bag) { return bag && bag->number("_cut") && bag->parent ? bag->parent : bag; }

bool blankBag(const std::shared_ptr<Bag> &bag) {
    if (!bag) return true;
    const auto parent = cutParentOf(bag);
    const Value *r = parent->find("resource");
    return r && r->type == Value::Text && r->text == "blank";
}

int lengthOf(const std::shared_ptr<Bag> &b) { return b ? b->number("length") : 0; }
int inOf(const std::shared_ptr<Bag> &b) { return b ? b->number("in") : 0; }
int outOf(const std::shared_ptr<Bag> &b) { return b ? b->number("out") : 0; }
int playtimeOf(const std::shared_ptr<Bag> &b) { return outOf(b) - inOf(b) + 1; }

// A producer's in and out points, clamped to its length.
void setInOut(const std::shared_ptr<Bag> &b, int in, int out) {
    if (!b) return;
    const int length = lengthOf(b);
    if (in < 0) in = 0;
    else if (in >= length) in = std::max(0, length - 1);
    if (blankBag(b) && out >= lengthOf(b)) b->setNumber("length", out + 1);
    else if (out < 0 || out >= lengthOf(b)) out = std::max(0, lengthOf(b) - 1);
    if (out < in) std::swap(in, out);
    b->setNumber("in", in);
    b->setNumber("out", out);
}

// A cut of a producer: its own in and out over the same source.
std::shared_ptr<Bag> cutOf(const std::shared_ptr<Bag> &self, int in, int out) {
    auto result = newBag();
    auto parent = cutParentOf(self);
    if (in <= 0) in = 0;
    if ((out < 0 || out >= lengthOf(parent)) && !blankBag(self)) out = std::max(0, lengthOf(parent) - 1);
    result->setNumber("_cut", 1);
    result->parent = parent;
    result->setNumber("length", lengthOf(parent));
    if (const Value *a = parent->find("aspect_ratio")) result->slot("aspect_ratio") = *a;
    setInOut(result, in, out);
    return result;
}

void clearBag(const std::shared_ptr<Bag> &b) {
    if (!b) return;
    b->setNumber("in", 0);
    b->setNumber("out", -1);
}

// A tractor's length: its longest track.
void refreshTractor(const std::shared_ptr<Bag> &t) {
    int length = 0;
    for (const auto &track : t->tractor->tracks)
        if (track) length = std::max(length, playtimeOf(track));
    t->setNumber("length", length);
    t->setNumber("in", 0);
    t->setNumber("out", length - 1);
}

}  // namespace

// ── Profile ─────────────────────────────────────────────────────────────
Profile::Profile() = default;

// ── Properties ──────────────────────────────────────────────────────────
Properties::Properties() : d(newBag()) {}
Properties::~Properties() = default;

const char *Properties::get(const char *name) const {
    if (!d) return nullptr;
    const Value *v = d->find(name);
    if (!v) return nullptr;
    switch (v->type) {
    case Value::Text: return v->text.constData();
    case Value::Int: v->shown = QByteArray::number(v->i); return v->shown.constData();
    case Value::Int64: v->shown = QByteArray::number(qlonglong(v->l)); return v->shown.constData();
    case Value::Double: v->shown = QByteArray::number(v->d, 'g', 6); return v->shown.constData();
    default: return nullptr;
    }
}

int Properties::get_int(const char *name) const { return d ? d->number(name) : 0; }

int64_t Properties::get_int64(const char *name) const {
    if (!d) return 0;
    const Value *v = d->find(name);
    if (!v) return 0;
    switch (v->type) {
    case Value::Int: return v->i;
    case Value::Int64: return v->l;
    case Value::Double: return int64_t(v->d);
    case Value::Text: { const QByteArray n = leadingNumber(v->text); const int dot = int(n.indexOf('.')); return (dot < 0 ? n : n.left(dot)).toLongLong(); }
    default: return 0;
    }
}

double Properties::get_double(const char *name) const {
    if (!d) return 0;
    const Value *v = d->find(name);
    if (!v) return 0;
    switch (v->type) {
    case Value::Int: return v->i;
    case Value::Int64: return double(v->l);
    case Value::Double: return v->d;
    case Value::Text: return textToDouble(v->text);
    default: return 0;
    }
}

void *Properties::get_data(const char *name) const {
    int size = 0;
    return get_data(name, size);
}

void *Properties::get_data(const char *name, int &size) const {
    if (!d) return nullptr;
    const Value *v = d->find(name);
    if (!v || v->type != Value::Data) return nullptr;
    size = v->size;
    return v->data.get();
}

int Properties::set(const char *name, const char *value) {
    if (!d || !name) return 1;
    Value &v = d->slot(name);
    v = Value{};
    if (value) {
        v.type = Value::Text;
        v.text = QByteArray(value);
    }
    return 0;
}

int Properties::set(const char *name, int value) {
    if (!d || !name) return 1;
    d->setNumber(name, value);
    return 0;
}

int Properties::set(const char *name, int64_t value) {
    if (!d || !name) return 1;
    Value &v = d->slot(name);
    v = Value{};
    v.type = Value::Int64;
    v.l = value;
    return 0;
}

int Properties::set(const char *name, double value) {
    if (!d || !name) return 1;
    Value &v = d->slot(name);
    v = Value{};
    v.type = Value::Double;
    v.d = value;
    return 0;
}

int Properties::set(const char *name, void *value, int size, Destructor destroy, void *) {
    if (!d || !name) return 1;
    Value &v = d->slot(name);
    v = Value{};
    v.type = Value::Data;
    v.size = size;
    v.data = destroy ? std::shared_ptr<void>(value, destroy) : std::shared_ptr<void>(value, [](void *) {});
    return 0;
}

int Properties::count() const { return d ? int(d->values.size()) : 0; }

const char *Properties::get_name(int index) const {
    return d && index >= 0 && index < count() ? d->values[size_t(index)].first.constData() : nullptr;
}

const char *Properties::get(int index) const {
    return d && index >= 0 && index < count() ? get(d->values[size_t(index)].first.constData()) : nullptr;
}

void Properties::pass_values(const Properties &other, const char *prefix) {
    if (!d || !other.d) return;
    const QByteArray start(prefix);
    for (const auto &[key, value] : other.d->values)
        if (key.startsWith(start)) d->slot(key.mid(start.size()).constData()) = value;
}

void Properties::inherit(const Properties &other) {
    if (!d || !other.d) return;
    for (const auto &[key, value] : other.d->values) d->slot(key.constData()) = value;
}

int Properties::clear(const char *name) {
    const quint64 at = touch();
    if (!d) return 1;
    d->written = at;
    auto &values = d->values;
    for (auto it = values.begin(); it != values.end(); ++it)
        if (it->first == name) { values.erase(it); return 0; }
    return 0;
}

// ── Service ─────────────────────────────────────────────────────────────
int Service::attach(Filter &filter) {
    touch();
    if (!d || !filter.bag()) return 1;
    for (const auto &f : d->filters)
        if (f == filter.bag()) return 0;
    d->filters.push_back(filter.bag());
    return 0;
}

int Service::detach(Filter &filter) {
    touch();
    if (!d) return 1;
    auto &list = d->filters;
    const auto it = std::find(list.begin(), list.end(), filter.bag());
    if (it == list.end()) return 1;
    list.erase(it);
    return 0;
}

int Service::filter_count() const { return d ? int(d->filters.size()) : 0; }

Filter *Service::filter(int index) const {
    if (!d || index < 0 || index >= filter_count()) return nullptr;
    return new Filter(d->filters[size_t(index)]);
}

// ── Filter ──────────────────────────────────────────────────────────────
Filter::Filter() = default;
Filter::Filter(Profile &, const char *service) : Filter(service) {}
Filter::Filter(const char *service) : Service(newBag()) {
    set("service", service);
    set("type", "filter");
    set("in", 0);
    set("out", -1);
}

// ── Producer ────────────────────────────────────────────────────────────
Producer::Producer(Profile &, const char *resource, int length) : Service(newBag()) {
    set("resource", resource);
    set("type", "producer");
    set("length", length);
    set("in", 0);
    set("out", length - 1);
}

int Producer::get_length() const { return lengthOf(d); }
int Producer::get_in() const { return inOf(d); }
int Producer::get_out() const { return outOf(d); }
int Producer::set_in_and_out(int in, int out) {
    touch();
    setInOut(d, in, out);
    return 0;
}
bool Producer::is_cut() const { return d && d->number("_cut"); }
bool Producer::is_blank() const { return blankBag(d); }
bool Producer::same_clip(const Producer &other) const { return d && cutParentOf(d) == cutParentOf(other.d); }
Producer *Producer::cut(int in, int out) const { return d ? new Producer(cutOf(d, in, out)) : nullptr; }
Producer Producer::parent() const { return Producer(cutParentOf(d)); }
bool Producer::is_mix() const { return d && cutParentOf(d)->mix; }

// ── Tractor ─────────────────────────────────────────────────────────────
Tractor::Tractor() : Producer(newBag()) {
    d->tractor = std::make_shared<TractorState>();
    set("type", "producer");
    set("service", "tractor");
    set("resource", "<tractor>");
    refreshTractor(d);
}

int Tractor::set_track(Producer &producer, int index) {
    touch();
    if (!d || !d->tractor || index < 0) return 1;
    auto &tracks = d->tractor->tracks;
    if (int(tracks.size()) <= index) tracks.resize(size_t(index) + 1);
    tracks[size_t(index)] = producer.bag();
    refreshTractor(d);
    return 0;
}

Producer *Tractor::track(int index) const {
    if (!d || !d->tractor || index < 0 || index >= int(d->tractor->tracks.size()) || !d->tractor->tracks[size_t(index)]) return nullptr;
    return new Producer(d->tractor->tracks[size_t(index)]);
}

int Tractor::count() const { return d && d->tractor ? int(d->tractor->tracks.size()) : 0; }

int Tractor::plant_transition(Filter &transition, int a, int b) {
    touch();
    if (!d || !d->tractor) return 1;
    transition.set("a_track", a);
    transition.set("b_track", b);
    d->tractor->transitions.push_back(transition.bag());
    return 0;
}

int Tractor::transition_count() const { return d && d->tractor ? int(d->tractor->transitions.size()) : 0; }

Filter *Tractor::transition(int index) const {
    if (!d || !d->tractor || index < 0 || index >= transition_count()) return nullptr;
    return new Filter(d->tractor->transitions[size_t(index)]);
}

// ── ClipInfo ────────────────────────────────────────────────────────────
ClipInfo::~ClipInfo() {
    delete producer;
    delete cut;
    std::free(resource);
}

// ── Playlist ─────────────────────────────────────────────────────────────
Playlist::Playlist() : Producer(newBag()) {
    d->playlist = std::make_shared<PlaylistState>();
    set("type", "producer");
    set("service", "playlist");
    set("resource", "<playlist>");
    refresh();
}

Playlist::Playlist(Profile &) : Playlist() {}

Playlist::Playlist(const Producer &producer) : Producer(producer.bag() && producer.bag()->playlist ? producer.bag() : nullptr) {}

void Playlist::refresh() const {
    if (!d || !d->playlist) return;
    // Called on every read and after every change, so it must not cost a
    // look at every cut each time: placing clips one after another would
    // take time with the square of their number. A cut whose values have not
    // been written since it was last found in step is passed over.
    static const bool verify = qEnvironmentVariableIsSet("OMAEDIT_VERIFY_CLIP_LIST");
    const quint64 now = revision();
    int total = 0;
    for (auto &e : d->playlist->list) {
        const bool inStep = e.cut && e.syncedCut == e.cut.get() && e.cut->written <= e.synced
                            && e.in == e.syncedIn && e.out == e.syncedOut;
        if (inStep && verify && (e.in != inOf(e.cut) || e.out != outOf(e.cut)))
            qFatal("a cut passed over by refresh() is out of step");
        if (e.cut && !inStep) {
            const int current = playtimeOf(e.cut);
            if (e.in != inOf(e.cut) || e.out != outOf(e.cut)) {
                if (current < 1) {
                    e.in = 0;
                    e.out = -1;
                    e.count = 0;
                } else {
                    e.in = inOf(e.cut);
                    e.out = outOf(e.cut);
                    e.count = current;
                }
                e.producerLength = current;
            }
            e.syncedCut = e.cut.get();
            e.synced = now;
            e.syncedIn = e.in;
            e.syncedOut = e.out;
        }
        e.count = (e.out - e.in + 1) * e.repeat;
        total += e.count;
    }
    // Written only when they differ: this runs on every read of the list,
    // and a read is not a change (see revision()).
    const auto keep = [this](const char *name, int value) {
        const Value *v = d->find(name);
        if (!v || v->type != Value::Int || v->i != value) d->setNumber(name, value);
    };
    keep("length", total);
    keep("in", 0);
    keep("out", total - 1);
}

Producer Playlist::blankProducer() {
    auto &st = *d->playlist;
    if (!st.blank) {
        st.blank = newBag();
        Producer blank(st.blank);
        blank.set("resource", "blank");
        blank.set("service", "blank");
        blank.set("length", 15000);
        blank.set("in", 0);
        blank.set("out", 14999);
    }
    return Producer(st.blank);
}

int Playlist::virtualAppend(const Producer &source, int in, int out) {
    touch();
    if (!d || !d->playlist) return 1;
    std::shared_ptr<Bag> producer;
    if (!source.bag() || source.is_blank()) {
        const int length = out - in + 1;
        Producer blank = blankProducer();
        if (length > blank.get_length()) setInOut(blank.bag(), in, out);
        if (!source.bag() || !source.is_cut() || cutParentOf(source.bag()) != blank.bag())
            producer = cutOf(blank.bag(), in, out);
        else
            producer = source.bag();
        if (length > lengthOf(producer)) producer->setNumber("length", length);
    } else if (source.is_cut()) {
        producer = source.bag();
        if (in < 0) in = inOf(producer);
        if (out < 0 || out > outOf(producer)) out = outOf(producer);
    } else {
        producer = cutOf(source.bag(), in, out);
        if (in < 0 || in < inOf(producer)) in = inOf(producer);
        if (out < 0 || out > outOf(producer)) out = outOf(producer);
    }
    PlaylistEntry e;
    e.cut = producer;
    e.in = in;
    e.out = out;
    e.count = out - in + 1;
    e.repeat = 1;
    e.producerLength = playtimeOf(producer);
    d->playlist->list.push_back(e);
    refresh();
    return 0;
}

int Playlist::count() const { return d && d->playlist ? int(d->playlist->list.size()) : 0; }

int Playlist::clear() {
    touch();
    if (!d || !d->playlist) return 1;
    d->playlist->list.clear();
    refresh();
    return 0;
}

int Playlist::position(int clip) const {
    refresh();
    clip = std::clamp(clip, 0, count());
    int at = 0;
    for (int i = 0; i < clip; ++i) at += d->playlist->list[size_t(i)].count;
    return at;
}

int Playlist::append(Producer &producer, int in, int out) {
    touch();
    if (in < 0 && out < 0) return virtualAppend(producer, 0, producer.get_playtime() - 1);
    return virtualAppend(producer, in, out);
}

int Playlist::blank(int out) {
    touch();
    if (out < 0) return 1;
    return virtualAppend(blankProducer(), 0, out);
}

int Playlist::insert(Producer &producer, int where, int in, int out) {
    touch();
    append(producer, in, out);
    move(count() - 1, where);
    refresh();
    return 0;
}

int Playlist::remove(int where) {
    touch();
    refresh();
    const int n = count();
    const bool error = where < 0 || where >= n;
    if (!error && unmix(where) != 0) {
        auto &list = d->playlist->list;
        const PlaylistEntry entry = list[size_t(where)];
        list.erase(list.begin() + where);
        if (!entry.preserve && entry.cut) {
            // Decouple from the mixes on either side.
            if (auto mix = entry.cut->mixIn.lock()) mix->mixOut.reset();
            if (auto mix = entry.cut->mixOut.lock()) mix->mixIn.reset();
            if (entry.cut.use_count() <= 1) clearBag(entry.cut);
        }
        refresh();
    }
    return error ? 1 : 0;
}

int Playlist::move(int src, int dest) {
    touch();
    refresh();
    const int n = count();
    if (src < 0) src = 0;
    if (src >= n) src = n - 1;
    if (dest < 0) dest = 0;
    if (dest >= n) dest = n - 1;
    if (src != dest && n > 1) {
        auto &list = d->playlist->list;
        const PlaylistEntry entry = list[size_t(src)];
        list.erase(list.begin() + src);
        list.insert(list.begin() + dest, entry);
        refresh();
    }
    return 0;
}

int Playlist::repeat(int clip, int times) {
    touch();
    if (times < 1 || clip < 0 || clip >= count()) return 1;
    d->playlist->list[size_t(clip)].repeat = times;
    refresh();
    return 0;
}

int Playlist::resize_clip(int clip, int in, int out) {
    touch();
    refresh();
    const bool error = clip < 0 || clip >= count();
    if (!error && resizeMix(clip, in, out) != 0) {
        const auto producer = d->playlist->list[size_t(clip)].cut;
        if (blankBag(producer)) {
            const int length = out - in + 1;
            Producer blank = blankProducer();
            if (length > blank.get_length()) {
                blank.set("length", length);
                setInOut(blank.bag(), 0, out - in);
            }
            if (length > lengthOf(producer)) producer->setNumber("length", length);
        }
        if (in < 0) in = 0;
        if (out < 0 || out >= lengthOf(producer)) out = lengthOf(producer) - 1;
        if (out < in) std::swap(in, out);
        setInOut(producer, in, out);
        refresh();
    }
    return error ? 1 : 0;
}

int Playlist::split(int clip, int position) {
    touch();
    refresh();
    if (clip < 0 || clip >= count()) return 1;
    const PlaylistEntry entry = d->playlist->list[size_t(clip)];
    position = position < 0 ? entry.count + position - 1 : position;
    if (position < 0 || position >= entry.count - 1) return 1;
    const int in = entry.in, out = entry.out;
    resize_clip(clip, in, in + position);
    if (!blankBag(entry.cut)) {
        Producer split(cutOf(entry.cut, in + position + 1, out));
        insert(split, clip + 1, 0, -1);
        // The cut's meta values ride along (as a split copies them).
        for (const auto &[key, value] : entry.cut->values)
            if (key.startsWith("meta.")) split.bag()->slot(key.constData()) = value;
    } else {
        Producer blank = blankProducer();
        insert(blank, clip + 1, 0, out - position - 1);
    }
    refresh();
    return 0;
}

int Playlist::split_at(int position, bool left) {
    touch();
    const int playtime = get_playtime();
    if (position >= 0 && position < playtime) {
        const int clip = get_clip_index_at(position);
        const int start = clip_start(clip);
        if (left && position != start) split(clip, position - start - 1);
        else if (!left) split(clip, position - start);
        return position;
    }
    return position <= 0 ? 0 : playtime;
}

int Playlist::join(int clip, int count, int) {
    touch();
    refresh();
    if (clip < 0 || clip >= this->count()) return 1;
    Playlist joined;
    if (clip + count >= this->count()) count = this->count() - clip - 1;
    for (int i = 0; i <= count; ++i) {
        PlaylistEntry &entry = d->playlist->list[size_t(clip)];
        Producer cut(entry.cut);
        joined.append(cut);
        joined.repeat(i, entry.repeat);
        entry.preserve = true;
        remove(clip);
    }
    insert(joined, clip, 0, -1);
    return 0;
}

int Playlist::mix(int clip, int length, Filter *transition) {
    touch();
    refresh();
    if (clip < 0 || clip + 1 >= count()) return 1;
    auto &list = d->playlist->list;
    PlaylistEntry a = list[size_t(clip)], b = list[size_t(clip) + 1];
    Tractor tractor;
    tractor.bag()->mix = true;
    const int largest = std::max(a.count, b.count);
    length = std::min(length, largest);
    Producer trackA(length != a.count ? cutOf(a.cut, a.out - length + 1, a.out) : a.cut);
    Producer trackB(length != b.count ? cutOf(b.cut, b.in, b.in + length - 1) : b.cut);
    tractor.set_track(trackA, 0);
    tractor.set_track(trackB, 1);
    insert(tractor, clip + 1, -1, -1);
    if (transition) {
        tractor.plant_transition(*transition, 0, 1);
        transition->set("in", 0);
        transition->set("out", length - 1);
    }
    // What is left of the right-hand clip.
    if (trackB.bag() == b.cut) {
        list[size_t(clip) + 2].preserve = true;
        remove(clip + 2);
    } else if (b.out - b.in >= length) {
        resize_clip(clip + 2, b.in + length, b.out);
        b.cut->mixIn = tractor.bag();
        tractor.bag()->mixOut = b.cut;
    } else {
        clearBag(b.cut);
        remove(clip + 2);
    }
    // And of the left-hand clip.
    if (trackA.bag() == a.cut) {
        list[size_t(clip)].preserve = true;
        remove(clip);
    } else if (a.out - a.in >= length) {
        resize_clip(clip, a.in, a.out - length);
        a.cut->mixOut = tractor.bag();
        tractor.bag()->mixIn = a.cut;
    } else {
        clearBag(a.cut);
        remove(clip);
    }
    refresh();
    return 0;
}

int Playlist::mix_add(int clip, Filter *transition) {
    touch();
    refresh();
    if (!transition || clip < 0 || clip >= count()) return 1;
    const auto parent = cutParentOf(d->playlist->list[size_t(clip)].cut);
    if (!parent || !parent->mix) return 1;
    Tractor tractor(parent);
    tractor.plant_transition(*transition, 0, 1);
    transition->set("in", 0);
    transition->set("out", d->playlist->list[size_t(clip)].count - 1);
    return 0;
}

Producer *Playlist::get_clip(int index) const {
    refresh();
    if (index < 0 || index >= count()) return nullptr;
    return new Producer(d->playlist->list[size_t(index)].cut);
}

Producer *Playlist::get_clip_at(int position) const {
    const int index = get_clip_index_at(position);
    return get_clip(index);
}

int Playlist::get_clip_index_at(int position) const {
    refresh();
    int clip = 0;
    for (; clip < count(); ++clip) {
        const int c = d->playlist->list[size_t(clip)].count;
        if (position < c) break;
        position -= c;
    }
    return clip;
}

bool Playlist::is_mix(int clip) const {
    if (clip < 0 || clip >= count()) return false;
    const auto parent = cutParentOf(d->playlist->list[size_t(clip)].cut);
    return parent && parent->mix;
}

int Playlist::unmix(int clip) {
    touch();
    if (clip < 0 || clip >= count()) return 1;
    auto &list = d->playlist->list;
    const auto parent = cutParentOf(list[size_t(clip)].cut);
    if (!parent || !parent->mix || list[size_t(clip)].preserve) return 1;
    auto clipA = parent->mixIn.lock(), clipB = parent->mixOut.lock();
    const int length = playtimeOf(parent);
    if (clipA) {
        setInOut(clipA, inOf(clipA), outOf(clipA) + length);
    } else if (parent->tractor && !parent->tractor->tracks.empty()) {
        Producer cut(parent->tractor->tracks[0]);
        insert(cut, clip, -1, -1);
        ++clip;
    }
    if (clipB) {
        setInOut(clipB, inOf(clipB) - length, outOf(clipB));
    } else if (parent->tractor && parent->tractor->tracks.size() > 1) {
        Producer cut(parent->tractor->tracks[1]);
        insert(cut, clip + 1, -1, -1);
    }
    parent->mix = false;
    remove(clip);
    refresh();
    return 0;
}

int Playlist::resizeMix(int clip, int in, int out) {
    touch();
    if (clip < 0 || clip >= count()) return 1;
    auto &entry = d->playlist->list[size_t(clip)];
    const auto parent = cutParentOf(entry.cut);
    if (!parent || !parent->mix) return 1;
    auto clipA = parent->mixIn.lock(), clipB = parent->mixOut.lock();
    const auto trackA = parent->tractor && !parent->tractor->tracks.empty() ? parent->tractor->tracks[0] : nullptr;
    const auto trackB = parent->tractor && parent->tractor->tracks.size() > 1 ? parent->tractor->tracks[1] : nullptr;
    const int length = out - in + 1;
    const int diff = length - playtimeOf(parent);
    if (clipA) setInOut(clipA, inOf(clipA), outOf(clipA) - diff);
    if (clipB) setInOut(clipB, inOf(clipB) + diff, outOf(clipB));
    if (trackA) setInOut(trackA, inOf(trackA) - diff, outOf(trackA));
    if (trackB) setInOut(trackB, inOf(trackB), outOf(trackB) + diff);
    refreshTractor(parent);
    setInOut(parent, in, out);
    entry.cut->setNumber("length", out - in + 1);
    setInOut(entry.cut, in, out);
    refresh();
    return 0;
}

void Playlist::consolidate_blanks(int keep_length) {
    touch();
    refresh();
    for (int i = 1; i < count(); ++i) {
        const auto &list = d->playlist->list;
        if (blankBag(list[size_t(i) - 1].cut) && blankBag(list[size_t(i)].cut)) {
            resize_clip(i - 1, 0, list[size_t(i) - 1].count + list[size_t(i)].count - 1);
            remove(i--);
        }
    }
    if (!keep_length && count() > 0 && blankBag(d->playlist->list.back().cut)) remove(count() - 1);
    refresh();
}

bool Playlist::is_blank(int clip) const {
    if (!d || !d->playlist) return true;
    if (clip < 0 || clip >= count()) return true;
    return blankBag(d->playlist->list[size_t(clip)].cut);
}

bool Playlist::is_blank_at(int position) const { return is_blank(get_clip_index_at(position)); }

Producer *Playlist::replace_with_blank(int clip) {
    touch();
    if (is_blank(clip)) return nullptr;
    refresh();
    const PlaylistEntry entry = d->playlist->list[size_t(clip)];
    remove(clip);
    blank(entry.out - entry.in);
    move(count() - 1, clip);
    refresh();
    setInOut(entry.cut, entry.in, entry.out);
    return new Producer(entry.cut);
}

void Playlist::insert_blank(int clip, int out) {
    touch();
    if (out < 0) return;
    blank(out);
    move(count() - 1, clip);
    refresh();
}

void Playlist::pad_blanks(int position, int length, int find) {
    touch();
    if (length == 0) return;
    int clip = get_clip_index_at(position);
    if (find && clip < count() && !is_blank(clip)) ++clip;
    if (clip < count() && is_blank(clip)) {
        const PlaylistEntry &e = d->playlist->list[size_t(clip)];
        if (e.out + length > e.in) resize_clip(clip, e.in, e.out + length);
        else remove(clip);
    } else if (find && clip < count() && length > 0) {
        insert_blank(clip, length);
    }
    refresh();
}

int Playlist::insert_at(int position, Producer *producer, int mode) {
    touch();
    if (position < 0 || !producer || !d) return -1;
    const int length = producer->get_playtime();
    int clip = get_clip_index_at(position);
    int start = clip_start(clip), frames = clip_length(clip);
    int result;
    if (clip < count() && is_blank(clip)) {
        if (position != start && split(clip, position - start - 1) == 0) {
            ++clip;
            frames = clip_length(clip);
        }
        if (length < frames) split(clip, length - 1);
        remove(clip);
        insert(*producer, clip, -1, -1);
        result = clip;
    } else if (clip < count()) {
        if (position > start + frames / 2) ++clip;
        if (mode == 1 && clip < count() && is_blank(clip)) {
            if (length < clip_length(clip)) split(clip, length);
            remove(clip);
        }
        insert(*producer, clip, -1, -1);
        result = clip;
    } else {
        if (mode == 1) {
            if (position == start) remove(clip);
            else blank(position - get_playtime() - 1);
        }
        append(*producer);
        result = count() - 1;
    }
    refresh();
    return result;
}

int Playlist::clip_start(int clip) const {
    refresh();
    if (clip >= 0 && clip < count()) return position(clip);
    return clip < 0 ? 0 : get_playtime();
}

int Playlist::clip_length(int clip) const {
    refresh();
    return clip >= 0 && clip < count() ? d->playlist->list[size_t(clip)].count : 0;
}

int Playlist::blanks_from(int clip, int bounded) const {
    int total = 0;
    if (clip < count()) {
        if (is_blank(clip)) total += clip_length(clip);
        if (bounded == 0) bounded = count();
        for (++clip; clip < count() && bounded >= 0; ++clip) {
            if (is_blank(clip)) total += clip_length(clip);
            else --bounded;
        }
    }
    return total;
}

int Playlist::remove_region(int position, int length) {
    touch();
    int index = get_clip_index_at(position);
    if (index >= 0 && index < count()) {
        const int start = clip_start(index);
        const int total = get_playtime();
        if (position + length > total) length -= position + length - total;
        if (start < position) split(index++, position - start - 1);
        while (length > 0) {
            if (clip_length(index) > length) split(index, length - 1);
            length -= clip_length(index);
            remove(index);
        }
        consolidate_blanks(0);
        refresh();
        index = get_clip_index_at(position);
    }
    return index;
}

int Playlist::get_playtime() const {
    refresh();
    return d ? d->number("length") : 0;
}

// The info of one entry that has a cut and starts at `start`, the list
// already refreshed.
ClipInfo *Playlist::infoOf(int index, int start) const {
    const PlaylistEntry &e = d->playlist->list[size_t(index)];
    auto *info = new ClipInfo;
    const auto parent = cutParentOf(e.cut);
    info->clip = index;
    info->producer = new Producer(parent);
    info->cut = new Producer(e.cut);
    info->start = start;
    const Value *r = parent->find("resource");
    info->resource = r && r->type == Value::Text ? ::strdup(r->text.constData()) : nullptr;
    info->frame_in = e.in;
    info->frame_out = e.out;
    info->frame_count = e.count;
    info->repeat = e.repeat;
    info->length = lengthOf(parent);
    return info;
}

ClipInfo *Playlist::clip_info(int index) const {
    refresh();
    if (index < 0 || index >= count() || !d->playlist->list[size_t(index)].cut) return nullptr;
    return infoOf(index, position(index));
}

std::vector<std::unique_ptr<ClipInfo>> Playlist::clip_infos() const {
    std::vector<std::unique_ptr<ClipInfo>> infos;
    if (!d || !d->playlist) return infos;
    refresh();
    const int n = count();
    infos.reserve(size_t(n));
    int start = 0;
    for (int index = 0; index < n; ++index) {
        const PlaylistEntry &e = d->playlist->list[size_t(index)];
        infos.emplace_back(e.cut ? infoOf(index, start) : nullptr);
        start += e.count;
    }
    return infos;
}

quint64 revision() { return graphRevision.load(std::memory_order_relaxed); }

}  // namespace edit
