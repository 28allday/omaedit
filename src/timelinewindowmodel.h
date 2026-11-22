#pragma once
#include <QAbstractListModel>
#include <QVariantList>
#include <QSet>

// A presentation window over the engine's clip list. Source addresses are
// carried explicitly; a delegate's visual row must never address an edit.
// Incremental window changes retain the same objects during a scroll/drag.
class TimelineWindowModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QVariantList source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(double fps READ fps WRITE setFps NOTIFY fpsChanged)
    Q_PROPERTY(double firstFrame READ firstFrame WRITE setFirstFrame NOTIFY windowChanged)
    Q_PROPERTY(double lastFrame READ lastFrame WRITE setLastFrame NOTIFY windowChanged)
    Q_PROPERTY(double minimumFrames READ minimumFrames WRITE setMinimumFrames NOTIFY windowChanged)
    // Entries shorter than this are left to an overview drawing instead of
    // getting a delegate each, unless pinned (0 = every entry gets one).
    Q_PROPERTY(double narrowFrames READ narrowFrames WRITE setNarrowFrames NOTIFY windowChanged)
    Q_PROPERTY(QString kind READ kind WRITE setKind NOTIFY windowChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY windowChanged)
    Q_PROPERTY(QVariantList pinned READ pinned WRITE setPinned NOTIFY windowChanged)
public:
    // timelineStart and timelineFrames are the block's own: where a transition
    // made of the clips' handles sits across a cut the two blocks meet at
    // the cut, not at the transition's end as the entries do. blockLead and
    // blockTail are the frames it starts before the entry's start and ends
    // before its end; blockBefore is the rest of the transition into it.
    // mixMostBefore / mixMostAfter bound the grips on a transition's edges.
    enum Role { EntryRole=Qt::UserRole+1, SourceIndexRole, TimelineStartRole, TimelineFramesRole, BlockLeadRole, BlockTailRole, BlockBeforeRole, MostBeforeRole, MostAfterRole };
    explicit TimelineWindowModel(QObject *parent=nullptr):QAbstractListModel(parent) {}
    int rowCount(const QModelIndex &parent={}) const override { return parent.isValid()?0:m_rows.size(); }
    QVariant data(const QModelIndex &index,int role) const override;
    QHash<int,QByteArray> roleNames() const override { return {{EntryRole,"modelData"},{SourceIndexRole,"sourceIndex"},{TimelineStartRole,"timelineStart"},{TimelineFramesRole,"timelineFrames"},
        {BlockLeadRole,"blockLead"},{BlockTailRole,"blockTail"},{BlockBeforeRole,"blockBefore"},
        {MostBeforeRole,"mixMostBefore"},{MostAfterRole,"mixMostAfter"}}; }
    QVariantList source() const { return m_source; }
    void setSource(const QVariantList &value);
    double fps() const { return m_fps; }
    void setFps(double value);
    double firstFrame() const { return m_first; }
    double lastFrame() const { return m_last; }
    double minimumFrames() const { return m_minimum; }
    double narrowFrames() const { return m_narrow; }
    QString kind() const { return m_kind; }
    bool active() const { return m_active; }
    QVariantList pinned() const { return m_pinned; }
    void setFirstFrame(double value);
    void setLastFrame(double value);
    void setMinimumFrames(double value);
    void setNarrowFrames(double value);
    void setKind(const QString &value);
    void setActive(bool value);
    void setPinned(const QVariantList &value);
signals:
    void fpsChanged();
    void sourceChanged();
    void windowChanged();
private:
    void schedule();
    void refresh();
    struct Span { double start=0,end=0,frames=0; bool eligible=true; int lead=0,tail=0,before=0,mostBefore=0,mostAfter=0; };
    QVariantList m_source,m_pinned;
    QList<Span> m_spans;
    QList<int> m_rows;
    QSet<int> m_pinSet;
    double m_first=0,m_last=0,m_minimum=0,m_narrow=0;
    double m_fps=25;
    QString m_kind;
    bool m_active=true,m_pending=false;
    void indexSpans();
};
