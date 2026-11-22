#include "agentskill.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUuid>
#include <QVariantMap>
#include <cstdio>

namespace {
QString skillFolder() {
  const auto appDir = QCoreApplication::applicationDirPath();
  QStringList candidates;
  // Local test installs switch 'current' atomically. Keep that indirection in
  // skill links so the next install also updates agents without re-linking.
  const auto current = QDir::homePath() + "/.local/opt/omaedit/current";
  if (QFileInfo(current + "/bin").canonicalFilePath() == appDir)
    candidates << current + "/share/omaedit/skills/omaedit";
  candidates << appDir + "/../share/omaedit/skills/omaedit"
             << appDir + "/../skills/omaedit" << appDir + "/skills/omaedit";
  for (const auto &candidate : candidates)
    if (QFileInfo(candidate + "/SKILL.md").isFile())
      return QDir::cleanPath(candidate);
  return {};
}

QStringList skillPlaces() {
  const auto home = QDir::homePath();
  QStringList places{home + "/.agents/skills", home + "/.claude/skills"};
  auto codex = qEnvironmentVariable("CODEX_HOME");
  if (codex.isEmpty())
    codex = home + "/.codex";
  codex = QFileInfo(codex).absoluteFilePath();
  if (QFileInfo(codex).isDir())
    places << codex + "/skills";
  if (QFileInfo(home + "/.pi").isDir())
    places << home + "/.pi/agent/skills";
  if (QFileInfo(home + "/.hermes").isDir())
    places << home + "/.hermes/skills";
  const QDir profiles(home + "/.hermes/profiles");
  for (const auto &profile :
       profiles.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
    places << profiles.filePath(profile + "/skills");
  places.removeDuplicates();
  return places;
}

int answer(QVariantMap payload, const QString &code = {},
           const QString &message = {}) {
  payload["ok"] = code.isEmpty();
  if (!code.isEmpty())
    payload["error"] = QVariantMap{{"code", code}, {"message", message}};
  const auto bytes =
      QJsonDocument::fromVariant(payload).toJson(QJsonDocument::Compact) + '\n';
  fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
  return code.isEmpty() ? 0 : 1;
}
} // namespace

int agentSkillMain(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QCommandLineParser parser;
  parser.setApplicationDescription("OmaEdit agent skill: report registration "
                                   "or link the bundled skill for this user.");
  parser.addHelpOption();
  parser.addPositionalArgument("skill",
                               "Report the bundled skill and agent locations.");
  parser.addOption(QCommandLineOption(
      "link", "Register the skill with this user's agents."));
  parser.addOption(
      QCommandLineOption("force", "Move conflicting entries to a backup before "
                                  "linking (requires --link)."));
  if (!parser.parse(app.arguments()))
    return answer({}, "usage", parser.errorText());
  if (parser.isSet("help")) {
    fputs(parser.helpText().toUtf8().constData(), stdout);
    return 0;
  }
  if (parser.positionalArguments() != QStringList{"skill"} ||
      (parser.isSet("force") && !parser.isSet("link")))
    return answer({}, "usage", "Use omaedit skill [--link [--force]].");
  const auto folder = skillFolder();
  if (folder.isEmpty())
    return answer(
        {}, "skill_missing",
        "The skill is missing from this copy of OmaEdit. Reinstall the "
        "complete application or keep skills/omaedit beside the source build.");
  const auto canonical = QFileInfo(folder).canonicalFilePath();
  QVariantList rows;
  QStringList errors;
  int linked = 0;
  for (const auto &place : skillPlaces()) {
    const auto target = place + "/omaedit";
    const QFileInfo existing(target);
    bool matches =
        existing.exists() && existing.canonicalFilePath() == canonical;
    QVariantMap row{{"place", target}, {"linked", matches}};
    if (parser.isSet("link") && !matches) {
      QString problem, backup;
      if ((existing.exists() || existing.isSymLink()) && !parser.isSet("force"))
        problem = "Something else is here; left unchanged. --link --force "
                  "backs it up before replacing it.";
      else if (!QDir().mkpath(place))
        problem = "Could not create the skill directory.";
      else {
        if (existing.exists() || existing.isSymLink()) {
          // Keep backups outside the discovery directory, where an agent might
          // otherwise load the old skill as well as the replacement.
          backup = QFileInfo(place).dir().filePath(
              "skill-backups/omaedit-" +
              QUuid::createUuid().toString(QUuid::WithoutBraces));
          if (!QDir().mkpath(QFileInfo(backup).absolutePath()) ||
              !QDir().rename(target, backup))
            problem = "Could not move the existing entry to a backup.";
          else
            row["backup"] = backup;
        }
        if (problem.isEmpty()) {
          if (QFile::link(folder, target))
            matches = true;
          else {
            problem = "Could not create the skill link.";
            if (!backup.isEmpty() && !QDir().rename(backup, target))
              problem += " The previous entry remains at " + backup + '.';
          }
        }
      }
      row["linked"] = matches;
      if (!problem.isEmpty()) {
        row["note"] = problem;
        errors << target + ": " + problem;
      }
    }
    if (matches)
      ++linked;
    rows << row;
  }
  QVariantMap payload{{"executable", QCoreApplication::applicationFilePath()},
                      {"skill", folder + "/SKILL.md"},
                      {"places", rows},
                      {"linked", linked}};
  if (!errors.isEmpty())
    return answer(payload, "skill_link_failed", errors.join("\n"));
  payload["advice"] =
      parser.isSet("link") ? "Start a new agent session to discover the skill. "
                             "Links follow application updates."
      : linked == rows.size()
          ? "The skill is registered with every detected agent."
          : "Run omaedit skill --link to register it with this user's agents.";
  return answer(payload);
}
