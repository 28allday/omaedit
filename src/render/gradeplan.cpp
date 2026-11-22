#include "gradeplan.h"

#include "colourpipeline.h"
#include "gradeengine.h"

#include <QColor>
#include <QHash>
#include <QMap>
#include <QPair>
#include <QSet>
#include <QVariantList>
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

namespace render {
namespace {

double number(const QVariantMap &map, const char *key, double fallback = 0) {
    const double v = map.value(key, fallback).toDouble();
    return std::isfinite(v) ? v : fallback;
}
double wheel(const QVariantMap &map, const char *key, int channel, double fallback = 0) {
    const auto values = map.value(key).toList();
    return values.isEmpty() ? fallback : values.value(channel).toDouble();
}
using Points = std::vector<std::array<double, 2>>;
Points points(const QVariant &value) {
    Points out;
    for (const auto &v : value.toList()) {
        const auto p = v.toList();
        if (p.size() >= 2 && std::isfinite(p[0].toDouble()) && std::isfinite(p[1].toDouble())) out.push_back({p[0].toDouble(), p[1].toDouble()});
    }
    std::stable_sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a[0] < b[0]; });
    return out;
}
bool changed(const Points &p, bool flat = false) {
    for (const auto &v : p)
        if (std::abs(v[1] - (flat ? .5 : v[0])) > 1e-12) return true;
    return false;
}

// A node's corrections as the engine's table (engine::node), as LinearGrade
// reads them; null when they change nothing.
std::shared_ptr<const std::vector<float>> nodeTable(const QVariantMap &n, QString *error) {
    namespace N = engine::node;
    const auto p = n.value("primaries").toMap(), curves = n.value("curves").toMap(), hdr = n.value("hdr").toMap();
    auto table = std::make_shared<std::vector<float>>(size_t(N::Size), 0.f);
    auto &t = *table;
    const double temperature = std::clamp(number(p, "temp", 6500), 1000., 40000.), tint = number(p, "tint") / 100;
    double lift[3], gamma[3], gain[3], offset[3];
    for (int c = 0; c < 3; ++c) {
        lift[c] = wheel(p, "lift", 0) + wheel(p, "lift", c + 1) + number(p, "shadows") * .3;
        gamma[c] = 1 / std::clamp(1 + wheel(p, "gamma", 0) + wheel(p, "gamma", c + 1), .1, 5.);
        gain[c] = wheel(p, "gain", 0, 1) * (1 + wheel(p, "gain", c + 1)) * (1 + number(p, "highlights") * .5);
        offset[c] = wheel(p, "offset", 0) + wheel(p, "offset", c + 1);
        gain[c] *= std::pow(temperature / 6500., c == 0 ? -.5 : c == 2 ? .5 : 0.);
        gain[c] *= std::exp2(c == 1 ? -tint * .5 : tint * .25);
        t[size_t(N::Lift + c)] = float(lift[c]);
        t[size_t(N::Gamma + c)] = float(gamma[c]);
        t[size_t(N::Gain + c)] = float(gain[c]);
        t[size_t(N::Offset + c)] = float(offset[c]);
    }
    const double exposure = std::exp2(number(p, "exposure")), contrast = number(p, "contrast", 1);
    const double black = number(p, "blacks") * .25, white = number(p, "whites") * .25;
    const double angle = number(p, "hue") * M_PI / 180, saturation = number(p, "saturation", 1);
    t[N::Contrast] = float(contrast);
    t[N::Pivot] = float(number(p, "pivot", .435));
    t[N::Exposure] = float(exposure);
    t[N::Black] = float(black);
    t[N::White] = float(white);
    t[N::Angle] = float(angle);
    t[N::Saturation] = float(saturation);
    const char *zones[] = {"black", "dark", "shadow", "light", "highlight", "specular"};
    bool hdrActive = false;
    for (int z = 0; z < 6; ++z)
        for (int c = 0; c < 4; ++c) {
            const double v = hdr.value(zones[z]).toList().value(c).toDouble();
            t[size_t(N::Pushes + z * 4 + c)] = float(v);
            hdrActive |= v != 0;
        }
    int warps = 0;
    for (const auto &v : n.value("warper").toList()) {
        const auto m = v.toMap();
        if (number(m, "dh") == 0 && number(m, "ds") == 0) continue;
        if (warps == N::MaxWarps) { *error = QStringLiteral("a warper with more points than the engine takes"); return nullptr; }
        float *w = &t[size_t(N::Warps + warps * 4)];
        w[0] = float(number(m, "hue")); w[1] = float(number(m, "sat")); w[2] = float(number(m, "dh")); w[3] = float(number(m, "ds"));
        ++warps;
    }
    t[N::WarpCount] = float(warps);
    const char *names[] = {"master", "red", "green", "blue", "hueHue", "hueSat", "hueLum", "lumaLuma", "satSat"};
    Points curve[9];
    for (int i = 0; i < 9; ++i) {
        curve[i] = points(curves.value(names[i]));
        if (int(curve[i].size()) > N::MaxPoints) { *error = QStringLiteral("a curve with more points than the engine takes"); return nullptr; }
        float *c = &t[size_t(N::Curves + i * N::CurveStride)];
        c[0] = float(curve[i].size());
        for (size_t k = 0; k < curve[i].size(); ++k) { c[1 + k * 2] = float(curve[i][k][0]); c[2 + k * 2] = float(curve[i][k][1]); }
    }
    const bool chroma = angle != 0 || saturation != 1 || warps > 0 || changed(curve[N::HueHue], true) || changed(curve[N::HueSat], true)
                     || changed(curve[N::HueLum], true) || changed(curve[N::Luma]) || changed(curve[N::Sat]);
    t[N::Flags] = float((chroma ? N::Chroma : 0) | (hdrActive ? N::Hdr : 0));
    bool active = chroma || hdrActive || exposure != 1 || contrast != 1 || black != 0 || white != 0 || changed(curve[N::Master]);
    for (int c = 0; c < 3; ++c) active |= lift[c] != 0 || gamma[c] != 1 || gain[c] != 1 || offset[c] != 0 || changed(curve[N::Red + c]);
    return active ? table : nullptr;
}

// A node's mask (qualifier and windows) as the engine's table; null when it
// keys everything (no qualifier, no window).
std::shared_ptr<const std::vector<float>> maskTable(const QVariantMap &node, int frame, QString *error) {
    namespace M = engine::mask;
    auto table = std::make_shared<std::vector<float>>(size_t(M::Size), 0.f);
    auto &t = *table;
    const QVariantMap q = node.value("qualifier").toMap();
    const bool qualifier = q.value("enabled").toBool();
    if (qualifier) {
        const QColor picked(q.value("color", "#ff0000").toString());
        t[M::Qualifier] = 1;
        t[M::Rgb] = q.value("subspace") == "rgb" ? 1 : 0;
        t[M::Invert] = q.value("invert").toBool() ? 1 : 0;
        t[M::Picked] = float(picked.redF()); t[M::Picked + 1] = float(picked.greenF()); t[M::Picked + 2] = float(picked.blueF());
        t[M::PickedHsl] = float(picked.hslHueF()); t[M::PickedHsl + 1] = float(picked.hslSaturationF()); t[M::PickedHsl + 2] = float(picked.lightnessF());
        t[M::Bounds] = float(number(q, "deltaH", .2)); t[M::Bounds + 1] = float(number(q, "deltaS", .3)); t[M::Bounds + 2] = float(number(q, "deltaL", .3));
        t[M::Softness] = float(std::max(.00001, number(q, "softness", .1)));
    }
    QVariantList windows;
    windows.append(node.value("window"));
    windows.append(node.value("windows").toList());
    int count = 0;
    for (const QVariant &v : windows) {
        const QVariantMap window = GradeEngine::trackedWindowAt(v.toMap(), frame);
        if (!window.value("enabled").toBool()) continue;
        if (count == M::MaxWindows) { *error = QStringLiteral("more mask windows than the engine takes"); return nullptr; }
        float *w = &t[size_t(M::Windows + count * M::WindowStride)];
        const QString name = window.value("shape", "circle").toString();
        w[0] = float(name == "rect" ? M::Rectangle : name == "diamond" ? M::Diamond : name == "gradient" ? M::Gradient
                                    : name == "polygon" ? M::Polygon : M::Ellipse);
        const QString op = window.value("combine", "intersect").toString();
        w[1] = float(op == "add" ? M::Add : op == "subtract" ? M::Subtract : M::Intersect);
        const double angle = -number(window, "rotation") * M_PI / 180;
        w[2] = float(number(window, "x", .5)); w[3] = float(number(window, "y", .5));
        w[4] = float(std::cos(angle)); w[5] = float(std::sin(angle));
        // The aspect is the picture's: kept here as a scale on x (see maskAt).
        w[6] = float(std::max(.0001, number(window, "w", .4) * .5));
        w[7] = float(std::max(.0001, number(window, "h", .4) * .5));
        w[8] = float(number(window, "softness", .1));
        w[9] = window.value("invert").toBool() ? 1 : 0;
        if (w[0] == M::Polygon) {
            std::vector<std::array<double, 2>> polygon;
            for (const auto &pv : window.value("points").toList()) {
                const auto pt = pv.toList();
                if (pt.size() == 2) polygon.push_back({pt[0].toDouble(), pt[1].toDouble()});
            }
            if (polygon.size() < 3) polygon = {{0, -1}, {1, 1}, {-1, 1}};
            if (int(polygon.size()) > M::MaxPolygon) { *error = QStringLiteral("a mask shape with more points than the engine takes"); return nullptr; }
            w[10] = float(polygon.size());
            w[11] = float(M::Polygons + count * M::MaxPolygon * 2);
            for (size_t k = 0; k < polygon.size(); ++k) {
                t[size_t(int(w[11]) + int(k) * 2)] = float(polygon[k][0]);
                t[size_t(int(w[11]) + int(k) * 2 + 1)] = float(polygon[k][1]);
            }
        }
        ++count;
    }
    t[M::WindowCount] = float(count);
    return qualifier || count ? table : nullptr;
}

// The compiler: virtual pictures, numbered as made; register 0 the input.
struct Compiler {
    engine::GradePlan plan;
    int frame, width;
    QString *error;
    int next = 1;
    int make() { return next++; }
    int op(engine::GradeOp o) {
        o.target = make();
        plan.ops.push_back(std::move(o));
        return plan.ops.back().target;
    }
    // A picture of nothing (transparent), and a key of nothing (0) or of
    // everything (-1: no register needed).
    int clear() { engine::GradeOp o; o.kind = engine::GradeOp::Kind::Clear; o.a = -1; return op(o); }
    int mix(int a, int b, int key, float amount) {
        engine::GradeOp o;
        o.kind = engine::GradeOp::Kind::Mix;
        o.a = a; o.b = b; o.mask = key; o.amount = amount;
        return op(o);
    }

    struct Result { int image = 0, raw = 0, key = -1; };
    QMap<QPair<int, float>, int> peopleFound;
    QString working = QStringLiteral("oma_interchange");  // the space the nodes grade in

    // A node graph on picture `input`; the picture it ends in. `matte`: the
    // nested view's matte when a compound's child is viewed.
    int run(int input, const QVariantMap &grade, int depth, int *matte) {
        if (depth > 8) { *error = QStringLiteral("nested nodes go too many levels deep"); return input; }
        if (grade.value("bypass").toBool()) return input;
        const QVariantList nodes = grade.value("nodes").toList();
        QHash<QString, int> ids;
        for (int i = 0; i < nodes.size(); ++i) ids.insert(nodes[i].toMap().value("id").toString(), i);
        const Result original{input, input, -1};
        std::map<int, Result> cache;
        QSet<int> visiting;
        int nestedMatte = -1;
        std::function<Result(int)> evaluate = [&](int index) -> Result {
            if (!error->isEmpty() || index < 0) return original;
            if (auto it = cache.find(index); it != cache.end()) return it->second;
            if (visiting.contains(index)) { *error = QStringLiteral("the node graph contains a cycle"); return original; }
            visiting.insert(index);
            const QVariantMap node = GradeEngine::evaluateNode(nodes[index].toMap(), frame);
            const QString kind = node.value("kind", "serial").toString();
            QList<Result> incoming;
            if (node.contains("inputs")) {
                for (const QVariant &v : node.value("inputs").toList())
                    incoming << evaluate(v.toString() == "input" ? -1 : ids.value(v.toString(), -1));
            } else if ((kind == "parallel" || kind == "layer") && index > 0) {
                incoming << evaluate(index - 1) << evaluate(index - 2);
            } else {
                incoming << evaluate(index - 1);
            }
            if (!error->isEmpty()) return original;
            Result base = incoming.isEmpty() ? original : incoming.first();
            if (incoming.isEmpty()) {
                const int nothing = clear();
                base = {nothing, nothing, nothing};
            }
            if (!node.value("enabled", true).toBool()) {
                visiting.remove(index);
                cache[index] = base;
                return base;
            }
            if (kind == "parallel" && incoming.size() > 1) {
                base.image = incoming.first().image;
                for (int i = 1; i < incoming.size(); ++i) base.image = mix(base.image, incoming[i].image, -1, 1.f / float(i + 1));
            } else if (kind == "layer" && incoming.size() > 1) {
                base.image = incoming.first().image;
                for (int i = 1; i < incoming.size(); ++i) base.image = mix(base.image, incoming[i].raw, incoming[i].key, 1.f);
            } else if (kind == "combiner" && incoming.size() > 1) {
                int sum = incoming.first().image;
                for (int i = 1; i < incoming.size(); ++i) {
                    engine::GradeOp o;
                    o.kind = engine::GradeOp::Kind::Sum;
                    o.a = sum; o.b = incoming[i].image; o.mask = input;  // alpha from the grade's input
                    sum = op(o);
                }
                base.image = sum;
            }
            if (!node.contains("inputs") && (kind == "parallel" || kind == "layer") && incoming.size() > 1) base = incoming.last();
            // The node's corrections (LinearGrade), then mid detail, then its LUT.
            int corrected = base.image;
            QString why;
            if (const auto table = nodeTable(node, &why)) {
                engine::GradeOp o;
                o.kind = engine::GradeOp::Kind::Correct;
                o.a = corrected;
                o.table = table;
                corrected = op(o);
            }
            if (!why.isEmpty()) { *error = std::move(why); return original; }
            const double detail = std::clamp(number(node.value("primaries").toMap(), "midDetail"), -1., 1.);
            if (std::abs(detail) >= 1e-6) {
                const float radius = float(std::max(1, int(std::round(8 * std::abs(detail) * width / 1920.))));
                engine::GradeOp across;
                across.kind = engine::GradeOp::Kind::Box;
                across.a = corrected; across.amount = radius; across.channel = 0;
                engine::GradeOp down = across;
                down.a = op(across); down.channel = 1;
                engine::GradeOp apply;
                apply.kind = engine::GradeOp::Kind::Detail;
                apply.a = corrected; apply.b = op(down); apply.amount = float(detail);
                corrected = op(apply);
            }
            const QVariantMap lut = node.value("lut").toMap();
            const double intensity = std::clamp(number(lut, "intensity", 1), 0., 1.);
            if (!lut.value("path").toString().isEmpty() && intensity > 0) {
                // As LinearGrade: on Rec.709-encoded values, the look mixed in
                // there by its strength, then decoded.
                auto step = [&](int from, std::shared_ptr<const engine::ColourProgram> program) {
                    engine::GradeOp o;
                    o.kind = engine::GradeOp::Kind::Program;
                    o.a = from;
                    o.program = std::move(program);
                    return o.program ? op(o) : -1;
                };
                const QString encoding = QStringLiteral("rec709/rec709");
                const int encoded = step(corrected, ColourPipeline::engineProgram(working, encoding, {}, error));
                if (encoded < 0) return original;
                const int looked = step(encoded, ColourPipeline::engineInputProgram(lut.value("path").toString(), encoding, encoding, error));
                if (looked < 0) return original;
                const int mixed = mix(encoded, looked, -1, float(intensity));
                corrected = step(mixed, ColourPipeline::engineProgram(encoding, working, {}, error));
                if (corrected < 0) return original;
            }
            if (kind == "splitter") {
                const QString channel = node.value("channel", "red").toString();
                engine::GradeOp o;
                o.kind = engine::GradeOp::Kind::Split;
                o.a = corrected;
                o.channel = channel == "green" ? 1 : channel == "blue" ? 2 : 0;
                corrected = op(o);
            }
            if (kind == "compound") {
                const QString view = grade.value("viewNode").toString();
                const QString childPrefix = node.value("id").toString() + "/";
                const bool viewingChild = view.startsWith(childPrefix);
                int childMatte = -1;
                corrected = run(corrected, {{"nodes", node.value("children")}, {"outputNode", node.value("childOutput")},
                                            {"viewNode", viewingChild ? view.mid(childPrefix.size()) : QString()}},
                                depth + 1, &childMatte);
                if (!error->isEmpty()) return original;
                if (viewingChild) nestedMatte = corrected;
            }
            // Its key: qualifier and windows on its input, cleaned up.
            if (!node.value("externalMatte").toString().isEmpty()) {
                *error = QStringLiteral("an external matte");
                return original;
            }
            int key = -1;
            if (const auto table = maskTable(node, frame, error)) {
                engine::GradeOp o;
                o.kind = engine::GradeOp::Kind::Mask;
                o.a = base.image;
                o.table = table;
                key = op(o);
                const QVariantMap q = node.value("qualifier").toMap();
                const int grow = std::clamp(int(std::round(number(q, "grow") * width / 1920.0)), -24, 24);
                if (grow) {
                    for (int direction = 0; direction < 2; ++direction) {
                        engine::GradeOp g;
                        g.kind = engine::GradeOp::Kind::Grow;
                        g.a = key; g.amount = float(grow); g.channel = direction;
                        key = op(g);
                    }
                }
                const int blur = std::clamp(int(std::round(number(q, "blur") * width / 1920.0)), 0, 64);
                if (blur) {
                    for (int direction = 0; direction < 2; ++direction) {
                        engine::GradeOp b;
                        b.kind = engine::GradeOp::Kind::Blur;
                        b.a = key; b.amount = float(blur); b.channel = direction;
                        key = op(b);
                    }
                }
            }
            if (!error->isEmpty()) return original;
            // The people in the picture: found once in the grade's input, whatever
            // the nodes before this have done to it, and refined per node.
            const QVariantMap ai = node.value("aiMask").toMap();
            if (ai.value("enabled").toBool()) {
                const int quality = std::max(0, int(QStringList{"fast", "balanced", "best"}.indexOf(ai.value("quality", "best").toString())));
                const float tighten = float(std::clamp(number(ai, "tighten"), 0., 1.));
                const QPair<int, float> found(quality, tighten);
                int people = peopleFound.value(found, -1);
                if (people < 0) {
                    engine::GradeOp o;
                    o.kind = engine::GradeOp::Kind::People;
                    o.a = 0; o.channel = quality; o.amount = tighten;
                    people = op(o);
                    peopleFound.insert(found, people);
                }
                const int grow = std::clamp(int(std::round(number(ai, "grow") * width / 1920.0)), -24, 24);
                if (grow) {
                    for (int direction = 0; direction < 2; ++direction) {
                        engine::GradeOp g;
                        g.kind = engine::GradeOp::Kind::Grow;
                        g.a = people; g.amount = float(grow); g.channel = direction;
                        people = op(g);
                    }
                }
                const int blur = std::clamp(int(std::round(number(ai, "blur") * width / 1920.0)), 0, 64);
                if (blur) {
                    for (int direction = 0; direction < 2; ++direction) {
                        engine::GradeOp b;
                        b.kind = engine::GradeOp::Kind::Blur;
                        b.a = people; b.amount = float(blur); b.channel = direction;
                        people = op(b);
                    }
                }
                engine::GradeOp both;
                both.kind = engine::GradeOp::Kind::Key;
                both.a = people; both.channel = ai.value("invert").toBool() ? 1 : 0;
                people = op(both);
                if (key < 0) key = people;
                else {
                    both.a = key; both.b = people; both.channel = 0;
                    key = op(both);
                }
            }
            int other = -1;
            const QString keyInput = node.value("keyInput").toString();
            if (!keyInput.isEmpty() && ids.contains(keyInput)) other = evaluate(ids.value(keyInput)).key;
            const bool keyInvert = node.value("keyInvert").toBool();
            const float gainOpacity = float(number(node, "keyGain", 1) * number(node, "opacity", 1));
            if (key >= 0 || other >= 0 || keyInvert || gainOpacity != 1.f) {
                engine::GradeOp k;
                k.kind = engine::GradeOp::Kind::Key;
                k.a = key; k.b = other; k.channel = keyInvert ? 1 : 0; k.amount = gainOpacity;
                key = op(k);
            }
            Result result{key == -1 ? corrected : mix(base.image, corrected, key, 1.f), corrected, key};
            if (!node.contains("inputs") && incoming.size() > 1) {
                if (kind == "parallel") result.image = mix(incoming.first().image, result.image, -1, .5f);
                else if (kind == "layer") result.image = mix(incoming.first().image, corrected, key, 1.f);
            }
            visiting.remove(index);
            cache[index] = result;
            return result;
        };
        const QString output = grade.value("outputNode").toString();
        const Result final = output == "input" ? original : evaluate(ids.value(output, int(nodes.size()) - 1));
        if (!error->isEmpty()) return input;
        const QString view = grade.value("viewNode").toString();
        if (view.contains('/') && ids.contains(view.section('/', 0, 0))) {
            evaluate(ids.value(view.section('/', 0, 0)));
            if (nestedMatte >= 0) { if (matte) *matte = nestedMatte; return nestedMatte; }
        }
        if (!view.isEmpty() && ids.contains(view)) {
            const Result viewed = evaluate(ids.value(view));
            engine::GradeOp o;
            o.kind = engine::GradeOp::Kind::MatteView;
            o.a = viewed.key;
            return op(o);
        }
        return final.image;
    }

    // Real pictures for the virtual ones: one is free again after the last
    // operation reading it, never reused by the operation that reads it.
    void allocate(int result) {
        std::map<int, size_t> lastRead;
        for (size_t i = 0; i < plan.ops.size(); ++i)
            for (int r : {plan.ops[i].a, plan.ops[i].b, plan.ops[i].mask})
                if (r >= 0) lastRead[r] = i;
        lastRead[result] = plan.ops.size();
        std::map<int, int> real{{0, 0}};
        std::vector<int> free;
        int count = 1;
        for (size_t i = 0; i < plan.ops.size(); ++i) {
            auto &o = plan.ops[i];
            for (int *r : {&o.a, &o.b, &o.mask})
                if (*r >= 0) *r = real.at(*r);
            int target;
            if (!free.empty()) { target = free.back(); free.pop_back(); } else target = count++;
            const int virtualTarget = o.target;
            o.target = target;
            real[virtualTarget] = target;
            // Pictures last read here (not the input, register 0) are free from the next operation.
            for (auto it = lastRead.begin(); it != lastRead.end();)
                if (it->second == i && it->first != 0 && real.count(it->first) && real.at(it->first) != target) {
                    free.push_back(real.at(it->first));
                    it = lastRead.erase(it);
                } else {
                    ++it;
                }
        }
        plan.registers = count;
        // The step shows the last operation's target: end on the result.
        if (plan.ops.empty() || plan.ops.back().target != real.at(result)) {
            engine::GradeOp copy;
            copy.kind = engine::GradeOp::Kind::Copy;
            copy.a = real.at(result);
            copy.target = plan.registers++;
            plan.ops.push_back(copy);
        }
    }
};

}  // namespace

bool gradeAnimated(const QVariantList &nodes) {
    for (const auto &v : nodes) {
        const QVariantMap n = v.toMap();
        if (!n.value("keyframes").toMap().isEmpty() || gradeAnimated(n.value("children").toList())) return true;
        QVariantList windows = n.value("windows").toList();
        windows.prepend(n.value("window"));
        for (const auto &w : windows)
            if (!w.toMap().value("track").toList().isEmpty()) return true;
    }
    return false;
}

std::shared_ptr<const engine::GradePlan> gradePlan(const QVariantMap &grade, int frame, int width, QString *error) {
    QString local;
    if (!error) error = &local;
    error->clear();
    Compiler compiler{{}, frame, width, error};
    // A grade viewed at the matte of a key keeps its input's colour where
    // the view shows it (GradeEngine's matte view is an opaque grey).
    // The nodes in the space chosen to grade in, back to the working light after
    // them. A matte on view is not a picture: it stays as it is.
    const QString space = grade.value("management").toMap().value("gradeSpace").toString();
    const bool elsewhere = !space.isEmpty() && space != QLatin1String("oma_interchange") && !grade.value("bypass").toBool();
    const auto through = [&](int from, const QString &a, const QString &b) {
        engine::GradeOp o;
        o.kind = engine::GradeOp::Kind::Program;
        o.a = from;
        o.program = ColourPipeline::engineProgram(a, b, {}, error);
        return o.program ? compiler.op(o) : from;
    };
    int input = 0;
    if (elsewhere) { compiler.working = space; input = through(0, QStringLiteral("oma_interchange"), space); }
    if (!error->isEmpty()) return nullptr;
    int result = compiler.run(input, grade, 0, nullptr);
    if (!error->isEmpty()) return nullptr;
    if (elsewhere && grade.value("viewNode").toString().isEmpty()) result = through(result, space, QStringLiteral("oma_interchange"));
    if (!error->isEmpty()) return nullptr;
    compiler.allocate(result);
    return std::make_shared<engine::GradePlan>(std::move(compiler.plan));
}

bool gradeActive(const QVariantMap &grade) {
    if (grade.isEmpty() || grade.value("bypass").toBool()) return false;
    if (!grade.value("viewNode").toString().isEmpty()) return true;
    for (const auto &n : grade.value("nodes").toList())
        if (n.toMap().value("enabled", true).toBool()) return true;
    return false;
}

}  // namespace render
