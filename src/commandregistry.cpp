#include "commandregistry.h"
#include <QCryptographicHash>
#include <QJSValue>
#include <QKeySequence>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <algorithm>

namespace {
QStringList strings(const QVariant &v) {
  const auto plain = v.metaType().id() == qMetaTypeId<QJSValue>()
                         ? v.value<QJSValue>().toVariant()
                         : v;
  QStringList out;
  for (const auto &item : plain.toList()) {
    if (item.metaType().id() == QMetaType::Int) {
      for (const auto &key : QKeySequence::keyBindings(
               static_cast<QKeySequence::StandardKey>(item.toInt())))
        out.append(key.toString(QKeySequence::PortableText));
    } else
      out.append(item.toString());
  }
  if (out.isEmpty())
    out = plain.toStringList();
  out.removeDuplicates();
  return out;
}
QString presetKey(const QString &name) {
  return "commands/v1/presets/" +
         QString::fromLatin1(
             QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Sha256)
                 .toHex());
}
bool matches(const QString &a, const QString &b) {
  const auto first = QKeySequence::fromString(a, QKeySequence::PortableText);
  const auto second = QKeySequence::fromString(b, QKeySequence::PortableText);
  return first.matches(second) != QKeySequence::NoMatch ||
         second.matches(first) != QKeySequence::NoMatch;
}
} // namespace
CommandRegistry::CommandRegistry(QObject *parent)
    : QObject(parent),
      m_overrides(QSettings().value("commands/v1/shortcuts").toMap()) {}
bool CommandRegistry::fail(const QString &message) {
  m_error = message;
  emit errorChanged();
  return false;
}
void CommandRegistry::refresh() {
  if (m_refreshPending)
    return;
  m_refreshPending = true;
  QTimer::singleShot(0, this, [this] {
    m_refreshPending = false;
    ++m_revision;
    emit changed();
  });
}
void CommandRegistry::notifyBindings() {
  if (m_bindingsPending)
    return;
  m_bindingsPending = true;
  QTimer::singleShot(0, this, [this] {
    m_bindingsPending = false;
    ++m_bindingRevision;
    emit bindingsChanged();
  });
}
bool CommandRegistry::attach(QObject *object) {
  if (!object)
    return false;
  const auto id = object->property("commandId").toString();
  if (id.isEmpty() || (m_commands.value(id) && m_commands.value(id) != object))
    return fail(tr("Duplicate command: %1").arg(id));
  m_commands[id] = object;
  connect(object, &QObject::destroyed, this, [this, id] {
    m_commands.remove(id);
    refresh();
    notifyBindings();
  });
  refresh();
  notifyBindings();
  return true;
}
QObject *CommandRegistry::command(const QString &id) const {
  return m_commands.value(id);
}
QStringList CommandRegistry::defaults(const QString &id) const {
  const auto *object = command(id);
  QStringList out;
  if (object)
    for (const auto &key : strings(object->property("defaultShortcuts"))) {
      const auto clean = canonical(key);
      if (!clean.isEmpty() && !out.contains(clean))
        out.append(clean);
    }
  return out;
}
QStringList CommandRegistry::shortcuts(const QString &id) const {
  if (!m_overrides.contains(id))
    return defaults(id);
  QStringList out;
  for (const auto &key : strings(m_overrides.value(id))) {
    const auto clean = canonical(key);
    if (!clean.isEmpty() && !out.contains(clean))
      out.append(clean);
  }
  return out;
}
// A symbol such as "?" or "+" is typed with Shift on many layouts, and the
// key event then carries Shift+? rather than ?. Listening for the Shift+
// form too makes a bare symbol binding fire however the layout reaches it.
// Letters and digits are left alone (Shift+T and Shift+1 are their own
// bindings), as is a Shift+ form some command already uses. Only dispatch
// sees the extra form; the editor, hints and conflicts keep shortcuts().
QStringList CommandRegistry::dispatchSequences(const QString &id) const {
  auto out = shortcuts(id);
  QStringList taken;
  for (auto i = m_commands.cbegin(); i != m_commands.cend(); ++i)
    taken += shortcuts(i.key());
  for (const auto &text : shortcuts(id)) {
    const auto key = QKeySequence::fromString(text, QKeySequence::PortableText);
    if (key.count() != 1 || key[0].keyboardModifiers() != Qt::NoModifier)
      continue;
    const int code = key[0].key();
    if (code > 0xffff || !QChar(code).isPrint() || QChar(code).isSpace() ||
        QChar(code).isLetterOrNumber())
      continue;
    const auto shifted =
        QKeySequence(QKeyCombination(Qt::ShiftModifier, key[0].key()))
            .toString(QKeySequence::PortableText);
    if (!taken.contains(shifted) && !out.contains(shifted))
      out.append(shifted);
  }
  return out;
}
QString CommandRegistry::hint(const QString &id) const {
  return shortcuts(id).join(" / ");
}
QString CommandRegistry::canonical(const QString &text) const {
  const auto key =
      QKeySequence::fromString(text.trimmed(), QKeySequence::PortableText);
  if (key.isEmpty() || key.count() > 4)
    return {};
  for (int i = 0; i < key.count(); ++i)
    if (key[i].key() == Qt::Key_unknown || key[i].key() == 0)
      return {};
  return key.toString(QKeySequence::PortableText);
}
QString CommandRegistry::captured(int key, int modifiers) const {
  if (key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt ||
      key == Qt::Key_Meta || key == Qt::Key_unknown)
    return {};
  return QKeySequence(
             QKeyCombination(Qt::KeyboardModifiers(modifiers), Qt::Key(key)))
      .toString(QKeySequence::PortableText);
}
bool CommandRegistry::overlap(const QString &a, const QString &b) const {
  const auto *first = command(a), *second = command(b);
  if (!first || !second)
    return true;
  const auto x = first->property("scope").toString(),
             y = second->property("scope").toString();
  return x.isEmpty() || y.isEmpty() || x == "Global" || y == "Global" || x == y;
}
QStringList CommandRegistry::conflicts(const QString &id,
                                       const QStringList &sequences) const {
  QStringList out;
  for (auto i = m_commands.cbegin(); i != m_commands.cend(); ++i) {
    if (i.key() == id || !overlap(id, i.key()))
      continue;
    bool conflict = false;
    for (const auto &wanted : sequences)
      for (const auto &existing : shortcuts(i.key()))
        if (!canonical(wanted).isEmpty() &&
            matches(canonical(wanted), existing))
          conflict = true;
    if (conflict)
      out.append(i.key());
  }
  return out;
}
void CommandRegistry::persist() {
  QSettings settings;
  settings.setValue("commands/v1/shortcuts", m_overrides);
  settings.sync();
  m_error.clear();
  emit errorChanged();
  refresh();
  notifyBindings();
}
bool CommandRegistry::assign(const QString &id, const QStringList &sequences,
                             bool reassign) {
  if (!command(id))
    return fail(tr("Command is no longer available."));
  if (sequences.size() > 4)
    return fail(tr("Use up to four shortcuts per command."));
  QStringList clean;
  for (const auto &key : sequences) {
    const auto value = canonical(key);
    if (value.isEmpty())
      return fail(tr("Invalid shortcut: %1").arg(key));
    if (!clean.contains(value))
      clean.append(value);
  }
  for (int i = 0; i < clean.size(); ++i)
    for (int j = i + 1; j < clean.size(); ++j)
      if (matches(clean[i], clean[j]))
        return fail(
            tr("A shortcut cannot also be the prefix of another shortcut."));
  const auto clashes = conflicts(id, clean);
  if (!clashes.isEmpty() && !reassign)
    return fail(
        tr("Shortcut already assigned. Choose another key or use Reassign."));
  for (const auto &other : clashes) {
    auto keys = shortcuts(other);
    keys.removeIf([&](const QString &key) {
      return std::any_of(clean.begin(), clean.end(),
                         [&](const QString &c) { return matches(c, key); });
    });
    m_overrides[other] = keys;
  }
  m_overrides[id] = clean;
  persist();
  return true;
}
bool CommandRegistry::resetCommand(const QString &id, bool reassign) {
  return assign(id, defaults(id), reassign);
}
void CommandRegistry::resetAll() {
  m_overrides.clear();
  persist();
}
bool CommandRegistry::trigger(const QString &id) {
  auto *object = command(id);
  if (!object || !object->property("available").toBool())
    return false;
  emit triggered(id);
  return QMetaObject::invokeMethod(object, "execute", Qt::DirectConnection);
}
QVariantList CommandRegistry::search(const QString &query,
                                     const QString &category) const {
  QVariantList rows;
  const auto words = query.simplified().split(' ', Qt::SkipEmptyParts);
  for (auto i = m_commands.cbegin(); i != m_commands.cend(); ++i) {
    auto *c = i.value().data();
    if (!c)
      continue;
    const auto title = c->property("text").toString(),
               group = c->property("category").toString();
    if (!category.isEmpty() && category != group)
      continue;
    const auto keys = shortcuts(i.key());
    const auto haystack =
        title + ' ' + group + ' ' + i.key() + ' ' + keys.join(' ');
    if (!std::all_of(words.begin(), words.end(), [&](const QString &word) {
          return haystack.contains(word, Qt::CaseInsensitive);
        }))
      continue;
    rows.append(QVariantMap{{"id", i.key()},
                            {"text", title},
                            {"category", group},
                            {"scope", c->property("scope")},
                            {"available", c->property("available")},
                            {"shortcuts", keys},
                            {"hint", keys.join(" / ")},
                            {"modified", keys != defaults(i.key())}});
  }
  std::sort(rows.begin(), rows.end(), [](const QVariant &a, const QVariant &b) {
    const auto x = a.toMap(), y = b.toMap();
    return (x.value("category").toString() + x.value("text").toString())
               .localeAwareCompare(y.value("category").toString() +
                                   y.value("text").toString()) < 0;
  });
  return rows;
}
QStringList CommandRegistry::presetNames() const {
  QSettings settings;
  settings.beginGroup("commands/v1/presets");
  QStringList names;
  for (const auto &key : settings.childKeys())
    names.append(settings.value(key).toMap().value("name").toString());
  names.sort(Qt::CaseInsensitive);
  return names;
}
bool CommandRegistry::savePreset(const QString &name, bool replace) {
  const auto clean = name.trimmed();
  if (clean.isEmpty() || clean.size() > 80)
    return fail(tr("Use a preset name from 1 to 80 characters."));
  QSettings settings;
  const auto key = presetKey(clean);
  if (settings.contains(key) && !replace)
    return fail(tr("That preset exists. Use Update or choose another name."));
  QVariantMap map = m_overrides;
  for (auto i = m_commands.cbegin(); i != m_commands.cend(); ++i)
    map[i.key()] = shortcuts(i.key());
  settings.setValue(
      key, QVariantMap{{"name", clean}, {"version", 1}, {"shortcuts", map}});
  settings.sync();
  refresh();
  return true;
}
bool CommandRegistry::applyPreset(const QString &name) {
  const auto preset = QSettings().value(presetKey(name)).toMap();
  if (preset.value("version").toInt() != 1)
    return fail(tr("Shortcut preset is missing or unsupported."));
  const auto old = m_overrides;
  m_overrides = preset.value("shortcuts").toMap();
  for (auto i = m_commands.cbegin(); i != m_commands.cend(); ++i) {
    if (!conflicts(i.key(), shortcuts(i.key())).isEmpty()) {
      m_overrides = old;
      return fail(tr("This preset contains conflicting shortcuts. The current "
                     "map was kept."));
    }
  }
  persist();
  return true;
}
void CommandRegistry::deletePreset(const QString &name) {
  QSettings settings;
  settings.remove(presetKey(name));
  settings.sync();
  refresh();
}

QString CommandRegistry::hintForDefault(const QString &sequence) const {
  if (sequence.trimmed().isEmpty())
    return {};
  const auto key = canonical(sequence);
  for (auto i = m_commands.cbegin(); i != m_commands.cend(); ++i)
    if (defaults(i.key()).contains(key))
      return hint(i.key());
  return sequence;
}
