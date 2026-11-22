#pragma once

// The beats of mapped music as lines down the tracks: one item the size of
// the view, laid over the clips, that draws only the lines in view and takes
// no mouse input. It is used by the Edit timeline and by the Audio page.
//
// Its owner hands it every beat on the timeline (Backend::beatLines: frame,
// level, bar, track, index), which timeline frame its left edge is and how
// wide a frame is, and where each track's row lies in it. A line runs from
// `spanTop` to `spanBottom` in a lighter tone and is solid on the row of the
// clip whose music it is; the first beat of a bar has a cap at the top of its
// own clip.
//
// Zoomed out, the lines thin by level so they are never closer than `spacing`:
// every beat, then bars only, then every fourth bar, every sixteenth, and so
// on. Each clip is thinned by itself (music at another tempo or speed keeps
// its own), and a clip in view always keeps at least one line. Whatever the
// finest lines shown are, the bars above them are the strong ones (wider, and
// solid all the way down): the bars among beats, every fourth bar among bars.

#include <QColor>
#include <QQuickItem>
#include <QVariantList>
#include <array>

class BeatLinesItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QVariantList lines READ lines WRITE setLines NOTIFY linesChanged)
    // [{track, y, height}]: the rows of the tracks, in this item's own space.
    Q_PROPERTY(QVariantList rows READ rows WRITE setRows NOTIFY rowsChanged)
    Q_PROPERTY(double firstFrame READ firstFrame WRITE setFirstFrame NOTIFY viewChanged)
    Q_PROPERTY(double pxPerFrame READ pxPerFrame WRITE setPxPerFrame NOTIFY viewChanged)
    Q_PROPERTY(double spanTop READ spanTop WRITE setSpanTop NOTIFY viewChanged)
    Q_PROPERTY(double spanBottom READ spanBottom WRITE setSpanBottom NOTIFY viewChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY lookChanged)
    Q_PROPERTY(double lineWidth READ lineWidth WRITE setLineWidth NOTIFY lookChanged)
    Q_PROPERTY(double barWidth READ barWidth WRITE setBarWidth NOTIFY lookChanged)
    Q_PROPERTY(double spacing READ spacing WRITE setSpacing NOTIFY lookChanged)
    // How much of the tone a line has across the other tracks (0 to 1): the
    // finest lines shown, and the strong ones. `barWidth` is the strong ones' width.
    Q_PROPERTY(double beatOpacity READ beatOpacity WRITE setBeatOpacity NOTIFY lookChanged)
    Q_PROPERTY(double barOpacity READ barOpacity WRITE setBarOpacity NOTIFY lookChanged)
    Q_PROPERTY(QSizeF cap READ cap WRITE setCap NOTIFY lookChanged)
    // What is drawn now: how many lines, and the lowest level among them (-1 for none).
    Q_PROPERTY(int count READ count NOTIFY shownChanged)
    Q_PROPERTY(int level READ level NOTIFY shownChanged)

public:
    explicit BeatLinesItem(QQuickItem *parent = nullptr);

    QVariantList lines() const { return m_lines; }
    void setLines(const QVariantList &lines);
    QVariantList rows() const { return m_rows; }
    void setRows(const QVariantList &rows);
    double firstFrame() const { return m_firstFrame; }
    void setFirstFrame(double frame);
    double pxPerFrame() const { return m_pxPerFrame; }
    void setPxPerFrame(double px);
    double spanTop() const { return m_spanTop; }
    void setSpanTop(double y);
    double spanBottom() const { return m_spanBottom; }
    void setSpanBottom(double y);
    QColor color() const { return m_color; }
    void setColor(const QColor &color);
    double lineWidth() const { return m_lineWidth; }
    void setLineWidth(double width);
    double barWidth() const { return m_barWidth; }
    void setBarWidth(double width);
    double spacing() const { return m_spacing; }
    void setSpacing(double spacing);
    double beatOpacity() const { return m_beatOpacity; }
    void setBeatOpacity(double opacity);
    double barOpacity() const { return m_barOpacity; }
    void setBarOpacity(double opacity);
    QSizeF cap() const { return m_cap; }
    void setCap(const QSizeF &cap);
    int count();
    int level();
    // The lines drawn now, left to right: [{x (the line's middle), frame, level, bar, strong, tracks}].
    Q_INVOKABLE QVariantList shown();

signals:
    void linesChanged();
    void rowsChanged();
    void viewChanged();
    void lookChanged();
    void shownChanged();

protected:
    void updatePolish() override;
    QSGNode *updatePaintNode(QSGNode *node, UpdatePaintNodeData *data) override;
    void geometryChange(const QRectF &now, const QRectF &before) override;

private:
    static constexpr int kLevels = 9;
    struct Line { int frame = 0, level = 0; bool bar = false; };
    struct Clip {
        int track = 0, top = 0;
        QVector<Line> lines; // by frame
        // The lines of each level and above: how many, and the least gap between two, in frames.
        std::array<int, kLevels> held{};
        std::array<double, kLevels> gap{};
    };
    struct Shown { double x = 0; int frame = 0, level = 0; bool bar = false, strong = false; QVector<int> tracks; };
    struct Row { int track = 0; double y = 0, height = 0; };
    void changed();
    void lay();

    QVariantList m_lines, m_rows;
    QVector<Clip> m_clips;
    QVector<Row> m_rowList;
    QVector<Shown> m_shown;
    int m_level = -1;
    bool m_stale = true;
    double m_firstFrame = 0, m_pxPerFrame = 1, m_spanTop = 0, m_spanBottom = -1;
    QColor m_color = Qt::white;
    double m_lineWidth = 1, m_barWidth = 2, m_spacing = 8, m_beatOpacity = 0.7, m_barOpacity = 1;
    QSizeF m_cap{9, 5};
};
