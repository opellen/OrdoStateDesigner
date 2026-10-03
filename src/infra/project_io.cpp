#include "infra/project_io.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QPointF>

namespace app {

namespace {

// Format history; keys added after v1 are optional on load. v1: start state is kind
// "Initial" (promoted to "initialStateId" on load, first in document order); v2: "initialStateId",
// Parallel/History. v3: transition `from` may be 0 (the machine itself). v4: "notes". v5: "parentId"/
// "initialChildId". v6: "historyDeep". v7: "color". v8: "context" (omitted when empty).
// v9: "invokeSrc"/"invokeId"/"invokeOutputType". v10: "types", "externalHeaders",
// "payloadType", "customTypeName", "labelRatio" (written only when set).
constexpr int kFormatVersion = 10;

// Field type <-> name. fieldTypeFromString returns nullopt for a non-empty
// unrecognized string (machineFromJson() then fails the load); an absent
// "type" reads Int.
QString fieldTypeToString(FieldType type) {
    switch (type) {
        case FieldType::Bool:
            return QStringLiteral("Bool");
        case FieldType::Double:
            return QStringLiteral("Double");
        case FieldType::String:
            return QStringLiteral("String");
        case FieldType::Custom:
            return QStringLiteral("Custom");
        case FieldType::Int:
        default:
            return QStringLiteral("Int");
    }
}

std::optional<FieldType> fieldTypeFromString(const QString& text) {
    if (text.isEmpty()) return FieldType::Int;
    if (text == QStringLiteral("Bool")) return FieldType::Bool;
    if (text == QStringLiteral("Int")) return FieldType::Int;
    if (text == QStringLiteral("Double")) return FieldType::Double;
    if (text == QStringLiteral("String")) return FieldType::String;
    if (text == QStringLiteral("Custom")) return FieldType::Custom;
    return std::nullopt;
}

// The "supported list" texts below iterate the enum through its *ToString()
// function, so a new enumerator needs only its *ToString case.
QString supportedFieldTypesText() {
    QStringList names;
    for (FieldType type : {FieldType::Bool, FieldType::Int, FieldType::Double, FieldType::String, FieldType::Custom}) {
        names << fieldTypeToString(type);
    }
    return names.join(QStringLiteral(", "));
}

// Element color <-> palette name. An absent "color" reads Default; a present but
// unrecognized name returns nullopt (machineFromJson() fails the load).
QString elementColorToString(ElementColor color) {
    switch (color) {
        case ElementColor::Red:
            return QStringLiteral("Red");
        case ElementColor::Orange:
            return QStringLiteral("Orange");
        case ElementColor::Yellow:
            return QStringLiteral("Yellow");
        case ElementColor::Green:
            return QStringLiteral("Green");
        case ElementColor::Blue:
            return QStringLiteral("Blue");
        case ElementColor::Purple:
            return QStringLiteral("Purple");
        case ElementColor::Pink:
            return QStringLiteral("Pink");
        case ElementColor::Gray:
            return QStringLiteral("Gray");
        case ElementColor::Default:
        default:
            return QStringLiteral("Default");
    }
}

std::optional<ElementColor> elementColorFromString(const QString& text) {
    if (text.isEmpty()) return ElementColor::Default;
    if (text == QStringLiteral("Default")) return ElementColor::Default;
    if (text == QStringLiteral("Red")) return ElementColor::Red;
    if (text == QStringLiteral("Orange")) return ElementColor::Orange;
    if (text == QStringLiteral("Yellow")) return ElementColor::Yellow;
    if (text == QStringLiteral("Green")) return ElementColor::Green;
    if (text == QStringLiteral("Blue")) return ElementColor::Blue;
    if (text == QStringLiteral("Purple")) return ElementColor::Purple;
    if (text == QStringLiteral("Pink")) return ElementColor::Pink;
    if (text == QStringLiteral("Gray")) return ElementColor::Gray;
    return std::nullopt;
}

QString supportedElementColorsText() {
    QStringList names;
    for (ElementColor color : {ElementColor::Default, ElementColor::Red, ElementColor::Orange, ElementColor::Yellow,
                                ElementColor::Green, ElementColor::Blue, ElementColor::Purple, ElementColor::Pink,
                                ElementColor::Gray}) {
        names << elementColorToString(color);
    }
    return names.join(QStringLiteral(", "));
}

// Context variable type name <-> ContextType. An absent "type" reads Int; a
// present but unrecognized name returns nullopt (machineFromJson() fails the
// load). contextTypeToString lives at namespace-app scope below.
std::optional<ContextType> contextTypeFromString(const QString& text) {
    if (text.isEmpty()) return ContextType::Int;
    if (text == QStringLiteral("Bool")) return ContextType::Bool;
    if (text == QStringLiteral("Int")) return ContextType::Int;
    if (text == QStringLiteral("Double")) return ContextType::Double;
    if (text == QStringLiteral("String")) return ContextType::String;
    if (text == QStringLiteral("Object")) return ContextType::Object;
    return std::nullopt;
}

QString supportedContextTypesText() {
    QStringList names;
    for (ContextType type :
         {ContextType::Bool, ContextType::Int, ContextType::Double, ContextType::String, ContextType::Object}) {
        names << contextTypeToString(type);
    }
    return names.join(QStringLiteral(", "));
}

// State kind <-> name. An absent "kind" reads Normal; "Initial" stays legal
// (promoted to initialStateId in machineFromJson()). stateKindFromString returns
// nullopt only for a present but unrecognized name (the load then fails).
QString stateKindToString(StateKind kind) {
    switch (kind) {
        case StateKind::Parallel:
            return QStringLiteral("Parallel");
        case StateKind::Final:
            return QStringLiteral("Final");
        case StateKind::History:
            return QStringLiteral("History");
        case StateKind::Normal:
        default:
            return QStringLiteral("Normal");
    }
}

std::optional<StateKind> stateKindFromString(const QString& text) {
    if (text.isEmpty()) return StateKind::Normal;
    if (text == QStringLiteral("Normal")) return StateKind::Normal;
    if (text == QStringLiteral("Parallel")) return StateKind::Parallel;
    if (text == QStringLiteral("Final")) return StateKind::Final;
    if (text == QStringLiteral("History")) return StateKind::History;
    if (text == QStringLiteral("Initial")) return StateKind::Normal;  // v1 legacy; promoted in machineFromJson()
    return std::nullopt;
}

QString supportedStateKindsText() {
    QStringList names;
    for (StateKind kind : {StateKind::Normal, StateKind::Parallel, StateKind::Final, StateKind::History}) {
        names << stateKindToString(kind);
    }
    return names.join(QStringLiteral(", "));
}

// Shared diagnostic for every unknown-enum-string failure: machine, element,
// field, offending value and the supported set.
QString enumLoadError(const QString& machineName, const QString& elementText, const QString& fieldName,
                       const QString& badValue, const QString& supportedLabel, const QString& supportedValues) {
    return QStringLiteral("Failed to load machine '%1': %2 has invalid %3 '%4'.\nSupported %5: %6.")
        .arg(machineName, elementText, fieldName, badValue, supportedLabel, supportedValues);
}

QStringList toStringList(const QJsonArray& array) {
    QStringList result;
    result.reserve(array.size());
    for (const QJsonValue& value : array) {
        result.push_back(value.toString());
    }
    return result;
}

bool writeJsonFile(const QJsonObject& object, const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    const QJsonDocument document(object);
    if (file.write(document.toJson(QJsonDocument::Indented)) < 0) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    return true;
}

bool readJsonFile(const QString& path, QJsonObject* object, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error) {
            *error = parseError.errorString();
        }
        return false;
    }
    if (!document.isObject()) {
        if (error) {
            *error = QStringLiteral("root is not a JSON object");
        }
        return false;
    }
    *object = document.object();
    return true;
}

}  // namespace

// Context variable type -> name; at namespace scope (declared in project_io.h)
// for callers outside this TU.
QString contextTypeToString(ContextType type) {
    switch (type) {
        case ContextType::Bool:
            return QStringLiteral("Bool");
        case ContextType::Double:
            return QStringLiteral("Double");
        case ContextType::String:
            return QStringLiteral("String");
        case ContextType::Object:
            return QStringLiteral("Object");
        case ContextType::Int:
        default:
            return QStringLiteral("Int");
    }
}

QJsonObject machineToJson(const Machine& machine) {
    QJsonObject object;
    object[QStringLiteral("formatVersion")] = kFormatVersion;
    object[QStringLiteral("name")] = machine.name;
    object[QStringLiteral("nextId")] = static_cast<qint64>(machine.nextId);
    object[QStringLiteral("initialStateId")] = static_cast<qint64>(machine.initialStateId);

    QJsonArray states;
    for (const State& state : machine.states) {
        QJsonObject stateObject;
        stateObject[QStringLiteral("id")] = static_cast<qint64>(state.id);
        stateObject[QStringLiteral("name")] = state.name;
        stateObject[QStringLiteral("kind")] = stateKindToString(state.kind);
        stateObject[QStringLiteral("x")] = state.pos.x();
        stateObject[QStringLiteral("y")] = state.pos.y();
        stateObject[QStringLiteral("entryActions")] = QJsonArray::fromStringList(state.entryActions);
        stateObject[QStringLiteral("exitActions")] = QJsonArray::fromStringList(state.exitActions);
        stateObject[QStringLiteral("description")] = state.description;
        stateObject[QStringLiteral("tags")] = QJsonArray::fromStringList(state.tags);
        stateObject[QStringLiteral("parentId")] = static_cast<qint64>(state.parentId);
        stateObject[QStringLiteral("initialChildId")] = static_cast<qint64>(state.initialChildId);
        // v6: meaningful only for History states, serialized unconditionally.
        stateObject[QStringLiteral("historyDeep")] = state.historyDeep;
        stateObject[QStringLiteral("color")] = elementColorToString(state.color);  // v7
        // v9: meaningful only when invokeSrc is non-empty, serialized unconditionally.
        stateObject[QStringLiteral("invokeSrc")] = state.invokeSrc;
        stateObject[QStringLiteral("invokeId")] = state.invokeId;
        // v9 (no bump: meaningless without invokeSrc); same name mapping as "context" types.
        stateObject[QStringLiteral("invokeOutputType")] = contextTypeToString(state.invokeOutputType);
        if (!state.invocations.isEmpty()) {
            QJsonArray invArray;
            for (const Invocation& inv : state.invocations) {
                QJsonObject invObj;
                invObj[QStringLiteral("src")] = inv.src;
                invObj[QStringLiteral("id")] = inv.id;
                invObj[QStringLiteral("outputType")] = contextTypeToString(inv.outputType);
                invArray.append(invObj);
            }
            stateObject[QStringLiteral("invocations")] = invArray;
        }
        states.append(stateObject);
    }
    object[QStringLiteral("states")] = states;

    QJsonArray transitions;
    for (const Transition& transition : machine.transitions) {
        QJsonObject transitionObject;
        transitionObject[QStringLiteral("id")] = static_cast<qint64>(transition.id);
        transitionObject[QStringLiteral("from")] = static_cast<qint64>(transition.from);
        transitionObject[QStringLiteral("to")] = static_cast<qint64>(transition.to);
        transitionObject[QStringLiteral("event")] = transition.event;
        transitionObject[QStringLiteral("guard")] = transition.guard;
        transitionObject[QStringLiteral("action")] = transition.action;
        transitionObject[QStringLiteral("delayMs")] = transition.delayMs;
        transitionObject[QStringLiteral("labelOffsetX")] = transition.labelOffset.x();
        transitionObject[QStringLiteral("labelOffsetY")] = transition.labelOffset.y();
        if (transition.labelRatio.has_value()) {  // v10
            transitionObject[QStringLiteral("labelRatio")] = *transition.labelRatio;
        }
        transitionObject[QStringLiteral("color")] = elementColorToString(transition.color);  // v7
        transitionObject[QStringLiteral("machineSelf")] = transition.machineSelf;            // v7
        transitionObject[QStringLiteral("reenter")] = transition.reenter;
        transitionObject[QStringLiteral("always")] = transition.always;
        if (transition.periodic) {
            transitionObject[QStringLiteral("periodic")] = true;
        }
        if (!transition.payloadType.isEmpty()) {
            transitionObject[QStringLiteral("payloadType")] = transition.payloadType;
        }
        if (transition.isMultiTarget()) {
            QJsonArray targetArray;
            for (quint64 tid : transition.targets) {
                targetArray.append(static_cast<qint64>(tid));
            }
            transitionObject[QStringLiteral("targets")] = targetArray;
        }
        transitions.append(transitionObject);
    }
    object[QStringLiteral("transitions")] = transitions;

    QJsonArray notes;
    for (const Note& note : machine.notes) {
        QJsonObject noteObject;
        noteObject[QStringLiteral("id")] = static_cast<qint64>(note.id);
        noteObject[QStringLiteral("x")] = note.pos.x();
        noteObject[QStringLiteral("y")] = note.pos.y();
        noteObject[QStringLiteral("text")] = note.text;
        noteObject[QStringLiteral("color")] = elementColorToString(note.color);  // v7
        notes.append(noteObject);
    }
    object[QStringLiteral("notes")] = notes;

    // v8: omitted entirely when empty, so a context-free machine's bytes differ
    // from v7 only in the formatVersion stamp (byte-pinned reference machines).
    if (!machine.context.isEmpty()) {
        QJsonArray context;
        for (const ContextVariable& variable : machine.context) {
            QJsonObject variableObject;
            variableObject[QStringLiteral("id")] = static_cast<qint64>(variable.id);
            variableObject[QStringLiteral("name")] = variable.name;
            variableObject[QStringLiteral("type")] = contextTypeToString(variable.type);
            variableObject[QStringLiteral("initialValue")] = variable.initialValue;
            if (!variable.customTypeName.isEmpty()) {
                variableObject[QStringLiteral("customTypeName")] = variable.customTypeName;
            }
            context.append(variableObject);
        }
        object[QStringLiteral("context")] = context;
    }

    // v10 -- user-defined struct definitions and external C++ headers
    if (!machine.types.isEmpty()) {
        QJsonArray typesArray;
        for (const StructDefinition& def : machine.types) {
            QJsonObject defObject;
            defObject[QStringLiteral("id")] = static_cast<qint64>(def.id);
            defObject[QStringLiteral("name")] = def.name;
            defObject[QStringLiteral("external")] = def.external;
            if (!def.headerPath.isEmpty()) {
                defObject[QStringLiteral("headerPath")] = def.headerPath;
            }
            QJsonArray fieldsArray;
            for (const StructField& field : def.fields) {
                QJsonObject fieldObject;
                fieldObject[QStringLiteral("name")] = field.name;
                fieldObject[QStringLiteral("type")] = fieldTypeToString(field.type);
                if (!field.customTypeName.isEmpty()) {
                    fieldObject[QStringLiteral("customTypeName")] = field.customTypeName;
                }
                fieldObject[QStringLiteral("isArray")] = field.isArray;
                if (field.arraySize > 0) {
                    fieldObject[QStringLiteral("arraySize")] = field.arraySize;
                }
                if (!field.initialValue.isEmpty()) {
                    fieldObject[QStringLiteral("initialValue")] = field.initialValue;
                }
                fieldsArray.append(fieldObject);
            }
            defObject[QStringLiteral("fields")] = fieldsArray;
            typesArray.append(defObject);
        }
        object[QStringLiteral("types")] = typesArray;
    }

    if (!machine.externalHeaders.isEmpty()) {
        object[QStringLiteral("externalHeaders")] = QJsonArray::fromStringList(machine.externalHeaders);
    }

    return object;
}

Machine machineFromJson(const QJsonObject& json, QString* error) {
    if (error) {
        error->clear();
    }
    Machine machine;
    // Absent key -> v1.
    const int formatVersion = json.value(QStringLiteral("formatVersion")).toInt(1);
    machine.name = json.value(QStringLiteral("name")).toString();
    machine.nextId = static_cast<quint64>(json.value(QStringLiteral("nextId")).toInteger(1));
    machine.initialStateId = static_cast<quint64>(json.value(QStringLiteral("initialStateId")).toInteger(0));

    for (const QJsonValue& value : json.value(QStringLiteral("states")).toArray()) {
        const QJsonObject stateObject = value.toObject();
        State state;
        state.id = static_cast<quint64>(stateObject.value(QStringLiteral("id")).toInteger());
        state.name = stateObject.value(QStringLiteral("name")).toString();
        const QString kindText = stateObject.value(QStringLiteral("kind")).toString();
        const std::optional<StateKind> kindOpt = stateKindFromString(kindText);
        if (!kindOpt.has_value()) {
            if (error) {
                *error = enumLoadError(machine.name, QStringLiteral("state '%1' (id: %2)").arg(state.name).arg(state.id),
                                        QStringLiteral("kind"), kindText, QStringLiteral("kinds"),
                                        supportedStateKindsText());
            }
            return machine;
        }
        state.kind = *kindOpt;
        // v1 legacy: kind "Initial" becomes Normal plus initialStateId; the
        // first Initial in document order wins.
        if (formatVersion < 2 && machine.initialStateId == 0 && kindText == QStringLiteral("Initial")) {
            machine.initialStateId = state.id;
        }
        state.pos = QPointF(stateObject.value(QStringLiteral("x")).toDouble(),
                             stateObject.value(QStringLiteral("y")).toDouble());
        state.entryActions = toStringList(stateObject.value(QStringLiteral("entryActions")).toArray());
        state.exitActions = toStringList(stateObject.value(QStringLiteral("exitActions")).toArray());
        state.description = stateObject.value(QStringLiteral("description")).toString();
        state.tags = toStringList(stateObject.value(QStringLiteral("tags")).toArray());
        // Absent before v5: 0 = no parent / no initial child.
        state.parentId = static_cast<quint64>(stateObject.value(QStringLiteral("parentId")).toInteger(0));
        state.initialChildId =
            static_cast<quint64>(stateObject.value(QStringLiteral("initialChildId")).toInteger(0));
        // Absent before v6: false (shallow).
        state.historyDeep = stateObject.value(QStringLiteral("historyDeep")).toBool(false);
        // Absent before v7: reads Default; an unrecognized name fails the load.
        const QString stateColorText = stateObject.value(QStringLiteral("color")).toString();
        const std::optional<ElementColor> stateColorOpt = elementColorFromString(stateColorText);
        if (!stateColorOpt.has_value()) {
            if (error) {
                *error = enumLoadError(machine.name, QStringLiteral("state '%1' (id: %2)").arg(state.name).arg(state.id),
                                        QStringLiteral("color"), stateColorText, QStringLiteral("colors"),
                                        supportedElementColorsText());
            }
            return machine;
        }
        state.color = *stateColorOpt;
        // Absent before v9: empty = no invoke declared.
        state.invokeSrc = stateObject.value(QStringLiteral("invokeSrc")).toString();
        state.invokeId = stateObject.value(QStringLiteral("invokeId")).toString();
        // Absent in v8 files and early v9 files alike: reads Int, no version check
        // needed. An unrecognized name fails the load.
        const QString invokeOutputTypeText = stateObject.value(QStringLiteral("invokeOutputType")).toString();
        const std::optional<ContextType> invokeOutputTypeOpt = contextTypeFromString(invokeOutputTypeText);
        if (!invokeOutputTypeOpt.has_value()) {
            if (error) {
                *error = enumLoadError(machine.name, QStringLiteral("state '%1' (id: %2)").arg(state.name).arg(state.id),
                                        QStringLiteral("invokeOutputType"), invokeOutputTypeText,
                                        QStringLiteral("types"), supportedContextTypesText());
            }
            return machine;
        }
        state.invokeOutputType = *invokeOutputTypeOpt;
        if (stateObject.contains(QStringLiteral("invocations"))) {
            const QJsonArray invArray = stateObject.value(QStringLiteral("invocations")).toArray();
            for (const QJsonValue& iv : invArray) {
                const QJsonObject io = iv.toObject();
                Invocation invocation;
                invocation.src = io.value(QStringLiteral("src")).toString();
                invocation.id = io.value(QStringLiteral("id")).toString();
                const QString invOutputTypeText = io.value(QStringLiteral("outputType")).toString();
                const std::optional<ContextType> invOutputTypeOpt = contextTypeFromString(invOutputTypeText);
                if (!invOutputTypeOpt.has_value()) {
                    if (error) {
                        *error = enumLoadError(
                            machine.name,
                            QStringLiteral("state '%1' (id: %2) invocation '%3'")
                                .arg(state.name)
                                .arg(state.id)
                                .arg(effectiveInvocationId(invocation)),
                            QStringLiteral("outputType"), invOutputTypeText, QStringLiteral("types"),
                            supportedContextTypesText());
                    }
                    return machine;
                }
                invocation.outputType = *invOutputTypeOpt;
                state.invocations.push_back(invocation);
            }
        }
        machine.states.push_back(state);
    }

    for (const QJsonValue& value : json.value(QStringLiteral("transitions")).toArray()) {
        const QJsonObject transitionObject = value.toObject();
        Transition transition;
        transition.id = static_cast<quint64>(transitionObject.value(QStringLiteral("id")).toInteger());
        transition.from = static_cast<quint64>(transitionObject.value(QStringLiteral("from")).toInteger());
        transition.to = static_cast<quint64>(transitionObject.value(QStringLiteral("to")).toInteger());
        transition.event = transitionObject.value(QStringLiteral("event")).toString();
        transition.guard = transitionObject.value(QStringLiteral("guard")).toString();
        transition.action = transitionObject.value(QStringLiteral("action")).toString();
        transition.delayMs = transitionObject.value(QStringLiteral("delayMs")).toInt();
        transition.labelOffset = QPointF(transitionObject.value(QStringLiteral("labelOffsetX")).toDouble(),
                                          transitionObject.value(QStringLiteral("labelOffsetY")).toDouble());
        // Absent: nullopt = router default.
        if (transitionObject.contains(QStringLiteral("labelRatio"))) {
            transition.labelRatio = transitionObject.value(QStringLiteral("labelRatio")).toDouble();
        }
        // v7: an empty name reads Default; an unrecognized name fails the load.
        const QString transitionColorText = transitionObject.value(QStringLiteral("color")).toString();
        const std::optional<ElementColor> transitionColorOpt = elementColorFromString(transitionColorText);
        if (!transitionColorOpt.has_value()) {
            if (error) {
                *error = enumLoadError(machine.name, QStringLiteral("transition %1").arg(transition.id),
                                        QStringLiteral("color"), transitionColorText, QStringLiteral("colors"),
                                        supportedElementColorsText());
            }
            return machine;
        }
        transition.color = *transitionColorOpt;
        transition.machineSelf = transitionObject.value(QStringLiteral("machineSelf")).toBool(false);
        transition.reenter = transitionObject.value(QStringLiteral("reenter")).toBool(false);
        transition.always = transitionObject.value(QStringLiteral("always")).toBool(false);
        transition.periodic = transitionObject.value(QStringLiteral("periodic")).toBool(false);
        transition.payloadType = transitionObject.value(QStringLiteral("payloadType")).toString();
        const QJsonArray targetsArray = transitionObject.value(QStringLiteral("targets")).toArray();
        if (!targetsArray.isEmpty()) {
            for (const QJsonValue& tval : targetsArray) {
                transition.targets.append(static_cast<quint64>(tval.toInteger()));
            }
            if (transition.to == 0 && !transition.targets.isEmpty()) {
                transition.to = transition.targets.first();
            }
        }
        machine.transitions.push_back(transition);
    }

    for (const QJsonValue& value : json.value(QStringLiteral("notes")).toArray()) {
        const QJsonObject noteObject = value.toObject();
        Note note;
        note.id = static_cast<quint64>(noteObject.value(QStringLiteral("id")).toInteger());
        note.pos = QPointF(noteObject.value(QStringLiteral("x")).toDouble(),
                            noteObject.value(QStringLiteral("y")).toDouble());
        note.text = noteObject.value(QStringLiteral("text")).toString();
        // v7: an empty name reads Default; an unrecognized name fails the load.
        const QString noteColorText = noteObject.value(QStringLiteral("color")).toString();
        const std::optional<ElementColor> noteColorOpt = elementColorFromString(noteColorText);
        if (!noteColorOpt.has_value()) {
            if (error) {
                *error = enumLoadError(machine.name, QStringLiteral("note %1").arg(note.id), QStringLiteral("color"),
                                        noteColorText, QStringLiteral("colors"), supportedElementColorsText());
            }
            return machine;
        }
        note.color = *noteColorOpt;
        machine.notes.push_back(note);
    }

    for (const QJsonValue& value : json.value(QStringLiteral("context")).toArray()) {
        const QJsonObject variableObject = value.toObject();
        ContextVariable variable;
        variable.id = static_cast<quint64>(variableObject.value(QStringLiteral("id")).toInteger());
        variable.name = variableObject.value(QStringLiteral("name")).toString();
        const QString variableTypeText = variableObject.value(QStringLiteral("type")).toString();
        const std::optional<ContextType> variableTypeOpt = contextTypeFromString(variableTypeText);
        if (!variableTypeOpt.has_value()) {
            if (error) {
                *error = enumLoadError(machine.name, QStringLiteral("context variable '%1'").arg(variable.name),
                                        QStringLiteral("type"), variableTypeText, QStringLiteral("types"),
                                        supportedContextTypesText());
            }
            return machine;
        }
        variable.type = *variableTypeOpt;
        variable.initialValue = variableObject.value(QStringLiteral("initialValue")).toString();
        variable.customTypeName = variableObject.value(QStringLiteral("customTypeName")).toString();
        machine.context.push_back(variable);
    }

    for (const QJsonValue& value : json.value(QStringLiteral("types")).toArray()) {
        const QJsonObject defObject = value.toObject();
        StructDefinition def;
        def.id = static_cast<quint64>(defObject.value(QStringLiteral("id")).toInteger());
        def.name = defObject.value(QStringLiteral("name")).toString();
        def.external = defObject.value(QStringLiteral("external")).toBool(false);
        def.headerPath = defObject.value(QStringLiteral("headerPath")).toString();
        for (const QJsonValue& fval : defObject.value(QStringLiteral("fields")).toArray()) {
            const QJsonObject fieldObject = fval.toObject();
            StructField field;
            field.name = fieldObject.value(QStringLiteral("name")).toString();
            const QString fieldTypeText = fieldObject.value(QStringLiteral("type")).toString();
            const std::optional<FieldType> fieldTypeOpt = fieldTypeFromString(fieldTypeText);
            if (!fieldTypeOpt.has_value()) {
                if (error) {
                    *error = enumLoadError(machine.name,
                                            QStringLiteral("struct '%1' field '%2'").arg(def.name, field.name),
                                            QStringLiteral("type"), fieldTypeText, QStringLiteral("types"),
                                            supportedFieldTypesText());
                }
                return machine;
            }
            field.type = *fieldTypeOpt;
            field.customTypeName = fieldObject.value(QStringLiteral("customTypeName")).toString();
            field.isArray = fieldObject.value(QStringLiteral("isArray")).toBool(false);
            field.arraySize = fieldObject.value(QStringLiteral("arraySize")).toInt(0);
            field.initialValue = fieldObject.value(QStringLiteral("initialValue")).toString();
            def.fields.push_back(field);
        }
        machine.types.push_back(def);
    }

    machine.externalHeaders = toStringList(json.value(QStringLiteral("externalHeaders")).toArray());

    return machine;
}

bool saveMachine(const Machine& machine, const QString& path, QString* error) {
    return writeJsonFile(machineToJson(machine), path, error);
}

bool loadMachine(const QString& path, Machine* machine, QString* error) {
    QJsonObject object;
    if (!readJsonFile(path, &object, error)) {
        return false;
    }
    // Always pass an error string: a non-empty one is the only failure signal,
    // since machineFromJson() returns a Machine by value.
    QString parseError;
    *machine = machineFromJson(object, &parseError);
    if (!parseError.isEmpty()) {
        if (error) {
            *error = parseError;
        }
        return false;
    }
    return true;
}

QJsonObject projectToJson(const Project& project) {
    QJsonObject object;
    // Stamped for parity with .sdm; projectFromJson() needs no version branch.
    object[QStringLiteral("formatVersion")] = kFormatVersion;
    object[QStringLiteral("name")] = project.name;
    object[QStringLiteral("machineFiles")] = QJsonArray::fromStringList(project.machineFiles);
    object[QStringLiteral("outputDir")] = project.outputDir;
    object[QStringLiteral("rootNamespace")] = project.rootNamespace;
    return object;
}

Project projectFromJson(const QJsonObject& json) {
    Project project;
    project.name = json.value(QStringLiteral("name")).toString();
    project.machineFiles = toStringList(json.value(QStringLiteral("machineFiles")).toArray());
    project.outputDir = json.value(QStringLiteral("outputDir")).toString();
    project.rootNamespace = json.value(QStringLiteral("rootNamespace")).toString();
    return project;
}

bool saveProject(const Project& project, const QString& path, QString* error) {
    return writeJsonFile(projectToJson(project), path, error);
}

bool loadProject(const QString& path, Project* project, QString* error) {
    QJsonObject object;
    if (!readJsonFile(path, &object, error)) {
        return false;
    }
    *project = projectFromJson(object);
    return true;
}

std::optional<LoadedProjectData> readProjectFiles(
    const QString& sdpPath,
    std::function<void(int percentage, const QString& message)> progress,
    QString* error
) {
    if (progress) {
        progress(5, QStringLiteral("Reading project manifest..."));
    }

    Project project;
    if (!loadProject(sdpPath, &project, error)) {
        return std::nullopt;
    }

    if (project.machineFiles.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("project has no machines");
        }
        return std::nullopt;
    }

    const QDir baseDir = QFileInfo(sdpPath).dir();
    std::vector<MachineFileEntry> machines;
    machines.reserve(project.machineFiles.size());

    const int total = project.machineFiles.size();
    for (int i = 0; i < total; ++i) {
        const QString& relFile = project.machineFiles[i];
        if (progress) {
            const int pct = 10 + (i * 85) / total;
            progress(pct, QStringLiteral("Loading %1...").arg(relFile));
        }

        Machine machine;
        QString machineError;
        const QString fullPath = baseDir.filePath(relFile);
        if (!loadMachine(fullPath, &machine, &machineError)) {
            if (error != nullptr) {
                *error = relFile + QStringLiteral(": ") + machineError;
            }
            return std::nullopt;
        }

        const QString name = !machine.name.isEmpty() ? machine.name : QFileInfo(relFile).completeBaseName();
        machines.push_back(MachineFileEntry{
            .relativePath = relFile,
            .name = name,
            .machine = std::move(machine)
        });
    }

    if (progress) {
        progress(100, QStringLiteral("Project loaded successfully"));
    }

    return LoadedProjectData{
        .project = std::move(project),
        .machines = std::move(machines)
    };
}

bool writeProjectFiles(
    const QString& sdpPath,
    const Project& project,
    const std::vector<MachineFileEntry>& machines,
    std::function<void(int percentage, const QString& message)> progress,
    QString* error
) {
    const QDir baseDir = QFileInfo(sdpPath).dir();
    if (!baseDir.exists()) {
        baseDir.mkpath(QStringLiteral("."));
    }

    const int total = static_cast<int>(machines.size());
    for (int i = 0; i < total; ++i) {
        const auto& entry = machines[static_cast<std::size_t>(i)];
        if (progress) {
            const int pct = 10 + (i * 80) / std::max(1, total);
            progress(pct, QStringLiteral("Saving %1...").arg(entry.relativePath));
        }

        const QString fullPath = baseDir.filePath(entry.relativePath);
        QString machineError;
        if (!saveMachine(entry.machine, fullPath, &machineError)) {
            if (error != nullptr) {
                *error = entry.relativePath + QStringLiteral(": ") + machineError;
            }
            return false;
        }
    }

    if (progress) {
        progress(95, QStringLiteral("Writing project manifest..."));
    }

    if (!saveProject(project, sdpPath, error)) {
        return false;
    }

    if (progress) {
        progress(100, QStringLiteral("Project saved successfully"));
    }

    return true;
}

}  // namespace app
