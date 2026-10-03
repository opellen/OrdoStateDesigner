#include "infra/scxml_io.h"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

namespace app {

namespace {

void emitAction(QXmlStreamWriter& writer, const QString& action) {
    const QString trimmed = action.trimmed();
    if (trimmed.isEmpty()) return;

    // Check for assign form: loc = expr
    const int eqIdx = trimmed.indexOf(QLatin1Char('='));
    if (eqIdx > 0 && !trimmed.startsWith(QStringLiteral("==")) && trimmed.indexOf(QStringLiteral("==")) != eqIdx) {
        const QString loc = trimmed.left(eqIdx).trimmed();
        const QString expr = trimmed.mid(eqIdx + 1).trimmed();
        // Valid identifier check for location
        static const QRegularExpression kIdRe(QStringLiteral("^[a-zA-Z_][a-zA-Z0-9_.]*$"));
        if (kIdRe.match(loc).hasMatch()) {
            writer.writeStartElement(QStringLiteral("assign"));
            writer.writeAttribute(QStringLiteral("location"), loc);
            writer.writeAttribute(QStringLiteral("expr"), expr);
            writer.writeEndElement();
            return;
        }
    }

    // Check for raise(EVENT)
    if (trimmed.startsWith(QStringLiteral("raise(")) && trimmed.endsWith(QLatin1Char(')'))) {
        const QString ev = trimmed.mid(6, trimmed.length() - 7).trimmed();
        writer.writeStartElement(QStringLiteral("raise"));
        writer.writeAttribute(QStringLiteral("event"), ev);
        writer.writeEndElement();
        return;
    }

    // Check for sendTo(target, EVENT)
    if (trimmed.startsWith(QStringLiteral("sendTo(")) && trimmed.endsWith(QLatin1Char(')'))) {
        const QString inner = trimmed.mid(7, trimmed.length() - 8).trimmed();
        const QStringList parts = inner.split(QLatin1Char(','));
        if (parts.size() >= 2) {
            const QString target = parts[0].trimmed();
            const QString ev = parts[1].trimmed();
            writer.writeStartElement(QStringLiteral("send"));
            writer.writeAttribute(QStringLiteral("target"), target);
            writer.writeAttribute(QStringLiteral("event"), ev);
            writer.writeEndElement();
            return;
        }
    }

    // Default: <script>
    writer.writeStartElement(QStringLiteral("script"));
    writer.writeCharacters(trimmed);
    writer.writeEndElement();
}

QString parseActionFromElement(QXmlStreamReader& reader) {
    const QStringView tag = reader.name();
    if (tag == QStringLiteral("assign")) {
        const QString loc = reader.attributes().value(QStringLiteral("location")).toString();
        const QString expr = reader.attributes().value(QStringLiteral("expr")).toString();
        reader.skipCurrentElement();
        if (!loc.isEmpty() && !expr.isEmpty()) {
            return QStringLiteral("%1 = %2").arg(loc, expr);
        }
        return QString();
    }
    if (tag == QStringLiteral("raise")) {
        const QString ev = reader.attributes().value(QStringLiteral("event")).toString();
        reader.skipCurrentElement();
        if (!ev.isEmpty()) {
            return QStringLiteral("raise(%1)").arg(ev);
        }
        return QString();
    }
    if (tag == QStringLiteral("send")) {
        const QString target = reader.attributes().value(QStringLiteral("target")).toString();
        const QString ev = reader.attributes().value(QStringLiteral("event")).toString();
        reader.skipCurrentElement();
        if (!target.isEmpty() && !ev.isEmpty()) {
            return QStringLiteral("sendTo(%1, %2)").arg(target, ev);
        }
        if (!ev.isEmpty()) {
            return QStringLiteral("raise(%1)").arg(ev);
        }
        return QString();
    }
    if (tag == QStringLiteral("script")) {
        const QString text = reader.readElementText().trimmed();
        return text;
    }
    if (tag == QStringLiteral("log")) {
        const QString expr = reader.attributes().value(QStringLiteral("expr")).toString();
        reader.skipCurrentElement();
        return expr.isEmpty() ? QString() : QStringLiteral("log(%1)").arg(expr);
    }
    reader.skipCurrentElement();
    return QString();
}

ContextType inferContextType(const QString& val) {
    const QString t = val.trimmed();
    if (t == QStringLiteral("true") || t == QStringLiteral("false")) {
        return ContextType::Bool;
    }
    bool ok = false;
    t.toInt(&ok);
    if (ok) return ContextType::Int;
    t.toDouble(&ok);
    if (ok) return ContextType::Double;
    if (t.startsWith(QLatin1Char('{')) && t.endsWith(QLatin1Char('}'))) {
        return ContextType::Object;
    }
    return ContextType::String;
}

}  // namespace

ScxmlExportResult machineToScxml(const Machine& machine, const ScxmlExportOptions& options) {
    ScxmlExportResult result;
    QByteArray buffer;
    QBuffer device(&buffer);
    device.open(QIODevice::WriteOnly);

    QXmlStreamWriter writer(&device);
    writer.setAutoFormatting(true);
    writer.setAutoFormattingIndent(2);
    writer.writeStartDocument(QStringLiteral("1.0"));

    writer.writeStartElement(QStringLiteral("scxml"));
    writer.writeAttribute(QStringLiteral("xmlns"), QStringLiteral("http://www.w3.org/2005/07/scxml"));
    writer.writeAttribute(QStringLiteral("version"), QStringLiteral("1.0"));
    if (!machine.name.isEmpty()) {
        writer.writeAttribute(QStringLiteral("name"), machine.name);
    }

    // Name mapping helper
    auto stateNameById = [&](quint64 id) -> QString {
        for (const auto& s : machine.states) {
            if (s.id == id) return s.name;
        }
        return QString();
    };

    if (machine.initialStateId != 0) {
        const QString initName = stateNameById(machine.initialStateId);
        if (!initName.isEmpty()) {
            writer.writeAttribute(QStringLiteral("initial"), initName);
        }
    }

    // Datamodel
    if (!machine.context.isEmpty()) {
        writer.writeStartElement(QStringLiteral("datamodel"));
        for (const auto& var : machine.context) {
            writer.writeStartElement(QStringLiteral("data"));
            writer.writeAttribute(QStringLiteral("id"), var.name);
            writer.writeAttribute(QStringLiteral("expr"), var.initialValue);
            writer.writeEndElement();  // data
        }
        writer.writeEndElement();  // datamodel
    }

    // Recursive state emitter
    auto hasChildren = [&](quint64 stateId) -> bool {
        for (const auto& s : machine.states) {
            if (s.parentId == stateId) return true;
        }
        return false;
    };

    auto emitStates = [&](auto self, quint64 parentId) -> void {
        for (const auto& state : machine.states) {
            if (state.parentId != parentId) continue;

            QString tag;
            switch (state.kind) {
                case StateKind::Parallel:
                    tag = QStringLiteral("parallel");
                    break;
                case StateKind::Final:
                    tag = QStringLiteral("final");
                    break;
                case StateKind::History:
                    tag = QStringLiteral("history");
                    break;
                case StateKind::Normal:
                default:
                    tag = QStringLiteral("state");
                    break;
            }

            writer.writeStartElement(tag);
            writer.writeAttribute(QStringLiteral("id"), state.name);

            if (state.kind == StateKind::Normal && hasChildren(state.id) && state.initialChildId != 0) {
                const QString childInit = stateNameById(state.initialChildId);
                if (!childInit.isEmpty()) {
                    writer.writeAttribute(QStringLiteral("initial"), childInit);
                }
            } else if (state.kind == StateKind::History) {
                writer.writeAttribute(QStringLiteral("type"),
                                      state.historyDeep ? QStringLiteral("deep") : QStringLiteral("shallow"));
            }

            // Invocations
            if (!state.invocations.isEmpty()) {
                for (const auto& inv : state.invocations) {
                    writer.writeStartElement(QStringLiteral("invoke"));
                    writer.writeAttribute(QStringLiteral("type"), QStringLiteral("scxml"));
                    if (!inv.src.isEmpty()) writer.writeAttribute(QStringLiteral("src"), inv.src);
                    if (!inv.id.isEmpty()) writer.writeAttribute(QStringLiteral("id"), inv.id);
                    writer.writeEndElement();
                }
            } else if (!state.invokeSrc.isEmpty()) {
                writer.writeStartElement(QStringLiteral("invoke"));
                writer.writeAttribute(QStringLiteral("type"), QStringLiteral("scxml"));
                writer.writeAttribute(QStringLiteral("src"), state.invokeSrc);
                if (!state.invokeId.isEmpty()) {
                    writer.writeAttribute(QStringLiteral("id"), state.invokeId);
                }
                writer.writeEndElement();
            }

            // Actions
            if (options.includeActions) {
                if (!state.entryActions.isEmpty()) {
                    writer.writeStartElement(QStringLiteral("onentry"));
                    for (const auto& act : state.entryActions) {
                        emitAction(writer, act);
                    }
                    writer.writeEndElement();
                }
                if (!state.exitActions.isEmpty()) {
                    writer.writeStartElement(QStringLiteral("onexit"));
                    for (const auto& act : state.exitActions) {
                        emitAction(writer, act);
                    }
                    writer.writeEndElement();
                }
            }

            // Transitions from this state
            for (const auto& t : machine.transitions) {
                if (t.from != state.id) continue;

                writer.writeStartElement(QStringLiteral("transition"));
                if (!t.always && !t.event.isEmpty()) {
                    writer.writeAttribute(QStringLiteral("event"), t.event);
                }
                if (!t.guard.isEmpty()) {
                    writer.writeAttribute(QStringLiteral("cond"), t.guard);
                }
                if (t.reenter) {
                    writer.writeAttribute(QStringLiteral("type"), QStringLiteral("external"));
                }

                if (t.isMultiTarget()) {
                    QStringList targetNames;
                    for (quint64 tid : t.targets) {
                        const QString tn = stateNameById(tid);
                        if (!tn.isEmpty()) targetNames.append(tn);
                    }
                    writer.writeAttribute(QStringLiteral("target"), targetNames.join(QLatin1Char(' ')));
                } else if (t.to != 0) {
                    const QString tn = stateNameById(t.to);
                    if (!tn.isEmpty()) {
                        writer.writeAttribute(QStringLiteral("target"), tn);
                    }
                }

                if (options.includeActions && !t.action.isEmpty()) {
                    emitAction(writer, t.action);
                }
                writer.writeEndElement();  // transition
            }

            // Children
            self(self, state.id);

            writer.writeEndElement();  // state/parallel/final/history
        }
    };

    emitStates(emitStates, 0);

    // Root-level transitions (from == 0)
    for (const auto& t : machine.transitions) {
        if (t.from != 0) continue;

        writer.writeStartElement(QStringLiteral("transition"));
        if (!t.always && !t.event.isEmpty()) {
            writer.writeAttribute(QStringLiteral("event"), t.event);
        }
        if (!t.guard.isEmpty()) {
            writer.writeAttribute(QStringLiteral("cond"), t.guard);
        }
        if (t.reenter) {
            writer.writeAttribute(QStringLiteral("type"), QStringLiteral("external"));
        }

        if (t.isMultiTarget()) {
            QStringList targetNames;
            for (quint64 tid : t.targets) {
                const QString tn = stateNameById(tid);
                if (!tn.isEmpty()) targetNames.append(tn);
            }
            writer.writeAttribute(QStringLiteral("target"), targetNames.join(QLatin1Char(' ')));
        } else if (t.to != 0) {
            const QString tn = stateNameById(t.to);
            if (!tn.isEmpty()) {
                writer.writeAttribute(QStringLiteral("target"), tn);
            }
        }

        if (options.includeActions && !t.action.isEmpty()) {
            emitAction(writer, t.action);
        }
        writer.writeEndElement();  // transition
    }

    // Metadata layout
    if (options.includeLayout) {
        writer.writeStartElement(QStringLiteral("metadata"));
        writer.writeStartElement(QStringLiteral("layout"));
        for (const auto& state : machine.states) {
            writer.writeStartElement(QStringLiteral("state"));
            writer.writeAttribute(QStringLiteral("id"), state.name);
            writer.writeAttribute(QStringLiteral("x"), QString::number(state.pos.x(), 'f', 1));
            writer.writeAttribute(QStringLiteral("y"), QString::number(state.pos.y(), 'f', 1));
            writer.writeEndElement();
        }
        writer.writeEndElement();  // layout
        writer.writeEndElement();  // metadata
    }

    writer.writeEndElement();  // scxml
    writer.writeEndDocument();

    device.close();
    result.xml = buffer;
    result.ok = true;
    return result;
}

ScxmlImportResult scxmlToMachine(const QByteArray& xmlData) {
    ScxmlImportResult result;
    if (xmlData.trimmed().isEmpty()) {
        result.ok = false;
        result.error = QStringLiteral("Empty XML input");
        return result;
    }

    QXmlStreamReader reader(xmlData);
    Machine& machine = result.machine;
    machine.nextId = 1;

    QString rootInitialName;
    QList<quint64> stateStack;
    QMap<quint64, QString> stateInitialChildNames;

    struct PendingTransition {
        quint64 fromStateId = 0;
        QString event;
        QString guard;
        QString action;
        bool always = false;
        bool reenter = false;
        QStringList targetNames;
    };
    QList<PendingTransition> pendingTransitions;

    QMap<QString, QPointF> layoutPositions;
    QMap<QString, quint64> nameToId;

    while (!reader.atEnd()) {
        reader.readNext();

        if (reader.isStartElement()) {
            const QStringView tag = reader.name();

            if (tag == QStringLiteral("scxml")) {
                machine.name = reader.attributes().value(QStringLiteral("name")).toString();
                if (machine.name.isEmpty()) {
                    machine.name = QStringLiteral("ImportedMachine");
                }
                rootInitialName = reader.attributes().value(QStringLiteral("initial")).toString();
            } else if (tag == QStringLiteral("data")) {
                const QString id = reader.attributes().value(QStringLiteral("id")).toString();
                const QString expr = reader.attributes().value(QStringLiteral("expr")).toString();
                if (!id.isEmpty()) {
                    ContextVariable cv;
                    cv.id = machine.nextId++;
                    cv.name = id;
                    cv.initialValue = expr;
                    cv.type = inferContextType(expr);
                    machine.context.append(cv);
                }
                reader.skipCurrentElement();
            } else if (tag == QStringLiteral("layout") || reader.namespaceUri().contains(QStringLiteral("layout"))) {
                // Inside layout metadata, read states
                while (!reader.atEnd()) {
                    reader.readNext();
                    if (reader.isStartElement() && reader.name() == QStringLiteral("state")) {
                        const QString id = reader.attributes().value(QStringLiteral("id")).toString();
                        const double x = reader.attributes().value(QStringLiteral("x")).toDouble();
                        const double y = reader.attributes().value(QStringLiteral("y")).toDouble();
                        if (!id.isEmpty()) {
                            layoutPositions[id] = QPointF(x, y);
                        }
                        reader.skipCurrentElement();
                    } else if (reader.isEndElement() && (reader.name() == QStringLiteral("layout") ||
                                                         reader.name() == QStringLiteral("metadata"))) {
                        break;
                    }
                }
            } else if (tag == QStringLiteral("state") || tag == QStringLiteral("parallel") ||
                       tag == QStringLiteral("final") || tag == QStringLiteral("history")) {
                State state;
                state.id = machine.nextId++;
                state.name = reader.attributes().value(QStringLiteral("id")).toString();
                if (state.name.isEmpty()) {
                    state.name = QStringLiteral("state_%1").arg(state.id);
                }

                state.parentId = stateStack.isEmpty() ? 0 : stateStack.last();

                if (tag == QStringLiteral("parallel")) {
                    state.kind = StateKind::Parallel;
                } else if (tag == QStringLiteral("final")) {
                    state.kind = StateKind::Final;
                } else if (tag == QStringLiteral("history")) {
                    state.kind = StateKind::History;
                    state.historyDeep = (reader.attributes().value(QStringLiteral("type")) == QStringLiteral("deep"));
                } else {
                    state.kind = StateKind::Normal;
                    const QString initChild = reader.attributes().value(QStringLiteral("initial")).toString();
                    if (!initChild.isEmpty()) {
                        stateInitialChildNames[state.id] = initChild;
                    }
                }

                nameToId[state.name] = state.id;
                machine.states.append(state);
                stateStack.append(state.id);
            } else if (tag == QStringLiteral("onentry")) {
                quint64 currentId = stateStack.isEmpty() ? 0 : stateStack.last();
                while (!reader.atEnd()) {
                    reader.readNext();
                    if (reader.isStartElement()) {
                        const QString act = parseActionFromElement(reader);
                        if (!act.isEmpty() && currentId != 0) {
                            for (auto& s : machine.states) {
                                if (s.id == currentId) {
                                    s.entryActions.append(act);
                                    break;
                                }
                            }
                        }
                    } else if (reader.isEndElement() && reader.name() == QStringLiteral("onentry")) {
                        break;
                    }
                }
            } else if (tag == QStringLiteral("onexit")) {
                quint64 currentId = stateStack.isEmpty() ? 0 : stateStack.last();
                while (!reader.atEnd()) {
                    reader.readNext();
                    if (reader.isStartElement()) {
                        const QString act = parseActionFromElement(reader);
                        if (!act.isEmpty() && currentId != 0) {
                            for (auto& s : machine.states) {
                                if (s.id == currentId) {
                                    s.exitActions.append(act);
                                    break;
                                }
                            }
                        }
                    } else if (reader.isEndElement() && reader.name() == QStringLiteral("onexit")) {
                        break;
                    }
                }
            } else if (tag == QStringLiteral("invoke")) {
                quint64 currentId = stateStack.isEmpty() ? 0 : stateStack.last();
                const QString src = reader.attributes().value(QStringLiteral("src")).toString();
                const QString id = reader.attributes().value(QStringLiteral("id")).toString();
                if (currentId != 0 && (!src.isEmpty() || !id.isEmpty())) {
                    for (auto& s : machine.states) {
                        if (s.id == currentId) {
                            Invocation inv;
                            inv.src = src;
                            inv.id = id;
                            s.invocations.append(inv);
                            if (s.invokeSrc.isEmpty()) {
                                s.invokeSrc = src;
                                s.invokeId = id;
                            }
                            break;
                        }
                    }
                }
                reader.skipCurrentElement();
            } else if (tag == QStringLiteral("transition")) {
                PendingTransition pt;
                pt.fromStateId = stateStack.isEmpty() ? 0 : stateStack.last();
                if (reader.attributes().hasAttribute(QStringLiteral("event"))) {
                    pt.event = reader.attributes().value(QStringLiteral("event")).toString();
                } else {
                    pt.always = true;
                }
                pt.guard = reader.attributes().value(QStringLiteral("cond")).toString();
                if (reader.attributes().value(QStringLiteral("type")) == QStringLiteral("external")) {
                    pt.reenter = true;
                }
                const QString targetAttr = reader.attributes().value(QStringLiteral("target")).toString().trimmed();
                if (!targetAttr.isEmpty()) {
                    static const QRegularExpression kSpaceRe(QStringLiteral("\\s+"));
                    pt.targetNames = targetAttr.split(kSpaceRe, Qt::SkipEmptyParts);
                }

                // Read child elements of transition (executable content / action)
                while (!reader.atEnd()) {
                    reader.readNext();
                    if (reader.isStartElement()) {
                        const QString act = parseActionFromElement(reader);
                        if (!act.isEmpty()) {
                            if (pt.action.isEmpty()) {
                                pt.action = act;
                            } else {
                                pt.action += QStringLiteral("; ") + act;
                            }
                        }
                    } else if (reader.isEndElement() && reader.name() == QStringLiteral("transition")) {
                        break;
                    }
                }

                pendingTransitions.append(pt);
            }
        } else if (reader.isEndElement()) {
            const QStringView tag = reader.name();
            if (tag == QStringLiteral("state") || tag == QStringLiteral("parallel") ||
                tag == QStringLiteral("final") || tag == QStringLiteral("history")) {
                if (!stateStack.isEmpty()) {
                    stateStack.removeLast();
                }
            }
        }
    }

    if (reader.hasError() && reader.error() != QXmlStreamReader::PrematureEndOfDocumentError) {
        result.ok = false;
        result.error = reader.errorString();
        return result;
    }

    // Resolve initial state
    if (!rootInitialName.isEmpty()) {
        machine.initialStateId = nameToId.value(rootInitialName, 0);
    }
    if (machine.initialStateId == 0) {
        for (const auto& s : machine.states) {
            if (s.parentId == 0) {
                machine.initialStateId = s.id;
                break;
            }
        }
    }

    // Resolve compound initial child IDs
    for (auto it = stateInitialChildNames.begin(); it != stateInitialChildNames.end(); ++it) {
        const quint64 parentId = it.key();
        const QString& childName = it.value();
        const quint64 childId = nameToId.value(childName, 0);
        for (auto& s : machine.states) {
            if (s.id == parentId) {
                s.initialChildId = childId;
                break;
            }
        }
    }

    // Resolve transitions
    for (const auto& pt : pendingTransitions) {
        Transition t;
        t.id = machine.nextId++;
        t.from = pt.fromStateId;
        t.event = pt.event;
        t.guard = pt.guard;
        t.action = pt.action;
        t.always = pt.always;
        t.reenter = pt.reenter;

        if (pt.targetNames.isEmpty()) {
            t.to = 0;
        } else if (pt.targetNames.size() == 1) {
            t.to = nameToId.value(pt.targetNames.first(), 0);
            t.targets = {t.to};
        } else {
            for (const auto& tn : pt.targetNames) {
                const quint64 tid = nameToId.value(tn, 0);
                if (tid != 0) {
                    t.targets.append(tid);
                }
            }
            if (!t.targets.isEmpty()) {
                t.to = t.targets.first();
            }
        }

        machine.transitions.append(t);
    }

    // Importers never guess geometry: a state named in <layout> keeps that
    // position, every other stays at the (0,0) origin. DocumentSession lays out
    // a fully coordinate-free machine (machineHasNoGeometry()) when opening it;
    // a mixed file keeps what it has and is not laid out.
    for (auto& s : machine.states) {
        if (layoutPositions.contains(s.name)) {
            s.pos = layoutPositions.value(s.name);
        }
    }

    result.ok = true;
    return result;
}

ScxmlImportResult importScxmlFile(const QString& filePath) {
    ScxmlImportResult result;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.ok = false;
        result.error = QStringLiteral("Could not open file: %1 (%2)").arg(filePath, file.errorString());
        return result;
    }
    const QByteArray data = file.readAll();
    result = scxmlToMachine(data);
    if (result.ok && (result.machine.name.isEmpty() || result.machine.name == QStringLiteral("ImportedMachine"))) {
        result.machine.name = QFileInfo(filePath).completeBaseName();
    }
    return result;
}

}  // namespace app
