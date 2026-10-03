#include "infra/code_generator_internal.h"

namespace app {

// ---- <machine>_types.h ------------------------------------------------------

GeneratedFile buildTypesFile(const Machine& machine, const GenModel& model) {
    QString text = bannerLines(QStringLiteral("%1's user-defined struct types and external header bindings.")
                                   .arg(model.machinePascal));
    text += QStringLiteral("#pragma once\n\n#include <cstdint>\n");

    bool hasString = false;
    bool hasFixedArray = false;
    bool hasDynamicArray = false;
    for (const StructDefinition& def : machine.types) {
        for (const StructField& field : def.fields) {
            if (field.type == FieldType::String) {
                hasString = true;
            }
            if (field.isArray) {
                if (field.arraySize > 0) {
                    hasFixedArray = true;
                } else {
                    hasDynamicArray = true;
                }
            }
        }
    }
    if (hasString) {
        text += QStringLiteral("#include <string>\n");
    }
    if (hasFixedArray) {
        text += QStringLiteral("#include <array>\n");
    }
    if (hasDynamicArray) {
        text += QStringLiteral("#include <vector>\n");
    }

    // External headers (from machine.externalHeaders and external StructDefinition::headerPath)
    QSet<QString> includedHeaders;
    for (const QString& header : machine.externalHeaders) {
        const QString trimmed = header.trimmed();
        if (trimmed.isEmpty() || includedHeaders.contains(trimmed)) {
            continue;
        }
        includedHeaders.insert(trimmed);
        if (trimmed.startsWith(QLatin1Char('<')) || trimmed.startsWith(QLatin1Char('"'))) {
            text += QStringLiteral("#include %1\n").arg(trimmed);
        } else {
            text += QStringLiteral("#include \"%1\"\n").arg(trimmed);
        }
    }
    for (const StructDefinition& def : machine.types) {
        if (def.external) {
            const QString trimmed = def.headerPath.trimmed();
            if (trimmed.isEmpty() || includedHeaders.contains(trimmed)) {
                continue;
            }
            includedHeaders.insert(trimmed);
            if (trimmed.startsWith(QLatin1Char('<')) || trimmed.startsWith(QLatin1Char('"'))) {
                text += QStringLiteral("#include %1\n").arg(trimmed);
            } else {
                text += QStringLiteral("#include \"%1\"\n").arg(trimmed);
            }
        }
    }
    text += QStringLiteral("\n");
    text += openNamespace(model.nsJoined) + QStringLiteral("\n");

    QVector<StructDefinition> internalStructs;
    for (const StructDefinition& def : machine.types) {
        if (!def.external) {
            internalStructs.push_back(def);
        }
    }

    // Topological sort by dependency
    QVector<StructDefinition> sortedStructs;
    QSet<QString> emittedNames;
    int prevCount = -1;
    while (sortedStructs.size() < internalStructs.size() && sortedStructs.size() != prevCount) {
        prevCount = sortedStructs.size();
        for (const StructDefinition& def : internalStructs) {
            if (emittedNames.contains(def.name)) {
                continue;
            }
            bool allDepsEmitted = true;
            for (const StructField& f : def.fields) {
                if (f.type == FieldType::Custom && !f.customTypeName.isEmpty()) {
                    bool isInternal = false;
                    for (const StructDefinition& other : internalStructs) {
                        if (other.name == f.customTypeName) {
                            isInternal = true;
                            break;
                        }
                    }
                    if (isInternal && !emittedNames.contains(f.customTypeName)) {
                        allDepsEmitted = false;
                        break;
                    }
                }
            }
            if (allDepsEmitted) {
                sortedStructs.push_back(def);
                emittedNames.insert(def.name);
            }
        }
    }
    for (const StructDefinition& def : internalStructs) {
        if (!emittedNames.contains(def.name)) {
            sortedStructs.push_back(def);
            emittedNames.insert(def.name);
        }
    }

    for (const StructDefinition& def : sortedStructs) {
        text += QStringLiteral("struct %1 {\n").arg(def.name);
        for (const StructField& f : def.fields) {
            QString baseType;
            switch (f.type) {
                case FieldType::Bool: baseType = QStringLiteral("bool"); break;
                case FieldType::Int: baseType = QStringLiteral("long long"); break;
                case FieldType::Double: baseType = QStringLiteral("double"); break;
                case FieldType::String: baseType = QStringLiteral("std::string"); break;
                case FieldType::Custom: baseType = f.customTypeName; break;
            }
            QString fullType = baseType;
            if (f.isArray) {
                if (f.arraySize > 0) {
                    fullType = QStringLiteral("std::array<%1, %2>").arg(baseType).arg(f.arraySize);
                } else {
                    fullType = QStringLiteral("std::vector<%1>").arg(baseType);
                }
            }
            QString init;
            if (!f.initialValue.trimmed().isEmpty()) {
                if (f.type == FieldType::String && !f.isArray) {
                    QString val = f.initialValue.trimmed();
                    if (val.startsWith(QLatin1Char('"')) && val.endsWith(QLatin1Char('"')) && val.size() >= 2) {
                        val = val.mid(1, val.size() - 2);
                    }
                    init = QStringLiteral(" = \"%1\"").arg(val);
                } else if (!f.isArray) {
                    init = QStringLiteral(" = %1").arg(f.initialValue.trimmed());
                } else {
                    init = QStringLiteral("{}");
                }
            } else {
                init = QStringLiteral("{}");
            }
            text += QString::fromLatin1(kIndent) + QStringLiteral("%1 %2%3;\n").arg(fullType, f.name, init);
        }
        text += QString::fromLatin1(kIndent) + QStringLiteral("bool operator==(const %1&) const = default;\n").arg(def.name);
        text += QStringLiteral("};\n\n");
    }

    text += closeNamespace(model.nsJoined);
    return GeneratedFile{
        .relativePath = model.machineNs + QStringLiteral("_types.h"),
        .content = text,
    };
}

GeneratedFile buildStateFile(const Machine& machine, const GenModel& model, bool hierarchical) {
    QString text = bannerLines(QStringLiteral("<%1>State enum + toString().").arg(model.machinePascal));
    text += QStringLiteral("#pragma once\n\n#include <cstdint>\n#include <string_view>\n\n");
    text += openNamespace(model.nsJoined) + QStringLiteral("\n");

    text += QStringLiteral("enum class %1State : std::uint32_t {\n").arg(model.machinePascal);
    for (const State& state : machine.states) {
        text += QString::fromLatin1(kIndent) + stateIdent(model, state.id);
        // The flat core runs Parallel/History states as plain states, so say so;
        // the hierarchical core executes them, so no note there.
        if (!hierarchical && state.kind == StateKind::Parallel) {
            text += QStringLiteral(",  // Parallel in the diagram -- no child states, so it runs as a plain state\n");
        } else if (!hierarchical && state.kind == StateKind::History) {
            text += QStringLiteral(",  // History in the diagram -- no parent state, so it runs as a plain state\n");
        } else {
            text += QStringLiteral(",\n");
        }
    }
    text += QStringLiteral("};\n\n");

    text += QStringLiteral("constexpr std::string_view toString(%1State state) {\n").arg(model.machinePascal);
    if (!machine.states.isEmpty()) {
        text += QStringLiteral("    switch (state) {\n");
        for (const State& state : machine.states) {
            const QString ident = stateIdent(model, state.id);
            text += QStringLiteral("        case %1State::%2: return \"%2\";\n").arg(model.machinePascal, ident);
        }
        text += QStringLiteral("    }\n");
    }
    text += QStringLiteral("    return \"\";\n}\n\n");
    text += closeNamespace(model.nsJoined);

    return GeneratedFile{.relativePath = model.machineNs + QStringLiteral("_state.h"), .content = text};
}

// ---- <machine>_events.h -----------------------------------------------------



GeneratedFile buildEventsFile(const GenModel& model) {
    QString text = bannerLines(QStringLiteral("%1's event vocabulary (namespaced ::events).").arg(model.machinePascal));
    text += QStringLiteral("#pragma once\n\n#include <string_view>\n\n#include \"%1_state.h\"\n").arg(model.machineNs);
    if (model.hasTypes) {
        text += QStringLiteral("#include \"%1_types.h\"\n").arg(model.machineNs);
    }
    text += QStringLiteral("\n");
    text += openNamespace(model.nsJoined + QStringLiteral("::events")) + QStringLiteral("\n");

    for (const QString& rawEvent : model.eventOrder) {
        const QString structName = model.eventStructNameByRaw.value(rawEvent);
        text += QStringLiteral("struct %1 {\n").arg(structName);
        text += QStringLiteral("    static constexpr std::string_view eventName = \"%1\";\n").arg(rawEvent);
        const QString payloadType = model.eventPayloadTypeByRaw.value(rawEvent);
        if (!payloadType.isEmpty()) {
            text += QStringLiteral("    %1 payload{};\n").arg(cppPayloadTypeSpelling(payloadType));
        }
        text += QStringLiteral("};\n\n");
    }

    // One broadcast for every state change; no per-state entering/exiting fact.
    text += QStringLiteral("struct StateChanged {\n");
    text += QStringLiteral("    static constexpr std::string_view eventName = \"StateChanged\";\n");
    if (!model.defaultStateIdent.isEmpty()) {
        text += QStringLiteral("    %1State from = %1State::%2;\n").arg(model.machinePascal, model.defaultStateIdent);
        text += QStringLiteral("    %1State to = %1State::%2;\n").arg(model.machinePascal, model.defaultStateIdent);
    } else {
        text += QStringLiteral("    %1State from{};\n").arg(model.machinePascal);
        text += QStringLiteral("    %1State to{};\n").arg(model.machinePascal);
    }
    text += QStringLiteral("};\n\n");

    text += closeNamespace(model.nsJoined + QStringLiteral("::events"));
    return GeneratedFile{.relativePath = model.machineNs + QStringLiteral("_events.h"), .content = text};
}

// ---- <machine>_hooks.h ------------------------------------------------------



GeneratedFile buildHooksFile(const GenModel& model, bool hierarchical) {
    QString text = bannerLines(QStringLiteral("%1's Guards/Actions[/Scheduler][/Invocations] pure-virtual interfaces.")
                                    .arg(model.machinePascal));
    text += QStringLiteral("#pragma once\n\n");
    // Only the includes this machine needs, alphabetical; none at all for a
    // machine without context, Scheduler or invoke.
    {
        QStringList includes;
        if (model.hasScheduler || model.hasInvoke) {
            includes.push_back(QStringLiteral("<functional>"));
        }
        if (model.hasInvoke) {
            // start<Service>() takes a std::stop_token.
            includes.push_back(QStringLiteral("<stop_token>"));
        }
        if (model.hasStringContext || model.hasInvoke) {
            // onError's payload is always `const std::string&`.
            includes.push_back(QStringLiteral("<string>"));
        }
        if (model.hasTypes) {
            includes.push_back(QStringLiteral("\"%1_types.h\"").arg(model.machineNs));
        }
        if (!includes.isEmpty()) {
            for (const QString& include : includes) {
                text += QStringLiteral("#include %1\n").arg(include);
            }
            text += QStringLiteral("\n");
        }
    }
    text += openNamespace(model.nsJoined) + QStringLiteral("\n");

    if (model.hasContext) {
        // Before both interfaces: their method signatures name Context.
        text += QStringLiteral(
                    "// The machine's EXTENDED STATE: one typed member per context variable, in\n"
                    "// DOCUMENT order, names VERBATIM -- the designer's validator already\n"
                    "// guarantees they are unique, valid, non-reserved C++ identifiers, so\n"
                    "// nothing is sanitized here. Each member's initialiser is the variable's\n"
                    "// initialValue, read under the same literal rules the designer's\n"
                    "// simulator uses, so generated code and simulation start every run from\n"
                    "// the identical extended state. %1Core owns the one instance; the hooks\n"
                    "// below only borrow it.\n")
                    .arg(model.machinePascal);
        text += QStringLiteral("struct Context {\n");
        for (const ContextVariable& variable : model.context) {
            if (variable.type == ContextType::Object) {
                if (!variable.customTypeName.isEmpty()) {
                    text += QString::fromLatin1(kIndent) + QStringLiteral("%1 %2{};\n")
                                .arg(variable.customTypeName, variable.name);
                } else {
                    QJsonParseError err;
                    const QJsonDocument jsonDoc = QJsonDocument::fromJson(variable.initialValue.trimmed().toUtf8(), &err);
                    const QJsonObject obj = (!jsonDoc.isNull() && jsonDoc.isObject()) ? jsonDoc.object() : QJsonObject{};
                    const QString structTypeName = sanitizeIdentifier(variable.name, true) + QStringLiteral("_t");
                    emitNestedStruct(text, obj, structTypeName, variable.name, /*indentLevel=*/1);
                }
            } else {
                text += QString::fromLatin1(kIndent) + QStringLiteral("%1 %2 = %3;\n")
                                                            .arg(contextTypeSpelling(variable.type), variable.name,
                                                                 contextInitialiser(variable));
            }
        }
        text += QStringLiteral("};\n\n");
    }

    if (model.hasContext) {
        text += QStringLiteral(
                    "// One pure-virtual per distinct guard NAME on the diagram -- a guard whose\n"
                    "// whole text is one bare identifier. An EXPRESSION guard\n"
                    "// (`count > 3 && !locked`) is compiled INLINE against Context in\n"
                    "// %2_core.h and has no method here at all. The `const Context&` is the\n"
                    "// machine's live extended state: read it, never cast the const away -- a\n"
                    "// guard that mutates the context would make which candidate fires depend\n"
                    "// on how many candidates were TRIED. Implement this interface in the\n"
                    "// domain/ stub, which is never regenerated, and inject the\n"
                    "// implementation at register%1() time\n"
                    "// (%2_bootstrap.h) -- renaming a guard on canvas changes this method's\n"
                    "// name on the next [Generate C++], so a stale implementation fails to\n"
                    "// compile instead of silently going stale.\n")
                    .arg(model.machinePascal, model.machineNs);
    } else {
        text += QStringLiteral(
                    "// One pure-virtual per distinct guard name on the diagram, name-only\n"
                    "// (no expression evaluation). Implement this interface in the domain/\n"
                    "// stub, which is never regenerated, and inject the implementation at\n"
                    "// register%1() time\n"
                    "// (%2_bootstrap.h) -- renaming a guard on canvas changes this method's\n"
                    "// name on the next [Generate C++], so a stale implementation fails to\n"
                    "// compile instead of silently going stale.\n")
                    .arg(model.machinePascal, model.machineNs);
    }
    const QString guardParam = model.hasContext ? QStringLiteral("const Context&") : QString();
    const QString actionParam = model.hasContext ? QStringLiteral("Context&") : QString();
    text += QStringLiteral("struct %1Guards {\n").arg(model.machinePascal);
    text += QStringLiteral("    virtual ~%1Guards() = default;\n").arg(model.machinePascal);
    for (const QString& raw : model.guards.rawOrder()) {
        text += QStringLiteral("    virtual bool %1(%2) = 0;\n").arg(model.guards.idOf(raw), guardParam);
    }
    text += QStringLiteral("};\n\n");

    if (model.hasContext) {
        text += QStringLiteral(
                    "// One pure-virtual per distinct action name -- covers BOTH transition\n"
                    "// actions and state entry actions (they share one vocabulary and one\n"
                    "// interface; %1_core.h calls the right ones in the right order per\n"
                    "// transition). An ASSIGN-form action (`count = 3`) is emitted as a plain\n"
                    "// assignment on Context in that same file and has no method here. The\n"
                    "// `Context&` is mutable: an action IS the place effects belong, and the\n"
                    "// XState v5 firing order (actions before the state change) is what makes\n"
                    "// a write here visible to the destination's entry actions and NOT to the\n"
                    "// guard that selected this transition.\n")
                    .arg(model.machineNs);
    } else {
        text += QStringLiteral(
                    "// One pure-virtual per distinct action name -- covers BOTH transition\n"
                    "// actions and state entry actions (they share one vocabulary and one\n"
                    "// interface; %1_core.h calls the right ones in the right order per\n"
                    "// transition).\n")
                    .arg(model.machineNs);
    }
    text += QStringLiteral("struct %1Actions {\n").arg(model.machinePascal);
    text += QStringLiteral("    virtual ~%1Actions() = default;\n").arg(model.machinePascal);
    for (const QString& raw : model.actions.rawOrder()) {
        text += QStringLiteral("    virtual void %1(%2) = 0;\n").arg(model.actions.idOf(raw), actionParam);
    }
    text += QStringLiteral("};\n\n");

    if (model.hasScheduler) {
        text += QStringLiteral(
                    "// Emitted because this machine has at least one delayed transition\n"
                    "// (blank event, delayMs > 0).\n"
                    "// Generated code is Qt-free and cannot own a timer directly, so the\n"
                    "// consuming app supplies the timer technology (QTimer, a manual test\n"
                    "// clock, ...) by implementing this interface: `fire` must run at least\n"
                    "// `delayMs` after scheduleAfter() returns (late is fine, early is not).\n"
                    "// The core that arms this hook (%1_core.h) re-checks its guard (if any)\n"
                    "// AND the machine's current active state at fire time, not at arm time --\n"
                    "// see that file's %2() for why a stale, already-\n"
                    "// superseded callback firing late is a safe no-op rather than a real\n"
                    "// problem (that re-check stands in for literally cancelling an\n"
                    "// in-flight timer).\n")
                    .arg(model.machineNs,
                         hierarchical ? QStringLiteral("armState()/armRoot()") : QStringLiteral("armDelayedTransitions()"));
        text += QStringLiteral("struct %1Scheduler {\n").arg(model.machinePascal);
        text += QStringLiteral("    virtual ~%1Scheduler() = default;\n").arg(model.machinePascal);
        text += QStringLiteral("    virtual void scheduleAfter(int delayMs, std::function<void()> fire) = 0;\n");
        text += QStringLiteral("    virtual void scheduleTimer(int timerId, int delayMs, bool periodic, std::function<void()> fire) {\n"
                               "        (void)timerId; (void)periodic;\n"
                               "        scheduleAfter(delayMs, std::move(fire));\n"
                               "    }\n");
        text += QStringLiteral("    virtual void cancelTimer(int timerId) { (void)timerId; }\n");
        text += QStringLiteral("};\n\n");
    }

    if (model.hasInvoke) {
        // The emitted comment below is the user-facing threading and
        // cancellation contract for Invocations.
        text += QStringLiteral(
                    "// Emitted because this machine has at least one invoking state: one\n"
                    "// pure-virtual per DISTINCT service -- the invoking state's EFFECTIVE\n"
                    "// invoke id (the invoke's id when set, its src otherwise), never per\n"
                    "// state, so two states naming the same service share one method,\n"
                    "// exactly like a same-named guard/action shares one hook across\n"
                    "// transitions.\n"
                    "//\n"
                    "// THREADING CONTRACT (read before implementing): start<Service>()\n"
                    "// itself is called on the machine's own thread. onDone/onError may\n"
                    "// be invoked from ANY thread by the implementation, but MUST be\n"
                    "// marshalled back onto the machine's thread before being called --\n"
                    "// the core's completion lambda reads state_ and writes context_ with\n"
                    "// no locking at all, so calling it from a worker thread is a data\n"
                    "// race, and the fire-time re-check inside it (is the machine still\n"
                    "// in the invoking state? -- %1_core.h's cancelInvocations()/\n"
                    "// armInvocations()) would itself be racing rather than deciding\n"
                    "// anything. Marshalling is the APP's job, not this generated code's:\n"
                    "// the core is framework-free, has no event loop, and cannot know\n"
                    "// what \"the machine's thread\" even means -- the same reason every\n"
                    "// other hook interface here is framework-agnostic (Guards/Actions,\n"
                    "// and Scheduler when this machine declares one): the app supplies\n"
                    "// the technology, never the core/adapter. In a Qt app, a typical\n"
                    "// shape is an event-loop-affine relay object living on the\n"
                    "// machine's thread that a worker posts through instead of calling\n"
                    "// onDone/onError directly.\n"
                    "//\n"
                    "// `stopToken` is this invoke's cooperative cancellation handle:\n"
                    "// %1_core.h's cancelInvocations() requests it the instant the\n"
                    "// invoking state is left, by ANY route (an ordinary event, a\n"
                    "// delayed transition, or the root fallback) -- polling it and\n"
                    "// giving up promptly is the implementation's OPTION, never its\n"
                    "// obligation, because a completion that arrives anyway after the\n"
                    "// state was left is still handled safely: the fire-time re-check\n"
                    "// inside the completion lambda makes it a silent no-op -- the same\n"
                    "// contract a delayed transition's own stale-callback fire lambda\n"
                    "// relies on elsewhere in this file (Scheduler's, when this machine\n"
                    "// declares one).\n")
                    .arg(model.machineNs);
        text += QStringLiteral("struct %1Invocations {\n").arg(model.machinePascal);
        text += QStringLiteral("    virtual ~%1Invocations() = default;\n").arg(model.machinePascal);
        for (const QString& effectiveId : model.invokeIdOrder) {
            const QString outputSpelling = contextTypeSpelling(model.invokeOutputTypeByEffectiveId.value(effectiveId));
            text += QStringLiteral("    virtual void %1(std::stop_token stopToken,\n"
                                    "                       std::function<void(%2 output)> onDone,\n"
                                    "                       std::function<void(const std::string& error)> onError) = 0;\n")
                        .arg(invokeHookMethodName(effectiveId), outputSpelling);
        }
        text += QStringLiteral("};\n\n");
    }

    text += closeNamespace(model.nsJoined);
    return GeneratedFile{.relativePath = model.machineNs + QStringLiteral("_hooks.h"), .content = text};
}

// ---- <machine>_agent.h ------------------------------------------------------


GeneratedFile buildAgentFile(const GenModel& model, bool hierarchical) {
    QString text = bannerLines(QStringLiteral("%1StateAgent -- the kernel face of %1Core.").arg(model.machinePascal));
    text += QStringLiteral("#pragma once\n\n#include <ordo/core/agent.h>\n\n#include \"%1_core.h\"\n#include "
                            "\"%1_events.h\"\n#include \"%1_hooks.h\"\n#include \"%1_state.h\"\n\n")
                .arg(model.machineNs);
    text += openNamespace(model.nsJoined) + QStringLiteral("\n");

    text += QStringLiteral(
                "// Mutate-then-publish, the ordo Agent role: the agent OWNS a %1Core\n"
                "// and wires its state-change callback to ONE events::StateChanged fact\n"
                "// per committed transition -- the \"changed\" broadcast. No firing logic\n"
                "// lives here; the core is the only place it is emitted. Multiple\n"
                "// machines/kernels may register an instance of this class each under\n"
                "// kName -- unique per machine, not per kernel.\n"
                "//\n"
                "// The injected hooks reach the core through this constructor, so they\n"
                "// must outlive the agent -- %2_bootstrap.h's register%1() std::ref-\n"
                "// injects them.\n")
                .arg(model.machinePascal, model.machineNs);
    text += QStringLiteral("class %1StateAgent : public ordo::core::Agent {\n").arg(model.machinePascal);
    text += QStringLiteral("public:\n");
    text += QStringLiteral("    static constexpr const char* kName = \"%1_state\";\n\n").arg(model.machineNs);
    text += QStringLiteral("    %1StateAgent(%1Guards& guards, %1Actions& actions%2)\n")
                .arg(model.machinePascal, extraCtorParams(model));
    text += QStringLiteral("        : Agent(kName), core_(guards, actions%1) {\n").arg(extraArgForward(model));
    text += QStringLiteral("        core_.setOnStateChanged([this](%1State previous, %1State next) {\n")
                .arg(model.machinePascal);
    text += QStringLiteral("            send(events::StateChanged{previous, next});\n");
    text += QStringLiteral("        });\n");
    text += QStringLiteral("    }\n\n");
    if (hierarchical) {
        // No state(): Parallel regions can have several active leaves.
        text += QStringLiteral(
                    "    // No single state() accessor here -- see %1Core's own header\n"
                    "    // comment for why (a Parallel configuration has no one value).\n")
                    .arg(model.machinePascal);
        text += QStringLiteral("    bool isActive(%1State state) const { return core_.isActive(state); }\n\n")
                    .arg(model.machinePascal);
    } else {
        text += QStringLiteral("    %1State state() const { return core_.state(); }\n\n").arg(model.machinePascal);
    }
    text += QStringLiteral(
        "    // The kernel-free face: %1_commands.h fires events through this,\n"
        "    // and any non-kernel owner can drive the same core directly.\n").arg(model.machineNs);
    text += QStringLiteral("    %1Core& core() { return core_; }\n").arg(model.machinePascal);
    text += QStringLiteral("    const %1Core& core() const { return core_; }\n\n").arg(model.machinePascal);
    if (!hierarchical) {
        // Flat only: forcing one state is meaningless with several active.
        text += QStringLiteral("    void transitionTo(%1State next) { core_.transitionTo(next); }\n\n")
                    .arg(model.machinePascal);
    }
    text += QStringLiteral("private:\n");
    text += QStringLiteral("    %1Core core_;\n").arg(model.machinePascal);
    text += QStringLiteral("};\n\n");
    text += closeNamespace(model.nsJoined);

    return GeneratedFile{.relativePath = model.machineNs + QStringLiteral("_agent.h"), .content = text};
}

// ---- <machine>_commands.h ---------------------------------------------------



GeneratedFile buildCommandsFile(const GenModel& model, bool hierarchical) {
    QStringList armFunctionLabels;
    if (model.hasScheduler && !hierarchical) {
        armFunctionLabels.push_back(QStringLiteral("the delayed-transition arm function"));
    }
    if (model.hasInvoke && !hierarchical) {
        armFunctionLabels.push_back(QStringLiteral("the invocation arm function"));
    }
    QString text = bannerLines(QStringLiteral("%1's transition commands (one per distinct event)%2.")
                                    .arg(model.machinePascal, armFunctionLabels.isEmpty()
                                                                   ? QString()
                                                                   : QStringLiteral(" + ") + armFunctionLabels.join(QStringLiteral(" + "))));
    text += QStringLiteral("#pragma once\n\n#include <ordo/core/command.h>\n\n#include \"%1_agent.h\"\n#include "
                            "\"%1_events.h\"\n\n")
                .arg(model.machineNs);
    text += openNamespace(model.nsJoined) + QStringLiteral("\n");

    // Only the flat core needs an explicit arm step; a hierarchical core arms
    // delayed transitions from its own constructor.
    if (model.hasScheduler && !hierarchical) {
        text += QStringLiteral(
                    "// Arms the agent's current state's delayed transitions -- a named entry\n"
                    "// point for the kernel side (%1_bootstrap.h's register%2() calls it\n"
                    "// once for the starting state); the core re-arms itself on every\n"
                    "// subsequent state entry, so nothing else has to call this.\n")
                    .arg(model.machineNs, model.machinePascal);
        text += QStringLiteral("inline void arm%1DelayedTransitions(%1StateAgent& agent) {\n").arg(model.machinePascal);
        text += QStringLiteral("    agent.core().armDelayedTransitions();\n}\n\n");
        if (model.delayedBySource.contains(0)) {
            text += QStringLiteral(
                        "// The machine's own root `after` countdowns -- armed exactly once, at\n"
                        "// register%1() time (the machine root's entry), never on state entry.\n")
                        .arg(model.machinePascal);
            text += QStringLiteral("inline void arm%1RootDelayedTransitions(%1StateAgent& agent) {\n")
                        .arg(model.machinePascal);
            text += QStringLiteral("    agent.core().armRootDelayedTransitions();\n}\n\n");
        }
    }

    // Same for invocations: the hierarchical constructor already arms them.
    if (model.hasInvoke && !hierarchical) {
        text += QStringLiteral(
                    "// Arms the agent's current state's invoke (if it has one) -- a named\n"
                    "// entry point for the kernel side (%1_bootstrap.h's register%2() calls\n"
                    "// it once for the starting state); the core re-arms itself on every\n"
                    "// subsequent state entry, so nothing else has to call this.\n")
                    .arg(model.machineNs, model.machinePascal);
        text += QStringLiteral("inline void arm%1Invocations(%1StateAgent& agent) {\n").arg(model.machinePascal);
        text += QStringLiteral("    agent.core().armInvocations();\n}\n\n");
    }

    text += QStringLiteral(
                "// One Command per distinct event, each a thin delegation to the agent's\n"
                "// %1Core: the guard/action/exit/entry order is emitted in\n"
                "// %2_core.h and nowhere else, so the kernel path and a kernel-free\n"
                "// owner cannot drift apart.\n")
                .arg(model.machinePascal, model.machineNs);
    for (const QString& rawEvent : model.eventOrder) {
        const QString structName = model.eventStructNameByRaw.value(rawEvent);
        const QString commandName = structName + QStringLiteral("Command");
        const QString methodName = model.eventMethodByRaw.value(rawEvent);
        const QString payloadType = model.eventPayloadTypeByRaw.value(rawEvent);

        text += QStringLiteral("class %1 : public ordo::core::Command<events::%2> {\n").arg(commandName, structName);
        text += QStringLiteral("public:\n");
        if (payloadType.isEmpty()) {
            text += QStringLiteral("    void execute(const events::%1&, ordo::core::CommandContext& context) override {\n")
                        .arg(structName);
            text += QStringLiteral("        auto agent = context.agentAs<%1StateAgent>(%1StateAgent::kName);\n")
                        .arg(model.machinePascal);
            text += QStringLiteral("        if (agent == nullptr) {\n            return;\n        }\n");
            text += QStringLiteral("        agent->core().%1();\n").arg(methodName);
            text += QStringLiteral("    }\n");
        } else {
            text += QStringLiteral("    void execute(const events::%1& event, ordo::core::CommandContext& context) override {\n")
                        .arg(structName);
            text += QStringLiteral("        auto agent = context.agentAs<%1StateAgent>(%1StateAgent::kName);\n")
                        .arg(model.machinePascal);
            text += QStringLiteral("        if (agent == nullptr) {\n            return;\n        }\n");
            text += QStringLiteral("        agent->core().%1(event.payload);\n").arg(methodName);
            text += QStringLiteral("    }\n");
        }
        text += QStringLiteral("};\n\n");
    }

    text += closeNamespace(model.nsJoined);
    return GeneratedFile{.relativePath = model.machineNs + QStringLiteral("_commands.h"), .content = text};
}

// ---- <machine>_bootstrap.h ---------------------------------------------------



GeneratedFile buildBootstrapFile(const GenModel& model, bool hierarchical) {
    QStringList wordList;
    if (model.hasScheduler) {
        wordList.push_back(QStringLiteral("Scheduler"));
    }
    if (model.hasInvoke) {
        wordList.push_back(QStringLiteral("Invocations"));
    }
    QString text = bannerLines(
        QStringLiteral("register%1()/unregister%1() -- std::ref injection of your Guards/Actions%2 implementation.")
            .arg(model.machinePascal,
                 wordList.isEmpty() ? QString() : QStringLiteral("/") + wordList.join(QStringLiteral("/"))));
    text += QStringLiteral("#pragma once\n\n#include <functional>\n#include <memory>\n\n#include "
                            "<ordo/core/kernel.h>\n\n#include \"%1_agent.h\"\n#include \"%1_commands.h\"\n#include "
                            "\"%1_events.h\"\n#include \"%1_hooks.h\"\n\n")
                .arg(model.machineNs);
    text += openNamespace(model.nsJoined) + QStringLiteral("\n");

    // What register%1() arms: flat needs explicit steps, hierarchical none.
    QString armNote;
    if (!hierarchical) {
        QStringList armedThings;
        if (model.hasScheduler) {
            armedThings.push_back(QStringLiteral("delayed transitions"));
        }
        if (model.hasInvoke) {
            armedThings.push_back(QStringLiteral("invoke"));
        }
        if (!armedThings.isEmpty()) {
            armNote = QStringLiteral(" Arms the agent's\n// starting state's %1 once, the same way\n"
                                      "// the designer's simulator arms its initial state.")
                          .arg(armedThings.join(QStringLiteral(" and ")));
        }
    } else if (model.hasScheduler || model.hasInvoke) {
        QStringList armedThings;
        if (model.hasScheduler) {
            armedThings.push_back(QStringLiteral("delayed transitions (plus the root's, once)"));
        }
        if (model.hasInvoke) {
            armedThings.push_back(QStringLiteral("invoke"));
        }
        armNote = QStringLiteral(" The agent's own\n// %1Core constructor already armed every initially-active "
                                  "state's\n// %2 -- there is no\n// separate arm step here.")
                      .arg(model.machinePascal, armedThings.join(QStringLiteral(" and ")));
    }
    text += QStringLiteral(
                "// Registers this machine's agent + one command per distinct event onto\n"
                "// `kernel` (plain code, run once, in order) -- `guards`/`actions`%1\n"
                "// are std::ref-injected into the AGENT, which forwards them to the\n"
                "// %2Core it owns, so they must outlive the agent -- that is, until\n"
                "// the matching unregister%2() call below. The commands themselves hold\n"
                "// no hooks: they resolve the agent and call its core.%3\n")
                .arg(extraRefComment(model), model.machinePascal, armNote);
    text += QStringLiteral("inline void register%1(ordo::core::Kernel& kernel, %1Guards& guards, %1Actions& "
                            "actions%2) {\n")
                .arg(model.machinePascal, extraCtorParams(model));
    text += QStringLiteral("    kernel.registerAgent(std::make_shared<%1StateAgent>(std::ref(guards), "
                            "std::ref(actions)%2));\n")
                .arg(model.machinePascal, extraStdRefForward(model));
    for (const QString& rawEvent : model.eventOrder) {
        const QString structName = model.eventStructNameByRaw.value(rawEvent);
        text += QStringLiteral("    kernel.registerCommand<events::%1, %1Command>();\n").arg(structName);
    }
    if ((model.hasScheduler || model.hasInvoke) && !hierarchical) {
        text += QStringLiteral("\n    auto agent = kernel.agentAs<%1StateAgent>(%1StateAgent::kName);\n")
                    .arg(model.machinePascal);
        if (model.hasScheduler) {
            text += QStringLiteral("    arm%1DelayedTransitions(*agent);\n").arg(model.machinePascal);
            if (model.delayedBySource.contains(0)) {
                // The machine's own entry (v5 root node) happens exactly here.
                text += QStringLiteral("    arm%1RootDelayedTransitions(*agent);\n").arg(model.machinePascal);
            }
        }
        if (model.hasInvoke) {
            text += QStringLiteral("    arm%1Invocations(*agent);\n").arg(model.machinePascal);
        }
    }
    // Hierarchical: nothing to arm here.
    text += QStringLiteral("}\n\n");

    QString extraPlainWords;
    if (model.hasScheduler) {
        extraPlainWords += QStringLiteral("/scheduler");
    }
    if (model.hasInvoke) {
        extraPlainWords += QStringLiteral("/invocations");
    }
    text += QStringLiteral(
                "// Reverses register%1() -- drop this before `kernel` (or the injected\n"
                "// guards/actions%2) go out of scope. Removing the agent also destroys\n"
                "// the core that holds the injected references, which is what makes this\n"
                "// the safe teardown point.\n")
                .arg(model.machinePascal, extraPlainWords);
    text += QStringLiteral("inline void unregister%1(ordo::core::Kernel& kernel) {\n").arg(model.machinePascal);
    for (const QString& rawEvent : model.eventOrder) {
        const QString structName = model.eventStructNameByRaw.value(rawEvent);
        text += QStringLiteral("    kernel.removeCommand<events::%1>();\n").arg(structName);
    }
    text += QStringLiteral("    kernel.removeAgent(%1StateAgent::kName);\n").arg(model.machinePascal);
    text += QStringLiteral("}\n\n");

    text += closeNamespace(model.nsJoined);
    return GeneratedFile{.relativePath = model.machineNs + QStringLiteral("_bootstrap.h"), .content = text};
}

// ---- domain/ stubs -----------------------------------------------------------



QVector<GeneratedFile> buildDomainStubFiles(const Machine& machine, const GenModel& model) {
    (void)machine;
    // A different banner: once written, these files belong to the developer.
    const QString stubBanner = QStringLiteral(
        "// Starting-point stub written ONCE by Ordo State Designer's [Generate C++]\n"
        "// -- NEVER overwritten on regeneration. This file is yours: rename, move,\n"
        "// and fill it in. A guard/action renamed on the canvas surfaces here as a\n"
        "// compile error (unimplemented pure virtual), never as silent drift.\n");

    QString hooks = stubBanner;
    hooks += QStringLiteral("#pragma once\n\n#include \"../%1_hooks.h\"\n\nnamespace domain {\n\n").arg(model.machineNs);
    hooks += QStringLiteral("class %1GuardsImpl final : public %2::%1Guards {\npublic:\n")
                 .arg(model.machinePascal, model.nsJoined);
    if (model.guards.isEmpty()) {
        hooks += QStringLiteral("    // no guards on the diagram yet\n");
    }
    // Unnamed to avoid -Wunused-parameter in stub bodies; fully qualified
    // because the stubs live in `namespace domain`.
    const QString guardParam =
        model.hasContext ? QStringLiteral("const %1::Context&").arg(model.nsJoined) : QString();
    const QString actionParam = model.hasContext ? QStringLiteral("%1::Context&").arg(model.nsJoined) : QString();
    for (const QString& raw : model.guards.rawOrder()) {
        hooks += QStringLiteral("    bool %1(%3) override { return true; }  // TODO: real check for \"%2\"\n")
                     .arg(model.guards.idOf(raw), raw, guardParam);
    }
    hooks += QStringLiteral("};\n\n");
    hooks += QStringLiteral("class %1ActionsImpl final : public %2::%1Actions {\npublic:\n")
                 .arg(model.machinePascal, model.nsJoined);
    if (model.actions.isEmpty()) {
        hooks += QStringLiteral("    // no actions on the diagram yet\n");
    }
    for (const QString& raw : model.actions.rawOrder()) {
        const expr::SendToForm sendTo = expr::parseSendToForm(raw);
        if (sendTo.ok) {
            hooks += QStringLiteral("    void %1(%3) override {}  // TODO: sendTo actor '%2' event '%4'\n")
                         .arg(model.actions.idOf(raw), sendTo.target, actionParam, sendTo.event);
            continue;
        }
        const expr::SendParentForm sendParent = expr::parseSendParentForm(raw);
        if (sendParent.ok) {
            hooks += QStringLiteral("    void %1(%2) override {}  // TODO: sendParent event '%3'\n")
                         .arg(model.actions.idOf(raw), actionParam, sendParent.event);
            continue;
        }
        hooks += QStringLiteral("    void %1(%3) override {}  // TODO: real effect for \"%2\"\n")
                     .arg(model.actions.idOf(raw), raw, actionParam);
    }
    hooks += QStringLiteral("};\n\n");
    if (model.hasScheduler) {
        hooks += QStringLiteral(
                     "// The machine has delayed transitions -- supply a real timer here\n"
                     "// (QTimer::singleShot in a Qt app, a manual clock in tests).\n");
        hooks += QStringLiteral("class %1SchedulerImpl final : public %2::%1Scheduler {\npublic:\n")
                     .arg(model.machinePascal, model.nsJoined);
        hooks += QStringLiteral(
            "    void scheduleAfter(int delayMs, std::function<void()> fire) override {\n"
            "        (void)delayMs;\n"
            "        (void)fire;  // TODO: run `fire` at least delayMs later\n"
            "    }\n"
            "    void scheduleTimer(int timerId, int delayMs, bool periodic, std::function<void()> fire) override {\n"
            "        (void)timerId;\n"
            "        (void)periodic;\n"
            "        scheduleAfter(delayMs, std::move(fire));\n"
            "    }\n"
            "    void cancelTimer(int timerId) override {\n"
            "        (void)timerId;\n"
            "    }\n};\n\n");
    }
    if (model.hasInvoke) {
        hooks += QStringLiteral(
                     "// The machine has at least one invoke -- supply a real async call here\n"
                     "// (for example a worker thread plus a Qt relay object) and remember the\n"
                     "// threading contract %1Invocations's own header comment\n"
                     "// (../%2_hooks.h) states: onDone/onError must be marshalled back onto\n"
                     "// the machine's thread.\n")
                     .arg(model.machinePascal, model.machineNs);
        hooks += QStringLiteral("class %1InvocationsImpl final : public %2::%1Invocations {\npublic:\n")
                     .arg(model.machinePascal, model.nsJoined);
        for (const QString& effectiveId : model.invokeIdOrder) {
            const QString outputSpelling = contextTypeSpelling(model.invokeOutputTypeByEffectiveId.value(effectiveId));
            hooks += QStringLiteral(
                         "    void %1(std::stop_token stopToken, std::function<void(%2 output)> onDone,\n"
                         "               std::function<void(const std::string& error)> onError) override {\n"
                         "        (void)stopToken;\n"
                         "        (void)onDone;\n"
                         "        (void)onError;  // TODO: start the real async service for \"%3\"\n"
                         "    }\n")
                         .arg(invokeHookMethodName(effectiveId), outputSpelling, effectiveId);
        }
        hooks += QStringLiteral("};\n\n");
    }
    hooks += QStringLiteral("}  // namespace domain\n");

    QString bootstrap = stubBanner;
    bootstrap += QStringLiteral("#include \"%1_hooks_impl.h\"\n\n#include \"../%1_bootstrap.h\"\n\n"
                                 "namespace domain {\n\n")
                     .arg(model.machineNs);
    bootstrap += QStringLiteral(
                     "// The analogue of XState's .provide(): the implementations\n"
                     "// are std::ref-injected into the agent (which forwards them to the core it\n"
                     "// owns) and must outlive it -- static lifetime keeps this starting point\n"
                     "// correct until you own the wiring.\n");
    bootstrap += QStringLiteral("inline void bootstrap%1(ordo::core::Kernel& kernel) {\n").arg(model.machinePascal);
    bootstrap += QStringLiteral("    static %1GuardsImpl guards;\n    static %1ActionsImpl actions;\n")
                     .arg(model.machinePascal);
    if (model.hasScheduler) {
        bootstrap += QStringLiteral("    static %1SchedulerImpl scheduler;\n").arg(model.machinePascal);
    }
    if (model.hasInvoke) {
        bootstrap += QStringLiteral("    static %1InvocationsImpl invocations;\n").arg(model.machinePascal);
    }
    bootstrap += QStringLiteral("    %1::register%2(kernel, guards, actions%3);\n")
                     .arg(model.nsJoined, model.machinePascal, extraStdRefForward(model));
    bootstrap += QStringLiteral("}\n\n}  // namespace domain\n");

    QVector<GeneratedFile> stubs;
    stubs.push_back(GeneratedFile{.relativePath = QStringLiteral("domain/%1_hooks_impl.h").arg(model.machineNs),
                                   .content = hooks});
    stubs.push_back(GeneratedFile{.relativePath = QStringLiteral("domain/%1_bootstrap.cpp").arg(model.machineNs),
                                   .content = bootstrap});
    return stubs;
}


}  // namespace app
