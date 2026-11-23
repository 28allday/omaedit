#pragma once
#include <QObject>
#include <QProcess>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <functional>
#include <memory>

// Speech to text for one clip at a time (Inspector ▸ Transcript).
//
// whisper.cpp's CLI over the clip's audio (ffmpeg extracts 16 kHz mono to
// the cache first). An explicit, labelled AI action: nothing runs until
// asked and it runs on this machine. The package brings whisper.cpp and its
// multilingual base model, so it works with no setting made. Command, model
// and language live in QSettings (transcriptCommand / transcriptModel /
// transcriptLanguage: a model of the user's own, "auto" or a language code);
// any whisper.cpp-compatible CLI will do.
//
// This runs the two processes and reports progress. Where the text goes is
// the owner's business: it hands in `store` and listens for finished().
class Transcriber : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString command READ command WRITE setCommand NOTIFY settingsChanged)
    Q_PROPERTY(QString model READ model WRITE setModel NOTIFY settingsChanged)
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY settingsChanged)
    Q_PROPERTY(bool available READ available NOTIFY settingsChanged)
    Q_PROPERTY(QString status READ status NOTIFY settingsChanged)
    Q_PROPERTY(QVariantMap job READ job NOTIFY jobChanged)
  public:
    explicit Transcriber(QObject *parent = nullptr);
    ~Transcriber() override;
    QString command() const { return m_command; }
    void setCommand(const QString &command);
    // The model in use: the user's own while that file is there, else the
    // one that comes with the package.
    QString model() const;
    void setModel(const QString &path);
    // The packaged model when it is there; "" otherwise.
    static QString packagedModel();
    // How many threads whisper is given: one for each core it may use.
    static int threads();
    QString language() const { return m_language; }
    void setLanguage(const QString &language);
    bool available() const { return status().isEmpty(); }
    QString status() const; // "" when everything needed is there
    // {index, path, libraryId, stage, percent} while one runs; after a
    // failure {index: -1, error, forIndex}; empty otherwise.
    QVariantMap job() const { return m_job; }
    // A process exists, running or being cancelled.
    bool active() const { return bool(m_process); }
    bool running() const { return m_process && m_process->state() != QProcess::NotRunning; }
    // Transcribes the file at `path`. `index` and `libraryId` name the clip
    // for the job map, `name` for the status line. Call only when available() and not active().
    // With `twoSpeakers` the sound goes to whisper in stereo, run with
    // --diarize: each segment it finds louder on one channel is that
    // channel's speaker (segment "channel" 0 left, 1 right, -1 neither).
    void start(int index, const QString &path, const QString &libraryId, const QString &name, bool twoSpeakers = false);
    Q_INVOKABLE void cancel();
    // Stops without a word, for an owner that is going away.
    void abandon();
    // Where the job's clip is now (-1 when it is no longer there), and the
    // finished text for the clip at that index.
    std::function<int(const QVariantMap &job)> locate;
    std::function<void(int index, const QVariantList &segments, const QString &language)> store;
    // whisper.cpp -oj output as [{start, end, text}] in ms (and with
    // --diarize each segment's channel).
    static QVariantList parseWhisperJson(const QByteArray &json, QString *language);
    // Whether a transcript was made with each channel a speaker.
    static bool hasChannels(const QVariantList &segments);
  signals:
    void settingsChanged();
    void jobChanged();
    void message(const QString &text); // a line for the status bar
    // `index` is the clip the job ended on; `error` may carry a note when ok.
    void finished(bool ok, int index, const QString &error);

  private:
    void textStage(bool onCpu = false);
    void finish(bool ok, const QString &error);
    void setJob(const QVariantMap &job);
    QString m_command, m_language;
    QString m_model; // the user's own model; empty for the packaged one
    QVariantMap m_job;
    std::unique_ptr<QProcess> m_process;
    QString m_wav, m_base, m_stderr;
};
