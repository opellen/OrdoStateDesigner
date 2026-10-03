#include <cstdio>

#include "view/generated/canvas_interaction_core.h"
#include "harness/harness.h"

// Legality of the canvas interaction machine, asserted by driving the generated
// canvas_interaction_core.h directly (no QApplication, scene or presenter):
// sessions enter only from Idle, only in Design mode, and never chain; each
// SessionCommitted runs its session's one commit; SessionAborted never commits.
// Actions are only counted, so "commits nothing" is an assertion.
struct NoInteractionGuards final : app::generated::canvas_interaction::CanvasInteractionGuards {};

struct CountingInteractionActions final : app::generated::canvas_interaction::CanvasInteractionActions {
    using Context = app::generated::canvas_interaction::Context;
    int frameMove = 0;
    int inlineEdit = 0;
    int nodeMove = 0;
    int noteMove = 0;
    int pillOffset = 0;
    int reconnect = 0;
    int bandSelection = 0;
    int wire = 0;
    void commitFrameMove(Context&) override { ++frameMove; }
    void commitInlineEdit(Context&) override { ++inlineEdit; }
    void commitNodeMove(Context&) override { ++nodeMove; }
    void commitNoteMove(Context&) override { ++noteMove; }
    void commitPillOffset(Context&) override { ++pillOffset; }
    void commitReconnect(Context&) override { ++reconnect; }
    void commitBandSelection(Context&) override { ++bandSelection; }
    void commitWire(Context&) override { ++wire; }
    int total() const {
        return frameMove + inlineEdit + nodeMove + noteMove + pillOffset + reconnect + bandSelection + wire;
    }
};

int runInteractionFsmSmoke() {
    using State = app::generated::canvas_interaction::CanvasInteractionState;
    using Core = app::generated::canvas_interaction::CanvasInteractionCore;

    // Each session with its opening event and the one commit its
    // SessionCommitted must run. NodeGrabbed carries the start parent (0 = root).
    struct Entry {
        const char* name;
        State state;
        void (*grab)(Core&);
        int CountingInteractionActions::*commitCount;
        bool isDrag;  // entered on first real travel (the Strict Drag Guard's entry timing)
    };
    const Entry entries[] = {
        {"NodeDrag", State::NodeDrag, [](Core& core) { core.nodeGrabbedRequested(0); },
         &CountingInteractionActions::nodeMove, true},
        {"NoteDrag", State::NoteDrag, [](Core& core) { core.noteGrabbedRequested(); },
         &CountingInteractionActions::noteMove, true},
        {"PillDrag", State::PillDrag, [](Core& core) { core.pillGrabbedRequested(); },
         &CountingInteractionActions::pillOffset, true},
        {"WireDrag", State::WireDrag, [](Core& core) { core.wireGrabbedRequested(); },
         &CountingInteractionActions::wire, false},
        {"ReconnectDrag", State::ReconnectDrag, [](Core& core) { core.reconnectGrabbedRequested(); },
         &CountingInteractionActions::reconnect, false},
        {"FrameDrag", State::FrameDrag, [](Core& core) { core.frameGrabbedRequested(); },
         &CountingInteractionActions::frameMove, true},
        {"RubberBand", State::RubberBand, [](Core& core) { core.bandStartedRequested(); },
         &CountingInteractionActions::bandSelection, false},
        {"InlineEdit", State::InlineEdit, [](Core& core) { core.editOpenedRequested(); },
         &CountingInteractionActions::inlineEdit, false},
    };

    // The sessions are children of a `Session` compound, so the core reports a
    // configuration (isActive(X)), not a single state().
    // ---- Simulate refuses every entry; Design re-enables them ----------------
    {
        NoInteractionGuards guards;
        CountingInteractionActions actions;
        Core core(guards, actions);
        if (!core.context().designMode) {
            std::fprintf(stderr, "FAIL: the interaction machine did not start in Design mode\n");
            return 1;
        }
        core.modeChangedRequested(false);
        if (core.context().designMode || !core.isActive(State::Idle)) {
            std::fprintf(stderr, "FAIL: ModeChanged(false) did not clear designMode from Idle in place\n");
            return 1;
        }
        for (const Entry& entry : entries) {
            entry.grab(core);
            if (!core.isActive(State::Idle)) {
                std::fprintf(stderr, "FAIL: %s entered outside Design mode\n", entry.name);
                return 1;
            }
        }
        core.modeChangedRequested(true);
        for (const Entry& entry : entries) {
            entry.grab(core);
            if (!core.isActive(entry.state)) {
                std::fprintf(stderr, "FAIL: %s did not open after ModeChanged(true)\n", entry.name);
                return 1;
            }
            core.sessionAbortedRequested();
        }
        if (actions.total() != 0) {
            std::fprintf(stderr, "FAIL: the mode gate ran %d commit action(s)\n", actions.total());
            return 1;
        }
    }

    // ---- sessions enter from Idle, and never chain ---------------------------
    {
        NoInteractionGuards guards;
        CountingInteractionActions actions;
        for (const Entry& entry : entries) {
            Core core(guards, actions);
            entry.grab(core);
            if (!core.isActive(entry.state)) {
                std::fprintf(stderr, "FAIL: %s did not open from Idle in Design mode\n", entry.name);
                return 1;
            }
            for (const Entry& intruder : entries) {
                intruder.grab(core);
                if (!core.isActive(entry.state)) {
                    std::fprintf(stderr, "FAIL: %s was interrupted by a %s entry\n", entry.name, intruder.name);
                    return 1;
                }
            }
        }
        if (actions.total() != 0) {
            std::fprintf(stderr, "FAIL: opening sessions ran %d commit action(s)\n", actions.total());
            return 1;
        }
    }

    // ---- Strict Drag Guard as entry timing: abort commits nothing, commit ---
    // ---- runs exactly the session's own one commit ---------------------------
    // A drag that returns to its press point ends in SessionAborted and commits
    // nothing; every SessionCommitted runs its own session's commit exactly once.
    for (const Entry& entry : entries) {
        NoInteractionGuards guards;
        CountingInteractionActions actions;
        Core core(guards, actions);
        if (entry.isDrag) {
            entry.grab(core);
            core.sessionAbortedRequested();
            if (!core.isActive(State::Idle) || actions.total() != 0) {
                std::fprintf(stderr, "FAIL: a travelled-and-returned %s did not abort to Idle with no commit\n",
                             entry.name);
                return 1;
            }
        }
        entry.grab(core);
        core.sessionCommittedRequested();
        if (!core.isActive(State::Idle) || actions.*entry.commitCount != 1 || actions.total() != 1) {
            std::fprintf(stderr, "FAIL: SessionCommitted from %s did not run exactly its one commit\n", entry.name);
            return 1;
        }
    }

    // ---- context: dropTargetId moves only on NodeGrabbed / DropTargetChanged --
    {
        NoInteractionGuards guards;
        CountingInteractionActions actions;
        Core core(guards, actions);
        if (core.context().dropTargetId != 0) {
            std::fprintf(stderr, "FAIL: dropTargetId did not start at 0 (machine root)\n");
            return 1;
        }
        // Outside NodeDrag the targetless row is out of scope: from Idle ...
        core.dropTargetChangedRequested(9);
        if (core.context().dropTargetId != 0 || !core.isActive(State::Idle)) {
            std::fprintf(stderr, "FAIL: DropTargetChanged from Idle changed the machine\n");
            return 1;
        }
        // ... and from another session.
        core.wireGrabbedRequested();
        core.dropTargetChangedRequested(9);
        if (core.context().dropTargetId != 0 || !core.isActive(State::WireDrag)) {
            std::fprintf(stderr, "FAIL: DropTargetChanged from WireDrag changed the machine\n");
            return 1;
        }
        core.sessionAbortedRequested();

        // A refused NodeGrabbed assigns nothing: the assign rides the row.
        core.modeChangedRequested(false);
        core.nodeGrabbedRequested(5);
        if (core.context().dropTargetId != 0) {
            std::fprintf(stderr, "FAIL: a refused NodeGrabbed still assigned dropTargetId\n");
            return 1;
        }
        core.modeChangedRequested(true);

        core.nodeGrabbedRequested(7);
        if (!core.isActive(State::NodeDrag) || core.context().dropTargetId != 7) {
            std::fprintf(stderr, "FAIL: NodeGrabbed(7) did not open NodeDrag seeded with dropTargetId 7\n");
            return 1;
        }
        core.dropTargetChangedRequested(3);
        if (!core.isActive(State::NodeDrag) || core.context().dropTargetId != 3) {
            std::fprintf(stderr, "FAIL: DropTargetChanged(3) in NodeDrag did not set 3 in place\n");
            return 1;
        }
        core.dropTargetChangedRequested(0);  // back to the machine root -- a legal verdict
        if (!core.isActive(State::NodeDrag) || core.context().dropTargetId != 0) {
            std::fprintf(stderr, "FAIL: DropTargetChanged(0) in NodeDrag did not set the root verdict\n");
            return 1;
        }

        // ModeChanged from inside a session: designMode follows, the session
        // stays (the root row is targetless -- leaving is SessionAborted's job).
        core.modeChangedRequested(false);
        if (core.context().designMode || !core.isActive(State::NodeDrag)) {
            std::fprintf(stderr, "FAIL: ModeChanged(false) inside NodeDrag left the session or kept designMode\n");
            return 1;
        }
        core.modeChangedRequested(true);
        if (!core.context().designMode || !core.isActive(State::NodeDrag)) {
            std::fprintf(stderr, "FAIL: ModeChanged(true) inside NodeDrag left the session or kept designMode false\n");
            return 1;
        }
        if (actions.total() != 0) {
            std::fprintf(stderr, "FAIL: context events ran %d commit action(s)\n", actions.total());
            return 1;
        }
    }

    // ---- SessionAborted: every session back to Idle, nothing committed -------
    {
        NoInteractionGuards guards;
        CountingInteractionActions actions;
        for (const Entry& entry : entries) {
            Core core(guards, actions);
            entry.grab(core);
            core.sessionAbortedRequested();
            if (!core.isActive(State::Idle)) {
                std::fprintf(stderr, "FAIL: SessionAborted from %s did not land on Idle\n", entry.name);
                return 1;
            }
        }
        if (actions.total() != 0) {
            std::fprintf(stderr, "FAIL: SessionAborted ran %d commit action(s)\n", actions.total());
            return 1;
        }
    }

    std::printf("PASS: state-designer interaction-fsm smoke (designMode entry gate via ModeChanged, no chaining, "
                "one commit per SessionCommitted, abort commits nothing, dropTargetId/designMode context events)\n");
    return 0;
}
