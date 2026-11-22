#pragma once
#include <QtGlobal>

// The timeline's edit graph: tracks as playlists of clip cuts, blanks and
// mixes, each clip's attributes as filter records on its cut. It keeps the
// shape and names the timeline was written against, with the same rules for
// cuts, blanks, mixes and in/out points, so edits behave exactly as they did. Nothing here makes pictures or sound: the
// engine and the mixer read the timeline's entries.

#include <QByteArray>
#include <QString>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace edit {

using Destructor = void (*)(void *);

// A clock and frame shape: the timeline's profile.
class Profile {
public:
    Profile();
    Profile(const Profile &) = default;
    Profile &operator=(const Profile &) = default;
    int frame_rate_num() const { return m_fpsNum; }
    int frame_rate_den() const { return m_fpsDen; }
    double fps() const { return m_fpsDen > 0 ? double(m_fpsNum) / m_fpsDen : 0; }
    int width() const { return m_width; }
    int height() const { return m_height; }
    int sample_aspect_num() const { return m_sarNum; }
    int sample_aspect_den() const { return m_sarDen; }
    double sar() const { return m_sarDen > 0 ? double(m_sarNum) / m_sarDen : 1; }
    int display_aspect_num() const { return m_darNum; }
    int display_aspect_den() const { return m_darDen; }
    double dar() const { return m_darDen > 0 ? double(m_darNum) / m_darDen : 1; }
    int progressive() const { return m_progressive; }
    int colorspace() const { return m_colourspace; }
    int is_explicit() const { return m_explicit; }
    void set_frame_rate(int num, int den) { m_fpsNum = num; m_fpsDen = den; }
    void set_width(int w) { m_width = w; }
    void set_height(int h) { m_height = h; }
    void set_sample_aspect(int num, int den) { m_sarNum = num; m_sarDen = den; }
    void set_display_aspect(int num, int den) { m_darNum = num; m_darDen = den; }
    void set_progressive(int p) { m_progressive = p; }
    void set_colorspace(int c) { m_colourspace = c; }
    void set_explicit(int e) { m_explicit = e; }

private:
    // The default clock: 25 fps PAL 4:3, as an unset profile has always been.
    int m_fpsNum = 25, m_fpsDen = 1, m_width = 720, m_height = 576, m_sarNum = 16, m_sarDen = 15, m_darNum = 4, m_darDen = 3;
    int m_progressive = 0, m_colourspace = 601, m_explicit = 0;
};

struct Bag;

// Named values, typed as they were set: an integer reads back exactly, a
// double as "%g" text but in full as a number, text as written. Copies share.
class Properties {
public:
    Properties();
    Properties(const Properties &other) = default;
    Properties &operator=(const Properties &other) = default;
    virtual ~Properties();
    bool is_valid() const { return bool(d); }
    const char *get(const char *name) const;
    int get_int(const char *name) const;
    int64_t get_int64(const char *name) const;
    double get_double(const char *name) const;
    void *get_data(const char *name) const;
    void *get_data(const char *name, int &size) const;
    int set(const char *name, const char *value);
    int set(const char *name, const QByteArray &value) { return set(name, value.constData()); }
    int set(const char *name, int value);
    int set(const char *name, int64_t value);
    int set(const char *name, double value);
    int set(const char *name, void *value, int size, Destructor destroy = nullptr, void *serialise = nullptr);
    int count() const;
    const char *get_name(int index) const;
    const char *get(int index) const;
    // Every value of `other` whose name starts with `prefix`, copied.
    void pass_values(const Properties &other, const char *prefix);
    // Every value of `other`, copied over these.
    void inherit(const Properties &other);
    // The value named, gone.
    int clear(const char *name);
    // The identity of what these properties belong to (equal for copies).
    const void *identity() const { return d.get(); }
    std::shared_ptr<Bag> bag() const { return d; }

protected:
    explicit Properties(std::shared_ptr<Bag> bag) : d(std::move(bag)) {}
    std::shared_ptr<Bag> d;
};

class Filter;

// Properties with filters attached in order.
class Service : public Properties {
public:
    Service() : Properties(std::shared_ptr<Bag>()) {}  // invalid, as an unset service is
    explicit Service(std::shared_ptr<Bag> bag) : Properties(std::move(bag)) {}
    int attach(Filter &filter);
    int detach(Filter &filter);
    int filter_count() const;
    // The filter at `index`, new (the caller deletes it), or null.
    Filter *filter(int index) const;
    const void *get_service() const { return identity(); }
};

// An attribute's record: a service name and its values.
class Filter : public Service {
public:
    Filter();
    explicit Filter(std::shared_ptr<Bag> bag) : Service(std::move(bag)) {}
    Filter(Profile &profile, const char *service);
    Filter(const char *service);
    // Where on its producer it applies (0 / -1: all of it).
    int get_in() const { return get_int("in"); }
    int get_out() const { return get_int("out"); }
    void set_in_and_out(int in, int out) { set("in", in); set("out", out); }
};

// A clip source (resource, length) or a cut of one (its own in and out, its
// own filters, its parent's everything else).
class Producer : public Service {
public:
    Producer() = default;
    explicit Producer(std::shared_ptr<Bag> bag) : Service(std::move(bag)) {}
    // A source of `length` frames.
    Producer(Profile &profile, const char *resource, int length = 15000);
    Producer(const Producer &) = default;
    Producer &operator=(const Producer &) = default;
    explicit Producer(Producer *other) : Service(other ? other->bag() : nullptr) {}
    int get_length() const;
    int get_in() const;
    int get_out() const;
    int get_playtime() const { return get_out() - get_in() + 1; }
    int set_in_and_out(int in, int out);
    bool is_cut() const;
    bool is_blank() const;
    bool same_clip(const Producer &other) const;
    // A new cut of this producer's parent (the caller deletes it).
    Producer *cut(int in = 0, int out = -1) const;
    // The source it cuts (itself when it is not a cut).
    Producer parent() const;
    const void *get_producer() const { return identity(); }
    void set_speed(double) {}
    // Nothing to probe: a producer's length is given when it is made.
    int probe() { return 0; }
    // A play position, kept for callers that note one.
    int seek(int position) { set("_position", position); return 0; }
    int position() const { return get_int("_position"); }
    // Its parent's (for a mix entry: a tractor's) tracks.
    bool is_mix() const;
};

// Several tracks played together: a mix entry's two cuts and its transitions.
class Tractor : public Producer {
public:
    Tractor();
    explicit Tractor(std::shared_ptr<Bag> bag) : Producer(std::move(bag)) {}
    int set_track(Producer &producer, int index);
    // The track at `index`, new (the caller deletes it), or null.
    Producer *track(int index) const;
    int count() const;
    int plant_transition(Filter &transition, int a = 0, int b = 1);
    int transition_count() const;
    Filter *transition(int index) const;
};

struct ClipInfo {
    ~ClipInfo();
    int clip = 0;
    Producer *producer = nullptr;  // the cut's parent
    Producer *cut = nullptr;
    int start = 0;
    char *resource = nullptr;
    int frame_in = 0, frame_out = 0, frame_count = 0, length = 0, repeat = 1;
};

// A track: cuts, blanks and mixes, end to end, entry for entry.
class Playlist : public Producer {
public:
    Playlist();
    explicit Playlist(Profile &profile);
    // The playlist a producer is (invalid when it is not one).
    explicit Playlist(const Producer &producer);
    int count() const;
    int clear();
    int append(Producer &producer, int in = -1, int out = -1);
    int blank(int out);
    int insert(Producer &producer, int where, int in = -1, int out = -1);
    int remove(int where);
    int move(int from, int to);
    int resize_clip(int clip, int in, int out);
    int split(int clip, int position);
    int split_at(int position, bool left = true);
    int join(int clip, int count = 1, int merge = 1);
    int mix(int clip, int length, Filter *transition = nullptr);
    int mix_add(int clip, Filter *transition);
    int repeat(int clip, int count);
    ClipInfo *clip_info(int index) const;
    // Every entry's info in one pass (null where clip_info gives null): one
    // refresh and running starts. clip_info(i) refreshes the list and sums
    // the starts before it on every call, so asking for each clip in turn
    // takes time with the square of the playlist's length.
    std::vector<std::unique_ptr<ClipInfo>> clip_infos() const;
    static void delete_clip_info(ClipInfo *info) { delete info; }
    bool is_mix(int clip) const;
    bool is_blank(int clip) const;
    bool is_blank_at(int position) const;
    void consolidate_blanks(int keep_length = 0);
    Producer *replace_with_blank(int clip);
    void insert_blank(int clip, int out);
    void pad_blanks(int position, int length, int find = 0);
    int insert_at(int position, Producer *producer, int mode = 0);
    int insert_at(int position, Producer &producer, int mode = 0) { return insert_at(position, &producer, mode); }
    int clip_start(int clip) const;
    int clip_length(int clip) const;
    int blanks_from(int clip, int bounded = 0) const;
    int remove_region(int position, int length);
    int get_clip_index_at(int position) const;
    // The cut at `index`, new (the caller deletes it), or null.
    Producer *get_clip(int index) const;
    Producer *get_clip_at(int position) const;
    int get_playtime() const;
    int get_length() const { return get_playtime(); }

private:
    void refresh() const;
    ClipInfo *infoOf(int index, int start) const;
    int virtualAppend(const Producer &source, int in, int out);
    int unmix(int clip);
    int resizeMix(int clip, int in, int out);
    Producer blankProducer();
    int position(int clip) const;
};

// A number that goes up with every change to any graph object: a value set,
// a filter attached or detached, a playlist's entries added, moved, trimmed,
// mixed or removed. Reading never changes it. Something derived from the
// graph (a timeline's clip list) is still good while this has not moved.
quint64 revision();

}  // namespace edit
