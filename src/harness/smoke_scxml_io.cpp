#include "harness/harness.h"

#include <cassert>
#include <cstdio>
#include <QPointF>
#include <QStringList>

#include "infra/machine_io.h"
#include "infra/scxml_io.h"
#include "model/machine.h"
#include "view/geometry/auto_layout.h"  // machineHasNoGeometry()

namespace {

void testBasicRoundTrip() {
    app::Machine m;
    m.name = QStringLiteral("Turnstile");
    m.nextId = 10;

    app::State s1;
    s1.id = 1;
    s1.name = QStringLiteral("Locked");
    s1.kind = app::StateKind::Normal;
    s1.pos = QPointF(100.0, 150.0);
    s1.entryActions = {QStringLiteral("lockDoor()")};

    app::State s2;
    s2.id = 2;
    s2.name = QStringLiteral("Unlocked");
    s2.kind = app::StateKind::Normal;
    s2.pos = QPointF(350.0, 150.0);
    s2.entryActions = {QStringLiteral("unlockDoor()")};

    m.states = {s1, s2};
    m.initialStateId = 1;

    app::Transition t1;
    t1.id = 3;
    t1.from = 1;
    t1.to = 2;
    t1.event = QStringLiteral("COIN");
    t1.action = QStringLiteral("coins = coins + 1");

    app::Transition t2;
    t2.id = 4;
    t2.from = 2;
    t2.to = 1;
    t2.event = QStringLiteral("PUSH");

    m.transitions = {t1, t2};

    app::ContextVariable cv;
    cv.id = 5;
    cv.name = QStringLiteral("coins");
    cv.type = app::ContextType::Int;
    cv.initialValue = QStringLiteral("0");
    m.context = {cv};

    // 1. Export
    app::ScxmlExportResult exp = app::machineToScxml(m);
    assert(exp.ok && !exp.xml.isEmpty());
    assert(exp.xml.contains("<scxml"));
    assert(exp.xml.contains("name=\"Turnstile\""));
    assert(exp.xml.contains("initial=\"Locked\""));
    assert(exp.xml.contains("<data id=\"coins\" expr=\"0\""));
    assert(exp.xml.contains("<state id=\"Locked\""));
    assert(exp.xml.contains("<state id=\"Unlocked\""));
    assert(exp.xml.contains("<transition event=\"COIN\" target=\"Unlocked\""));
    assert(exp.xml.contains("<assign location=\"coins\" expr=\"coins + 1\""));

    // 2. Import back
    app::ScxmlImportResult imp = app::scxmlToMachine(exp.xml);
    assert(imp.ok);
    const app::Machine& back = imp.machine;
    assert(back.name == QStringLiteral("Turnstile"));
    assert(back.states.size() == 2);
    assert(back.transitions.size() == 2);
    assert(back.context.size() == 1);
    assert(back.context.first().name == QStringLiteral("coins"));

    // Verify state names and initial
    bool foundLocked = false;
    bool foundUnlocked = false;
    quint64 lockedId = 0;
    for (const auto& s : back.states) {
        if (s.name == QStringLiteral("Locked")) {
            foundLocked = true;
            lockedId = s.id;
            assert(s.pos == QPointF(100.0, 150.0));
            assert(s.entryActions.contains(QStringLiteral("lockDoor()")));
        } else if (s.name == QStringLiteral("Unlocked")) {
            foundUnlocked = true;
            assert(s.pos == QPointF(350.0, 150.0));
            assert(s.entryActions.contains(QStringLiteral("unlockDoor()")));
        }
    }
    assert(foundLocked && foundUnlocked);
    assert(back.initialStateId == lockedId);
}

void testHierarchyAndKinds() {
    app::Machine m;
    m.name = QStringLiteral("PlayerMachine");
    m.nextId = 20;

    app::State root;
    root.id = 1;
    root.name = QStringLiteral("Active");
    root.kind = app::StateKind::Normal;
    root.initialChildId = 2;

    app::State child1;
    child1.id = 2;
    child1.name = QStringLiteral("Playing");
    child1.kind = app::StateKind::Normal;
    child1.parentId = 1;

    app::State child2;
    child2.id = 3;
    child2.name = QStringLiteral("ParallelModes");
    child2.kind = app::StateKind::Parallel;
    child2.parentId = 1;

    app::State r1;
    r1.id = 4;
    r1.name = QStringLiteral("Audio");
    r1.kind = app::StateKind::Normal;
    r1.parentId = 3;

    app::State r2;
    r2.id = 5;
    r2.name = QStringLiteral("Video");
    r2.kind = app::StateKind::Normal;
    r2.parentId = 3;

    app::State hist;
    hist.id = 6;
    hist.name = QStringLiteral("HistDeep");
    hist.kind = app::StateKind::History;
    hist.historyDeep = true;
    hist.parentId = 1;

    app::State fin;
    fin.id = 7;
    fin.name = QStringLiteral("Stopped");
    fin.kind = app::StateKind::Final;

    m.states = {root, child1, child2, r1, r2, hist, fin};
    m.initialStateId = 1;

    // Multi-target transition to orthogonal regions
    app::Transition mt;
    mt.id = 8;
    mt.from = 2;
    mt.to = 4;
    mt.targets = {4, 5};
    mt.event = QStringLiteral("SPLIT");

    // Always transition
    app::Transition alw;
    alw.id = 9;
    alw.from = 4;
    alw.to = 5;
    alw.always = true;
    alw.guard = QStringLiteral("isSynced");

    // Reentering transition
    app::Transition ext;
    ext.id = 10;
    ext.from = 1;
    ext.to = 7;
    ext.event = QStringLiteral("STOP");
    ext.reenter = true;

    m.transitions = {mt, alw, ext};

    app::ScxmlExportResult exp = app::machineToScxml(m);
    assert(exp.ok);
    assert(exp.xml.contains("<parallel id=\"ParallelModes\""));
    assert(exp.xml.contains("<final id=\"Stopped\""));
    assert(exp.xml.contains("<history id=\"HistDeep\" type=\"deep\""));
    assert(exp.xml.contains("initial=\"Playing\""));
    assert(exp.xml.contains("target=\"Audio Video\""));
    assert(exp.xml.contains("cond=\"isSynced\""));
    assert(exp.xml.contains("type=\"external\""));

    // Import and inspect hierarchy
    app::ScxmlImportResult imp = app::scxmlToMachine(exp.xml);
    assert(imp.ok);
    const app::Machine& back = imp.machine;
    assert(back.states.size() == 7);

    quint64 backParallelId = 0;
    quint64 backActiveId = 0;
    for (const auto& s : back.states) {
        if (s.name == QStringLiteral("ParallelModes")) {
            assert(s.kind == app::StateKind::Parallel);
            backParallelId = s.id;
        } else if (s.name == QStringLiteral("Active")) {
            assert(s.kind == app::StateKind::Normal);
            backActiveId = s.id;
        } else if (s.name == QStringLiteral("Stopped")) {
            assert(s.kind == app::StateKind::Final);
        } else if (s.name == QStringLiteral("HistDeep")) {
            assert(s.kind == app::StateKind::History);
            assert(s.historyDeep == true);
        }
    }
    assert(backParallelId != 0 && backActiveId != 0);

    // Verify children parentage
    for (const auto& s : back.states) {
        if (s.name == QStringLiteral("Audio") || s.name == QStringLiteral("Video")) {
            assert(s.parentId == backParallelId);
        } else if (s.name == QStringLiteral("ParallelModes") || s.name == QStringLiteral("Playing") ||
                   s.name == QStringLiteral("HistDeep")) {
            assert(s.parentId == backActiveId);
        }
    }

    // Verify multi-target transition
    bool foundMt = false;
    for (const auto& t : back.transitions) {
        if (t.event == QStringLiteral("SPLIT")) {
            assert(t.isMultiTarget());
            assert(t.targets.size() == 2);
            foundMt = true;
        } else if (t.isAlways()) {
            assert(t.event.isEmpty());
            assert(t.guard == QStringLiteral("isSynced"));
        } else if (t.event == QStringLiteral("STOP")) {
            assert(t.reenter == true);
        }
    }
    assert(foundMt);
}

void testActionsAndInvocations() {
    app::Machine m;
    m.name = QStringLiteral("ActorMachine");
    m.nextId = 10;

    app::State s;
    s.id = 1;
    s.name = QStringLiteral("Working");
    s.entryActions = {QStringLiteral("raise(TICK)"), QStringLiteral("sendTo(logger, STARTED)")};
    s.exitActions = {QStringLiteral("count = count + 1")};
    s.invocations = {app::Invocation{.src = QStringLiteral("workerService"), .id = QStringLiteral("w1")}};

    m.states = {s};
    m.initialStateId = 1;

    app::ScxmlExportResult exp = app::machineToScxml(m);
    assert(exp.ok);
    assert(exp.xml.contains("<raise event=\"TICK\""));
    assert(exp.xml.contains("<send target=\"logger\" event=\"STARTED\""));
    assert(exp.xml.contains("<assign location=\"count\" expr=\"count + 1\""));
    assert(exp.xml.contains("<invoke type=\"scxml\" src=\"workerService\" id=\"w1\""));

    app::ScxmlImportResult imp = app::scxmlToMachine(exp.xml);
    assert(imp.ok);
    const auto& backState = imp.machine.states.first();
    assert(backState.entryActions.contains(QStringLiteral("raise(TICK)")));
    assert(backState.entryActions.contains(QStringLiteral("sendTo(logger, STARTED)")));
    assert(backState.exitActions.contains(QStringLiteral("count = count + 1")));
    assert(backState.invocations.size() == 1);
    assert(backState.invocations.first().src == QStringLiteral("workerService"));
}

void testOptionsExclusion() {
    app::Machine m;
    m.name = QStringLiteral("OptMachine");
    m.nextId = 5;

    app::State s;
    s.id = 1;
    s.name = QStringLiteral("S1");
    s.pos = QPointF(120, 240);
    s.entryActions = {QStringLiteral("doWork()")};
    m.states = {s};
    m.initialStateId = 1;

    // Export with layout=false and actions=false
    app::ScxmlExportOptions opts;
    opts.includeLayout = false;
    opts.includeActions = false;

    app::ScxmlExportResult exp = app::machineToScxml(m, opts);
    assert(exp.ok);
    assert(!exp.xml.contains("<metadata>"));
    assert(!exp.xml.contains("<onentry>"));
    assert(!exp.xml.contains("doWork"));
}

void testLayoutFallback() {
    // Importers never guess geometry: a state that <layout> doesn't place stays
    // at State::pos's default (0,0) origin, to be laid out when the document opens.
    const QByteArray noLayoutXml(
        "<scxml xmlns=\"http://www.w3.org/2005/07/scxml\" version=\"1.0\" initial=\"Idle\">"
        "<state id=\"Idle\"><transition event=\"GO\" target=\"Running\"/></state>"
        "<state id=\"Running\"/>"
        "</scxml>");
    app::ScxmlImportResult noLayout = app::scxmlToMachine(noLayoutXml);
    assert(noLayout.ok);
    assert(noLayout.machine.states.size() == 2);
    for (const auto& s : noLayout.machine.states) {
        assert(s.pos == QPointF(0.0, 0.0));
    }
    assert(app::machineHasNoGeometry(noLayout.machine));

    // A file with <layout> entries for SOME states keeps those and leaves the
    // rest at the origin, so machineHasNoGeometry() reports false.
    const QByteArray mixedLayoutXml(
        "<scxml xmlns=\"http://www.w3.org/2005/07/scxml\" version=\"1.0\" initial=\"Idle\">"
        "<state id=\"Idle\"><transition event=\"GO\" target=\"Running\"/></state>"
        "<state id=\"Running\"/>"
        "<metadata><layout><state id=\"Idle\" x=\"40.0\" y=\"60.0\"/></layout></metadata>"
        "</scxml>");
    app::ScxmlImportResult mixed = app::scxmlToMachine(mixedLayoutXml);
    assert(mixed.ok);
    assert(mixed.machine.states.size() == 2);
    for (const auto& s : mixed.machine.states) {
        if (s.name == QStringLiteral("Idle")) {
            assert(s.pos == QPointF(40.0, 60.0));
        } else {
            assert(s.pos == QPointF(0.0, 0.0));
        }
    }
    assert(!app::machineHasNoGeometry(mixed.machine));
}

void testErrorHandling() {
    // Empty
    app::ScxmlImportResult emptyRes = app::scxmlToMachine(QByteArray());
    assert(!emptyRes.ok);

    // Malformed XML
    app::ScxmlImportResult malformedRes = app::scxmlToMachine(QByteArray("<scxml><state>unclosed</scxml>"));
    assert(!malformedRes.ok);
}

void testRegistryIntegration() {
    auto adapter = app::MachineIoRegistry::instance().findAdapterById(QStringLiteral("scxml"));
    assert(adapter != nullptr);

    const app::MachineFormatDescriptor desc = adapter->descriptor();
    assert(desc.id == QStringLiteral("scxml"));
    assert(desc.name == QStringLiteral("W3C SCXML"));
    assert(app::hasCapability(desc.capabilities, app::IoCapability::Import));
    assert(app::hasCapability(desc.capabilities, app::IoCapability::Export));
    assert(desc.payloadKind == app::PayloadKind::Text);
    assert(desc.supportedScopes.testFlag(app::ExportScope::SingleMachine));
    assert(desc.supportedScopes.testFlag(app::ExportScope::MultipleMachinesDirectory));
    assert(desc.options.size() == 2);

    // Filter string bidirectional mapping
    const QString filter = app::MachineIoRegistry::instance().filterStringForFormat(desc);
    assert(filter.contains("scxml"));
    auto resolved = app::MachineIoRegistry::instance().formatForFilter(filter);
    assert(resolved != nullptr);
    assert(resolved->descriptor().id == QStringLiteral("scxml"));
}

}  // namespace

int runScxmlSmoke() {
    std::printf("[SMOKE] Running W3C SCXML I/O smoke tests...\n");

    testBasicRoundTrip();
    testHierarchyAndKinds();
    testActionsAndInvocations();
    testOptionsExclusion();
    testLayoutFallback();
    testErrorHandling();
    testRegistryIntegration();

    std::printf("PASS: state-designer W3C SCXML I/O smoke (export + import + round-trip + hierarchy + datamodel + actions + options + no-layout/mixed-layout geometry + registry)\n");
    return 0;
}
