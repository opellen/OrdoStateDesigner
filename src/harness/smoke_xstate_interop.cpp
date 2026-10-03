#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <cstdio>

#include "infra/expression.h"
#include "infra/machine_validator.h"
#include "infra/xstate_v5_io.h"
#include "model/machine.h"
#include "view/geometry/auto_layout.h"  // machineHasNoGeometry()

#include "harness/harness.h"

// infra/xstate_v5_io.h driven headlessly, both directions. One machine
// exercises the whole vocabulary at once. The round-trip invariant is export
// idempotence, toJson(fromJson(toJson(M))) == toJson(M); ids differ after
// re-minting, so Machine deep equality is not the bar. A Stately-style
// hand-written sample covers the import-first path (auto-layout +
// deferred-construct diagnostics).
int runXStateInteropSmoke() {
    app::Machine machine;
    machine.name = QStringLiteral("Checkout");
    machine.states.push_back(app::State{.id = 1,
                                        .name = QStringLiteral("Cart"),
                                        .kind = app::StateKind::Normal,
                                        .pos = QPointF(0, 0),
                                        .entryActions = {QStringLiteral("logEnter")},
                                        .exitActions = {QStringLiteral("logExit")},
                                        .description = QStringLiteral("Items chosen"),
                                        .tags = {QStringLiteral("draft"), QStringLiteral("ui")}});
    machine.states.push_back(
        app::State{.id = 2, .name = QStringLiteral("Paying"), .kind = app::StateKind::Normal, .pos = QPointF(260, 0)});
    machine.states.push_back(
        app::State{.id = 3, .name = QStringLiteral("Done"), .kind = app::StateKind::Final, .pos = QPointF(520, 0)});
    machine.states.push_back(
        app::State{.id = 4, .name = QStringLiteral("Fork"), .kind = app::StateKind::Parallel, .pos = QPointF(0, 150)});
    machine.states.push_back(
        app::State{.id = 5, .name = QStringLiteral("Back"), .kind = app::StateKind::History, .pos = QPointF(260, 150)});
    // Duplicate (source, event) pair -> the exporter must emit a v5
    // guarded-candidate ARRAY for on.PAY, not overwrite one with the other.
    machine.transitions.push_back(app::Transition{.id = 6,
                                                  .from = 1,
                                                  .to = 2,
                                                  .event = QStringLiteral("PAY"),
                                                  .guard = QStringLiteral("hasItems"),
                                                  .action = QStringLiteral("charge")});
    machine.transitions.push_back(app::Transition{.id = 7, .from = 1, .to = 3, .event = QStringLiteral("PAY")});
    // Targetless (to == 0): the exported object must OMIT `target`.
    machine.transitions.push_back(
        app::Transition{.id = 8, .from = 1, .to = 0, .event = QStringLiteral("LOG"), .action = QStringLiteral("logOnly")});
    // Self transition carrying a user-dragged pill offset (meta.ordo channel).
    machine.transitions.push_back(app::Transition{
        .id = 9, .from = 2, .to = 2, .event = QStringLiteral("RETRY"), .labelOffset = QPointF(12, -8)});
    // Two delayed transitions sharing one delay -> after."3000" stays an array.
    machine.transitions.push_back(app::Transition{.id = 10, .from = 2, .to = 1, .delayMs = 3000});
    machine.transitions.push_back(app::Transition{.id = 11, .from = 2, .to = 3, .delayMs = 3000});
    // Decorative delayMs on an EVENTFUL transition -> exported without it, diagnosed.
    machine.transitions.push_back(
        app::Transition{.id = 12, .from = 2, .to = 3, .event = QStringLiteral("OK"), .delayMs = 500});
    // Dead transition (blank event, no delay) -> unmappable, dropped, diagnosed.
    machine.transitions.push_back(app::Transition{.id = 13, .from = 4, .to = 5});
    // Root transitions (from == 0) -> the machine-level `on`/`after`.
    machine.transitions.push_back(app::Transition{.id = 14, .from = 0, .to = 1, .event = QStringLiteral("PANIC")});
    machine.transitions.push_back(
        app::Transition{.id = 15, .from = 0, .to = 0, .event = QStringLiteral("AUDIT"), .action = QStringLiteral("audit")});
    machine.transitions.push_back(app::Transition{.id = 16, .from = 0, .to = 3, .delayMs = 9000});
    machine.nextId = 17;
    machine.initialStateId = 1;

    // The Cart.PAY pair (transitions 6/7) is v5's legal guarded-fallback shape
    // (guard first, bare fallback last), so validate() must not raise a
    // candidate-group Error; transition 13's dead-transition Error still stands.
    for (const app::Problem& problem : app::validate(machine)) {
        if ((problem.transitionId == 6 || problem.transitionId == 7) &&
            problem.severity == app::ProblemSeverity::Error) {
            std::fprintf(stderr, "FAIL: validate() wrongly flagged the legal guarded-fallback Cart.PAY pair: %s\n",
                         qUtf8Printable(problem.text));
            return 1;
        }
    }

    const app::XStateExportResult result = app::machineToXStateJson(machine);
    if (!result.ok) {
        std::fprintf(stderr, "FAIL: xstate export refused a well-formed machine: %s\n", qUtf8Printable(result.error));
        return 1;
    }
    const QJsonObject& json = result.json;
    if (json.value(QStringLiteral("id")).toString() != QStringLiteral("Checkout") ||
        json.value(QStringLiteral("initial")).toString() != QStringLiteral("Cart")) {
        std::fprintf(stderr, "FAIL: xstate export id/initial mismatch (want Checkout/Cart)\n");
        return 1;
    }
    const QJsonObject machineOrdo =
        json.value(QStringLiteral("meta")).toObject().value(QStringLiteral("ordo")).toObject();
    if (machineOrdo.value(QStringLiteral("formatVersion")).toInt() != 2) {
        std::fprintf(stderr, "FAIL: xstate export machine meta.ordo.formatVersion != 2\n");
        return 1;
    }
    const QJsonObject states = json.value(QStringLiteral("states")).toObject();
    if (states.size() != 5) {
        std::fprintf(stderr, "FAIL: xstate export emitted %d states (want 5)\n", static_cast<int>(states.size()));
        return 1;
    }

    // Payload-free export is unchanged: no transition on this "Checkout"
    // fixture sets payloadType, so meta.ordo.payloadType ("only when
    // non-empty") must stay out of every meta.ordo object; checked as a
    // substring absence on the full export.
    if (QJsonDocument(json).toJson(QJsonDocument::Compact).contains("payloadType")) {
        std::fprintf(stderr, "FAIL: payload-free 'Checkout' machine export gained a payloadType key\n");
        return 1;
    }

    // ---- root transitions -> machine-level on/after ---------------------------
    const QJsonObject rootOn = json.value(QStringLiteral("on")).toObject();
    const QJsonObject rootAudit = rootOn.value(QStringLiteral("AUDIT")).toObject();
    if (rootOn.value(QStringLiteral("PANIC")).toObject().value(QStringLiteral("target")).toString() !=
            QStringLiteral("Cart") ||
        rootAudit.contains(QStringLiteral("target")) ||
        rootAudit.value(QStringLiteral("actions")).toArray() != QJsonArray{QStringLiteral("audit")}) {
        std::fprintf(stderr, "FAIL: xstate export: root on.PANIC/on.AUDIT mapping mismatch\n");
        return 1;
    }
    const QJsonArray rootDelayed = json.value(QStringLiteral("after")).toObject().value(QStringLiteral("9000")).toArray();
    if (rootDelayed.size() != 1 ||
        rootDelayed.at(0).toObject().value(QStringLiteral("target")).toString() != QStringLiteral("Done")) {
        std::fprintf(stderr, "FAIL: xstate export: root after.9000 mapping mismatch\n");
        return 1;
    }

    // ---- Cart: entry/exit/description/tags + on.{PAY array, LOG targetless} ----
    const QJsonObject cart = states.value(QStringLiteral("Cart")).toObject();
    if (cart.contains(QStringLiteral("type"))) {
        std::fprintf(stderr, "FAIL: xstate export: Normal state must omit `type` (v5's atomic default)\n");
        return 1;
    }
    if (cart.value(QStringLiteral("entry")).toArray() != QJsonArray{QStringLiteral("logEnter")} ||
        cart.value(QStringLiteral("exit")).toArray() != QJsonArray{QStringLiteral("logExit")} ||
        cart.value(QStringLiteral("description")).toString() != QStringLiteral("Items chosen") ||
        cart.value(QStringLiteral("tags")).toArray() !=
            (QJsonArray{QStringLiteral("draft"), QStringLiteral("ui")})) {
        std::fprintf(stderr, "FAIL: xstate export: Cart entry/exit/description/tags mismatch\n");
        return 1;
    }
    const QJsonObject cartOn = cart.value(QStringLiteral("on")).toObject();
    if (!cartOn.value(QStringLiteral("PAY")).isArray()) {
        std::fprintf(stderr, "FAIL: xstate export: duplicate (Cart, PAY) pair must emit a candidate array\n");
        return 1;
    }
    const QJsonArray pay = cartOn.value(QStringLiteral("PAY")).toArray();
    if (pay.size() != 2 || pay.at(0).toObject().value(QStringLiteral("target")).toString() != QStringLiteral("Paying") ||
        pay.at(0).toObject().value(QStringLiteral("guard")).toString() != QStringLiteral("hasItems") ||
        pay.at(0).toObject().value(QStringLiteral("actions")).toArray() != QJsonArray{QStringLiteral("charge")} ||
        pay.at(1).toObject().value(QStringLiteral("target")).toString() != QStringLiteral("Done")) {
        std::fprintf(stderr, "FAIL: xstate export: on.PAY candidate array content mismatch\n");
        return 1;
    }
    const QJsonValue logValue = cartOn.value(QStringLiteral("LOG"));
    if (!logValue.isObject() || logValue.toObject().contains(QStringLiteral("target")) ||
        logValue.toObject().value(QStringLiteral("actions")).toArray() != QJsonArray{QStringLiteral("logOnly")}) {
        std::fprintf(stderr, "FAIL: xstate export: targetless LOG must be an object with actions and NO target\n");
        return 1;
    }

    // ---- Paying: self target, pill offset via meta.ordo, array-valued after ----
    const QJsonObject paying = states.value(QStringLiteral("Paying")).toObject();
    if (paying.contains(QStringLiteral("type")) || paying.contains(QStringLiteral("entry")) ||
        paying.contains(QStringLiteral("exit")) || paying.contains(QStringLiteral("description")) ||
        paying.contains(QStringLiteral("tags"))) {
        std::fprintf(stderr, "FAIL: xstate export: Paying's empty fields must be omitted, not emitted blank\n");
        return 1;
    }
    const QJsonObject payingOn = paying.value(QStringLiteral("on")).toObject();
    const QJsonObject retry = payingOn.value(QStringLiteral("RETRY")).toObject();
    const QJsonObject retryOrdo =
        retry.value(QStringLiteral("meta")).toObject().value(QStringLiteral("ordo")).toObject();
    if (retry.value(QStringLiteral("target")).toString() != QStringLiteral("Paying") ||
        retryOrdo.value(QStringLiteral("labelOffsetX")).toDouble() != 12.0 ||
        retryOrdo.value(QStringLiteral("labelOffsetY")).toDouble() != -8.0) {
        std::fprintf(stderr, "FAIL: xstate export: RETRY self-transition target/labelOffset mismatch\n");
        return 1;
    }
    if (payingOn.value(QStringLiteral("OK")).toObject().value(QStringLiteral("target")).toString() !=
        QStringLiteral("Done")) {
        std::fprintf(stderr, "FAIL: xstate export: on.OK must survive its decorative delayMs with target Done\n");
        return 1;
    }
    const QJsonObject after = paying.value(QStringLiteral("after")).toObject();
    const QJsonArray delayed = after.value(QStringLiteral("3000")).toArray();
    if (after.size() != 1 || delayed.size() != 2 ||
        delayed.at(0).toObject().value(QStringLiteral("target")).toString() != QStringLiteral("Cart") ||
        delayed.at(1).toObject().value(QStringLiteral("target")).toString() != QStringLiteral("Done")) {
        std::fprintf(stderr, "FAIL: xstate export: after.3000 must be an array of two targets (Cart, Done)\n");
        return 1;
    }

    // ---- kinds and geometry ---------------------------------------------------
    if (states.value(QStringLiteral("Done")).toObject().value(QStringLiteral("type")).toString() !=
            QStringLiteral("final") ||
        states.value(QStringLiteral("Fork")).toObject().value(QStringLiteral("type")).toString() !=
            QStringLiteral("parallel") ||
        states.value(QStringLiteral("Back")).toObject().value(QStringLiteral("type")).toString() !=
            QStringLiteral("history")) {
        std::fprintf(stderr, "FAIL: xstate export: Final/Parallel/History `type` mismatch\n");
        return 1;
    }
    const QJsonObject payingOrdo =
        paying.value(QStringLiteral("meta")).toObject().value(QStringLiteral("ordo")).toObject();
    if (payingOrdo.value(QStringLiteral("x")).toDouble() != 260.0 ||
        payingOrdo.value(QStringLiteral("y")).toDouble() != 0.0) {
        std::fprintf(stderr, "FAIL: xstate export: Paying meta.ordo geometry mismatch (want 260, 0)\n");
        return 1;
    }

    // ---- context-free export is unchanged ----
    // Checkout has no context variables, so it must carry no `context` key
    // and its root meta.ordo must hold nothing but the writer stamp.
    if (json.contains(QStringLiteral("context")) ||
        machineOrdo.keys() != QStringList{QStringLiteral("formatVersion")}) {
        std::fprintf(stderr,
                     "FAIL: a context-free machine's export grew a `context` key or a second meta.ordo member\n");
        return 1;
    }

    // ---- diagnostics: decorative delayMs + dead transition, nothing else ------
    if (result.diagnostics.size() != 2) {
        std::fprintf(stderr, "FAIL: xstate export produced %d diagnostics (want 2):\n",
                     static_cast<int>(result.diagnostics.size()));
        for (const QString& line : result.diagnostics) {
            std::fprintf(stderr, "  - %s\n", qUtf8Printable(line));
        }
        return 1;
    }
    bool foundDecorative = false;
    bool foundDead = false;
    for (const QString& line : result.diagnostics) {
        foundDecorative = foundDecorative || line.contains(QStringLiteral("delayMs 500"));
        foundDead = foundDead || line.contains(QStringLiteral("blank event"));
    }
    if (!foundDecorative || !foundDead) {
        std::fprintf(stderr, "FAIL: xstate export diagnostics missing the decorative-delay or dead-transition line\n");
        return 1;
    }

    // ---- refusal: blank state name --------------------------------------------
    {
        app::Machine blank = machine;
        blank.states[1].name = QStringLiteral("   ");
        const app::XStateExportResult refused = app::machineToXStateJson(blank);
        if (refused.ok || !refused.error.contains(QStringLiteral("blank name"))) {
            std::fprintf(stderr, "FAIL: xstate export accepted a blank state name\n");
            return 1;
        }
    }

    // ---- refusal: duplicate trimmed names (raw-name gate, pre-sanitization) ---
    {
        app::Machine duplicate = machine;
        duplicate.states[1].name = QStringLiteral(" Cart ");
        const app::XStateExportResult refused = app::machineToXStateJson(duplicate);
        if (refused.ok || !refused.error.contains(QStringLiteral("share the name"))) {
            std::fprintf(stderr, "FAIL: xstate export accepted two states sharing the trimmed name 'Cart'\n");
            return 1;
        }
    }

    // ---- round trip: export -> import -> export is byte-identical -------------
    // Importing this app's own export must produce zero diagnostics, reproduce
    // the surviving structure (the dead transition and the decorative delayMs
    // were already dropped by export), and re-export to a deep-equal QJsonObject.
    {
        const app::XStateImportResult imported = app::machineFromXStateJson(json);
        if (!imported.ok) {
            std::fprintf(stderr, "FAIL: xstate import refused this app's own export: %s\n",
                         qUtf8Printable(imported.error));
            return 1;
        }
        if (!imported.diagnostics.isEmpty()) {
            std::fprintf(stderr, "FAIL: importing this app's own export produced %d diagnostic(s):\n",
                         static_cast<int>(imported.diagnostics.size()));
            for (const QString& line : imported.diagnostics) {
                std::fprintf(stderr, "  - %s\n", qUtf8Printable(line));
            }
            return 1;
        }
        if (imported.machine.states.size() != 5 || imported.machine.transitions.size() != 10) {
            std::fprintf(stderr, "FAIL: reimported machine holds %d states / %d transitions (want 5 / 10)\n",
                         static_cast<int>(imported.machine.states.size()),
                         static_cast<int>(imported.machine.transitions.size()));
            return 1;
        }
        quint64 cartId = 0;
        for (const app::State& state : imported.machine.states) {
            if (state.name == QStringLiteral("Cart")) {
                cartId = state.id;
            }
        }
        if (cartId == 0 || imported.machine.initialStateId != cartId) {
            std::fprintf(stderr, "FAIL: the reimported machine's initial state is not Cart (name-keyed identity)\n");
            return 1;
        }
        const app::XStateExportResult second = app::machineToXStateJson(imported.machine);
        if (!second.ok || second.json != json) {
            std::fprintf(stderr, "FAIL: export is not idempotent -- toJson(fromJson(toJson(M))) != toJson(M)\n");
            return 1;
        }
    }

    // ---- import-first path: a Stately-style sample with NO meta.ordo ----------
    // Deferred constructs each produce a diagnostic (never a silent drop), the
    // mappable rest imports, and missing geometry comes from the deterministic
    // BFS auto-layout (260x150 grid from the initial state, unreachables
    // trailing). Only DOWN's '.'-relative target and BAD's unknown target fall
    // through to resolveTarget()'s diagnose-and-drop.
    {
        const char* sampleJson = R"json({
            "id": "Order",
            "initial": "Browsing",
            "context": { "items": 0 },
            "meta": { "stately": { "theme": "dark" } },
            "on": { "RESET": "Browsing" },
            "states": {
                "Browsing": {
                    "entry": { "type": "greet" },
                    "exit": "farewell",
                    "on": {
                        "ADD": { "target": "Cart", "guard": { "type": "hasRoom" }, "actions": ["track", "log"] },
                        "HELP": { "actions": { "type": "showHelp" } }
                    }
                },
                "Cart": {
                    "type": "compound",
                    "initial": "Empty",
                    "states": { "Empty": {} },
                    "on": {
                        "CHECKOUT": { "target": "Paying", "cond": "hasItems" },
                        "JUMP": { "target": "#Order.Paying" },
                        "DOWN": { "target": ".Empty" }
                    },
                    "after": { "30000": "Browsing", "SLOW": "Browsing" }
                },
                "Paying": {
                    "tags": "beta",
                    "invoke": { "src": "charge" },
                    "always": { "target": "Done" },
                    "on": { "OK": "Done", "BAD": { "target": "Nowhere" } }
                },
                "Done": { "type": "final" },
                "Orphan": { "on": { "*": "Done" } }
            }
        })json";
        const QJsonDocument sampleDocument = QJsonDocument::fromJson(QByteArray(sampleJson));
        const app::XStateImportResult imported = app::machineFromXStateJson(sampleDocument.object());
        if (!imported.ok) {
            std::fprintf(stderr, "FAIL: xstate import refused the Stately-style sample: %s\n",
                         qUtf8Printable(imported.error));
            return 1;
        }
        // Each still-deferred construct in the sample earns one diagnostic.
        const QStringList expectedFragments{
            // `context` imports for real; this is the type-inference notice a
            // file with no meta.ordo schema earns.
            QStringLiteral("context: no meta.ordo.context schema"),
            QStringLiteral("meta.stately"),                      // foreign meta content
            QStringLiteral("states.Cart.on.DOWN.target"),        // '.'-relative target: not a sibling, not unique
            QStringLiteral("states.Cart.on.CHECKOUT.cond"),      // v4 spelling
            QStringLiteral("states.Cart.after.SLOW"),            // named delays deferred
            QStringLiteral("states.Browsing.on.ADD.actions[1]"), // single action slot
            QStringLiteral("states.Paying.on.BAD.target: no state is named 'Nowhere'"),
        };
        for (const QString& fragment : expectedFragments) {
            bool found = false;
            for (const QString& line : imported.diagnostics) {
                found = found || line.contains(fragment);
            }
            if (!found) {
                std::fprintf(stderr, "FAIL: sample import diagnostics miss '%s'; got:\n",
                             qUtf8Printable(fragment));
                for (const QString& line : imported.diagnostics) {
                    std::fprintf(stderr, "  - %s\n", qUtf8Printable(line));
                }
                return 1;
            }
        }
        // The mappable rest: 6 states (including Cart's nested Empty), 9
        // transitions (root RESET, ADD, HELP targetless, after-30000, CHECKOUT,
        // OK, JUMP, Paying always, and Orphan's '*' wildcard transition to Done).
        const app::Machine& sample = imported.machine;
        if (sample.name != QStringLiteral("Order") || sample.states.size() != 6 ||
            sample.transitions.size() != 9) {
            std::fprintf(stderr, "FAIL: sample import holds %d states / %d transitions (want 6 / 9)\n",
                         static_cast<int>(sample.states.size()), static_cast<int>(sample.transitions.size()));
            return 1;
        }
        QHash<QString, const app::State*> stateByName;
        for (const app::State& state : sample.states) {
            stateByName.insert(state.name, &state);
        }
        if (sample.initialStateId != stateByName.value(QStringLiteral("Browsing"))->id ||
            stateByName.value(QStringLiteral("Done"))->kind != app::StateKind::Final ||
            stateByName.value(QStringLiteral("Browsing"))->entryActions != QStringList{QStringLiteral("greet")} ||
            stateByName.value(QStringLiteral("Browsing"))->exitActions != QStringList{QStringLiteral("farewell")} ||
            stateByName.value(QStringLiteral("Paying"))->tags != QStringList{QStringLiteral("beta")}) {
            std::fprintf(stderr, "FAIL: sample import initial/kind/entry/exit/tags mapping mismatch\n");
            return 1;
        }
        // Cart's nested compound: Empty is parented under Cart and Cart.initial
        // resolved to it.
        if (stateByName.value(QStringLiteral("Empty"))->parentId != stateByName.value(QStringLiteral("Cart"))->id ||
            stateByName.value(QStringLiteral("Cart"))->initialChildId !=
                stateByName.value(QStringLiteral("Empty"))->id) {
            std::fprintf(stderr, "FAIL: sample import did not parent Empty under Cart with Cart.initial resolved\n");
            return 1;
        }
        // `"invoke": { "src": "charge" }` imports. No `id` in the sample, so
        // invokeId stays empty (effectiveInvokeId falls back to invokeSrc).
        if (stateByName.value(QStringLiteral("Paying"))->invokeSrc != QStringLiteral("charge") ||
            !stateByName.value(QStringLiteral("Paying"))->invokeId.isEmpty()) {
            std::fprintf(stderr, "FAIL: sample import did not read Paying.invoke.src as 'charge'\n");
            return 1;
        }
        // `"context": { "items": 0 }`: a foreign file carries no meta.ordo
        // schema, so the type is inferred from the value (integral number ->
        // Int) and the id comes after every state and transition has minted.
        if (sample.context.size() != 1 || sample.context.first().name != QStringLiteral("items") ||
            sample.context.first().type != app::ContextType::Int ||
            sample.context.first().initialValue != QStringLiteral("0") || sample.context.first().id == 0) {
            std::fprintf(stderr, "FAIL: sample import did not infer `context.items` as an Int initialised to 0\n");
            return 1;
        }
        bool foundAdd = false;
        bool foundHelp = false;
        bool foundCheckout = false;
        bool foundDelayed = false;
        bool foundRootReset = false;
        bool foundJump = false;
        bool foundAlways = false;
        bool foundOrphanWildcard = false;
        for (const app::Transition& transition : sample.transitions) {
            if (transition.event == QStringLiteral("RESET")) {
                // Root `on` (string-shorthand target) -> a from==0 transition.
                foundRootReset = transition.from == 0 &&
                                 transition.to == stateByName.value(QStringLiteral("Browsing"))->id;
            } else if (transition.event == QStringLiteral("ADD")) {
                // Guard object form { "type": "hasRoom" } reads its type; the
                // actions array's FIRST entry lands, the second was diagnosed.
                foundAdd = transition.to == stateByName.value(QStringLiteral("Cart"))->id &&
                           transition.guard == QStringLiteral("hasRoom") &&
                           transition.action == QStringLiteral("track");
            } else if (transition.event == QStringLiteral("HELP")) {
                foundHelp = transition.to == 0 && transition.action == QStringLiteral("showHelp");
            } else if (transition.event == QStringLiteral("CHECKOUT")) {
                // cond is v4 -- diagnosed and DROPPED, so the guard stays empty.
                foundCheckout = transition.to == stateByName.value(QStringLiteral("Paying"))->id &&
                                transition.guard.isEmpty();
            } else if (transition.event == QStringLiteral("JUMP")) {
                // '#Order.Paying' (resolveTarget() rule b) must resolve rather
                // than be dropped.
                foundJump = transition.to == stateByName.value(QStringLiteral("Paying"))->id;
            } else if (transition.event == QStringLiteral("*")) {
                foundOrphanWildcard = transition.from == stateByName.value(QStringLiteral("Orphan"))->id &&
                                      transition.to == stateByName.value(QStringLiteral("Done"))->id;
            } else if (transition.event.isEmpty()) {
                if (transition.isAlways()) {
                    foundAlways = transition.from == stateByName.value(QStringLiteral("Paying"))->id &&
                                  transition.to == stateByName.value(QStringLiteral("Done"))->id;
                } else {
                    foundDelayed = transition.delayMs == 30000 &&
                                   transition.to == stateByName.value(QStringLiteral("Browsing"))->id;
                }
            }
        }
        if (!foundAdd || !foundHelp || !foundCheckout || !foundDelayed || !foundRootReset || !foundJump || !foundAlways || !foundOrphanWildcard) {
            std::fprintf(
                stderr,
                "FAIL: sample import transition mapping mismatch (ADD/HELP/CHECKOUT/after/root RESET/JUMP/always/wildcard)\n");
            return 1;
        }
        // This sample carries no meta.ordo, so importers never guess a layout:
        // every state stays at the (0,0) origin and machineHasNoGeometry() is
        // true. DocumentSession lays such a machine out when it opens.
        for (const app::State& state : sample.states) {
            if (state.pos != QPointF(0, 0)) {
                std::fprintf(stderr, "FAIL: coordinate-free sample import left '%s' off the origin\n",
                             qUtf8Printable(state.name));
                return 1;
            }
        }
        if (!app::machineHasNoGeometry(sample)) {
            std::fprintf(stderr, "FAIL: machineHasNoGeometry() is false for a coordinate-free import\n");
            return 1;
        }
    }

    // ---- mixed geometry: some states carry meta.ordo, others don't ------------
    // A mixed file keeps what it has: states without geometry land at (0,0)
    // and each earns a diagnostic; machineHasNoGeometry() is false because at
    // least one state carries geometry.
    {
        const char* mixedJson = R"json({
            "id": "Mixed",
            "initial": "A",
            "states": {
                "A": { "meta": { "ordo": { "x": 40, "y": 60 } }, "on": { "NEXT": "B" } },
                "B": {}
            }
        })json";
        const QJsonDocument mixedDocument = QJsonDocument::fromJson(QByteArray(mixedJson));
        const app::XStateImportResult mixed = app::machineFromXStateJson(mixedDocument.object());
        if (!mixed.ok) {
            std::fprintf(stderr, "FAIL: xstate import refused the mixed-geometry sample: %s\n",
                         qUtf8Printable(mixed.error));
            return 1;
        }
        QHash<QString, const app::State*> mixedByName;
        for (const app::State& state : mixed.machine.states) {
            mixedByName.insert(state.name, &state);
        }
        if (mixedByName.value(QStringLiteral("A"))->pos != QPointF(40, 60) ||
            mixedByName.value(QStringLiteral("B"))->pos != QPointF(0, 0)) {
            std::fprintf(stderr, "FAIL: mixed-geometry import did not keep A's position / place B at the origin\n");
            return 1;
        }
        if (app::machineHasNoGeometry(mixed.machine)) {
            std::fprintf(stderr, "FAIL: machineHasNoGeometry() is true for a mixed-geometry machine\n");
            return 1;
        }
        bool foundGapDiagnostic = false;
        for (const QString& line : mixed.diagnostics) {
            foundGapDiagnostic = foundGapDiagnostic || line.contains(QStringLiteral("states.B.meta.ordo"));
        }
        if (!foundGapDiagnostic) {
            std::fprintf(stderr, "FAIL: mixed-geometry import did not diagnose B's missing geometry\n");
            return 1;
        }
    }

    // ---- structural failure: only a missing `states` object refuses -----------
    {
        const app::XStateImportResult refused = app::machineFromXStateJson(QJsonObject());
        if (refused.ok || !refused.error.contains(QStringLiteral("states"))) {
            std::fprintf(stderr, "FAIL: xstate import accepted JSON with no `states` object\n");
            return 1;
        }
    }

    // ---- wildcard bidirectional round trip -----------------------------------
    {
        app::Machine wm;
        wm.name = QStringLiteral("WildcardInterop");
        app::State s1;
        s1.id = 1;
        s1.name = QStringLiteral("Active");
        app::State s2;
        s2.id = 2;
        s2.name = QStringLiteral("Handled");
        wm.states = {s1, s2};
        wm.initialStateId = 1;
        wm.nextId = 3;

        // Exact, Prefix.*, and Universal * transitions
        app::Transition t1;
        t1.id = 10;
        t1.from = 1;
        t1.to = 2;
        t1.event = QStringLiteral("mouse.click");
        app::Transition t2;
        t2.id = 11;
        t2.from = 1;
        t2.to = 2;
        t2.event = QStringLiteral("mouse.*");
        app::Transition t3;
        t3.id = 12;
        t3.from = 1;
        t3.to = 2;
        t3.event = QStringLiteral("*");
        wm.transitions = {t1, t2, t3};

        const app::XStateExportResult first = app::machineToXStateJson(wm);
        if (!first.ok) {
            std::fprintf(stderr, "FAIL: wildcard machine export failed: %s\n", qUtf8Printable(first.error));
            return 1;
        }
        const app::XStateImportResult imported = app::machineFromXStateJson(first.json);
        if (!imported.ok) {
            std::fprintf(stderr, "FAIL: wildcard machine import failed: %s\n", qUtf8Printable(imported.error));
            return 1;
        }
        if (imported.machine.transitions.size() != 3) {
            std::fprintf(stderr, "FAIL: wildcard machine reimport lost transitions (got %d, want 3)\n",
                         static_cast<int>(imported.machine.transitions.size()));
            return 1;
        }
        QSet<QString> reimportedEvents;
        for (const app::Transition& t : imported.machine.transitions) {
            reimportedEvents.insert(t.event);
        }
        if (!reimportedEvents.contains(QStringLiteral("mouse.click")) ||
            !reimportedEvents.contains(QStringLiteral("mouse.*")) ||
            !reimportedEvents.contains(QStringLiteral("*"))) {
            std::fprintf(stderr, "FAIL: wildcard machine reimport missed expected event descriptors\n");
            return 1;
        }
        const app::XStateExportResult second = app::machineToXStateJson(imported.machine);
        if (!second.ok || second.json != first.json) {
            std::fprintf(stderr, "FAIL: wildcard machine export idempotence failed -- toJson(fromJson(toJson(M))) != toJson(M)\n");
            return 1;
        }
    }

    // ---- nested machine ----
    // Compound P{A,B}, A's cross-boundary transition to X (resolves on reimport
    // only via resolveTarget()'s rule (c), X being globally unique), A's SIB
    // transition to B, and a Parallel state Fork with regions R1/R2. Export
    // nests children under their parent's `states`; import reproduces the
    // machine by name; export -> import -> export is byte-identical.
    {
        app::Machine nested;
        nested.name = QStringLiteral("Nested");
        nested.states.push_back(app::State{.id = 1,
                                           .name = QStringLiteral("P"),
                                           .kind = app::StateKind::Normal,
                                           .pos = QPointF(0, 0),
                                           .initialChildId = 2});
        nested.states.push_back(app::State{
            .id = 2, .name = QStringLiteral("A"), .kind = app::StateKind::Normal, .pos = QPointF(0, 150), .parentId = 1});
        nested.states.push_back(app::State{.id = 3,
                                           .name = QStringLiteral("B"),
                                           .kind = app::StateKind::Normal,
                                           .pos = QPointF(150, 150),
                                           .parentId = 1});
        nested.states.push_back(
            app::State{.id = 4, .name = QStringLiteral("X"), .kind = app::StateKind::Normal, .pos = QPointF(300, 0)});
        nested.states.push_back(app::State{.id = 5,
                                           .name = QStringLiteral("Fork"),
                                           .kind = app::StateKind::Parallel,
                                           .pos = QPointF(600, 0),
                                           .initialChildId = 6});
        nested.states.push_back(app::State{.id = 6,
                                           .name = QStringLiteral("R1"),
                                           .kind = app::StateKind::Normal,
                                           .pos = QPointF(600, 150),
                                           .parentId = 5});
        nested.states.push_back(app::State{.id = 7,
                                           .name = QStringLiteral("R2"),
                                           .kind = app::StateKind::Normal,
                                           .pos = QPointF(750, 150),
                                           .parentId = 5});
        nested.transitions.push_back(app::Transition{.id = 8, .from = 2, .to = 4, .event = QStringLiteral("OUT")});
        nested.transitions.push_back(app::Transition{.id = 9, .from = 2, .to = 3, .event = QStringLiteral("SIB")});
        nested.nextId = 10;
        nested.initialStateId = 1;

        const app::XStateExportResult exported = app::machineToXStateJson(nested);
        if (!exported.ok) {
            std::fprintf(stderr, "FAIL: nested xstate export refused a well-formed hierarchical machine: %s\n",
                         qUtf8Printable(exported.error));
            return 1;
        }
        const QJsonObject rootStates = exported.json.value(QStringLiteral("states")).toObject();
        if (rootStates.size() != 3 || !rootStates.contains(QStringLiteral("P")) ||
            !rootStates.contains(QStringLiteral("X")) || !rootStates.contains(QStringLiteral("Fork"))) {
            std::fprintf(stderr,
                         "FAIL: nested xstate export's machine root should hold exactly P/X/Fork (A/B/R1/R2 "
                         "nested away)\n");
            return 1;
        }
        const QJsonObject pJson = rootStates.value(QStringLiteral("P")).toObject();
        const QJsonObject pStates = pJson.value(QStringLiteral("states")).toObject();
        if (pJson.value(QStringLiteral("initial")).toString() != QStringLiteral("A") || pStates.size() != 2 ||
            !pStates.contains(QStringLiteral("A")) || !pStates.contains(QStringLiteral("B"))) {
            std::fprintf(stderr, "FAIL: nested xstate export did not nest P{A,B} with initial=A\n");
            return 1;
        }
        const QJsonObject aOn = pStates.value(QStringLiteral("A")).toObject().value(QStringLiteral("on")).toObject();
        if (aOn.value(QStringLiteral("OUT")).toObject().value(QStringLiteral("target")).toString() !=
                QStringLiteral("X") ||
            aOn.value(QStringLiteral("SIB")).toObject().value(QStringLiteral("target")).toString() !=
                QStringLiteral("B")) {
            std::fprintf(stderr, "FAIL: nested xstate export: A's cross-boundary/sibling transitions mismatch\n");
            return 1;
        }
        const QJsonObject forkJson = rootStates.value(QStringLiteral("Fork")).toObject();
        const QJsonObject forkStates = forkJson.value(QStringLiteral("states")).toObject();
        if (forkJson.value(QStringLiteral("type")).toString() != QStringLiteral("parallel") ||
            forkJson.value(QStringLiteral("initial")).toString() != QStringLiteral("R1") || forkStates.size() != 2 ||
            !forkStates.contains(QStringLiteral("R1")) || !forkStates.contains(QStringLiteral("R2"))) {
            std::fprintf(stderr, "FAIL: nested xstate export did not nest Fork{R1,R2} as a parallel state\n");
            return 1;
        }

        const app::XStateImportResult reimported = app::machineFromXStateJson(exported.json);
        if (!reimported.ok || !reimported.diagnostics.isEmpty()) {
            std::fprintf(stderr, "FAIL: reimporting the nested export was not clean (ok=%d, %d diagnostic(s)):\n",
                         reimported.ok ? 1 : 0, static_cast<int>(reimported.diagnostics.size()));
            for (const QString& line : reimported.diagnostics) {
                std::fprintf(stderr, "  - %s\n", qUtf8Printable(line));
            }
            return 1;
        }
        QHash<QString, const app::State*> byName;
        for (const app::State& state : reimported.machine.states) {
            byName.insert(state.name, &state);
        }
        if (reimported.machine.initialStateId != byName.value(QStringLiteral("P"))->id ||
            byName.value(QStringLiteral("A"))->parentId != byName.value(QStringLiteral("P"))->id ||
            byName.value(QStringLiteral("B"))->parentId != byName.value(QStringLiteral("P"))->id ||
            byName.value(QStringLiteral("P"))->initialChildId != byName.value(QStringLiteral("A"))->id ||
            byName.value(QStringLiteral("R1"))->parentId != byName.value(QStringLiteral("Fork"))->id ||
            byName.value(QStringLiteral("R2"))->parentId != byName.value(QStringLiteral("Fork"))->id ||
            byName.value(QStringLiteral("Fork"))->initialChildId != byName.value(QStringLiteral("R1"))->id ||
            byName.value(QStringLiteral("P"))->parentId != 0 || byName.value(QStringLiteral("X"))->parentId != 0 ||
            byName.value(QStringLiteral("Fork"))->parentId != 0) {
            std::fprintf(stderr,
                         "FAIL: reimported nested machine's initial/parentId/initialChildId do not match the "
                         "original\n");
            return 1;
        }
        bool reimportedOut = false;
        bool reimportedSib = false;
        for (const app::Transition& transition : reimported.machine.transitions) {
            if (transition.event == QStringLiteral("OUT")) {
                reimportedOut = transition.from == byName.value(QStringLiteral("A"))->id &&
                                transition.to == byName.value(QStringLiteral("X"))->id;
            } else if (transition.event == QStringLiteral("SIB")) {
                reimportedSib = transition.from == byName.value(QStringLiteral("A"))->id &&
                                transition.to == byName.value(QStringLiteral("B"))->id;
            }
        }
        if (!reimportedOut || !reimportedSib) {
            std::fprintf(stderr, "FAIL: reimported nested machine's OUT/SIB transitions do not match the original\n");
            return 1;
        }

        const app::XStateExportResult reexported = app::machineToXStateJson(reimported.machine);
        if (!reexported.ok || reexported.json != exported.json) {
            std::fprintf(stderr,
                         "FAIL: nested export is not idempotent -- toJson(fromJson(toJson(M))) != toJson(M)\n");
            return 1;
        }
    }

    // ---- unresolvable target inside a nested state: diagnose-and-drop, ok stays true (rule d) ----
    {
        const char* unresolvableJson = R"json({
            "id": "Unresolvable",
            "initial": "P",
            "states": {
                "P": {
                    "initial": "A",
                    "states": {
                        "A": { "on": { "OUT": { "target": "NoSuchState" } } }
                    }
                }
            }
        })json";
        const QJsonDocument document = QJsonDocument::fromJson(QByteArray(unresolvableJson));
        const app::XStateImportResult imported = app::machineFromXStateJson(document.object());
        if (!imported.ok) {
            std::fprintf(stderr,
                         "FAIL: an unresolvable nested target structurally refused the import (ok should stay "
                         "true -- partial-import law)\n");
            return 1;
        }
        bool foundDropLine = false;
        for (const QString& line : imported.diagnostics) {
            foundDropLine = foundDropLine || (line.contains(QStringLiteral("states.P.states.A.on.OUT.target")) &&
                                               line.contains(QStringLiteral("NoSuchState")) &&
                                               line.contains(QStringLiteral("dropped")));
        }
        if (!foundDropLine) {
            std::fprintf(stderr,
                         "FAIL: an unresolvable nested target did not produce a diagnose-and-drop line with its "
                         "JSON path; got:\n");
            for (const QString& line : imported.diagnostics) {
                std::fprintf(stderr, "  - %s\n", qUtf8Printable(line));
            }
            return 1;
        }
        // Partial import (DESIGN decision 3): P and A still mint, OUT is
        // simply absent -- everything ELSE in the file still imports.
        if (imported.machine.states.size() != 2 || !imported.machine.transitions.isEmpty()) {
            std::fprintf(stderr,
                         "FAIL: an unresolvable nested target should drop ONLY that transition (want 2 states, 0 "
                         "transitions)\n");
            return 1;
        }
    }

    // ---- context + expression guards + assigns ----
    // One machine carries all three mappings so they are proven to cross
    // together: four typed context variables in non-alphabetical document
    // order, an expression guard over three of them, a bare-identifier guard
    // beside it that must stay a hook, an assign on a transition, and an
    // assign plus a named hook in one entry list.
    {
        app::Machine ctx;
        ctx.name = QStringLiteral("Wallet");
        ctx.states.push_back(app::State{.id = 1,
                                        .name = QStringLiteral("Idle"),
                                        .kind = app::StateKind::Normal,
                                        .pos = QPointF(0, 0),
                                        // Assign and hook in one list: classified per entry.
                                        .entryActions = {QStringLiteral("count = -1"),
                                                          QStringLiteral("beginIdle")}});
        ctx.states.push_back(app::State{
            .id = 2, .name = QStringLiteral("Spending"), .kind = app::StateKind::Normal, .pos = QPointF(260, 0)});
        // Document order is non-alphabetical (QJsonObject keys are sorted), which
        // proves meta.ordo carries the order. `ratio` is a Double initialised to
        // "0", the same JSON number an Int 0 emits, which proves it carries the type.
        ctx.context.push_back(app::ContextVariable{
            .id = 3, .name = QStringLiteral("role"), .type = app::ContextType::String, .initialValue = QStringLiteral("admin")});
        ctx.context.push_back(app::ContextVariable{
            .id = 4, .name = QStringLiteral("count"), .type = app::ContextType::Int, .initialValue = QStringLiteral("0")});
        ctx.context.push_back(app::ContextVariable{
            .id = 5, .name = QStringLiteral("ratio"), .type = app::ContextType::Double, .initialValue = QStringLiteral("0")});
        ctx.context.push_back(app::ContextVariable{
            .id = 6, .name = QStringLiteral("locked"), .type = app::ContextType::Bool, .initialValue = QStringLiteral("false")});
        ctx.transitions.push_back(app::Transition{.id = 7,
                                                  .from = 1,
                                                  .to = 2,
                                                  .event = QStringLiteral("SPEND"),
                                                  .guard = QStringLiteral("count > 3 && !locked"),
                                                  .action = QStringLiteral("count = 7")});
        // The compatibility pair: a bare-identifier guard and a named action
        // hook, which must cross as the plain v5 strings they always did.
        ctx.transitions.push_back(app::Transition{.id = 8,
                                                  .from = 2,
                                                  .to = 1,
                                                  .event = QStringLiteral("STOP"),
                                                  .guard = QStringLiteral("canStop"),
                                                  .action = QStringLiteral("logStop")});
        ctx.transitions.push_back(app::Transition{.id = 9,
                                                  .from = 1,
                                                  .to = 1,
                                                  .event = QStringLiteral("TWEAK"),
                                                  .guard = QStringLiteral("!(role == 'admin') || ratio >= 0.5"),
                                                  .action = QStringLiteral("role = 'guest'")});
        ctx.nextId = 10;
        ctx.initialStateId = 1;

        const app::XStateExportResult exported = app::machineToXStateJson(ctx);
        if (!exported.ok || !exported.diagnostics.isEmpty()) {
            std::fprintf(stderr, "FAIL: context export refused or diagnosed a well-formed machine: %s\n",
                         qUtf8Printable(exported.error));
            for (const QString& line : exported.diagnostics) {
                std::fprintf(stderr, "  - %s\n", qUtf8Printable(line));
            }
            return 1;
        }

        // ---- `context`: v5's own top-level object, typed per variable -------
        const QJsonObject contextJson = exported.json.value(QStringLiteral("context")).toObject();
        if (contextJson.size() != 4 || !contextJson.value(QStringLiteral("locked")).isBool() ||
            contextJson.value(QStringLiteral("locked")).toBool() ||
            !contextJson.value(QStringLiteral("role")).isString() ||
            contextJson.value(QStringLiteral("role")).toString() != QStringLiteral("admin") ||
            contextJson.value(QStringLiteral("count")).toInt() != 0 ||
            contextJson.value(QStringLiteral("ratio")).toDouble() != 0.0) {
            std::fprintf(stderr, "FAIL: context export did not emit four typed JSON values\n");
            return 1;
        }
        // ---- meta.ordo carries what the values cannot: type and order ------
        const QJsonArray schema = exported.json.value(QStringLiteral("meta"))
                                      .toObject()
                                      .value(QStringLiteral("ordo"))
                                      .toObject()
                                      .value(QStringLiteral("context"))
                                      .toArray();
        const QStringList wantNames{QStringLiteral("role"), QStringLiteral("count"), QStringLiteral("ratio"),
                                    QStringLiteral("locked")};
        const QStringList wantTypes{QStringLiteral("String"), QStringLiteral("Int"), QStringLiteral("Double"),
                                    QStringLiteral("Bool")};
        if (schema.size() != wantNames.size()) {
            std::fprintf(stderr, "FAIL: meta.ordo.context holds %d entries (want 4)\n",
                         static_cast<int>(schema.size()));
            return 1;
        }
        for (int i = 0; i < schema.size(); ++i) {
            const QJsonObject entry = schema.at(i).toObject();
            if (entry.value(QStringLiteral("name")).toString() != wantNames.at(i) ||
                entry.value(QStringLiteral("type")).toString() != wantTypes.at(i)) {
                std::fprintf(stderr, "FAIL: meta.ordo.context[%d] is %s/%s (want %s/%s)\n", i,
                             qUtf8Printable(entry.value(QStringLiteral("name")).toString()),
                             qUtf8Printable(entry.value(QStringLiteral("type")).toString()),
                             qUtf8Printable(wantNames.at(i)), qUtf8Printable(wantTypes.at(i)));
                return 1;
            }
        }

        // ---- the structural guard form -------------------------------------
        const QJsonObject idleJson = exported.json.value(QStringLiteral("states")).toObject()
                                         .value(QStringLiteral("Idle")).toObject();
        const QJsonObject spendJson = idleJson.value(QStringLiteral("on")).toObject()
                                          .value(QStringLiteral("SPEND")).toObject();
        const QJsonObject spendGuard = spendJson.value(QStringLiteral("guard")).toObject();
        const QJsonArray spendOperands = spendGuard.value(QStringLiteral("guards")).toArray();
        const QJsonObject comparison = spendOperands.at(0).toObject();
        if (spendGuard.value(QStringLiteral("type")).toString() != QStringLiteral("and") ||
            spendOperands.size() != 2 ||
            comparison.value(QStringLiteral("type")).toString() != QStringLiteral(">") ||
            comparison.value(QStringLiteral("left")).toObject().value(QStringLiteral("type")).toString() !=
                QStringLiteral("context") ||
            comparison.value(QStringLiteral("left")).toObject().value(QStringLiteral("name")).toString() !=
                QStringLiteral("count") ||
            comparison.value(QStringLiteral("right")).toInt() != 3 ||
            spendOperands.at(1).toObject().value(QStringLiteral("type")).toString() != QStringLiteral("not")) {
            std::fprintf(stderr, "FAIL: `count > 3 && !locked` did not export as and[>(context.count, 3), not[...]]\n");
            return 1;
        }
        // ---- the assign form, on a transition and in an entry list ---------
        const QJsonObject spendAssign = spendJson.value(QStringLiteral("actions")).toArray().at(0).toObject();
        if (spendAssign.value(QStringLiteral("type")).toString() != QStringLiteral("xstate.assign") ||
            spendAssign.value(QStringLiteral("assignment")).toObject().value(QStringLiteral("count")).toInt() != 7) {
            std::fprintf(stderr, "FAIL: `count = 7` did not export as an xstate.assign action\n");
            return 1;
        }
        const QJsonArray entryJson = idleJson.value(QStringLiteral("entry")).toArray();
        const QJsonObject entryAssign = entryJson.at(0).toObject();
        if (entryJson.size() != 2 ||
            entryAssign.value(QStringLiteral("type")).toString() != QStringLiteral("xstate.assign") ||
            entryAssign.value(QStringLiteral("assignment")).toObject().value(QStringLiteral("count")).toObject()
                .value(QStringLiteral("type")).toString() != QStringLiteral("-") ||
            !entryJson.at(1).isString() || entryJson.at(1).toString() != QStringLiteral("beginIdle")) {
            std::fprintf(stderr, "FAIL: an entry list did not export as [assign, plain hook name]\n");
            return 1;
        }
        // ---- the compatibility pair: still plain v5 name strings -----------
        const QJsonObject stopJson = exported.json.value(QStringLiteral("states")).toObject()
                                         .value(QStringLiteral("Spending")).toObject()
                                         .value(QStringLiteral("on")).toObject()
                                         .value(QStringLiteral("STOP")).toObject();
        if (!stopJson.value(QStringLiteral("guard")).isString() ||
            stopJson.value(QStringLiteral("guard")).toString() != QStringLiteral("canStop") ||
            stopJson.value(QStringLiteral("actions")).toArray() != QJsonArray{QStringLiteral("logStop")}) {
            std::fprintf(stderr, "FAIL: a bare-identifier guard / named action hook did not stay a plain string\n");
            return 1;
        }

        // ---- a blank or duplicated context name -----------------------------
        // XState keys context by name, so the object can hold neither. Both are
        // validator Errors in-app, so export emits a diagnostic (not a refusal,
        // unlike a duplicate state name) and still carries everything it can.
        {
            app::Machine dirty = ctx;
            dirty.context[1].name = QStringLiteral("   ");
            dirty.context[3].name = QStringLiteral("role");
            const app::XStateExportResult noisy = app::machineToXStateJson(dirty);
            if (!noisy.ok || noisy.diagnostics.filter(QStringLiteral("blank name")).size() != 1 ||
                noisy.diagnostics.filter(QStringLiteral("share the name 'role'")).size() != 1 ||
                noisy.json.value(QStringLiteral("context")).toObject().size() != 2) {
                std::fprintf(stderr,
                             "FAIL: a blank/duplicated context name should be diagnosed and dropped, leaving the "
                             "other two variables (got %d diagnostic(s))\n",
                             static_cast<int>(noisy.diagnostics.size()));
                return 1;
            }
        }

        // ---- import: everything back, in document order ---------------------
        const app::XStateImportResult imported = app::machineFromXStateJson(exported.json);
        if (!imported.ok || !imported.diagnostics.isEmpty()) {
            std::fprintf(stderr, "FAIL: importing the context export was not clean (ok=%d, %d diagnostic(s)):\n",
                         imported.ok ? 1 : 0, static_cast<int>(imported.diagnostics.size()));
            for (const QString& line : imported.diagnostics) {
                std::fprintf(stderr, "  - %s\n", qUtf8Printable(line));
            }
            return 1;
        }
        if (imported.machine.context.size() != ctx.context.size()) {
            std::fprintf(stderr, "FAIL: the reimported machine holds %d context variables (want %d)\n",
                         static_cast<int>(imported.machine.context.size()),
                         static_cast<int>(ctx.context.size()));
            return 1;
        }
        for (int i = 0; i < ctx.context.size(); ++i) {
            const app::ContextVariable& before = ctx.context.at(i);
            const app::ContextVariable& after = imported.machine.context.at(i);
            // Everything but the id, which is re-minted like every other id
            // across this boundary (DESIGN decision 1).
            if (before.name != after.name || before.type != after.type ||
                before.initialValue != after.initialValue || after.id == 0) {
                std::fprintf(stderr,
                             "FAIL: context variable %d came back as %s/%d/'%s' (want %s/%d/'%s')\n", i,
                             qUtf8Printable(after.name), static_cast<int>(after.type),
                             qUtf8Printable(after.initialValue), qUtf8Printable(before.name),
                             static_cast<int>(before.type), qUtf8Printable(before.initialValue));
                return 1;
            }
        }

        // ---- idempotent at the AST level ---------------------------------
        // Guards/assigns are compared as trees (parse -> print on both sides):
        // print() may respace the strings, but not change the trees.
        const auto sameExpression = [](const QString& left, const QString& right) {
            const app::expr::ParseResult leftAst = app::expr::parse(left);
            const app::expr::ParseResult rightAst = app::expr::parse(right);
            return leftAst.ok && rightAst.ok && app::expr::print(leftAst.ast) == app::expr::print(rightAst.ast);
        };
        for (const app::Transition& before : ctx.transitions) {
            const app::Transition* after = nullptr;
            for (const app::Transition& candidate : imported.machine.transitions) {
                if (candidate.event == before.event) {
                    after = &candidate;
                }
            }
            if (after == nullptr) {
                std::fprintf(stderr, "FAIL: transition '%s' did not survive the round trip\n",
                             qUtf8Printable(before.event));
                return 1;
            }
            if (app::expr::isBareIdentifier(before.guard)) {
                // A hook name is not an expression, so it must come back
                // byte-identical, not reprinted.
                if (after->guard != before.guard) {
                    std::fprintf(stderr, "FAIL: hook guard '%s' came back as '%s'\n", qUtf8Printable(before.guard),
                                 qUtf8Printable(after->guard));
                    return 1;
                }
            } else if (!sameExpression(before.guard, after->guard)) {
                std::fprintf(stderr, "FAIL: expression guard '%s' came back as a different tree ('%s')\n",
                             qUtf8Printable(before.guard), qUtf8Printable(after->guard));
                return 1;
            }
            const app::expr::AssignForm beforeAssign = app::expr::parseAssignForm(before.action);
            const app::expr::AssignForm afterAssign = app::expr::parseAssignForm(after->action);
            if (beforeAssign.ok != afterAssign.ok) {
                std::fprintf(stderr, "FAIL: action '%s' changed class across the round trip ('%s')\n",
                             qUtf8Printable(before.action), qUtf8Printable(after->action));
                return 1;
            }
            if (beforeAssign.ok) {
                if (beforeAssign.target != afterAssign.target ||
                    !sameExpression(beforeAssign.valueSource, afterAssign.valueSource)) {
                    std::fprintf(stderr, "FAIL: assign '%s' came back as a different assign ('%s')\n",
                                 qUtf8Printable(before.action), qUtf8Printable(after->action));
                    return 1;
                }
            } else if (after->action != before.action) {
                std::fprintf(stderr, "FAIL: hook action '%s' came back as '%s'\n", qUtf8Printable(before.action),
                             qUtf8Printable(after->action));
                return 1;
            }
        }
        if (imported.machine.states.first().entryActions != ctx.states.first().entryActions) {
            std::fprintf(stderr, "FAIL: the [assign, hook] entry list did not come back verbatim\n");
            return 1;
        }
        const app::XStateExportResult second = app::machineToXStateJson(imported.machine);
        if (!second.ok || second.json != exported.json) {
            std::fprintf(stderr,
                         "FAIL: a context/expression/assign export is not idempotent -- "
                         "toJson(fromJson(toJson(M))) != toJson(M)\n");
            return 1;
        }

        // ---- a file from ANYWHERE ELSE: no meta.ordo schema at all ----------
        // Types are inferred and the order falls back to the JSON object's own
        // keys; the machine still imports and round-trips. Inference cannot
        // recover `ratio` (a Double initialised to 0 reads back as Int), and the
        // importer notes that loss.
        QJsonObject foreign = exported.json;
        QJsonObject foreignMeta = foreign.value(QStringLiteral("meta")).toObject();
        QJsonObject foreignOrdo = foreignMeta.value(QStringLiteral("ordo")).toObject();
        foreignOrdo.remove(QStringLiteral("context"));
        foreignMeta[QStringLiteral("ordo")] = foreignOrdo;
        foreign[QStringLiteral("meta")] = foreignMeta;

        const app::XStateImportResult foreignImport = app::machineFromXStateJson(foreign);
        if (!foreignImport.ok || foreignImport.diagnostics.size() != 1 ||
            !foreignImport.diagnostics.first().contains(QStringLiteral("no meta.ordo.context schema"))) {
            std::fprintf(stderr, "FAIL: a schema-less context import should produce exactly one inference note:\n");
            for (const QString& line : foreignImport.diagnostics) {
                std::fprintf(stderr, "  - %s\n", qUtf8Printable(line));
            }
            return 1;
        }
        const QStringList inferredNames{QStringLiteral("count"), QStringLiteral("locked"), QStringLiteral("ratio"),
                                        QStringLiteral("role")};
        const QVector<app::ContextType> inferredTypes{app::ContextType::Int, app::ContextType::Bool,
                                                      app::ContextType::Int, app::ContextType::String};
        if (foreignImport.machine.context.size() != inferredNames.size()) {
            std::fprintf(stderr, "FAIL: a schema-less context import holds %d variables (want 4)\n",
                         static_cast<int>(foreignImport.machine.context.size()));
            return 1;
        }
        for (int i = 0; i < inferredNames.size(); ++i) {
            if (foreignImport.machine.context.at(i).name != inferredNames.at(i) ||
                foreignImport.machine.context.at(i).type != inferredTypes.at(i)) {
                std::fprintf(stderr, "FAIL: schema-less inference gave %s/%d at %d (want %s/%d)\n",
                             qUtf8Printable(foreignImport.machine.context.at(i).name),
                             static_cast<int>(foreignImport.machine.context.at(i).type), i,
                             qUtf8Printable(inferredNames.at(i)), static_cast<int>(inferredTypes.at(i)));
                return 1;
            }
        }
        const app::XStateExportResult foreignExport = app::machineToXStateJson(foreignImport.machine);
        if (!foreignExport.ok ||
            foreignExport.json.value(QStringLiteral("context")) !=
                exported.json.value(QStringLiteral("context"))) {
            std::fprintf(stderr, "FAIL: a schema-less import did not re-export the same `context` values\n");
            return 1;
        }
        const app::XStateImportResult foreignAgain = app::machineFromXStateJson(foreignExport.json);
        const app::XStateExportResult foreignAgainExport = app::machineToXStateJson(foreignAgain.machine);
        if (!foreignAgainExport.ok || foreignAgainExport.json != foreignExport.json) {
            std::fprintf(stderr, "FAIL: a schema-less import's own export is not idempotent from there on\n");
            return 1;
        }
    }

    // ---- the four-scalar boundary, and the one guard shape with no Ordo
    // spelling. An object/array/null context value is diagnosed and dropped
    // while the rest of the file still imports. A guard that is a bare context
    // reference is dropped too: a lone identifier in a guard string names a
    // hook here, so importing it verbatim would change its meaning.
    {
        const char* narrowJson = R"json({
            "id": "Narrow",
            "initial": "S",
            "context": { "flag": true, "nested": { "a": 1 }, "list": [1, 2], "nothing": null },
            "states": {
                "S": {
                    "on": {
                        "GO": { "guard": { "type": "context", "name": "flag" } },
                        "TEXT": { "guard": "flag == true" }
                    }
                }
            }
        })json";
        const QJsonDocument document = QJsonDocument::fromJson(QByteArray(narrowJson));
        const app::XStateImportResult imported = app::machineFromXStateJson(document.object());
        if (!imported.ok) {
            std::fprintf(stderr, "FAIL: an object-valued context structurally refused the import (partial-import law)\n");
            return 1;
        }
        const QStringList expectedNarrow{QStringLiteral("context.list"),
                                          QStringLiteral("context.nothing"),
                                          QStringLiteral("a bare context reference cannot be a whole guard")};
        for (const QString& fragment : expectedNarrow) {
            if (imported.diagnostics.filter(fragment).isEmpty()) {
                std::fprintf(stderr, "FAIL: the narrowing import's diagnostics miss '%s'; got:\n",
                             qUtf8Printable(fragment));
                for (const QString& line : imported.diagnostics) {
                    std::fprintf(stderr, "  - %s\n", qUtf8Printable(line));
                }
                return 1;
            }
        }
        if (imported.machine.context.size() != 2 ||
            imported.machine.context.at(0).name != QStringLiteral("flag") ||
            imported.machine.context.at(0).type != app::ContextType::Bool ||
            imported.machine.context.at(0).initialValue != QStringLiteral("true") ||
            imported.machine.context.at(1).name != QStringLiteral("nested") ||
            imported.machine.context.at(1).type != app::ContextType::Object ||
            imported.machine.context.at(1).initialValue != QStringLiteral("{\"a\":1}")) {
            std::fprintf(stderr, "FAIL: mappable context values (Bool + nested Object) did not survive\n");
            return 1;
        }
        // GO's transition still imports (only its guard was dropped). TEXT covers
        // the other string shape: a guard string that is not a bare identifier is
        // already infix text and lands verbatim.
        const app::Transition* go = nullptr;
        const app::Transition* text = nullptr;
        for (const app::Transition& transition : imported.machine.transitions) {
            if (transition.event == QStringLiteral("GO")) {
                go = &transition;
            } else if (transition.event == QStringLiteral("TEXT")) {
                text = &transition;
            }
        }
        if (imported.machine.transitions.size() != 2 || go == nullptr || !go->guard.isEmpty() || text == nullptr ||
            text->guard != QStringLiteral("flag == true")) {
            std::fprintf(stderr,
                         "FAIL: a dropped structural guard should leave its transition guardless, and a plain "
                         "infix guard string should import verbatim\n");
            return 1;
        }
    }

    // ---- nested JSON object context bidirectional round-trip ---
    {
        app::Machine objMachine;
        objMachine.name = QStringLiteral("NestedContextRoundTrip");
        objMachine.states.push_back(app::State{.id = 1, .name = QStringLiteral("Active")});
        objMachine.initialStateId = 1;
        objMachine.context.push_back(app::ContextVariable{
            .id = 2,
            .name = QStringLiteral("user"),
            .type = app::ContextType::Object,
            .initialValue = QStringLiteral("{\"active\":true,\"age\":30,\"name\":\"Alice\"}")
        });
        objMachine.transitions.push_back(app::Transition{
            .id = 3,
            .from = 1,
            .to = 1,
            .event = QStringLiteral("CHECK"),
            .guard = QStringLiteral("user.active && user.age >= 18"),
            .action = QStringLiteral("user.age = user.age + 1")
        });
        objMachine.nextId = 4;

        const app::XStateExportResult exported = app::machineToXStateJson(objMachine);
        if (!exported.ok || !exported.diagnostics.isEmpty()) {
            std::fprintf(stderr, "FAIL: nested object context export failed: %s\n", qUtf8Printable(exported.error));
            return 1;
        }

        const QJsonObject contextJson = exported.json.value(QStringLiteral("context")).toObject();
        if (!contextJson.value(QStringLiteral("user")).isObject()) {
            std::fprintf(stderr, "FAIL: exported context.user is not a JSON object\n");
            return 1;
        }
        const QJsonObject userObj = contextJson.value(QStringLiteral("user")).toObject();
        if (userObj.value(QStringLiteral("name")).toString() != QStringLiteral("Alice") ||
            userObj.value(QStringLiteral("age")).toInt() != 30 ||
            !userObj.value(QStringLiteral("active")).toBool()) {
            std::fprintf(stderr, "FAIL: exported user object contents mismatched\n");
            return 1;
        }

        const app::XStateImportResult imported = app::machineFromXStateJson(exported.json);
        if (!imported.ok || !imported.diagnostics.isEmpty()) {
            std::fprintf(stderr, "FAIL: importing nested object context export failed:\n");
            for (const QString& d : imported.diagnostics) {
                std::fprintf(stderr, "  - %s\n", qUtf8Printable(d));
            }
            return 1;
        }

        if (imported.machine.context.size() != 1 ||
            imported.machine.context.first().name != QStringLiteral("user") ||
            imported.machine.context.first().type != app::ContextType::Object) {
            std::fprintf(stderr, "FAIL: imported machine did not restore ContextType::Object\n");
            return 1;
        }

        const app::XStateExportResult reExported = app::machineToXStateJson(imported.machine);
        if (!reExported.ok || reExported.json != exported.json) {
            std::fprintf(stderr, "FAIL: nested object context export -> import -> export was not byte-identical\n");
            return 1;
        }
    }

    // ---- invoke ----
    // "Loader": Loading invokes fetchUser with an explicit id and a non-default
    // invokeOutputType, with guards and assigns on both completion rows.
    // onDone/onError leave the state's `on` map for `invoke.onDone`/
    // `invoke.onError`; invokeOutputType is emitted only when non-default;
    // export -> import -> export is byte-identical.
    {
        app::Machine loader;
        loader.name = QStringLiteral("Loader");
        loader.states.push_back(
            app::State{.id = 1, .name = QStringLiteral("Idle"), .kind = app::StateKind::Normal, .pos = QPointF(0, 0)});
        loader.states.push_back(app::State{.id = 2,
                                           .name = QStringLiteral("Loading"),
                                           .kind = app::StateKind::Normal,
                                           .pos = QPointF(260, 0),
                                           .invokeSrc = QStringLiteral("fetchUser"),
                                           .invokeId = QStringLiteral("userLoader"),
                                           .invokeOutputType = app::ContextType::Double});
        loader.states.push_back(app::State{
            .id = 3, .name = QStringLiteral("Success"), .kind = app::StateKind::Normal, .pos = QPointF(520, 0)});
        loader.states.push_back(app::State{
            .id = 4, .name = QStringLiteral("Failure"), .kind = app::StateKind::Normal, .pos = QPointF(520, 150)});
        // A second invoking state with the default type (Int, unset) and no
        // explicit id: invokeOutputType is omitted and `id` derives from `src`
        // when invokeId is blank (effectiveInvokeId()).
        loader.states.push_back(app::State{.id = 5,
                                           .name = QStringLiteral("Pinging"),
                                           .kind = app::StateKind::Normal,
                                           .pos = QPointF(0, 150),
                                           .invokeSrc = QStringLiteral("ping")});
        loader.transitions.push_back(app::Transition{.id = 6, .from = 1, .to = 2, .event = QStringLiteral("GO")});
        loader.transitions.push_back(app::Transition{.id = 7,
                                                     .from = 2,
                                                     .to = 3,
                                                     .event = QStringLiteral("done.invoke.userLoader"),
                                                     .guard = QStringLiteral("output > 0"),
                                                     .action = QStringLiteral("result = output")});
        loader.transitions.push_back(app::Transition{.id = 8,
                                                     .from = 2,
                                                     .to = 4,
                                                     .event = QStringLiteral("error.platform.userLoader"),
                                                     .guard = QStringLiteral("error != ''"),
                                                     .action = QStringLiteral("lastError = error")});
        loader.nextId = 9;
        loader.initialStateId = 1;

        const app::XStateExportResult exported = app::machineToXStateJson(loader);
        if (!exported.ok || !exported.diagnostics.isEmpty()) {
            std::fprintf(stderr, "FAIL: invoke export refused or diagnosed a well-formed machine: %s\n",
                         qUtf8Printable(exported.error));
            for (const QString& line : exported.diagnostics) {
                std::fprintf(stderr, "  - %s\n", qUtf8Printable(line));
            }
            return 1;
        }
        const QJsonObject states = exported.json.value(QStringLiteral("states")).toObject();
        const QJsonObject loadingJson = states.value(QStringLiteral("Loading")).toObject();

        // ---- the lift: no leftover `on`, invoke.src/id present ------------
        if (loadingJson.contains(QStringLiteral("on"))) {
            std::fprintf(stderr, "FAIL: invoke export left onDone/onError behind in Loading's own `on` map\n");
            return 1;
        }
        const QJsonObject loadingInvoke = loadingJson.value(QStringLiteral("invoke")).toObject();
        if (loadingInvoke.value(QStringLiteral("src")).toString() != QStringLiteral("fetchUser") ||
            loadingInvoke.value(QStringLiteral("id")).toString() != QStringLiteral("userLoader")) {
            std::fprintf(stderr, "FAIL: invoke export src/id mismatch (want fetchUser/userLoader)\n");
            return 1;
        }

        // ---- onDone: structural guard (event.output > 0) + assign --------
        const QJsonObject onDone = loadingInvoke.value(QStringLiteral("onDone")).toObject();
        const QJsonObject onDoneGuard = onDone.value(QStringLiteral("guard")).toObject();
        const QJsonObject onDoneAssign = onDone.value(QStringLiteral("actions")).toArray().at(0).toObject();
        if (onDone.value(QStringLiteral("target")).toString() != QStringLiteral("Success") ||
            onDoneGuard.value(QStringLiteral("type")).toString() != QStringLiteral(">") ||
            onDoneGuard.value(QStringLiteral("left")).toObject().value(QStringLiteral("type")).toString() !=
                QStringLiteral("event") ||
            onDoneGuard.value(QStringLiteral("left")).toObject().value(QStringLiteral("name")).toString() !=
                QStringLiteral("output") ||
            onDoneGuard.value(QStringLiteral("right")).toInt() != 0 ||
            onDoneAssign.value(QStringLiteral("type")).toString() != QStringLiteral("xstate.assign") ||
            onDoneAssign.value(QStringLiteral("assignment")).toObject().value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("type")).toString() != QStringLiteral("event") ||
            onDoneAssign.value(QStringLiteral("assignment")).toObject().value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("name")).toString() != QStringLiteral("output")) {
            std::fprintf(stderr, "FAIL: invoke.onDone target/guard/assign mismatch\n");
            return 1;
        }

        // ---- onError: structural guard (event.error != '') + assign ------
        const QJsonObject onError = loadingInvoke.value(QStringLiteral("onError")).toObject();
        const QJsonObject onErrorGuard = onError.value(QStringLiteral("guard")).toObject();
        const QJsonObject onErrorAssign = onError.value(QStringLiteral("actions")).toArray().at(0).toObject();
        if (onError.value(QStringLiteral("target")).toString() != QStringLiteral("Failure") ||
            onErrorGuard.value(QStringLiteral("type")).toString() != QStringLiteral("!=") ||
            onErrorGuard.value(QStringLiteral("left")).toObject().value(QStringLiteral("type")).toString() !=
                QStringLiteral("event") ||
            onErrorGuard.value(QStringLiteral("left")).toObject().value(QStringLiteral("name")).toString() !=
                QStringLiteral("error") ||
            onErrorGuard.value(QStringLiteral("right")).toString() != QStringLiteral("") ||
            onErrorAssign.value(QStringLiteral("type")).toString() != QStringLiteral("xstate.assign") ||
            onErrorAssign.value(QStringLiteral("assignment")).toObject().value(QStringLiteral("lastError")).toObject()
                    .value(QStringLiteral("type")).toString() != QStringLiteral("event") ||
            onErrorAssign.value(QStringLiteral("assignment")).toObject().value(QStringLiteral("lastError")).toObject()
                    .value(QStringLiteral("name")).toString() != QStringLiteral("error")) {
            std::fprintf(stderr, "FAIL: invoke.onError target/guard/assign mismatch\n");
            return 1;
        }

        // ---- meta.ordo.invokeOutputType: non-default emitted, default omitted ----
        const QJsonObject loadingOrdo =
            loadingJson.value(QStringLiteral("meta")).toObject().value(QStringLiteral("ordo")).toObject();
        const QJsonObject pingingJson = states.value(QStringLiteral("Pinging")).toObject();
        const QJsonObject pingingOrdo =
            pingingJson.value(QStringLiteral("meta")).toObject().value(QStringLiteral("ordo")).toObject();
        const QJsonObject pingingInvoke = pingingJson.value(QStringLiteral("invoke")).toObject();
        if (loadingOrdo.value(QStringLiteral("invokeOutputType")).toString() != QStringLiteral("Double") ||
            pingingOrdo.contains(QStringLiteral("invokeOutputType")) ||
            pingingInvoke.keys() != QStringList{QStringLiteral("src")} ||
            pingingInvoke.value(QStringLiteral("src")).toString() != QStringLiteral("ping")) {
            std::fprintf(stderr,
                         "FAIL: invoke export invokeOutputType mismatch (Loading should carry 'Double', "
                         "Pinging's default Int and unset id should be omitted -- invoke = {src: ping} only)\n");
            return 1;
        }
        // Idle never invokes, so it must carry neither `invoke` nor `invokeOutputType`.
        const QJsonObject idleJson2 = states.value(QStringLiteral("Idle")).toObject();
        if (idleJson2.contains(QStringLiteral("invoke")) ||
            idleJson2.value(QStringLiteral("meta")).toObject().value(QStringLiteral("ordo")).toObject()
                .contains(QStringLiteral("invokeOutputType"))) {
            std::fprintf(stderr, "FAIL: a non-invoking state exported an `invoke` key or invokeOutputType\n");
            return 1;
        }

        // ---- round trip: export -> import -> export is byte-identical -----
        const app::XStateImportResult imported = app::machineFromXStateJson(exported.json);
        if (!imported.ok || !imported.diagnostics.isEmpty()) {
            std::fprintf(stderr, "FAIL: importing the invoke export was not clean (ok=%d, %d diagnostic(s)):\n",
                         imported.ok ? 1 : 0, static_cast<int>(imported.diagnostics.size()));
            for (const QString& line : imported.diagnostics) {
                std::fprintf(stderr, "  - %s\n", qUtf8Printable(line));
            }
            return 1;
        }
        QHash<QString, const app::State*> loaderByName;
        for (const app::State& state : imported.machine.states) {
            loaderByName.insert(state.name, &state);
        }
        const app::State* loadingState = loaderByName.value(QStringLiteral("Loading"));
        const app::State* pingingState = loaderByName.value(QStringLiteral("Pinging"));
        if (loadingState == nullptr || loadingState->invokeSrc != QStringLiteral("fetchUser") ||
            loadingState->invokeId != QStringLiteral("userLoader") ||
            loadingState->invokeOutputType != app::ContextType::Double || pingingState == nullptr ||
            pingingState->invokeSrc != QStringLiteral("ping") || !pingingState->invokeId.isEmpty() ||
            pingingState->invokeOutputType != app::ContextType::Int) {
            std::fprintf(stderr, "FAIL: reimported invoke declaration mismatch\n");
            return 1;
        }
        int loadingTransitionCount = 0;
        const app::Transition* reDone = nullptr;
        const app::Transition* reError = nullptr;
        for (const app::Transition& transition : imported.machine.transitions) {
            if (transition.from != loadingState->id) {
                continue;
            }
            ++loadingTransitionCount;
            if (transition.event == QStringLiteral("done.invoke.userLoader")) {
                reDone = &transition;
            } else if (transition.event == QStringLiteral("error.platform.userLoader")) {
                reError = &transition;
            }
        }
        const auto sameExpr = [](const QString& left, const QString& right) {
            const app::expr::ParseResult leftAst = app::expr::parse(left);
            const app::expr::ParseResult rightAst = app::expr::parse(right);
            return leftAst.ok && rightAst.ok && app::expr::print(leftAst.ast) == app::expr::print(rightAst.ast);
        };
        if (loadingTransitionCount != 2 || reDone == nullptr || reError == nullptr ||
            reDone->to != loaderByName.value(QStringLiteral("Success"))->id ||
            reError->to != loaderByName.value(QStringLiteral("Failure"))->id ||
            !sameExpr(reDone->guard, QStringLiteral("output > 0")) ||
            !sameExpr(reError->guard, QStringLiteral("error != ''")) ||
            app::expr::parseAssignForm(reDone->action).target != QStringLiteral("result") ||
            !sameExpr(app::expr::parseAssignForm(reDone->action).valueSource, QStringLiteral("output")) ||
            app::expr::parseAssignForm(reError->action).target != QStringLiteral("lastError") ||
            !sameExpr(app::expr::parseAssignForm(reError->action).valueSource, QStringLiteral("error"))) {
            std::fprintf(stderr, "FAIL: reimported onDone/onError transitions do not match (target/guard/assign)\n");
            return 1;
        }

        const app::XStateExportResult second = app::machineToXStateJson(imported.machine);
        if (!second.ok || second.json != exported.json) {
            std::fprintf(stderr,
                         "FAIL: an invoke export is not idempotent -- toJson(fromJson(toJson(M))) != toJson(M)\n");
            return 1;
        }

        // ---- multiple onDone candidates: v5's own guarded-fallback array --
        {
            app::Machine multi = loader;
            multi.transitions.push_back(app::Transition{
                .id = 9, .from = 2, .to = 1, .event = QStringLiteral("done.invoke.userLoader")});  // unguarded fallback
            multi.nextId = 10;
            const app::XStateExportResult multiExported = app::machineToXStateJson(multi);
            if (!multiExported.ok) {
                std::fprintf(stderr, "FAIL: multi-candidate invoke export refused: %s\n",
                             qUtf8Printable(multiExported.error));
                return 1;
            }
            const QJsonValue multiOnDone = multiExported.json.value(QStringLiteral("states")).toObject()
                                                .value(QStringLiteral("Loading")).toObject()
                                                .value(QStringLiteral("invoke")).toObject()
                                                .value(QStringLiteral("onDone"));
            if (!multiOnDone.isArray() || multiOnDone.toArray().size() != 2 ||
                multiOnDone.toArray().at(1).toObject().value(QStringLiteral("target")).toString() !=
                    QStringLiteral("Idle")) {
                std::fprintf(stderr,
                             "FAIL: two onDone candidates for one state must export invoke.onDone as a 2-element "
                             "array, guarded row first\n");
                return 1;
            }
            const app::XStateImportResult multiImported = app::machineFromXStateJson(multiExported.json);
            const app::XStateExportResult multiReexported = app::machineToXStateJson(multiImported.machine);
            if (!multiImported.ok || !multiImported.diagnostics.isEmpty() || !multiReexported.ok ||
                multiReexported.json != multiExported.json) {
                std::fprintf(stderr, "FAIL: multi-candidate invoke.onDone did not round-trip idempotently\n");
                return 1;
            }
        }
    }

    // ---- invoke: import also accepts the flat `on: { "done.invoke.<id>": ... }`
    // spelling, not just the nested `invoke.onDone`/`invoke.onError` slot the
    // export writes.
    {
        const char* flatJson = R"json({
            "id": "FlatLoader",
            "initial": "Loading",
            "states": {
                "Loading": {
                    "invoke": { "src": "fetchUser" },
                    "on": {
                        "done.invoke.fetchUser": { "target": "Success" },
                        "error.platform.fetchUser": { "target": "Failure" }
                    }
                },
                "Success": {},
                "Failure": {}
            }
        })json";
        const QJsonDocument document = QJsonDocument::fromJson(QByteArray(flatJson));
        const app::XStateImportResult imported = app::machineFromXStateJson(document.object());
        if (!imported.ok || !imported.diagnostics.isEmpty()) {
            std::fprintf(stderr,
                         "FAIL: flat on.\"done.invoke.<id>\" import was not clean (ok=%d, %d diagnostic(s)):\n",
                         imported.ok ? 1 : 0, static_cast<int>(imported.diagnostics.size()));
            for (const QString& line : imported.diagnostics) {
                std::fprintf(stderr, "  - %s\n", qUtf8Printable(line));
            }
            return 1;
        }
        QHash<QString, const app::State*> flatByName;
        for (const app::State& state : imported.machine.states) {
            flatByName.insert(state.name, &state);
        }
        const app::State* flatLoading = flatByName.value(QStringLiteral("Loading"));
        if (flatLoading == nullptr || flatLoading->invokeSrc != QStringLiteral("fetchUser")) {
            std::fprintf(stderr, "FAIL: flat-form import did not read invoke.src\n");
            return 1;
        }
        bool flatDone = false;
        bool flatError = false;
        for (const app::Transition& transition : imported.machine.transitions) {
            if (transition.from != flatLoading->id) {
                continue;
            }
            if (transition.event == QStringLiteral("done.invoke.fetchUser")) {
                flatDone = transition.to == flatByName.value(QStringLiteral("Success"))->id;
            } else if (transition.event == QStringLiteral("error.platform.fetchUser")) {
                flatError = transition.to == flatByName.value(QStringLiteral("Failure"))->id;
            }
        }
        if (!flatDone || !flatError) {
            std::fprintf(stderr, "FAIL: flat-form onDone/onError transitions did not resolve their targets\n");
            return 1;
        }
    }

    // ---- invoke: both forms present for the same completion -- the nested
    // `invoke.onDone` wins, the flat `on` entry is dropped with a note.
    {
        const char* bothJson = R"json({
            "id": "BothForms",
            "initial": "Loading",
            "states": {
                "Loading": {
                    "invoke": { "src": "fetchUser", "onDone": { "target": "Success" } },
                    "on": { "done.invoke.fetchUser": { "target": "Failure" } }
                },
                "Success": {},
                "Failure": {}
            }
        })json";
        const QJsonDocument document = QJsonDocument::fromJson(QByteArray(bothJson));
        const app::XStateImportResult imported = app::machineFromXStateJson(document.object());
        if (!imported.ok) {
            std::fprintf(stderr, "FAIL: both-forms import structurally refused: %s\n", qUtf8Printable(imported.error));
            return 1;
        }
        if (imported.diagnostics.size() != 1 ||
            !imported.diagnostics.first().contains(QStringLiteral("on.done.invoke.fetchUser")) ||
            !imported.diagnostics.first().contains(QStringLiteral("nested form wins"))) {
            std::fprintf(stderr, "FAIL: both-forms import should note exactly the dropped flat spelling; got:\n");
            for (const QString& line : imported.diagnostics) {
                std::fprintf(stderr, "  - %s\n", qUtf8Printable(line));
            }
            return 1;
        }
        QHash<QString, const app::State*> bothByName;
        for (const app::State& state : imported.machine.states) {
            bothByName.insert(state.name, &state);
        }
        const app::State* bothLoading = bothByName.value(QStringLiteral("Loading"));
        int bothCount = 0;
        const app::Transition* bothOnly = nullptr;
        for (const app::Transition& transition : imported.machine.transitions) {
            if (transition.from == bothLoading->id) {
                ++bothCount;
                bothOnly = &transition;
            }
        }
        if (bothCount != 1 || bothOnly == nullptr ||
            bothOnly->to != bothByName.value(QStringLiteral("Success"))->id) {
            std::fprintf(stderr,
                         "FAIL: both-forms import should keep exactly ONE transition (the nested onDone's target "
                         "Success), got %d\n",
                         bothCount);
            return 1;
        }
    }

    // ---- actor communication (raise, sendTo, spawn) ----
    // `{"type":"xstate.raise", ...}`-shaped action maps to native `raise(PING)` action,
    // exporting back as `{"type":"xstate.raise", "params":{"event":{"type":"PING"}}}`.
    // Unmapped actor communication (`sendTo`, `spawn`) crosses as opaque hook names.
    {
        const char* actorJson = R"json({
            "id": "Actor",
            "initial": "S",
            "states": {
                "S": {
                    "on": {
                        "GO": {
                            "target": "S",
                            "actions": [ { "type": "xstate.raise", "event": { "type": "PING" } } ]
                        }
                    }
                }
            }
        })json";
        const QJsonDocument document = QJsonDocument::fromJson(QByteArray(actorJson));
        const app::XStateImportResult imported = app::machineFromXStateJson(document.object());
        if (!imported.ok) {
            std::fprintf(stderr, "FAIL: actor-communication import structurally refused: %s\n",
                         qUtf8Printable(imported.error));
            return 1;
        }
        const app::Transition* go = nullptr;
        for (const app::Transition& transition : imported.machine.transitions) {
            if (transition.event == QStringLiteral("GO")) {
                go = &transition;
            }
        }
        if (go == nullptr || go->action != QStringLiteral("raise(PING)")) {
            std::fprintf(stderr,
                         "FAIL: an xstate.raise-shaped action should import as 'raise(PING)', got action='%s'\n",
                         go ? qUtf8Printable(go->action) : "<missing>");
            return 1;
        }
        const app::XStateExportResult reexported = app::machineToXStateJson(imported.machine);
        const QJsonValue reGo = reexported.json.value(QStringLiteral("states")).toObject()
                                     .value(QStringLiteral("S")).toObject()
                                     .value(QStringLiteral("on")).toObject()
                                     .value(QStringLiteral("GO"));
        const QJsonArray actionsArr = reGo.toObject().value(QStringLiteral("actions")).toArray();
        if (!reexported.ok || actionsArr.size() != 1 || !actionsArr.at(0).isObject() ||
            actionsArr.at(0).toObject().value(QStringLiteral("type")).toString() != QStringLiteral("xstate.raise")) {
            std::fprintf(stderr, "FAIL: 'raise(PING)' did not re-export as xstate.raise action object\n");
            return 1;
        }
    }

    // ---- RA: reenter flag and always transitions export/import/round-trip ----
    {
        app::Machine m;
        m.name = QStringLiteral("ReenterAlwaysMachine");
        m.states.push_back(app::State{.id = 1, .name = QStringLiteral("A"), .kind = app::StateKind::Normal, .pos = QPointF(0, 0)});
        m.states.push_back(app::State{.id = 2, .name = QStringLiteral("B"), .kind = app::StateKind::Normal, .pos = QPointF(260, 0)});
        m.initialStateId = 1;
        // 1: self-transition on A with reenter = true
        m.transitions.push_back(app::Transition{
            .id = 3, .from = 1, .to = 1, .event = QStringLiteral("SELF_EXT"), .reenter = true
        });
        // 2: self-transition on A with reenter = false (omitted from export)
        m.transitions.push_back(app::Transition{
            .id = 4, .from = 1, .to = 1, .event = QStringLiteral("SELF_INT"), .reenter = false
        });
        // 3: always transition on A -> B with guard
        m.transitions.push_back(app::Transition{
            .id = 5, .from = 1, .to = 2, .guard = QStringLiteral("ready"), .always = true
        });
        // 4: root always transition -> A
        m.transitions.push_back(app::Transition{
            .id = 6, .from = 0, .to = 1, .guard = QStringLiteral("start"), .always = true
        });
        m.nextId = 7;

        const app::XStateExportResult exported = app::machineToXStateJson(m);
        if (!exported.ok || !exported.diagnostics.isEmpty()) {
            std::fprintf(stderr, "FAIL: RA export failed or produced unexpected diagnostics: %s\n",
                         qUtf8Printable(exported.error));
            return 1;
        }

        const QJsonObject aJson = exported.json.value(QStringLiteral("states")).toObject().value(QStringLiteral("A")).toObject();
        const QJsonObject selfExt = aJson.value(QStringLiteral("on")).toObject().value(QStringLiteral("SELF_EXT")).toObject();
        const QJsonObject selfInt = aJson.value(QStringLiteral("on")).toObject().value(QStringLiteral("SELF_INT")).toObject();
        if (selfExt.value(QStringLiteral("reenter")).toBool() != true) {
            std::fprintf(stderr, "FAIL: reenter: true was not exported on SELF_EXT\n");
            return 1;
        }
        if (selfInt.contains(QStringLiteral("reenter"))) {
            std::fprintf(stderr, "FAIL: reenter: false should be omitted on SELF_INT\n");
            return 1;
        }
        const QJsonArray aAlways = aJson.value(QStringLiteral("always")).toArray();
        if (aAlways.size() != 1 || aAlways.at(0).toObject().value(QStringLiteral("target")).toString() != QStringLiteral("B") ||
            aAlways.at(0).toObject().value(QStringLiteral("guard")).toString() != QStringLiteral("ready")) {
            std::fprintf(stderr, "FAIL: state-level always array mismatch on A\n");
            return 1;
        }
        const QJsonArray rootAlways = exported.json.value(QStringLiteral("always")).toArray();
        if (rootAlways.size() != 1 || rootAlways.at(0).toObject().value(QStringLiteral("target")).toString() != QStringLiteral("A")) {
            std::fprintf(stderr, "FAIL: root-level always array mismatch\n");
            return 1;
        }

        // Reimport and check round trip
        const app::XStateImportResult imported = app::machineFromXStateJson(exported.json);
        if (!imported.ok || !imported.diagnostics.isEmpty()) {
            std::fprintf(stderr, "FAIL: RA reimport failed or produced diagnostics\n");
            return 1;
        }
        const app::XStateExportResult reexported = app::machineToXStateJson(imported.machine);
        if (!reexported.ok || reexported.json != exported.json) {
            std::fprintf(stderr, "FAIL: RA export -> import -> export is not idempotent\n");
            return 1;
        }
    }

    // Phase 5: Multi-target array import/export (W3C SCXML §3.3.1, XState v5 target: ['s1', 's2'])
    {
        app::Machine mtMachine;
        mtMachine.name = QStringLiteral("MultiTargetTest");
        mtMachine.states.push_back(app::State{.id = 1, .name = QStringLiteral("Root"), .kind = app::StateKind::Parallel});
        mtMachine.states.push_back(app::State{.id = 2, .name = QStringLiteral("RegA"), .kind = app::StateKind::Normal, .parentId = 1});
        mtMachine.states.push_back(app::State{.id = 3, .name = QStringLiteral("IdleA"), .kind = app::StateKind::Normal, .parentId = 2});
        mtMachine.states.push_back(app::State{.id = 4, .name = QStringLiteral("ActiveA"), .kind = app::StateKind::Normal, .parentId = 2});
        mtMachine.states.push_back(app::State{.id = 5, .name = QStringLiteral("RegB"), .kind = app::StateKind::Normal, .parentId = 1});
        mtMachine.states.push_back(app::State{.id = 6, .name = QStringLiteral("IdleB"), .kind = app::StateKind::Normal, .parentId = 5});
        mtMachine.states.push_back(app::State{.id = 7, .name = QStringLiteral("ActiveB"), .kind = app::StateKind::Normal, .parentId = 5});

        // Multi-target transition targeting ActiveA and ActiveB simultaneously
        mtMachine.transitions.push_back(app::Transition{
            .id = 8,
            .from = 3,
            .to = 4,
            .event = QStringLiteral("START"),
            .targets = {4, 7}
        });

        const app::XStateExportResult exported = app::machineToXStateJson(mtMachine);
        if (!exported.ok || !exported.diagnostics.isEmpty()) {
            std::fprintf(stderr, "FAIL: Multi-target machine export failed\n");
            return 1;
        }

        // Verify JSON contains array target
        const QJsonObject rootObj = exported.json.value(QStringLiteral("states")).toObject().value(QStringLiteral("Root")).toObject();
        const QJsonObject regAObj = rootObj.value(QStringLiteral("states")).toObject().value(QStringLiteral("RegA")).toObject();
        const QJsonObject idleAObj = regAObj.value(QStringLiteral("states")).toObject().value(QStringLiteral("IdleA")).toObject();
        const QJsonObject transObj = idleAObj.value(QStringLiteral("on")).toObject().value(QStringLiteral("START")).toObject();

        const QJsonValue targetVal = transObj.value(QStringLiteral("target"));
        if (!targetVal.isArray()) {
            std::fprintf(stderr, "FAIL: multi-target transition did not export as JSON array\n");
            return 1;
        }
        const QJsonArray targetArr = targetVal.toArray();
        if (targetArr.size() != 2 || targetArr.at(0).toString() != QStringLiteral("ActiveA") || targetArr.at(1).toString() != QStringLiteral("ActiveB")) {
            std::fprintf(stderr, "FAIL: exported target array mismatch: expected [\"ActiveA\", \"ActiveB\"]\n");
            return 1;
        }

        // Verify round trip idempotence
        const app::XStateImportResult imported = app::machineFromXStateJson(exported.json);
        if (!imported.ok || !imported.diagnostics.isEmpty()) {
            std::fprintf(stderr, "FAIL: Multi-target import produced errors or diagnostics\n");
            return 1;
        }
        bool foundMt = false;
        for (const app::Transition& tr : imported.machine.transitions) {
            if (tr.event == QStringLiteral("START")) {
                if (tr.targets.size() != 2) {
                    std::fprintf(stderr, "FAIL: imported transition targets size %lld != 2\n", static_cast<long long>(tr.targets.size()));
                    return 1;
                }
                if (tr.to != tr.targets.first()) {
                    std::fprintf(stderr, "FAIL: imported transition to (%llu) != targets.first (%llu)\n",
                                 static_cast<unsigned long long>(tr.to), static_cast<unsigned long long>(tr.targets.first()));
                    return 1;
                }
                foundMt = true;
                break;
            }
        }
        if (!foundMt) {
            std::fprintf(stderr, "FAIL: START transition not found in imported machine\n");
            return 1;
        }

        const app::XStateExportResult reexported = app::machineToXStateJson(imported.machine);
        if (!reexported.ok || reexported.json != exported.json) {
            std::fprintf(stderr, "FAIL: multi-target export -> import -> export is not idempotent\n");
            return 1;
        }

        // Multi-target transition array and single-element array normalization test
        const char* targetArrayJson = R"({
            "id": "TargetArrayTest",
            "initial": "A",
            "states": {
                "A": {
                    "on": {
                        "SPLIT": {
                            "target": ["B1", "B2"]
                        },
                        "SINGLE_ARR": {
                            "target": ["B1"]
                        }
                    }
                },
                "B1": {},
                "B2": {}
            }
        })";
        const QJsonDocument doc = QJsonDocument::fromJson(QByteArray(targetArrayJson));
        const app::XStateImportResult shortImp = app::machineFromXStateJson(doc.object());
        if (!shortImp.ok) {
            std::fprintf(stderr, "FAIL: target array import failed\n");
            return 1;
        }
        bool checkedSplit = false;
        bool checkedSingle = false;
        for (const app::Transition& tr : shortImp.machine.transitions) {
            if (tr.event == QStringLiteral("SPLIT")) {
                if (tr.targets.size() != 2 || tr.to == 0) {
                    std::fprintf(stderr, "FAIL: SPLIT target array resolution mismatch\n");
                    return 1;
                }
                checkedSplit = true;
            } else if (tr.event == QStringLiteral("SINGLE_ARR")) {
                if (tr.targets.size() != 0 || tr.to == 0) {
                    std::fprintf(stderr, "FAIL: single element array did not normalize to scalar target\n");
                    return 1;
                }
                checkedSingle = true;
            }
        }
        if (!checkedSplit || !checkedSingle) {
            std::fprintf(stderr, "FAIL: target array transitions not found\n");
            return 1;
        }
    }

    // Transition::payloadType crosses XState v5 via meta.ordo.payloadType. One
    // machine carries an Int payload row (Parent's SET_VOLUME), a struct payload
    // row (Parent's RX_FRAME; xstate_v5_io never reads machine.types), and a
    // parent/child pair handling SET_VOLUME with the same Int payloadType at
    // two nesting depths. Loader's onDone completion also sets payloadType, to
    // prove transitionToJson()'s isInvokeCompletion gate withholds it there.
    {
        app::Machine payload;
        payload.name = QStringLiteral("PayloadRoundTrip");
        payload.states.push_back(
            app::State{.id = 1, .name = QStringLiteral("Idle"), .kind = app::StateKind::Normal, .pos = QPointF(0, 0)});
        payload.states.push_back(app::State{.id = 2,
                                            .name = QStringLiteral("Parent"),
                                            .kind = app::StateKind::Normal,
                                            .pos = QPointF(200, 0),
                                            .initialChildId = 3});
        payload.states.push_back(app::State{
            .id = 3, .name = QStringLiteral("Sub"), .kind = app::StateKind::Normal, .pos = QPointF(200, 150), .parentId = 2});
        payload.states.push_back(app::State{.id = 4,
                                            .name = QStringLiteral("Loader"),
                                            .kind = app::StateKind::Normal,
                                            .pos = QPointF(400, 0),
                                            .invokeSrc = QStringLiteral("fetchData")});

        app::StructDefinition canMsg;
        canMsg.id = 100;
        canMsg.name = QStringLiteral("CanMessage");
        canMsg.external = true;
        canMsg.headerPath = QStringLiteral("can_types.h");
        canMsg.fields = {
            app::StructField{.name = QStringLiteral("id"), .type = app::FieldType::Int, .initialValue = QStringLiteral("0")},
            app::StructField{.name = QStringLiteral("dlc"), .type = app::FieldType::Int, .initialValue = QStringLiteral("8")}};
        payload.types.push_back(canMsg);

        payload.transitions.push_back(app::Transition{
            .id = 10, .from = 2, .to = 1, .event = QStringLiteral("SET_VOLUME"), .payloadType = QStringLiteral("Int")});
        payload.transitions.push_back(app::Transition{
            .id = 11, .from = 3, .to = 1, .event = QStringLiteral("SET_VOLUME"), .payloadType = QStringLiteral("Int")});
        payload.transitions.push_back(app::Transition{
            .id = 12, .from = 2, .to = 2, .event = QStringLiteral("RX_FRAME"), .payloadType = QStringLiteral("CanMessage")});
        payload.transitions.push_back(app::Transition{.id = 13,
                                                       .from = 4,
                                                       .to = 1,
                                                       .event = QStringLiteral("done.invoke.fetchData"),
                                                       .payloadType = QStringLiteral("ShouldNeverSurviveExport")});
        payload.nextId = 14;
        payload.initialStateId = 1;

        const app::XStateExportResult exported = app::machineToXStateJson(payload);
        if (!exported.ok || !exported.diagnostics.isEmpty()) {
            std::fprintf(stderr, "FAIL: payloadType-carrying machine export refused or diagnosed: %s\n",
                         qUtf8Printable(exported.error));
            for (const QString& line : exported.diagnostics) {
                std::fprintf(stderr, "  - %s\n", qUtf8Printable(line));
            }
            return 1;
        }

        const QJsonObject states = exported.json.value(QStringLiteral("states")).toObject();
        const QJsonObject parentJson = states.value(QStringLiteral("Parent")).toObject();
        const QJsonObject subJson =
            parentJson.value(QStringLiteral("states")).toObject().value(QStringLiteral("Sub")).toObject();

        const QJsonObject parentSetVolume =
            parentJson.value(QStringLiteral("on")).toObject().value(QStringLiteral("SET_VOLUME")).toObject();
        if (parentSetVolume.value(QStringLiteral("meta")).toObject().value(QStringLiteral("ordo")).toObject().value(
                QStringLiteral("payloadType")).toString() != QStringLiteral("Int")) {
            std::fprintf(stderr, "FAIL: Parent's SET_VOLUME lost meta.ordo.payloadType on export (want Int)\n");
            return 1;
        }
        const QJsonObject subSetVolume =
            subJson.value(QStringLiteral("on")).toObject().value(QStringLiteral("SET_VOLUME")).toObject();
        if (subSetVolume.value(QStringLiteral("meta")).toObject().value(QStringLiteral("ordo")).toObject().value(
                QStringLiteral("payloadType")).toString() != QStringLiteral("Int")) {
            std::fprintf(stderr,
                         "FAIL: Sub's (nested child) SET_VOLUME lost meta.ordo.payloadType on export (want Int)\n");
            return 1;
        }
        const QJsonObject parentRxFrame =
            parentJson.value(QStringLiteral("on")).toObject().value(QStringLiteral("RX_FRAME")).toObject();
        if (parentRxFrame.value(QStringLiteral("meta")).toObject().value(QStringLiteral("ordo")).toObject().value(
                QStringLiteral("payloadType")).toString() != QStringLiteral("CanMessage")) {
            std::fprintf(stderr,
                         "FAIL: Parent's RX_FRAME (struct payload) lost meta.ordo.payloadType on export (want "
                         "CanMessage)\n");
            return 1;
        }

        // The invoke onDone row carries no meta.ordo.payloadType even though
        // its Transition::payloadType is non-empty.
        const QJsonObject loaderJson = states.value(QStringLiteral("Loader")).toObject();
        if (loaderJson.contains(QStringLiteral("on"))) {
            std::fprintf(stderr, "FAIL: Loader's onDone leaked into its own `on` map instead of `invoke.onDone`\n");
            return 1;
        }
        const QJsonValue loaderOnDone = loaderJson.value(QStringLiteral("invoke")).toObject().value(QStringLiteral("onDone"));
        const QJsonObject loaderOnDoneObj = loaderOnDone.isObject() ? loaderOnDone.toObject() : QJsonObject();
        if (loaderOnDoneObj.value(QStringLiteral("meta")).toObject().value(QStringLiteral("ordo")).toObject().contains(
                QStringLiteral("payloadType"))) {
            std::fprintf(stderr, "FAIL: invoke onDone row exported meta.ordo.payloadType -- must never carry one\n");
            return 1;
        }

        // ---- round trip: export -> import -> export is byte-identical, payloadType included on every row ----
        const app::XStateImportResult imported = app::machineFromXStateJson(exported.json);
        if (!imported.ok || !imported.diagnostics.isEmpty()) {
            std::fprintf(stderr, "FAIL: reimporting the payloadType-carrying export produced errors or diagnostics\n");
            for (const QString& line : imported.diagnostics) {
                std::fprintf(stderr, "  - %s\n", qUtf8Printable(line));
            }
            return 1;
        }
        bool sawParentSetVolume = false;
        bool sawSubSetVolume = false;
        bool sawRxFrame = false;
        bool sawOnDone = false;
        for (const app::Transition& tr : imported.machine.transitions) {
            if (tr.event == QStringLiteral("SET_VOLUME")) {
                if (tr.payloadType != QStringLiteral("Int")) {
                    std::fprintf(stderr, "FAIL: reimported SET_VOLUME transition %llu payloadType is '%s', want Int\n",
                                 static_cast<unsigned long long>(tr.id), qUtf8Printable(tr.payloadType));
                    return 1;
                }
                // ids re-mint on import, so the two SET_VOLUME rows are told
                // apart by their source state's name.
                for (const app::State& st : imported.machine.states) {
                    if (st.id != tr.from) continue;
                    if (st.name == QStringLiteral("Parent")) sawParentSetVolume = true;
                    if (st.name == QStringLiteral("Sub")) sawSubSetVolume = true;
                }
            } else if (tr.event == QStringLiteral("RX_FRAME")) {
                if (tr.payloadType != QStringLiteral("CanMessage")) {
                    std::fprintf(stderr, "FAIL: reimported RX_FRAME transition payloadType is '%s', want CanMessage\n",
                                 qUtf8Printable(tr.payloadType));
                    return 1;
                }
                sawRxFrame = true;
            } else if (tr.event == QStringLiteral("done.invoke.fetchData")) {
                if (!tr.payloadType.isEmpty()) {
                    std::fprintf(stderr,
                                 "FAIL: reimported invoke onDone transition carries a payloadType ('%s') -- must "
                                 "stay untyped\n",
                                 qUtf8Printable(tr.payloadType));
                    return 1;
                }
                sawOnDone = true;
            }
        }
        if (!sawParentSetVolume || !sawSubSetVolume || !sawRxFrame || !sawOnDone) {
            std::fprintf(stderr,
                         "FAIL: reimported machine is missing one of Parent/Sub SET_VOLUME, RX_FRAME, or onDone "
                         "rows\n");
            return 1;
        }

        const app::XStateExportResult reexported = app::machineToXStateJson(imported.machine);
        if (!reexported.ok || reexported.json != exported.json) {
            std::fprintf(stderr, "FAIL: payloadType export -> import -> export is not idempotent\n");
            return 1;
        }
    }

    std::printf(
        "PASS: state-designer xstate v5 interop smoke (export + idempotent round trip + Stately sample import + "
        "nested/compound/parallel hierarchy + context/expression-guard/assign round trip + invoke lift/round-trip/"
        "flat-form/both-forms-precedence + actor-communication opaque-hook crossing + reenter/always (RA) + multiple targets array + refusals + diagnostics + "
        "transition payloadType meta.ordo round trip (Int/struct/hierarchical, invoke-completion withheld))\n");
    return 0;
}
