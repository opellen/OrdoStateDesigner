#pragma once

#include <functional>
#include <optional>

#include <QLatin1String>
#include <QPointF>
#include <QtGlobal>

#include "view/geometry/edge_router.h"  // PortSide -- the WireDrag payload's grabbed side
#include "view/generated/canvas_interaction_core.h"
#include "view/geometry/pill_port_resolver.h"  // PortAssignment -- the PillDrag payload's flip memory

namespace app {

// Opaque: canvas_presenter.h holds this class by value and includes this header.
enum class SelectionKind;

// Adapter over the generated canvas-interaction core, which owns every legal
// transition and the context values (`designMode`, `dropTargetId`). This class
// adds the commit hooks, per-move session payloads, and the transition trace.
// Sessions are mutually exclusive and enter only from Idle in Design mode.
// A commit hook runs BEFORE the core leaves the session state, so an event it
// fires is silently dropped: queue follow-ups and drain them after the commit.
class CanvasInteractionFsm {
public:
    using State = generated::canvas_interaction::CanvasInteractionState;
    using Context = generated::canvas_interaction::Context;

    // Every hook must be set: the generated core calls them unconditionally.
    struct Hooks {
        std::function<void()> commitNodeMove;
        std::function<void()> commitNoteMove;
        std::function<void()> commitPillOffset;
        std::function<void()> commitFrameMove;
        std::function<void()> commitWire;
        std::function<void()> commitReconnect;
        std::function<void()> commitBandSelection;
        std::function<void()> commitInlineEdit;
    };

    // Per-move session data the machine never sees. The gesture handler writes
    // the final input (releasePos/pos/centerScenePos) just before
    // sessionCommitted(), since the generated hooks take no arguments.
    struct WireDrag {
        quint64 sourceId = 0;
        PortSide side = PortSide::Top;
        QPointF startAnchor;
        quint64 snapTargetId = 0;  // 0 == no current magnetic-snap target
        QPointF releasePos;
    };
    // Rides WireDrag's overlay/snap geometry; adds only the wire to retarget.
    struct ReconnectDrag {
        quint64 transitionId = 0;
    };
    // The drop verdict lives in the machine context: see dropTargetId().
    struct NodeDrag {
        quint64 stateId = 0;
        QPointF pos;
    };
    struct NoteDrag {
        quint64 noteId = 0;
        QPointF pos;
    };
    struct PillDrag {
        quint64 transitionId = 0;
        QPointF centerScenePos;
        bool isAltDrag = false;  // Alt+drag detaches the label; a plain drag deforms the wire
        // Flip memory handed to the port resolver on every preview, so a pill
        // dragged along a side boundary does not chatter. Empty at session start.
        std::optional<PortAssignment> incumbent;
    };
    // Mirrors CanvasPresenter::Selection, which this header cannot name.
    struct InlineEdit {
        SelectionKind kind{};
        quint64 id = 0;
    };
    // The frame item never moves; each state goes to its pre-drag position plus
    // (current - grabScenePos), recomputed each move so snap rounding never
    // drifts states apart.
    struct FrameDrag {
        QPointF grabScenePos;
    };
    // Empty: Qt's RubberBandDrag drives the gesture and applies the selection.
    struct RubberBand {};

    explicit CanvasInteractionFsm(Hooks hooks);

    CanvasInteractionFsm(const CanvasInteractionFsm&) = delete;
    CanvasInteractionFsm& operator=(const CanvasInteractionFsm&) = delete;

    // The live atomic state. The configuration is always {Idle} or
    // {Session, <one session>}, so there is exactly one answer.
    State state() const {
        const auto& configuration = core_.configuration();
        for (std::size_t index = 0; index < configuration.size(); ++index) {
            const State candidate = static_cast<State>(index);
            if (configuration[index] && candidate != State::Session) {
                return candidate;  // Idle, or the one live session
            }
        }
        return State::Idle;  // unreachable: the configuration is never empty
    }

    // "Idle", "WireDrag", ... for trace lines.
    static QLatin1String stateName(State state);

    // Optional observer called after every committed transition, so the owner
    // can track "is a session in flight" in one place.
    void setStateObserver(std::function<void(State previous, State next)> observer) {
        stateObserver_ = std::move(observer);
    }

    // Each call is a request the core may refuse (entries outside Idle or
    // Design mode are silent no-ops), so read state() back.
    // Ids narrow once, here, to the machine's `long long`; document ids are
    // small counters, so the cast is lossless. dropTargetId() widens back.
    // `startParentId` seeds the drop verdict with the current parent (0 = root).
    void nodeGrabbed(quint64 startParentId) {
        core_.nodeGrabbedRequested(static_cast<long long>(startParentId));  // id narrowing, see above
    }
    // NodeDrag only; fire when the verdict changes, never per move.
    void dropTargetChanged(quint64 targetId) {
        core_.dropTargetChangedRequested(static_cast<long long>(targetId));  // id narrowing, see above
    }
    // Accepted in any configuration; forward every mode change, plus once at bind.
    void modeChanged(bool designMode) { core_.modeChangedRequested(designMode); }
    void pillGrabbed() { core_.pillGrabbedRequested(); }
    void wireGrabbed() { core_.wireGrabbedRequested(); }
    void reconnectGrabbed() { core_.reconnectGrabbedRequested(); }
    void frameGrabbed() { core_.frameGrabbedRequested(); }
    void bandStarted() { core_.bandStartedRequested(); }
    void editOpened() { core_.editOpenedRequested(); }
    void noteGrabbed() { core_.noteGrabbedRequested(); }
    void sessionCommitted() { core_.sessionCommittedRequested(); }
    void sessionAborted() { core_.sessionAbortedRequested(); }

    // Read-only: the machine's own assigns are the context's only writers.
    const Context& context() const { return core_.context(); }
    // Meaningful only while NodeDrag is active; nothing resets it afterwards.
    quint64 dropTargetId() const { return static_cast<quint64>(core_.context().dropTargetId); }

    WireDrag& wireDrag() { return wireDrag_; }
    const WireDrag& wireDrag() const { return wireDrag_; }
    ReconnectDrag& reconnectDrag() { return reconnectDrag_; }
    const ReconnectDrag& reconnectDrag() const { return reconnectDrag_; }
    NodeDrag& nodeDrag() { return nodeDrag_; }
    const NodeDrag& nodeDrag() const { return nodeDrag_; }
    NoteDrag& noteDrag() { return noteDrag_; }
    const NoteDrag& noteDrag() const { return noteDrag_; }
    PillDrag& pillDrag() { return pillDrag_; }
    const PillDrag& pillDrag() const { return pillDrag_; }
    InlineEdit& inlineEdit() { return inlineEdit_; }
    const InlineEdit& inlineEdit() const { return inlineEdit_; }
    FrameDrag& frameDrag() { return frameDrag_; }
    const FrameDrag& frameDrag() const { return frameDrag_; }
    RubberBand& rubberBand() { return rubberBand_; }
    const RubberBand& rubberBand() const { return rubberBand_; }

private:
    // Renaming a guard or action in the machine renames the pure virtual on
    // regeneration, so a stale adapter fails to compile. No guards exist yet.
    struct GuardsImpl final : generated::canvas_interaction::CanvasInteractionGuards {};
    // Hooks don't receive the Context, so the machine's assigns stay its only writers.
    struct ActionsImpl final : generated::canvas_interaction::CanvasInteractionActions {
        explicit ActionsImpl(Hooks& hooks) : hooks(hooks) {}
        void commitFrameMove(Context&) override { hooks.commitFrameMove(); }
        void commitInlineEdit(Context&) override { hooks.commitInlineEdit(); }
        void commitNodeMove(Context&) override { hooks.commitNodeMove(); }
        void commitNoteMove(Context&) override { hooks.commitNoteMove(); }
        void commitPillOffset(Context&) override { hooks.commitPillOffset(); }
        void commitReconnect(Context&) override { hooks.commitReconnect(); }
        void commitBandSelection(Context&) override { hooks.commitBandSelection(); }
        void commitWire(Context&) override { hooks.commitWire(); }
        Hooks& hooks;
    };

    // Declaration order is construction order: the core references guards_ and
    // actions_, and actions_ references hooks_.
    Hooks hooks_;
    GuardsImpl guards_;
    ActionsImpl actions_;
    generated::canvas_interaction::CanvasInteractionCore core_;

    std::function<void(State, State)> stateObserver_;

    WireDrag wireDrag_;
    ReconnectDrag reconnectDrag_;
    NodeDrag nodeDrag_;
    NoteDrag noteDrag_;
    PillDrag pillDrag_;
    InlineEdit inlineEdit_;
    FrameDrag frameDrag_;
    RubberBand rubberBand_;
};

}  // namespace app
