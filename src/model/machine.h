#pragma once

#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>
#include <optional>

namespace app {

// The machine document's plain-data shape: topology, geometry, and the
// name-only action/guard vocabulary.

// The machine's starting point is Machine::initialStateId, not a per-state kind.
// State::parentId/initialChildId carry the hierarchy (0 = child of the machine root).
enum class StateKind { Normal, Parallel, Final, History };

// Presentation-only palette hint on states, transitions and notes; persisted by
// name, ignored by the generator and the XState export.
enum class ElementColor { Default, Red, Orange, Yellow, Green, Blue, Purple, Pink, Gray };

// `initialValue` stays textual; the evaluator and emitter parse it per type.
// For Object it holds compact or formatted JSON.
enum class ContextType { Bool, Int, Double, String, Object };

enum class FieldType { Bool, Int, Double, String, Custom };

struct StructField {
    QString name;
    FieldType type = FieldType::Int;
    QString customTypeName;  // Non-empty when type == FieldType::Custom
    bool isArray = false;
    int arraySize = 0;       // > 0 for fixed-size arrays (e.g. data[8])
    QString initialValue;
    bool operator==(const StructField&) const = default;
};

struct StructDefinition {
    quint64 id = 0;
    QString name;            // C++ struct identifier (e.g. "CanMessage", "Point")
    QVector<StructField> fields;
    bool external = false;   // true if bound to an external C++ header
    QString headerPath;      // e.g. "can_types.h" or "geometry.hpp"
    bool operator==(const StructDefinition&) const = default;
};

struct ContextVariable {
    quint64 id = 0;
    QString name;                            // an identifier: [A-Za-z_][A-Za-z0-9_]*
    ContextType type = ContextType::Int;
    QString initialValue;                    // textual; parsed per `type` by evaluator and emitter (JSON string for Object)
    QString customTypeName;                  // non-empty when bound to a custom struct type
    bool operator==(const ContextVariable&) const = default;
};

struct Invocation {
    QString src;
    QString id;
    ContextType outputType = ContextType::Int;
    bool operator==(const Invocation&) const = default;
};

inline QString effectiveInvocationId(const Invocation& inv) {
    return inv.id.isEmpty() ? inv.src : inv.id;
}

struct State {
    quint64 id = 0;
    QString name;
    StateKind kind = StateKind::Normal;
    QPointF pos;
    QStringList entryActions;
    QStringList exitActions;
    // Documentation-only; no runtime semantics.
    QString description;
    QStringList tags;
    // 0 = direct child of the machine root (same 0-sentinel as Transition::from/to).
    // Only MachineDocAgent::reparentState changes it.
    quint64 parentId = 0;
    // First child a compound/parallel state enters; 0 = unset. MachineDocAgent keeps it on
    // the first remaining child in document order as children are added/reparented/removed.
    quint64 initialChildId = 0;

    // Only for kind == History. false = shallow (restore the parent's last active direct
    // child), true = deep (restore every recorded level). The simulator does the redirect.
    bool historyDeep = false;

    // Empty invokeSrc = invokes nothing. Empty invokeId = the id transitions reference is
    // derived from invokeSrc; the SetInvokeSrc/SetInvokeId commands keep Transition::event
    // references to that effective id in sync.
    QString invokeSrc;
    QString invokeId;
    ContextType invokeOutputType = ContextType::Int;

    // When non-empty, effectiveInvocations() returns this vector; invokeSrc/invokeId/
    // invokeOutputType stay synchronized with invocations.first().
    QVector<Invocation> invocations;

    [[nodiscard]] QVector<Invocation> effectiveInvocations() const {
        if (!invocations.isEmpty()) {
            return invocations;
        }
        if (!invokeSrc.isEmpty()) {
            return {Invocation{.src = invokeSrc, .id = invokeId, .outputType = invokeOutputType}};
        }
        return {};
    }

    [[nodiscard]] bool hasInvoke() const {
        return !invocations.isEmpty() || !invokeSrc.isEmpty();
    }

    ElementColor color = ElementColor::Default;

    bool operator==(const State&) const = default;
};

struct Transition {
    quint64 id = 0;
    // 0 = machine ROOT: the transition fires from any active state as a fallback after the
    // state's own handlers. The machine-side end is never rewireable.
    quint64 from = 0;
    // 0 = targetless: runs the action without leaving the state. from == to is a self-transition.
    quint64 to = 0;
    // onDone/onError are ordinary values here: "done.invoke.<id>" / "error.platform.<id>",
    // where <id> is the invoking state's effective invoke id.
    QString event;
    QString guard;
    // `<identifier> = <expression>` is an assign to a context variable (classified by
    // expr::parseAssignForm); anything else is a named action hook.
    QString action;
    int delayMs = 0;
    // true = repeat every delayMs (`every <interval>`) instead of firing once (`after`).
    bool periodic = false;
    // User-dragged offset of the event pill from its routed anchor; (0,0) = on the anchor.
    QPointF labelOffset;
    // Offset-free base position of the label as a fraction [0,1] of the wire length, set by
    // auto-layout. nullopt = the router's default placement. labelOffset applies on top.
    std::optional<qreal> labelRatio;
    ElementColor color = ElementColor::Default;
    // Only for from == 0: a machine restart. The target resolves at fire time to the current
    // initial state (the generator resolves it at generation time). Drawn as the root pill.
    bool machineSelf = false;

    // true forces a full exit/entry cycle up to the LCCA (SCXML type="external"); default is internal.
    bool reenter = false;

    // Eventless transition (`always`); requires a blank event and delayMs == 0.
    bool always = false;

    // Multi-target transition. When non-empty, holds every destination id and `to` is kept
    // equal to targets.first().
    QList<quint64> targets = {};

    [[nodiscard]] QList<quint64> effectiveTargets() const {
        if (!targets.isEmpty()) return targets;
        if (to != 0) return {to};
        return {};
    }

    [[nodiscard]] bool isMultiTarget() const {
        return targets.size() > 1;
    }

    bool isAlways() const { return always; }

    // Type of the event's payload (e.g. "CanMessage"); empty = untyped or payloadless.
    QString payloadType;

    // User-dragged orthogonal bendpoints of the wire; empty = the router decides.
    QVector<QPointF> manualBendpoints = {};

    bool operator==(const Transition&) const = default;
};

// Layout decisions shared by the auto-layout plan and ApplyLayoutPlanRequested;
// defined here so the layout need not include machine_events.h.

// Where one state goes: its new State::pos (top-left, scene coordinates).
struct StatePlacement {
    quint64 id = 0;
    QPointF pos;
};

// One Normal transition's new Transition::labelRatio; nullopt when the
// layout could route no wire for it (an endpoint without a box), so the
// router's own default applies.
struct LabelPlacement {
    quint64 id = 0;
    std::optional<qreal> ratio;
};

// Canvas annotation: native UI only, no runtime semantics, never exported to XState.
struct Note {
    quint64 id = 0;
    QPointF pos;
    QString text;
    ElementColor color = ElementColor::Default;

    bool operator==(const Note&) const = default;
};

// One machine document. `nextId` is one counter shared by states, transitions, notes and
// context variables: ids are unique per machine, not per kind, and per document only.
struct Machine {
    QString name;
    QVector<State> states;
    QVector<Transition> transitions;
    QVector<Note> notes;
    QVector<ContextVariable> context;
    QVector<StructDefinition> types;
    QStringList externalHeaders;
    quint64 nextId = 1;
    // The state a fresh Run activates; 0 = none set (a validator error). Deleting the
    // initial state resets it to 0.
    quint64 initialStateId = 0;

    bool operator==(const Machine&) const = default;
};

// True if any state has a parent or any transition is multi-target. Selects which core the
// generator emits; the flat core's output must stay byte-identical.
inline bool isHierarchical(const Machine& machine) {
    for (const State& state : machine.states) {
        if (state.parentId != 0) {
            return true;
        }
    }
    for (const Transition& transition : machine.transitions) {
        if (transition.isMultiTarget()) {
            return true;
        }
    }
    return false;
}

// The invoke id a state's onDone/onError transitions reference: the first invocation's id,
// else invokeId, else invokeSrc.
inline QString effectiveInvokeId(const State& state) {
    const auto invs = state.effectiveInvocations();
    if (!invs.isEmpty()) {
        return effectiveInvocationId(invs.first());
    }
    return state.invokeId.isEmpty() ? state.invokeSrc : state.invokeId;
}

// Which invoke-completion payload identifier (if any) a transition binds into scope,
// derived from its `from` state's invocations. A plain enum because model/ must not depend
// on expr::; each caller maps it to its own layer's type.
enum class InvokePayloadKind { None, Output, Error };

inline InvokePayloadKind invokePayloadKindForTransition(const Machine& machine, const Transition& transition) {
    if (transition.from == 0) {
        return InvokePayloadKind::None;  // a root transition -- only a STATE ever invokes
    }
    for (const State& state : machine.states) {
        if (state.id != transition.from) {
            continue;
        }
        const auto invs = state.effectiveInvocations();
        for (const Invocation& inv : invs) {
            const QString effId = effectiveInvocationId(inv);
            if (effId.isEmpty()) {
                continue;
            }
            if (transition.event == QStringLiteral("done.invoke.") + effId) {
                return InvokePayloadKind::Output;
            }
            if (transition.event == QStringLiteral("error.platform.") + effId) {
                return InvokePayloadKind::Error;
            }
        }
        return InvokePayloadKind::None;
    }
    return InvokePayloadKind::None;  // dangling `from` -- machine_validator's check 7 flags it separately
}

// Maps a payload/type name to one of the four built-in scalars (trimmed, case-insensitive
// Bool/Int/Double/String); nullopt means the caller's struct-name lookup decides.
inline std::optional<ContextType> builtInPayloadScalar(const QString& payloadType) {
    const QString trimmed = payloadType.trimmed();
    if (trimmed.compare(QLatin1String("Bool"), Qt::CaseInsensitive) == 0) {
        return ContextType::Bool;
    }
    if (trimmed.compare(QLatin1String("Int"), Qt::CaseInsensitive) == 0) {
        return ContextType::Int;
    }
    if (trimmed.compare(QLatin1String("Double"), Qt::CaseInsensitive) == 0) {
        return ContextType::Double;
    }
    if (trimmed.compare(QLatin1String("String"), Qt::CaseInsensitive) == 0) {
        return ContextType::String;
    }
    return std::nullopt;
}

// Event descriptor matching: exact, "*" (any non-empty event), or "prefix.*" (matches
// "prefix" and any "prefix.<sub>").
inline bool eventMatches(const QString& descriptor, const QString& eventName) {
    if (descriptor.isEmpty() || eventName.isEmpty()) {
        return false;
    }
    if (descriptor == eventName) {
        return true;
    }
    if (descriptor == QStringLiteral("*")) {
        return true;
    }
    if (descriptor.endsWith(QStringLiteral(".*"))) {
        const QStringView prefix = QStringView(descriptor).left(descriptor.size() - 2);
        if (prefix.isEmpty()) {
            return true;  // ".*" degenerates to "*"
        }
        if (eventName == prefix) {
            return true;
        }
        if (eventName.startsWith(prefix) && eventName.size() > prefix.size() && eventName.at(prefix.size()) == QLatin1Char('.')) {
            return true;
        }
    }
    return false;
}

inline bool isWildcardEventDescriptor(const QString& descriptor) {
    return descriptor == QStringLiteral("*") || descriptor.endsWith(QStringLiteral(".*"));
}

inline int eventDescriptorSpecificity(const QString& descriptor) {
    if (descriptor.isEmpty()) {
        return 0;
    }
    if (descriptor == QStringLiteral("*")) {
        return 1;
    }
    if (descriptor.endsWith(QStringLiteral(".*"))) {
        const QStringView prefix = QStringView(descriptor).left(descriptor.size() - 2);
        if (prefix.isEmpty()) {
            return 1;
        }
        return 10 + static_cast<int>(prefix.count(QLatin1Char('.'))) + 1;
    }
    return 1000;  // Exact match has highest specificity
}

}  // namespace app
