#pragma once

#include <functional>

#include <QHash>
#include <QPointF>
#include <QRectF>
#include <QVector>
#include <QtGlobal>

#include "model/machine.h"

class QGraphicsLineItem;
class QGraphicsScene;

namespace app {

class ItemRegistryDelegate;

// Node-drag mechanics for CanvasPresenter: subtree drag snapshots, drop-target
// and containment scans, alignment guides, and commit planning. It never sends
// intents; the presenter owns the FSM session and sends what planNodeCommit
// returns. machine() is the current document, or nullptr when none is attached.
class DragSessionDelegate {
public:
    // The presenter sends one MoveStateRequested per move (in order), then one
    // ReparentStateRequested if `reparent`, inside an undo batch if `needsBatch`.
    struct NodeCommitPlan {
        struct Move {
            quint64 id = 0;
            QPointF pos;
        };
        QVector<Move> moves;      // root move first, then descendant moves (subtree drag only)
        bool reparent = false;    // the drag crossed into a different container
        quint64 reparentTo = 0;   // valid when reparent (0 == the machine root)
        bool needsBatch = false;  // subtree drag or reparent -- one Ctrl+Z must revert the whole gesture
    };

    DragSessionDelegate(QGraphicsScene* scene, const ItemRegistryDelegate& items,
                        std::function<const Machine*()> machine);

    // Snapshots the parent's start rect and, if the state is drawn as a
    // container, the pre-drag positions of its subtree.
    void armForDrag(quint64 stateId);
    // 0 == no subtree drag in flight (a plain leaf NodeDrag).
    quint64 subtreeRootId() const { return subtreeDragRootId_; }
    void applySubtreeDragDelta();
    void cancelSubtreeDrag();
    // Call once a NodeDrag has ended, committed or aborted.
    void clearSessionState();

    // Moves every state by `delta` from its pre-drag document position,
    // recomputed each call so snap rounding never drifts states apart. The
    // preview sends nothing, so cancelling is just the zero-delta apply.
    void applyFramePreview(QPointF delta);
    void cancelFramePreview() { applyFramePreview(QPointF(0.0, 0.0)); }

    quint64 computeDropTarget(quint64 draggedId, QPointF mousePos = QPointF()) const;
    quint64 containerAtPoint(QPointF scenePos) const;
    QVector<quint64> collectSubtreeIds(quint64 rootId) const;

    // The caller gates this to the live NodeDrag's dragged root.
    QPointF resolveAlignment(quint64 draggedId, QPointF proposed);
    // Snaps the dragged pill onto a connected port centerline (or the shared
    // corridor axis) within kPillPortSnapThreshold, showing the guide.
    QPointF resolvePillSnap(quint64 transitionId, QPointF proposedCenter);
    void clearAlignmentGuides();
    bool verticalGuideVisible() const;
    bool horizontalGuideVisible() const;
    // Guide spanning the two segments a merge snap holds collinear; the other
    // guide is hidden. Clear with clearAlignmentGuides() on release.
    void showSegmentMergeGuide(bool vertical, qreal coordinate, const QRectF& draggedSpan,
                               const QRectF& partnerSpan);

    // After the presenter's scene_->clear(): drops the now-dangling guide
    // pointers and drag bookkeeping without deleting anything.
    void forgetSceneItems();

    NodeCommitPlan planNodeCommit(quint64 stateId, QPointF pos, quint64 dropTargetId) const;

private:
    bool liesInSubtree(quint64 candidateId, quint64 rootId) const;
    int depthOf(quint64 candidateId) const;
    void ensureAlignmentGuides();
    void showAlignmentGuide(QGraphicsLineItem* guide, bool vertical, qreal coordinate, const QRectF& a,
                            const QRectF& b, bool isPortSnap = false);

    QGraphicsScene* scene_;
    const ItemRegistryDelegate& items_;
    std::function<const Machine*()> machine_;

    // Subtree drag: descendants follow the root by a delta recomputed from
    // these start positions, never accumulated.
    quint64 subtreeDragRootId_ = 0;
    QPointF subtreeDragRootStartPos_;
    QHash<quint64, QPointF> subtreeDragDescendantStart_;  // descendant id -> item pos() at drag start

    // Parent box at drag start (null for a root state). The live rect grows
    // with the dragged child, so it cannot answer "still inside the parent?".
    // While inside, a same-or-shallower foreign container cannot claim it.
    QRectF dragParentStartRect_;

    // Created lazily on the interaction z-band, hidden between drags.
    QGraphicsLineItem* alignGuideV_ = nullptr;  // vertical line -- an active X-axis (left/centerX/right) alignment
    QGraphicsLineItem* alignGuideH_ = nullptr;  // horizontal line -- an active Y-axis (top/centerY/bottom) alignment
};

}  // namespace app
