#include "infra/plantuml_io.h"

#include <QFile>
#include <QFileInfo>
#include <QStringConverter>
#include <QTextStream>
#include <vector>

namespace app {
namespace {

QString stateAlias(uint64_t id) {
    return QStringLiteral("s%1").arg(id);
}

void emitStateHierarchy(const Machine& machine, uint64_t parentId, QStringList& lines, int indent,
                         const PlantUmlOptions& options) {
    const QString ind(indent * 2, ' ');

    std::vector<const State*> children;
    for (const auto& s : machine.states) {
        if (s.parentId == parentId) {
            children.push_back(&s);
        }
    }

    uint64_t initialChildId = 0;
    if (parentId == 0) {
        initialChildId = machine.initialStateId;
    } else {
        for (const auto& s : machine.states) {
            if (s.id == parentId) {
                initialChildId = s.initialChildId;
                break;
            }
        }
    }

    if (initialChildId != 0) {
        lines.append(QStringLiteral("%1[*] --> %2").arg(ind, stateAlias(initialChildId)));
    }

    for (const State* s : children) {
        const QString alias = stateAlias(s->id);
        const QString displayName = !s->name.isEmpty() ? s->name : alias;

        bool hasChildren = false;
        for (const auto& other : machine.states) {
            if (other.parentId == s->id) {
                hasChildren = true;
                break;
            }
        }

        if (hasChildren) {
            lines.append(QStringLiteral("%1state \"%2\" as %3 {").arg(ind, displayName, alias));
            emitStateHierarchy(machine, s->id, lines, indent + 1, options);
            lines.append(QStringLiteral("%1}").arg(ind));
        } else {
            lines.append(QStringLiteral("%1state \"%2\" as %3").arg(ind, displayName, alias));
        }

        if (options.includeActionsGuards) {
            for (const QString& entry : s->entryActions) {
                lines.append(QStringLiteral("%1%2 : entry / %3").arg(ind, alias, entry));
            }
            for (const QString& exit : s->exitActions) {
                lines.append(QStringLiteral("%1%2 : exit / %3").arg(ind, alias, exit));
            }
        }
        if (s->kind == StateKind::Final) {
            lines.append(QStringLiteral("%1%2 : <<final>>").arg(ind, alias));
        } else if (s->kind == StateKind::History) {
            lines.append(QStringLiteral("%1%2 : [H%3]").arg(ind, alias, s->historyDeep ? QStringLiteral("*") : QString()));
        }
    }
}

}  // namespace

PlantUmlExportResult machineToPlantUml(const Machine& machine, const PlantUmlOptions& options) {
    PlantUmlExportResult res;
    QStringList lines;

    lines.append(QStringLiteral("@startuml"));
    if (!machine.name.isEmpty()) {
        lines.append(QStringLiteral("title %1").arg(machine.name));
    }
    if (options.includeSkinparam) {
        lines.append(QStringLiteral("skinparam state {"));
        lines.append(QStringLiteral("  FontSize 12"));
        lines.append(QStringLiteral("}"));
        lines.append(QString());
    }

    emitStateHierarchy(machine, 0, lines, 0, options);
    lines.append(QString());

    for (const auto& t : machine.transitions) {
        const QString src = (t.from == 0) ? QStringLiteral("[*]") : stateAlias(t.from);

        QString label;
        if (!t.event.isEmpty()) {
            label = t.event;
        }
        if (options.includeActionsGuards) {
            if (!t.guard.isEmpty()) {
                label += QStringLiteral(" [%1]").arg(t.guard);
            }
            if (!t.action.isEmpty()) {
                label += QStringLiteral(" / %1").arg(t.action);
            }
        }

        const auto targets = t.effectiveTargets();
        if (targets.isEmpty()) {
            if (!label.isEmpty()) {
                lines.append(QStringLiteral("%1 : %2").arg(src, label));
            }
        } else {
            for (quint64 tgtId : targets) {
                const QString dst = stateAlias(tgtId);
                if (!label.isEmpty()) {
                    lines.append(QStringLiteral("%1 --> %2 : %3").arg(src, dst, label));
                } else {
                    lines.append(QStringLiteral("%1 --> %2").arg(src, dst));
                }
            }
        }
    }

    lines.append(QString());
    lines.append(QStringLiteral("@enduml"));
    lines.append(QString());

    res.plantUml = lines.join(QStringLiteral("\n"));
    res.ok = true;
    return res;
}

bool exportPlantUmlFile(const Machine& machine, const QString& path, const PlantUmlOptions& options,
                        QStringList* diagnostics, QString* error) {
    PlantUmlExportResult res = machineToPlantUml(machine, options);
    if (!res.ok) {
        if (error) *error = res.error;
        return false;
    }
    if (diagnostics) {
        *diagnostics = res.diagnostics;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (error) {
            *error = QStringLiteral("Could not write file %1: %2").arg(path, file.errorString());
        }
        return false;
    }

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << res.plantUml;
    return true;
}

}  // namespace app
