#include <algorithm>

#include "infra/machine_validator.h"

#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>

#include "infra/code_generator.h"
#include "infra/expression.h"
#include "infra/project_io.h"

namespace app {

namespace {

const QRegularExpression& identifierPattern() {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
    return pattern;
}

const QSet<QString>& reservedWords() {
    static const QSet<QString> words = {
        QStringLiteral("auto"),     QStringLiteral("bool"),      QStringLiteral("break"),
        QStringLiteral("case"),     QStringLiteral("catch"),     QStringLiteral("char"),
        QStringLiteral("class"),    QStringLiteral("const"),     QStringLiteral("constexpr"),
        QStringLiteral("continue"), QStringLiteral("default"),   QStringLiteral("delete"),
        QStringLiteral("do"),       QStringLiteral("double"),    QStringLiteral("else"),
        QStringLiteral("enum"),     QStringLiteral("explicit"),  QStringLiteral("extern"),
        QStringLiteral("false"),    QStringLiteral("float"),     QStringLiteral("for"),
        QStringLiteral("friend"),   QStringLiteral("goto"),      QStringLiteral("if"),
        QStringLiteral("inline"),   QStringLiteral("int"),       QStringLiteral("long"),
        QStringLiteral("mutable"),  QStringLiteral("namespace"), QStringLiteral("new"),
        QStringLiteral("noexcept"), QStringLiteral("nullptr"),   QStringLiteral("operator"),
        QStringLiteral("private"),  QStringLiteral("protected"), QStringLiteral("public"),
        QStringLiteral("return"),   QStringLiteral("short"),     QStringLiteral("signed"),
        QStringLiteral("sizeof"),   QStringLiteral("static"),    QStringLiteral("struct"),
        QStringLiteral("switch"),   QStringLiteral("template"),  QStringLiteral("this"),
        QStringLiteral("throw"),    QStringLiteral("true"),      QStringLiteral("try"),
        QStringLiteral("typedef"),  QStringLiteral("typename"),  QStringLiteral("union"),
        QStringLiteral("unsigned"), QStringLiteral("using"),     QStringLiteral("virtual"),
        QStringLiteral("void"),     QStringLiteral("volatile"),  QStringLiteral("while"),
    };
    return words;
}

struct NamedRef {
    QString raw;         // already-trimmed, non-blank
    quint64 stateId = 0;
    quint64 transitionId = 0;
};

// Shared by the four identifier categories (state/event/guard/action names).
// Uses the generator's own sanitizeIdentifier() so the check cannot drift from it.
void checkIdentifierCategory(const QVector<NamedRef>& refs, bool capitalizeFirst, const QString& categoryLabel,
                              QVector<Problem>& problems) {
    QHash<QString, QString> rawBySanitized;  // sanitized identifier -> first raw text seen
    for (const NamedRef& ref : refs) {
        const QString sanitized = sanitizeIdentifier(ref.raw, capitalizeFirst);
        if (sanitized.isEmpty()) {
            problems.push_back(Problem{
                .severity = ProblemSeverity::Error,
                .text = QStringLiteral("%1 name '%2' has no valid C++ identifier characters after sanitization")
                            .arg(categoryLabel, ref.raw),
                .stateId = ref.stateId,
                .transitionId = ref.transitionId,
            });
            continue;
        }
        const auto it = rawBySanitized.constFind(sanitized);
        if (it != rawBySanitized.constEnd() && it.value() != ref.raw) {
            problems.push_back(Problem{
                .severity = ProblemSeverity::Error,
                .text = QStringLiteral("%1 names '%2' and '%3' both sanitize to the same identifier '%4' -- rename one")
                            .arg(categoryLabel, it.value(), ref.raw, sanitized),
                .stateId = ref.stateId,
                .transitionId = ref.transitionId,
            });
        } else {
            rawBySanitized.insert(sanitized, ref.raw);
        }
    }
}

// Check 14: Validates event descriptor syntax (including SCXML / XState v5 wildcards).
// Returns true if the descriptor is a wildcard (valid or invalid), false if regular identifier.
bool validateEventDescriptor(const NamedRef& ref, QVector<Problem>& problems) {
    const QString& text = ref.raw;
    if (!text.contains(QLatin1Char('*'))) {
        return false;  // Regular identifier
    }

    // Universal wildcard "*"
    if (text == QStringLiteral("*")) {
        return true;  // Valid
    }

    // Must end with ".*" and not contain any other stars
    if (!text.endsWith(QStringLiteral(".*")) || text.count(QLatin1Char('*')) > 1) {
        problems.push_back(Problem{
            .severity = ProblemSeverity::Error,
            .text = QStringLiteral("event descriptor '%1' has invalid wildcard syntax -- wildcards must be '*' or end with '.*'")
                        .arg(text),
            .stateId = ref.stateId,
            .transitionId = ref.transitionId,
        });
        return true;
    }

    // Prefix before ".*" must be non-empty and comprise valid dot-separated segments
    const QString prefix = text.left(text.size() - 2);
    if (prefix.isEmpty() || prefix.startsWith(QLatin1Char('.')) || prefix.endsWith(QLatin1Char('.')) || prefix.contains(QStringLiteral(".."))) {
        problems.push_back(Problem{
            .severity = ProblemSeverity::Error,
            .text = QStringLiteral("event descriptor '%1' has an invalid prefix before '.*'")
                        .arg(text),
            .stateId = ref.stateId,
            .transitionId = ref.transitionId,
        });
        return true;
    }

    const QStringList segments = prefix.split(QLatin1Char('.'));
    for (const QString& seg : segments) {
        if (seg.isEmpty()) {
            problems.push_back(Problem{
                .severity = ProblemSeverity::Error,
                .text = QStringLiteral("event descriptor '%1' contains an empty segment")
                            .arg(text),
                .stateId = ref.stateId,
                .transitionId = ref.transitionId,
            });
            return true;
        }
        for (const QChar c : seg) {
            if (!c.isLetterOrNumber() && c != QLatin1Char('_')) {
                problems.push_back(Problem{
                    .severity = ProblemSeverity::Error,
                    .text = QStringLiteral("event descriptor '%1' contains invalid character '%2' in segment '%3'")
                                .arg(text, c, seg),
                    .stateId = ref.stateId,
                    .transitionId = ref.transitionId,
                });
                return true;
            }
        }
    }

    return true;  // Valid wildcard
}

}  // namespace

// An onDone/onError transition is recognised structurally by model/machine.h's
// invokePayloadKindForTransition(); this maps that kind onto the PayloadBinding
// expr::typeCheck() takes, so guard and action checks agree for a given transition.
// `output`/`error` resolve only on those rows and are unknown identifiers elsewhere.
expr::PayloadBinding payloadBindingForTransition(const Machine& machine, const Transition& transition) {
    switch (invokePayloadKindForTransition(machine, transition)) {
        case InvokePayloadKind::Output: {
            for (const State& state : machine.states) {
                if (state.id == transition.from) {
                    for (const Invocation& inv : state.effectiveInvocations()) {
                        const QString effId = effectiveInvocationId(inv);
                        if (transition.event == QStringLiteral("done.invoke.") + effId) {
                            return expr::PayloadBinding::forOnDone(inv.outputType);
                        }
                    }
                    return expr::PayloadBinding::forOnDone(state.invokeOutputType);
                }
            }
            break;
        }
        case InvokePayloadKind::Error:
            return expr::PayloadBinding::error();
        case InvokePayloadKind::None:
            if (!transition.payloadType.isEmpty()) {
                return expr::PayloadBinding::forEvent(transition.payloadType, machine.types);
            }
            break;
    }
    return expr::PayloadBinding::none();
}

namespace {

// True when `guard` is an actual named hook that would be emitted as a method in
// the generated <Machine>_hooks.h (matching code_generator's guardIsHook()).
// When machine.context is non-empty, inline expressions (!isBareIdentifier)
// are evaluated and compiled inline, never declared as hook methods.
bool isGuardHook(const Machine& machine, const Transition& transition) {
    const QString trimmed = transition.guard.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }
    if (expr::isBareIdentifier(trimmed)) {
        return true;
    }
    const bool payloadInScope = invokePayloadKindForTransition(machine, transition) != InvokePayloadKind::None;
    return payloadInScope ? false : machine.context.isEmpty();
}

// True when `action` is an actual named hook that would be emitted as a method in
// the generated <Machine>_hooks.h (matching code_generator's actionIsHook()).
// Assign forms, raise forms, sendTo, and sendParent actions are compiled inline or
// have specialized signatures, so they are not plain sanitized action hook names.
bool isActionHook(const QString& action) {
    const expr::SendToForm sendTo = expr::parseSendToForm(action);
    if (sendTo.ok || !sendTo.message.isEmpty()) {
        return false;
    }
    const expr::SendParentForm sendParent = expr::parseSendParentForm(action);
    if (sendParent.ok || !sendParent.message.isEmpty()) {
        return false;
    }
    const expr::RaiseForm raise = expr::parseRaiseForm(action);
    if (raise.ok || !raise.message.isEmpty()) {
        return false;
    }
    const expr::AssignForm assign = expr::parseAssignForm(action);
    if (assign.ok || !assign.message.isEmpty()) {
        return false;
    }
    return true;
}

void checkRaiseAction(const QString& action, const Machine& machine, quint64 stateId,
                      quint64 transitionId, QVector<Problem>& problems) {
    const expr::RaiseForm form = expr::parseRaiseForm(action);
    if (!form.ok) {
        if (!form.message.isEmpty()) {
            problems.push_back(Problem{
                .severity = ProblemSeverity::Error,
                .text = QStringLiteral("action '%1' looks like a raise but is not valid: %2").arg(action, form.message),
                .stateId = stateId,
                .transitionId = transitionId,
            });
        }
        return;
    }

    bool handled = false;
    for (const Transition& t : machine.transitions) {
        if (t.event == form.event) {
            handled = true;
            break;
        }
    }
    if (!handled) {
        problems.push_back(Problem{
            .severity = ProblemSeverity::Warning,
            .text = QStringLiteral("action '%1' raises event '%2', which is never handled by any transition in this machine")
                        .arg(action, form.event),
            .stateId = stateId,
            .transitionId = transitionId,
        });
    }
}

void checkSendToAction(const QString& action, const Machine& machine, quint64 stateId,
                       quint64 transitionId, QVector<Problem>& problems) {
    const expr::SendToForm form = expr::parseSendToForm(action);
    if (!form.ok) {
        if (!form.message.isEmpty()) {
            problems.push_back(Problem{
                .severity = ProblemSeverity::Error,
                .text = QStringLiteral("action '%1' looks like a sendTo but is not valid: %2").arg(action, form.message),
                .stateId = stateId,
                .transitionId = transitionId,
            });
        }
        return;
    }

    bool targetFound = false;
    for (const State& s : machine.states) {
        for (const Invocation& inv : s.effectiveInvocations()) {
            if (effectiveInvocationId(inv) == form.target) {
                targetFound = true;
                break;
            }
        }
        if (targetFound) {
            break;
        }
    }
    if (!targetFound) {
        problems.push_back(Problem{
            .severity = ProblemSeverity::Warning,
            .text = QStringLiteral("action '%1' targets actor '%2', which is not declared as an invokeId on any state in this machine")
                        .arg(action, form.target),
            .stateId = stateId,
            .transitionId = transitionId,
        });
    }
}

void checkSendParentAction(const QString& action, quint64 stateId,
                           quint64 transitionId, QVector<Problem>& problems) {
    const expr::SendParentForm form = expr::parseSendParentForm(action);
    if (!form.ok) {
        if (!form.message.isEmpty()) {
            problems.push_back(Problem{
                .severity = ProblemSeverity::Error,
                .text = QStringLiteral("action '%1' looks like a sendParent but is not valid: %2").arg(action, form.message),
                .stateId = stateId,
                .transitionId = transitionId,
            });
        }
        return;
    }
}

// Checks one action string (a transition's action or a state's entry/exit
// action) that may be an assign form. A blank-message non-assign is a plain named
// hook and yields nothing. `payload` is non-none() only for a transition action on
// an onDone/onError row; entry/exit actions are never completions.
void checkAssignAction(const QString& action, const QVector<ContextVariable>& schema, quint64 stateId,
                        quint64 transitionId, QVector<Problem>& problems,
                        const expr::PayloadBinding& payload = expr::PayloadBinding::none(),
                        const QVector<StructDefinition>& structDefs = {}) {
    if (expr::parseSendToForm(action).ok || expr::parseSendParentForm(action).ok || expr::parseRaiseForm(action).ok) {
        return;
    }
    const expr::AssignForm form = expr::parseAssignForm(action);
    if (!form.ok) {
        if (form.message.isEmpty()) {
            return;  // an ordinary named action hook
        }
        problems.push_back(Problem{
            .severity = ProblemSeverity::Error,
            .text = QStringLiteral("action '%1' looks like an assign but is not valid: %2").arg(action, form.message),
            .stateId = stateId,
            .transitionId = transitionId,
        });
        return;
    }

    if (form.rootTarget() == QLatin1String("event")) {
        problems.push_back(Problem{
            .severity = ProblemSeverity::Error,
            .text = QStringLiteral("event payload '%1' is read-only and cannot be assigned to").arg(form.target),
            .stateId = stateId,
            .transitionId = transitionId,
        });
        return;
    }

    const QString root = form.rootTarget();
    const ContextVariable* targetVariable = nullptr;
    for (const ContextVariable& variable : schema) {
        if (variable.name == root) {
            targetVariable = &variable;
            break;
        }
    }
    if (targetVariable == nullptr) {
        problems.push_back(Problem{
            .severity = ProblemSeverity::Error,
            .text = QStringLiteral("assign '%1' targets '%2', which names no context variable")
                        .arg(action, form.target),
            .stateId = stateId,
            .transitionId = transitionId,
        });
        return;
    }
    if (form.isMemberAssign() && targetVariable->type != ContextType::Object) {
        problems.push_back(Problem{
            .severity = ProblemSeverity::Error,
            .text = QStringLiteral("assign '%1' targets member of '%2', which is not an Object")
                        .arg(action, root),
            .stateId = stateId,
            .transitionId = transitionId,
        });
        return;
    }

    const expr::ParseResult rhsParsed = expr::parse(form.valueSource);
    if (!rhsParsed.ok) {
        problems.push_back(Problem{
            .severity = ProblemSeverity::Error,
            .text = QStringLiteral("assign '%1' does not parse: %2").arg(action, rhsParsed.message),
            .stateId = stateId,
            .transitionId = transitionId,
        });
        return;
    }

    // Type-check `(<rhs>) == <target>`: typeCheck() requires a Bool top level,
    // and its comparison rule (Int/Double interconvert, other pairings an Error)
    // doubles as the assign's type-match rule. Always parses: the RHS parsed
    // above and the target is a bare identifier.
    const expr::ParseResult wrapped = expr::parse(QStringLiteral("(%1) == %2").arg(form.valueSource, form.target));
    if (!wrapped.ok) {
        // Unreachable; reported rather than asserted (this unit never throws).
        problems.push_back(Problem{
            .severity = ProblemSeverity::Error,
            .text = QStringLiteral("assign '%1' could not be type-checked: %2").arg(action, wrapped.message),
            .stateId = stateId,
            .transitionId = transitionId,
        });
        return;
    }
    for (const expr::TypeProblem& problem : expr::typeCheck(wrapped.ast, schema, payload, structDefs)) {
        problems.push_back(Problem{
            .severity = ProblemSeverity::Error,
            .text = QStringLiteral("assign '%1' is ill-typed: %2").arg(action, problem.message),
            .stateId = stateId,
            .transitionId = transitionId,
        });
    }
}

// Reachability closure for check 3: reaching `id` also reaches what its default entry
// descends into (as SimulationAgent::descendInto does): every direct child of a Parallel,
// the initialChildId of a compound. A History target redirects to its parent's plain default
// descent (a fresh first visit is always a valid run). `visited` makes an initialChildId
// cycle harmless. A non-null `queue` also schedules each admitted state's outgoing transitions.
void closeDefaultDescent(const Machine& machine, quint64 id, QSet<quint64>& visited, QVector<quint64>* queue) {
    if (id == 0 || visited.contains(id)) {
        return;
    }
    visited.insert(id);
    if (queue != nullptr) {
        queue->push_back(id);
    }
    const auto it = std::find_if(machine.states.constBegin(), machine.states.constEnd(),
                                 [id](const State& candidate) { return candidate.id == id; });
    if (it == machine.states.constEnd()) {
        return;
    }
    if (it->kind == StateKind::Parallel) {
        for (const State& child : machine.states) {
            if (child.parentId == id) {
                closeDefaultDescent(machine, child.id, visited, queue);
            }
        }
    } else if (it->kind == StateKind::History) {
        closeDefaultDescent(machine, it->parentId, visited, queue);
    } else if (it->initialChildId != 0) {
        closeDefaultDescent(machine, it->initialChildId, visited, queue);
    }
}

}  // namespace

QVector<Problem> validate(const Machine& machine) {
    QVector<Problem> problems;

    // ---- 1. machine name -----------------------------------------------------
    if (machine.name.trimmed().isEmpty()) {
        problems.push_back(Problem{.severity = ProblemSeverity::Error, .text = QStringLiteral("machine name is empty")});
    }

    // ---- 2. initial state + 3. reachability BFS -------------------------------
    quint64 initialId = machine.initialStateId;
    if (initialId != 0) {
        bool initialExists = false;
        for (const State& state : machine.states) {
            if (state.id == initialId) {
                initialExists = true;
                break;
            }
        }
        if (!initialExists) {
            problems.push_back(Problem{
                .severity = ProblemSeverity::Error,
                .text = QStringLiteral("initial state id %1 names a state that does not exist").arg(initialId)});
            initialId = 0;  // nothing to BFS from either
        }
    }
    if (initialId == 0) {
        problems.push_back(
            Problem{.severity = ProblemSeverity::Error, .text = QStringLiteral("machine has no initial state")});
    } else {
        QSet<quint64> visited;
        QVector<quint64> queue;
        closeDefaultDescent(machine, initialId, visited, &queue);
        // Root transitions (from == 0) fire from any active state, so seed their
        // targets before the per-state walk.
        for (const Transition& transition : machine.transitions) {
            if (transition.from == 0) {
                for (quint64 tgt : transition.effectiveTargets()) {
                    closeDefaultDescent(machine, tgt, visited, &queue);
                }
            }
        }
        for (int head = 0; head < queue.size(); ++head) {
            const quint64 current = queue.at(head);
            // Entering a state enters its ancestor chain: a compound is reachable
            // whenever a descendant is, and its own outgoing transitions join the
            // walk. No default descent: it was entered through the descendant.
            const auto self = std::find_if(machine.states.constBegin(), machine.states.constEnd(),
                                           [current](const State& candidate) { return candidate.id == current; });
            if (self != machine.states.constEnd() && self->parentId != 0 && !visited.contains(self->parentId)) {
                visited.insert(self->parentId);
                queue.push_back(self->parentId);
            }
            for (const Transition& transition : machine.transitions) {
                if (transition.from == current) {
                    for (quint64 tgt : transition.effectiveTargets()) {
                        closeDefaultDescent(machine, tgt, visited, &queue);
                    }
                }
            }
        }
        // A Parallel ancestor admitted through a descendant does not enter its other
        // regions (a multi-target transition into two regions would wrongly enter a
        // third); regions are entered only when the Parallel itself is a target.
        for (const State& state : machine.states) {
            if (!visited.contains(state.id)) {
                problems.push_back(Problem{
                    .severity = ProblemSeverity::Warning,
                    .text = QStringLiteral("state '%1' is unreachable from the initial state").arg(state.name),
                    .stateId = state.id,
                });
            }
        }
    }

    // ---- 4. blank event + delayMs == 0 ----------------------------------------
    for (const Transition& transition : machine.transitions) {
        const QString event = transition.event.trimmed();
        if (transition.isAlways()) {
            if (!event.isEmpty() || transition.delayMs > 0) {
                problems.push_back(Problem{
                    .severity = ProblemSeverity::Error,
                    .text = QStringLiteral("always transition cannot have an event or delay"),
                    .transitionId = transition.id,
                });
            }
            if (transition.from != 0 && transition.from == transition.to && transition.guard.trimmed().isEmpty()) {
                problems.push_back(Problem{
                    .severity = ProblemSeverity::Error,
                    .text = QStringLiteral("always self-transition has no guard -- would cause an infinite eventless loop"),
                    .transitionId = transition.id,
                });
            }
        } else if (event.isEmpty() && transition.delayMs <= 0) {
            problems.push_back(Problem{
                .severity = ProblemSeverity::Error,
                .text = QStringLiteral("transition has a blank event and delayMs == 0 -- it can never fire"),
                .transitionId = transition.id,
            });
        }
    }

    // ---- 5. guard-aware candidate-fallback legality --------------------------
    // Transitions sharing a (source, event) pair, or a source for `always`, are a
    // candidate group tried in document order; every row but the last needs a guard,
    // or it shadows the rest.
    {
        QHash<quint64, QHash<QString, QVector<const Transition*>>> groups;
        QHash<quint64, QVector<const Transition*>> alwaysGroups;
        for (const Transition& transition : machine.transitions) {
            if (transition.isAlways()) {
                alwaysGroups[transition.from].push_back(&transition);
            } else {
                const QString event = transition.event.trimmed();
                if (!event.isEmpty()) {
                    groups[transition.from][event].push_back(&transition);
                }
            }
        }
        for (auto sourceIt = groups.constBegin(); sourceIt != groups.constEnd(); ++sourceIt) {
            for (auto eventIt = sourceIt.value().constBegin(); eventIt != sourceIt.value().constEnd(); ++eventIt) {
                const QVector<const Transition*>& candidates = eventIt.value();
                for (int i = 0; i + 1 < candidates.size(); ++i) {
                    if (candidates.at(i)->guard.trimmed().isEmpty()) {
                        problems.push_back(Problem{
                            .severity = ProblemSeverity::Error,
                            .text = QStringLiteral("event '%1': unguarded transition %2 is not the last candidate "
                                                    "for this (state, event) pair -- it shadows transition %3 (and "
                                                    "any candidates after it), which could never fire")
                                        .arg(eventIt.key())
                                        .arg(candidates.at(i)->id)
                                        .arg(candidates.at(i + 1)->id),
                            .transitionId = candidates.at(i)->id,
                        });
                    }
                }
            }
        }
        for (auto alwaysIt = alwaysGroups.constBegin(); alwaysIt != alwaysGroups.constEnd(); ++alwaysIt) {
            const QVector<const Transition*>& candidates = alwaysIt.value();
            for (int i = 0; i + 1 < candidates.size(); ++i) {
                if (candidates.at(i)->guard.trimmed().isEmpty()) {
                    problems.push_back(Problem{
                        .severity = ProblemSeverity::Error,
                        .text = QStringLiteral("always: unguarded transition %1 is not the last candidate "
                                                "for this state -- it shadows transition %2 (and "
                                                "any candidates after it), which could never fire")
                                    .arg(candidates.at(i)->id)
                                    .arg(candidates.at(i + 1)->id),
                        .transitionId = candidates.at(i)->id,
                    });
                }
            }
        }
    }

    // ---- 6. identifier collisions/emptiness after sanitization ---------------
    // Blank event/guard/action text means none and is skipped; inline guard
    // expressions and inline actions declare no hooks and are checked elsewhere.
    QVector<NamedRef> stateRefs;
    for (const State& state : machine.states) {
        const QString trimmedName = state.name.trimmed();
        if (trimmedName.isEmpty()) {
            problems.push_back(
                Problem{.severity = ProblemSeverity::Error, .text = QStringLiteral("state has a blank name"), .stateId = state.id});
        } else {
            stateRefs.push_back(NamedRef{.raw = trimmedName, .stateId = state.id});
        }
    }
    checkIdentifierCategory(stateRefs, /*capitalizeFirst=*/true, QStringLiteral("State"), problems);

    QVector<NamedRef> eventRefs;
    QVector<NamedRef> guardRefs;
    QVector<NamedRef> actionRefs;
    for (const Transition& transition : machine.transitions) {
        const QString event = transition.event.trimmed();
        if (!event.isEmpty()) {
            eventRefs.push_back(NamedRef{.raw = event, .transitionId = transition.id});
        }
        const QString guard = transition.guard.trimmed();
        if (!guard.isEmpty() && isGuardHook(machine, transition)) {
            guardRefs.push_back(NamedRef{.raw = guard, .transitionId = transition.id});
        }
        const QString action = transition.action.trimmed();
        if (!action.isEmpty() && isActionHook(action)) {
            actionRefs.push_back(NamedRef{.raw = action, .transitionId = transition.id});
        }
    }
    for (const State& state : machine.states) {
        for (const QString& entryAction : state.entryActions) {
            const QString trimmed = entryAction.trimmed();
            if (!trimmed.isEmpty() && isActionHook(trimmed)) {
                actionRefs.push_back(NamedRef{.raw = trimmed, .stateId = state.id});
            }
        }
        // Exit actions share the same generated Actions namespace.
        for (const QString& exitAction : state.exitActions) {
            const QString trimmed = exitAction.trimmed();
            if (!trimmed.isEmpty() && isActionHook(trimmed)) {
                actionRefs.push_back(NamedRef{.raw = trimmed, .stateId = state.id});
            }
        }
    }
    QVector<NamedRef> regularEventRefs;
    for (const NamedRef& ref : eventRefs) {
        if (!validateEventDescriptor(ref, problems)) {
            regularEventRefs.push_back(ref);
        }
    }
    checkIdentifierCategory(regularEventRefs, /*capitalizeFirst=*/true, QStringLiteral("Event"), problems);
    checkIdentifierCategory(guardRefs, /*capitalizeFirst=*/false, QStringLiteral("Guard"), problems);
    checkIdentifierCategory(actionRefs, /*capitalizeFirst=*/false, QStringLiteral("Action"), problems);

    // ---- 7. hierarchy tree invariants (structural, over persisted parentId/initialChildId) --
    {
        QHash<quint64, const State*> byId;
        for (const State& state : machine.states) {
            byId.insert(state.id, &state);
        }
        QHash<quint64, int> childCount;
        for (const State& state : machine.states) {
            if (state.parentId != 0) {
                childCount[state.parentId] += 1;
            }
        }
        for (const State& state : machine.states) {
            // a. parentId names a nonexistent state.
            if (state.parentId != 0 && !byId.contains(state.parentId)) {
                problems.push_back(Problem{
                    .severity = ProblemSeverity::Error,
                    .text = QStringLiteral("state '%1' has parentId %2, which names a state that does not exist")
                                .arg(state.name)
                                .arg(state.parentId),
                    .stateId = state.id,
                });
            }

            // b. parentId cycle -- walk the chain up with a visited guard.
            {
                QSet<quint64> visited;
                quint64 current = state.parentId;
                bool cyclic = false;
                while (current != 0) {
                    if (current == state.id || visited.contains(current)) {
                        cyclic = true;
                        break;
                    }
                    visited.insert(current);
                    const auto it = byId.constFind(current);
                    if (it == byId.constEnd()) {
                        break;  // dangling parent -- already reported as (a) above
                    }
                    current = it.value()->parentId;
                }
                if (cyclic) {
                    problems.push_back(Problem{
                        .severity = ProblemSeverity::Error,
                        .text = QStringLiteral("state '%1' has a parentId cycle").arg(state.name),
                        .stateId = state.id,
                    });
                }
            }

            // c. a Final/History parent; only Normal/Parallel may contain children.
            if (state.parentId != 0) {
                const auto it = byId.constFind(state.parentId);
                if (it != byId.constEnd() &&
                    (it.value()->kind == StateKind::Final || it.value()->kind == StateKind::History)) {
                    problems.push_back(Problem{
                        .severity = ProblemSeverity::Error,
                        .text = QStringLiteral(
                                    "state '%1' has parent '%2', which is Final/History and cannot contain children")
                                    .arg(state.name, it.value()->name),
                        .stateId = state.id,
                    });
                }
            }

            // d. a non-zero initialChildId that is not an actual direct child.
            if (state.initialChildId != 0) {
                const auto it = byId.constFind(state.initialChildId);
                if (it == byId.constEnd() || it.value()->parentId != state.id) {
                    problems.push_back(Problem{
                        .severity = ProblemSeverity::Error,
                        .text = QStringLiteral("state '%1' has initialChildId %2, which is not a direct child")
                                    .arg(state.name)
                                    .arg(state.initialChildId),
                        .stateId = state.id,
                    });
                }
            }

            // e. a Normal compound WITH children that leaves initialChildId at 0.
            // Parallel is exempt: both runtimes enter every region of a
            // Parallel state and never read its initialChildId (sim_agent
            // rule 7, the AOT kIsParallel branch), and SCXML <parallel> /
            // XState parallel nodes carry no initial at all.
            if (state.kind != StateKind::Parallel && state.initialChildId == 0 &&
                childCount.value(state.id, 0) > 0) {
                problems.push_back(Problem{
                    .severity = ProblemSeverity::Error,
                    .text = QStringLiteral("state '%1' has children but no initialChildId set").arg(state.name),
                    .stateId = state.id,
                });
            }

            // f. History is a leaf marker on its parent's children, never a
            // container itself -- an Error at the machine root or with
            // children of its own.
            if (state.kind == StateKind::History) {
                if (state.parentId == 0) {
                    problems.push_back(Problem{
                        .severity = ProblemSeverity::Error,
                        .text = QStringLiteral("state '%1' is a History state at the machine root (parentId == 0)")
                                    .arg(state.name),
                        .stateId = state.id,
                    });
                }
                if (childCount.value(state.id, 0) > 0) {
                    problems.push_back(Problem{
                        .severity = ProblemSeverity::Error,
                        .text = QStringLiteral("state '%1' is a History state with children of its own")
                                    .arg(state.name),
                        .stateId = state.id,
                    });
                }
            }
        }
    }

    // ---- 8. History-kind state with its own outgoing transition(s): Warning ----
    // A History state is never active, so these transitions never fire.
    {
        QHash<quint64, QString> historyNameById;
        for (const State& state : machine.states) {
            if (state.kind == StateKind::History) {
                historyNameById.insert(state.id, state.name);
            }
        }
        for (const Transition& transition : machine.transitions) {
            const auto it = historyNameById.constFind(transition.from);
            if (it == historyNameById.constEnd()) {
                continue;
            }
            problems.push_back(Problem{
                .severity = ProblemSeverity::Warning,
                .text = QStringLiteral("state '%1' is a History state with its own outgoing transition %2 -- v1 "
                                        "semantics never fire it (SendEvent can never select a History state as an "
                                        "atomic active, and the redirect always falls back to the parent's "
                                        "recorded child or initialChildId, never to a transition here)")
                            .arg(it.value())
                            .arg(transition.id),
                .stateId = transition.from,
                .transitionId = transition.id,
            });
        }
    }

    // ---- 9. context variable names ---------------------------------------------
    // Document-level findings (both ids 0, the name quoted). C++ keywords break the
    // generated `struct Context`; `output`/`error`/`event` are not keywords but are
    // shadowed inside onDone/onError guards and assigns, hence their own message.
    {
        static const QSet<QString> payloadReservedWords = {
            QStringLiteral("output"),
            QStringLiteral("error"),
            QStringLiteral("event"),
        };
        QSet<QString> seenNames;
        for (const ContextVariable& variable : machine.context) {
            if (!identifierPattern().match(variable.name).hasMatch()) {
                problems.push_back(Problem{
                    .severity = ProblemSeverity::Error,
                    .text = QStringLiteral("context variable '%1' is not a valid identifier").arg(variable.name)});
            } else if (reservedWords().contains(variable.name)) {
                // `else if`: an invalid identifier is reported once; every reserved
                // word matches identifierPattern, so nothing double-reports.
                problems.push_back(Problem{
                    .severity = ProblemSeverity::Error,
                    .text = QStringLiteral("context variable '%1' is a C++ reserved word -- the generated core "
                                            "would not compile")
                                .arg(variable.name)});
            } else if (payloadReservedWords.contains(variable.name)) {
                problems.push_back(Problem{
                    .severity = ProblemSeverity::Error,
                    .text = QStringLiteral("context variable '%1' is reserved for the onDone/onError invoke "
                                            "completion payload (infra/expression.h's PayloadBinding) -- a context "
                                            "variable of this name would be shadowed inside every such "
                                            "transition's own guard/assign")
                                .arg(variable.name)});
            }
            if (variable.type == ContextType::Object) {
                if (variable.customTypeName.isEmpty() || !variable.initialValue.trimmed().isEmpty()) {
                    QJsonParseError err;
                    const QJsonDocument doc = QJsonDocument::fromJson(variable.initialValue.trimmed().toUtf8(), &err);
                    if (doc.isNull() || !doc.isObject()) {
                        problems.push_back(Problem{
                            .severity = ProblemSeverity::Error,
                            .text = QStringLiteral("context object '%1' initialValue is not a valid JSON object: %2")
                                        .arg(variable.name, err.errorString())});
                    }
                }
            }
            if (seenNames.contains(variable.name)) {
                problems.push_back(Problem{
                    .severity = ProblemSeverity::Error,
                    .text = QStringLiteral("two context variables share the name '%1'").arg(variable.name)});
            }
            seenNames.insert(variable.name);
        }
    }

    // ---- 10. guard expression type-checking ------------------------------------
    // Blank guards and bare identifiers (named hooks) are skipped; anything else is
    // parsed and type-checked, each finding an Error on the transition.
    for (const Transition& transition : machine.transitions) {
        const QString guard = transition.guard;
        if (guard.trimmed().isEmpty() || expr::isBareIdentifier(guard)) {
            continue;  // no guard / a named hook
        }
        const expr::ParseResult parsed = expr::parse(guard);
        if (!parsed.ok) {
            problems.push_back(Problem{
                .severity = ProblemSeverity::Error,
                .text = QStringLiteral("guard '%1' does not parse: %2").arg(guard, parsed.message),
                .transitionId = transition.id,
            });
            continue;
        }
        for (const expr::TypeProblem& problem :
             expr::typeCheck(parsed.ast, machine.context, payloadBindingForTransition(machine, transition), machine.types)) {
            problems.push_back(Problem{
                .severity = ProblemSeverity::Error,
                .text = QStringLiteral("guard '%1' is ill-typed: %2").arg(guard, problem.message),
                .transitionId = transition.id,
            });
        }
    }

    // ---- 11. action checks: sendTo/sendParent/raise forms and assign type-checking ---
    // Runs over transition actions and every state's entry/exit actions.
    for (const Transition& transition : machine.transitions) {
        if (!transition.action.trimmed().isEmpty()) {
            checkSendToAction(transition.action, machine, /*stateId=*/0, transition.id, problems);
            checkSendParentAction(transition.action, /*stateId=*/0, transition.id, problems);
            checkRaiseAction(transition.action, machine, /*stateId=*/0, transition.id, problems);
            checkAssignAction(transition.action, machine.context, /*stateId=*/0, transition.id, problems,
                               payloadBindingForTransition(machine, transition), machine.types);
        }
    }
    for (const State& state : machine.states) {
        for (const QString& entryAction : state.entryActions) {
            if (!entryAction.trimmed().isEmpty()) {
                checkSendToAction(entryAction, machine, state.id, /*transitionId=*/0, problems);
                checkSendParentAction(entryAction, state.id, /*transitionId=*/0, problems);
                checkRaiseAction(entryAction, machine, state.id, /*transitionId=*/0, problems);
                checkAssignAction(entryAction, machine.context, state.id, /*transitionId=*/0, problems,
                                  expr::PayloadBinding::none(), machine.types);
            }
        }
        for (const QString& exitAction : state.exitActions) {
            if (!exitAction.trimmed().isEmpty()) {
                checkSendToAction(exitAction, machine, state.id, /*transitionId=*/0, problems);
                checkSendParentAction(exitAction, state.id, /*transitionId=*/0, problems);
                checkRaiseAction(exitAction, machine, state.id, /*transitionId=*/0, problems);
                checkAssignAction(exitAction, machine.context, state.id, /*transitionId=*/0, problems,
                                  expr::PayloadBinding::none(), machine.types);
            }
        }
    }

    // ---- 17. multi-target transition validation (W3C SCXML §3.3.1 / XState v5) ----
    // Targets must exist, be distinct, not combine with machineSelf, none an ancestor
    // of another, and any two must meet at a Parallel LCA.
    QHash<quint64, const State*> stateById;
    QHash<quint64, quint64> parentById;
    for (const State& state : machine.states) {
        stateById[state.id] = &state;
        parentById[state.id] = state.parentId;
    }

    auto isAncestorOf = [&](quint64 ancestorId, quint64 descendantId) -> bool {
        quint64 curr = parentById.value(descendantId, 0);
        while (curr != 0) {
            if (curr == ancestorId) return true;
            curr = parentById.value(curr, 0);
        }
        return false;
    };

    auto findLcca = [&](quint64 s1, quint64 s2) -> quint64 {
        QList<quint64> ancestors1;
        quint64 c1 = s1;
        while (c1 != 0) {
            ancestors1.append(c1);
            c1 = parentById.value(c1, 0);
        }
        quint64 c2 = s2;
        while (c2 != 0) {
            if (ancestors1.contains(c2)) return c2;
            c2 = parentById.value(c2, 0);
        }
        return 0;
    };

    for (const Transition& transition : machine.transitions) {
        if (!transition.isMultiTarget()) {
            continue;
        }

        if (transition.machineSelf) {
            problems.push_back(Problem{
                .severity = ProblemSeverity::Error,
                .text = QStringLiteral("multi-target transition cannot target the machine itself"),
                .transitionId = transition.id,
            });
        }

        QSet<quint64> seenTargets;
        bool hasDanglingTarget = false;
        for (quint64 tid : transition.targets) {
            if (!stateById.contains(tid)) {
                problems.push_back(Problem{
                    .severity = ProblemSeverity::Error,
                    .text = QStringLiteral("multi-target transition specifies non-existent target id %1").arg(tid),
                    .transitionId = transition.id,
                });
                hasDanglingTarget = true;
            }
            if (seenTargets.contains(tid)) {
                problems.push_back(Problem{
                    .severity = ProblemSeverity::Error,
                    .text = QStringLiteral("multi-target transition contains duplicate target id %1").arg(tid),
                    .transitionId = transition.id,
                });
            }
            seenTargets.insert(tid);
        }

        if (hasDanglingTarget) {
            continue;
        }

        // Pairwise orthogonality and ancestor checks (SCXML §3.3.1)
        for (int i = 0; i < transition.targets.size(); ++i) {
            const quint64 t1 = transition.targets.at(i);
            for (int j = i + 1; j < transition.targets.size(); ++j) {
                const quint64 t2 = transition.targets.at(j);
                if (isAncestorOf(t1, t2) || isAncestorOf(t2, t1)) {
                    problems.push_back(Problem{
                        .severity = ProblemSeverity::Error,
                        .text = QStringLiteral("multi-target transition targets '%1' and '%2' have ancestor-descendant conflict")
                                    .arg(stateById[t1]->name, stateById[t2]->name),
                        .transitionId = transition.id,
                    });
                } else {
                    const quint64 lcca = findLcca(t1, t2);
                    const State* lccaState = stateById.value(lcca, nullptr);
                    if (!lccaState || lccaState->kind != StateKind::Parallel) {
                        problems.push_back(Problem{
                            .severity = ProblemSeverity::Error,
                            .text = QStringLiteral("multi-target transition targets '%1' and '%2' do not have a parallel ancestor")
                                        .arg(stateById[t1]->name, stateById[t2]->name),
                            .transitionId = transition.id,
                        });
                    }
                }
            }
        }
    }

    // ---- 18. Struct definition and schema validation ----
    // Names must be valid non-reserved identifiers and unique, external structs need
    // a header path, custom field types must name a declared struct (no cycles), and
    // externalHeaders entries must be non-empty.
    QSet<QString> declaredStructNames;
    QHash<QString, QStringList> structDependencies;

    for (const StructDefinition& def : machine.types) {
        if (!identifierPattern().match(def.name).hasMatch()) {
            problems.push_back(Problem{
                .severity = ProblemSeverity::Error,
                .text = QStringLiteral("struct '%1' is not a valid identifier").arg(def.name),
            });
        } else if (reservedWords().contains(def.name)) {
            problems.push_back(Problem{
                .severity = ProblemSeverity::Error,
                .text = QStringLiteral("struct '%1' is a C++ reserved word").arg(def.name),
            });
        }

        if (declaredStructNames.contains(def.name)) {
            problems.push_back(Problem{
                .severity = ProblemSeverity::Error,
                .text = QStringLiteral("two struct definitions share the name '%1'").arg(def.name),
            });
        }
        declaredStructNames.insert(def.name);

        if (def.external) {
            if (def.headerPath.trimmed().isEmpty()) {
                problems.push_back(Problem{
                    .severity = ProblemSeverity::Error,
                    .text = QStringLiteral("external struct '%1' must specify a header path").arg(def.name),
                });
            }
        }

        QSet<QString> seenFieldNames;
        QStringList deps;
        for (const StructField& field : def.fields) {
            if (!identifierPattern().match(field.name).hasMatch()) {
                problems.push_back(Problem{
                    .severity = ProblemSeverity::Error,
                    .text = QStringLiteral("field '%1' in struct '%2' is not a valid identifier")
                                .arg(field.name, def.name),
                });
            } else if (reservedWords().contains(field.name)) {
                problems.push_back(Problem{
                    .severity = ProblemSeverity::Error,
                    .text = QStringLiteral("field '%1' in struct '%2' is a C++ reserved word")
                                .arg(field.name, def.name),
                });
            }

            if (seenFieldNames.contains(field.name)) {
                problems.push_back(Problem{
                    .severity = ProblemSeverity::Error,
                    .text = QStringLiteral("two fields in struct '%1' share the name '%2'")
                                .arg(def.name, field.name),
                });
            }
            seenFieldNames.insert(field.name);

            if (field.type == FieldType::Custom) {
                if (field.customTypeName.trimmed().isEmpty()) {
                    problems.push_back(Problem{
                        .severity = ProblemSeverity::Error,
                        .text = QStringLiteral("custom field '%1' in struct '%2' does not specify a type name")
                                    .arg(field.name, def.name),
                    });
                } else {
                    deps.append(field.customTypeName);
                }
            }

            if (field.isArray && field.arraySize < 0) {
                problems.push_back(Problem{
                    .severity = ProblemSeverity::Error,
                    .text = QStringLiteral("field '%1' in struct '%2' has invalid negative array size %3")
                                .arg(field.name, def.name).arg(field.arraySize),
                });
            }
        }
        structDependencies[def.name] = deps;
    }

    // Validate that custom field type names refer to declared structs in machine.types
    for (const StructDefinition& def : machine.types) {
        for (const StructField& field : def.fields) {
            if (field.type == FieldType::Custom && !field.customTypeName.trimmed().isEmpty()) {
                if (!declaredStructNames.contains(field.customTypeName)) {
                    problems.push_back(Problem{
                        .severity = ProblemSeverity::Error,
                        .text = QStringLiteral("field '%1' in struct '%2' references unknown struct type '%3'")
                                    .arg(field.name, def.name, field.customTypeName),
                    });
                }
            }
        }
    }

    // Circular dependency detection across custom struct fields
    QHash<QString, int> visitState;  // 0=unvisited, 1=visiting, 2=visited
    std::function<bool(const QString&, QStringList&)> checkCycle =
        [&](const QString& current, QStringList& path) -> bool {
            visitState[current] = 1;
            path.append(current);
            for (const QString& dep : structDependencies.value(current)) {
                if (!declaredStructNames.contains(dep)) {
                    continue;
                }
                if (visitState.value(dep, 0) == 1) {
                    path.append(dep);
                    return true;
                }
                if (visitState.value(dep, 0) == 0) {
                    if (checkCycle(dep, path)) {
                        return true;
                    }
                }
            }
            visitState[current] = 2;
            path.removeLast();
            return false;
        };

    for (const QString& sname : declaredStructNames) {
        if (visitState.value(sname, 0) == 0) {
            QStringList cyclePath;
            if (checkCycle(sname, cyclePath)) {
                problems.push_back(Problem{
                    .severity = ProblemSeverity::Error,
                    .text = QStringLiteral("circular dependency detected in struct definitions: %1")
                                .arg(cyclePath.join(QStringLiteral(" -> "))),
                });
                break;
            }
        }
    }

    for (const QString& header : machine.externalHeaders) {
        if (header.trimmed().isEmpty()) {
            problems.push_back(Problem{
                .severity = ProblemSeverity::Error,
                .text = QStringLiteral("externalHeaders contains an empty header path"),
            });
        }
    }

    // ---- 19. Typed event payload and struct context variable validation ----
    // builtInPayloadScalar() (model/machine.h) is the trimmed, case-insensitive
    // four-name authority shared with the code generator and the JIT.
    for (const Transition& transition : machine.transitions) {
        if (!transition.payloadType.isEmpty()) {
            if (!declaredStructNames.contains(transition.payloadType) &&
                !builtInPayloadScalar(transition.payloadType).has_value()) {
                problems.push_back(Problem{
                    .severity = ProblemSeverity::Error,
                    .text = QStringLiteral("transition specifies unknown payloadType '%1'")
                                .arg(transition.payloadType),
                    .transitionId = transition.id,
                });
            }
        }
    }

    for (const ContextVariable& variable : machine.context) {
        if (!variable.customTypeName.isEmpty()) {
            if (!declaredStructNames.contains(variable.customTypeName)) {
                problems.push_back(Problem{
                    .severity = ProblemSeverity::Error,
                    .text = QStringLiteral("context variable '%1' references unknown struct type '%2'")
                                .arg(variable.name, variable.customTypeName),
                });
            }
        }
    }

    // ---- 19b. Same-exact-event payloadType consistency ----
    // The generated <event>Requested(const T&) has one C++ type per exact event, but the
    // JIT and hierarchical emitter bind per row, so all non-invoke rows sharing an exact
    // event must declare the same payloadType (blank = "no payload" is its own value).
    // One Error per offending event.
    {
        struct SeenPayload {
            QString rawTrimmed;  // as authored, trimmed; empty = no payload declared
            QString canonical;   // contextTypeToString() of the built-in scalar, else rawTrimmed
            quint64 transitionId = 0;
        };
        QHash<QString, QVector<SeenPayload>> byEvent;
        QStringList eventOrder;  // first-seen order -- stable, deterministic reporting
        for (const Transition& transition : machine.transitions) {
            if (invokePayloadKindForTransition(machine, transition) != InvokePayloadKind::None) {
                continue;  // onDone/onError rows have their own reserved output/error payload
            }
            const QString event = transition.event.trimmed();
            if (event.isEmpty() || isWildcardEventDescriptor(event)) {
                continue;  // check 19c's territory -- not one exact event
            }
            if (!byEvent.contains(event)) {
                eventOrder.push_back(event);
            }
            const QString rawTrimmed = transition.payloadType.trimmed();
            // Only equality of `canonical` matters; distinctLabels reports rawTrimmed.
            const auto scalar = builtInPayloadScalar(rawTrimmed);
            const QString canonical = scalar.has_value() ? contextTypeToString(*scalar) : rawTrimmed;
            byEvent[event].push_back(SeenPayload{.rawTrimmed = rawTrimmed, .canonical = canonical, .transitionId = transition.id});
        }
        for (const QString& event : eventOrder) {
            const QVector<SeenPayload>& seen = byEvent.value(event);
            const QString firstCanonical = seen.first().canonical;
            QStringList distinctLabels;
            distinctLabels.push_back(seen.first().rawTrimmed.isEmpty() ? QStringLiteral("(none)") : seen.first().rawTrimmed);
            quint64 firstDivergentId = 0;
            for (int i = 1; i < seen.size(); ++i) {
                if (seen.at(i).canonical == firstCanonical) {
                    continue;
                }
                if (firstDivergentId == 0) {
                    firstDivergentId = seen.at(i).transitionId;
                }
                const QString label = seen.at(i).rawTrimmed.isEmpty() ? QStringLiteral("(none)") : seen.at(i).rawTrimmed;
                if (!distinctLabels.contains(label)) {
                    distinctLabels.push_back(label);
                }
            }
            if (firstDivergentId != 0) {
                problems.push_back(Problem{
                    .severity = ProblemSeverity::Error,
                    .text = QStringLiteral("event '%1' declares inconsistent payloadType across its transitions: "
                                            "%2 -- the generated method binds one payload type per exact event")
                                .arg(event, distinctLabels.join(QStringLiteral(", "))),
                    .transitionId = firstDivergentId,
                });
            }
        }
    }

    // ---- 19c. Wildcard/blank-event rows may not declare a payloadType ----
    // A wildcard row is reached by several exact events and a blank-event row by none, so
    // neither runtime has one value to bind `event` to. Invoke rows are excluded.
    for (const Transition& transition : machine.transitions) {
        if (invokePayloadKindForTransition(machine, transition) != InvokePayloadKind::None) {
            continue;
        }
        if (transition.payloadType.trimmed().isEmpty()) {
            continue;
        }
        const QString event = transition.event.trimmed();
        if (event.isEmpty()) {
            problems.push_back(Problem{
                .severity = ProblemSeverity::Error,
                .text = QStringLiteral("transition with a blank event (always/delayed) declares payloadType '%1' "
                                        "-- it matches no single event, so there is nothing to bind `event` to")
                            .arg(transition.payloadType.trimmed()),
                .transitionId = transition.id,
            });
        } else if (isWildcardEventDescriptor(event)) {
            problems.push_back(Problem{
                .severity = ProblemSeverity::Error,
                .text = QStringLiteral("transition on wildcard event descriptor '%1' declares payloadType '%2' -- "
                                        "a wildcard can match several distinct events, so there is no single "
                                        "payload type to bind `event` to")
                            .arg(event, transition.payloadType.trimmed()),
                .transitionId = transition.id,
            });
        }
    }

    // ---- 19e. onDone/onError rows may not declare a payloadType ----
    // Their payload is the reserved `output`/`error`; neither runtime binds `event`
    // there and the XState export withholds the field, so a payloadType is dead data.
    for (const Transition& transition : machine.transitions) {
        if (invokePayloadKindForTransition(machine, transition) == InvokePayloadKind::None ||
            transition.payloadType.trimmed().isEmpty()) {
            continue;
        }
        problems.push_back(Problem{
            .severity = ProblemSeverity::Error,
            .text = QStringLiteral("onDone/onError transition declares payloadType '%1' -- its payload is the "
                                    "reserved `output`/`error`, typed by the invoking state's invokeOutputType, "
                                    "never `event`")
                        .arg(transition.payloadType.trimmed()),
            .transitionId = transition.id,
        });
    }

    // ---- 19d. raise() of a payload event ----
    // Walks the same action sites as check 11 (classified by expr::parseRaiseForm()). Raising
    // an event that declares a payloadType is an Error: `raise` carries no payload argument,
    // so neither runtime has a value to bind to `event`.
    {
        QHash<QString, QString> payloadEvents;  // exact event -> one payloadType spelling (message text only)
        for (const Transition& transition : machine.transitions) {
            if (invokePayloadKindForTransition(machine, transition) != InvokePayloadKind::None) {
                continue;  // onDone/onError rows -- reserved output/error payload, not this vocabulary
            }
            const QString payloadType = transition.payloadType.trimmed();
            if (payloadType.isEmpty()) {
                continue;
            }
            const QString event = transition.event.trimmed();
            if (event.isEmpty() || isWildcardEventDescriptor(event)) {
                continue;  // already flagged by 19c -- raise() cannot name a wildcard/blank event anyway
            }
            if (!payloadEvents.contains(event)) {
                payloadEvents.insert(event, payloadType);
            }
        }

        auto checkRaiseNotPayload = [&](const QString& action, quint64 stateId, quint64 transitionId) {
            const expr::RaiseForm form = expr::parseRaiseForm(action);
            if (!form.ok) {
                return;  // not a raise, or malformed -- check 11's checkRaiseAction already reports that
            }
            const auto it = payloadEvents.constFind(form.event);
            if (it == payloadEvents.constEnd()) {
                return;
            }
            problems.push_back(Problem{
                .severity = ProblemSeverity::Error,
                .text = QStringLiteral("action '%1' raises event '%2', which declares payloadType '%3': raise "
                                        "carries no payload, so neither runtime has a value to bind to `event` "
                                        "(the JIT skips the row's use of it, the generated C++ passes a "
                                        "value-initialised payload)")
                            .arg(action, form.event, it.value()),
                .stateId = stateId,
                .transitionId = transitionId,
            });
        };

        for (const Transition& transition : machine.transitions) {
            if (!transition.action.trimmed().isEmpty()) {
                checkRaiseNotPayload(transition.action, /*stateId=*/0, transition.id);
            }
        }
        for (const State& state : machine.states) {
            for (const QString& entryAction : state.entryActions) {
                if (!entryAction.trimmed().isEmpty()) {
                    checkRaiseNotPayload(entryAction, state.id, /*transitionId=*/0);
                }
            }
            for (const QString& exitAction : state.exitActions) {
                if (!exitAction.trimmed().isEmpty()) {
                    checkRaiseNotPayload(exitAction, state.id, /*transitionId=*/0);
                }
            }
        }
    }

    // ---- 20. Dead-end / deadlock state detection ----
    // Warns on atomic non-Final, non-History states with no outgoing transition, unless an
    // ancestor or a root transition can leave.
    bool hasRootTransitions = false;
    for (const Transition& transition : machine.transitions) {
        if (transition.from == 0) {
            hasRootTransitions = true;
            break;
        }
    }

    if (!hasRootTransitions) {
        QSet<quint64> compoundStateIds;
        for (const State& state : machine.states) {
            if (state.parentId != 0) {
                compoundStateIds.insert(state.parentId);
            }
        }

        QSet<quint64> statesWithOutgoing;
        for (const Transition& transition : machine.transitions) {
            if (transition.from != 0) {
                statesWithOutgoing.insert(transition.from);
            }
        }

        for (const State& state : machine.states) {
            if (state.kind == StateKind::Final || state.kind == StateKind::History) {
                continue;
            }
            if (compoundStateIds.contains(state.id)) {
                continue;  // compound state
            }
            if (statesWithOutgoing.contains(state.id)) {
                continue;  // has outgoing transitions directly
            }
            bool ancestorHasOutgoing = false;
            quint64 ancestorId = state.parentId;
            while (ancestorId != 0) {
                if (statesWithOutgoing.contains(ancestorId)) {
                    ancestorHasOutgoing = true;
                    break;
                }
                const auto it = std::find_if(machine.states.constBegin(), machine.states.constEnd(),
                                             [ancestorId](const State& s) { return s.id == ancestorId; });
                if (it == machine.states.constEnd()) {
                    break;
                }
                ancestorId = it->parentId;
            }
            if (ancestorHasOutgoing) {
                continue;  // handled by ancestor transition
            }

            problems.push_back(Problem{
                .severity = ProblemSeverity::Warning,
                .text = QStringLiteral("state '%1' has no outgoing transitions and is not marked as Final (dead-end state)").arg(state.name),
                .stateId = state.id,
            });
        }
    }

    return problems;
}

}  // namespace app
