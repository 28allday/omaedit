#pragma once
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QVariantMap>

// One command catalogue and one persisted keyboard map for the application.
// QML owns action handlers and availability; this service owns identity,
// dispatch, portable key validation, conflicts and shortcut presets.
class CommandRegistry : public QObject {
  Q_OBJECT
  Q_PROPERTY(int revision READ revision NOTIFY changed)
  Q_PROPERTY(int bindingRevision READ bindingRevision NOTIFY bindingsChanged)
  Q_PROPERTY(QStringList presetNames READ presetNames NOTIFY changed)
  Q_PROPERTY(QString error READ error NOTIFY errorChanged)
public:
  explicit CommandRegistry(QObject *parent = nullptr);
  int revision() const { return m_revision; }
  int bindingRevision() const { return m_bindingRevision; }
  QString error() const { return m_error; }
  QStringList presetNames() const;
  Q_INVOKABLE bool attach(QObject *command);
  Q_INVOKABLE QObject *command(const QString &id) const;
  Q_INVOKABLE QVariantList search(const QString &query = {},
                                  const QString &category = {}) const;
  Q_INVOKABLE bool trigger(const QString &id);
  Q_INVOKABLE QStringList shortcuts(const QString &id) const;
  // The sequences the key handlers listen for: shortcuts() plus the
  // Shift+ form of each bare symbol key, see dispatchSequences().
  Q_INVOKABLE QStringList dispatchSequences(const QString &id) const;
  Q_INVOKABLE QStringList defaultBindings(const QString &id) const {
    return defaults(id);
  }
  Q_INVOKABLE QString hintForDefault(const QString &sequence) const;
  Q_INVOKABLE QString hint(const QString &id) const;
  Q_INVOKABLE QString canonical(const QString &sequence) const;
  Q_INVOKABLE QString captured(int key, int modifiers) const;
  Q_INVOKABLE QStringList conflicts(const QString &id,
                                    const QStringList &sequences) const;
  Q_INVOKABLE bool assign(const QString &id, const QStringList &sequences,
                          bool reassign = false);
  Q_INVOKABLE bool resetCommand(const QString &id, bool reassign = false);
  Q_INVOKABLE void resetAll();
  Q_INVOKABLE bool savePreset(const QString &name, bool replace = false);
  Q_INVOKABLE bool applyPreset(const QString &name);
  Q_INVOKABLE void deletePreset(const QString &name);
  Q_INVOKABLE void refresh();
signals:
  void changed();
  void errorChanged();
  void bindingsChanged();
  void triggered(const QString &id);

private:
  QStringList defaults(const QString &id) const;
  bool overlap(const QString &a, const QString &b) const;
  bool fail(const QString &message);
  void persist();
  QMap<QString, QPointer<QObject>> m_commands;
  QVariantMap m_overrides;
  int m_revision = 0;
  int m_bindingRevision = 0;
  bool m_refreshPending = false;
  bool m_bindingsPending = false;
  void notifyBindings();
  QString m_error;
};
