#include "view/canvas/drag_session_delegate.h"

#include <algorithm>
#include <cmath>

#include <QColor>
#include <QDebug>
#include <QGraphicsLineItem>
#include <QGraphicsScene>
#include <QLineF>
#include <QPen>
#include <QSet>

#include "constants/design_tokens.h"
#include "view/canvas/item_registry_delegate.h"
#include "view/items/state_item.h"
#include "view/items/transition_item.h"

namespace app {

namespace {

// Node-to-node snap lines during NodeDrag. All distances in scene px.
constexpr qreal kAlignThreshold = 5.0;
constexpr qreal kAlignGuideMargin = 12.0;  // overshoot past the two aligned rects
constexpr qreal kPillPortSnapThreshold = 10.0;
QColor alignmentGuideColor() { return design::color(design::kGuideAlignment); }

const State* findState(const Machine& machine, quint64 id) {
    for (const State& state : machine.states) {
        if (state.id == id) {
            return &state;
        }
    }
    return nullptr;
}

}  // namespace

DragSessionDelegate::DragSessionDelegate(QGraphicsScene* scene, const ItemRegistryDelegate& items,
                                         std::function<const Machine*()> machine)
    : scene_(scene), items_(items), machine_(std::move(machine)) {}

void DragSessionDelegate::armForDrag(quint64 stateId) {
    dragParentStartRect_ = QRectF();
    if (const Machine* machine = machine_()) {
        const State* dragged = findState(*machine, stateId);
        const quint64 parentId = dragged != nullptr ? dragged->parentId : 0;
        if (StateItem* parentItem = items_.stateItems().value(parentId, nullptr)) {
            dragParentStartRect_ = parentItem->sceneRect();
        }
    }

    if (StateItem* item = items_.stateItems().value(stateId, nullptr)) {
        if (item->isContainerMode()) {
            subtreeDragRootId_ = stateId;
            // pos() is already the first-move position when the drag starts.
            subtreeDragRootStartPos_ = item->dragStartScenePos();
            subtreeDragDescendantStart_.clear();
            for (quint64 descendantId : collectSubtreeIds(stateId)) {
                if (StateItem* descendant = items_.stateItems().value(descendantId, nullptr)) {
                    subtreeDragDescendantStart_.insert(descendantId, descendant->pos());
                }
            }
        }
    }
}

void DragSessionDelegate::applySubtreeDragDelta() {
    if (subtreeDragRootId_ == 0) {
        return;
    }
    StateItem* root = items_.stateItems().value(subtreeDragRootId_, nullptr);
    if (root == nullptr) {
        return;
    }
    // The root's pos() is already grid-snapped, so the delta stays a grid
    // multiple and descendants never snap apart from it.
    const QPointF delta = root->pos() - subtreeDragRootStartPos_;
    for (auto it = subtreeDragDescendantStart_.constBegin(); it != subtreeDragDescendantStart_.constEnd(); ++it) {
        if (StateItem* descendant = items_.stateItems().value(it.key(), nullptr)) {
            descendant->setPos(it.value() + delta);
        }
    }
}

void DragSessionDelegate::cancelSubtreeDrag() {
    if (subtreeDragRootId_ == 0) {
        return;  // a plain leaf NodeDrag never armed this -- nothing to restore
    }
    if (StateItem* root = items_.stateItems().value(subtreeDragRootId_, nullptr)) {
        // Re-fires applySubtreeDragDelta with a zero delta, restoring descendants.
        root->setPos(subtreeDragRootStartPos_);
    }
}

void DragSessionDelegate::clearSessionState() {
    subtreeDragRootId_ = 0;
    subtreeDragDescendantStart_.clear();
    dragParentStartRect_ = QRectF();
}

void DragSessionDelegate::applyFramePreview(QPointF delta) {
    const Machine* machine = machine_();
    if (machine == nullptr) {
        return;
    }
    // setPos goes through the per-state move callback, so wires and the frame follow.
    for (auto it = items_.stateItems().constBegin(); it != items_.stateItems().constEnd(); ++it) {
        if (const State* state = findState(*machine, it.key())) {
            it.value()->setPos(state->pos + delta);
        }
    }
}

QVector<quint64> DragSessionDelegate::collectSubtreeIds(quint64 rootId) const {
    QVector<quint64> result;
    const Machine* machine = machine_();
    if (machine == nullptr) {
        return result;
    }
    // Walks parentId without an index; this runs once per drag start.
    QVector<quint64> frontier{rootId};
    while (!frontier.isEmpty()) {
        const quint64 current = frontier.takeLast();
        for (const State& state : machine->states) {
            if (state.parentId == current) {
                result.push_back(state.id);
                frontier.push_back(state.id);
            }
        }
    }
    return result;
}

bool DragSessionDelegate::liesInSubtree(quint64 candidateId, quint64 rootId) const {
    // True if candidateId is rootId or a descendant. The visited set guards
    // against a corrupt parent cycle.
    const Machine* machine = machine_();
    if (machine == nullptr) {
        return false;
    }
    QSet<quint64> visited;
    quint64 current = candidateId;
    while (current != 0 && !visited.contains(current)) {
        if (current == rootId) {
            return true;
        }
        visited.insert(current);
        const State* state = findState(*machine, current);
        current = state != nullptr ? state->parentId : 0;
    }
    return false;
}

int DragSessionDelegate::depthOf(quint64 candidateId) const {
    // 0 for a root state. maxHops guards against a corrupt parent cycle.
    const Machine* machine = machine_();
    if (machine == nullptr) {
        return 0;
    }
    int depth = 0;
    quint64 current = candidateId;
    const int maxHops = machine->states.size();
    while (depth < maxHops) {
        const State* state = findState(*machine, current);
        const quint64 parent = state != nullptr ? state->parentId : 0;
        if (parent == 0) {
            break;
        }
        current = parent;
        ++depth;
    }
    return depth;
}

quint64 DragSessionDelegate::computeDropTarget(quint64 draggedId, QPointF mousePos) const {
    const Machine* machine = machine_();
    StateItem* dragged = items_.stateItems().value(draggedId, nullptr);
    if (machine == nullptr || dragged == nullptr) {
        return 0;
    }
    if (mousePos.isNull()) {
        mousePos = dragged->lastMouseScenePos();
    }
    const State* draggedState = findState(*machine, draggedId);
    const quint64 currentParentId = draggedState != nullptr ? draggedState->parentId : 0;
    const QPointF center = dragged->sceneRect().center();

    quint64 bestId = 0;
    int bestDepth = -1;
    for (auto it = items_.stateItems().constBegin(); it != items_.stateItems().constEnd(); ++it) {
        StateItem* candidate = it.value();
        if (candidate == dragged) {
            continue;
        }
        // Skip ancestors: their box grows with the dragged state, so they
        // always contain it. Staying put is the fallback below instead.
        if (liesInSubtree(draggedId, it.key())) {
            continue;
        }
        const bool hitByMouse = !mousePos.isNull() && candidate->sceneRect().contains(mousePos);
        const bool hitByCenter = candidate->sceneRect().contains(center);
        if (!hitByMouse && !hitByCenter) {
            continue;
        }
        const State* candidateState = findState(*machine, it.key());
        if (candidateState == nullptr) {
            continue;
        }
        // Keep in sync with ReparentStateCommand's rules, so the highlight
        // never promises a drop the command would refuse.
        if (candidateState->kind == StateKind::Final || candidateState->kind == StateKind::History) {
            continue;
        }
        if (liesInSubtree(it.key(), draggedId)) {
            continue;
        }
        const int depth = depthOf(it.key());
        // Deepest container wins; lowest id settles a tie deterministically
        // (hash iteration order is not).
        if (depth > bestDepth || (depth == bestDepth && it.key() < bestId)) {
            bestDepth = depth;
            bestId = it.key();
        }
    }
    // No other container claims it: keep the current parent. A plain drag
    // never leaves a container; that takes the Move Out One Level verb.
    if (bestId == 0) {
        return currentParentId;
    }
    // While still inside its start parent box, only a deeper nested container
    // may take the state; an overlapping same-or-shallower one may not.
    const bool cursorOrCenterInHome = dragParentStartRect_.contains(center) &&
                                      (mousePos.isNull() || dragParentStartRect_.contains(mousePos));
    if (currentParentId != 0 && cursorOrCenterInHome &&
        (!liesInSubtree(bestId, currentParentId) || depthOf(bestId) <= depthOf(currentParentId))) {
        return currentParentId;
    }
    return bestId;
}

quint64 DragSessionDelegate::containerAtPoint(QPointF scenePos) const {
    const Machine* machine = machine_();
    if (machine == nullptr) {
        return 0;
    }
    quint64 bestId = 0;
    int bestDepth = -1;
    for (auto it = items_.stateItems().constBegin(); it != items_.stateItems().constEnd(); ++it) {
        StateItem* candidate = it.value();
        if (!candidate->sceneRect().contains(scenePos)) {
            continue;
        }
        const State* candidateState = findState(*machine, it.key());
        if (candidateState == nullptr || candidateState->kind == StateKind::Final ||
            candidateState->kind == StateKind::History) {
            continue;
        }
        const int depth = depthOf(it.key());
        if (depth > bestDepth || (depth == bestDepth && it.key() < bestId)) {
            bestDepth = depth;
            bestId = it.key();
        }
    }
    return bestId;
}

QPointF DragSessionDelegate::resolveAlignment(quint64 draggedId, QPointF proposed) {
    StateItem* dragged = items_.stateItems().value(draggedId, nullptr);
    if (dragged == nullptr) {
        return proposed;
    }

    // Called before pos() updates, so only the size of sceneRect() is valid.
    const QSizeF size = dragged->sceneRect().size();

    // Descendants have not followed yet, so never align against them.
    QSet<quint64> excluded{draggedId};
    for (quint64 descendantId : collectSubtreeIds(draggedId)) {
        excluded.insert(descendantId);
    }

    // Per axis: nearest delta wins, ties go to the lowest id so the result
    // does not depend on QHash iteration order.
    struct AxisMatch {
        bool found = false;
        qreal delta = 0.0;
        qreal value = 0.0;       // the coordinate this axis snaps to
        qreal guideCoord = 0.0;  // coordinate along the alignment axis for the guide line
        quint64 otherId = 0;
        QRectF otherRect;
        bool isPortSnap = false;
    };
    AxisMatch bestX;
    AxisMatch bestY;

    // 1. Center alignment with directly connected states wins over edge
    // alignment, so their wire can run straight.
    const Machine* machine = machine_();
    if (machine != nullptr) {
        for (const Transition& t : machine->transitions) {
            quint64 otherId = 0;
            if (t.from == draggedId && t.to != 0 && t.to != draggedId) {
                otherId = t.to;
            } else if (t.to == draggedId && t.from != 0 && t.from != draggedId) {
                otherId = t.from;
            }
            if (otherId == 0 || excluded.contains(otherId)) {
                continue;
            }
            StateItem* other = items_.stateItems().value(otherId, nullptr);
            if (other == nullptr || !other->isVisible()) {
                continue;
            }
            const QRectF otherRect = other->sceneRect();

            const qreal candCenterY = proposed.y() + size.height() / 2.0;
            const qreal otherCenterY = otherRect.center().y();
            const qreal dy = std::abs(candCenterY - otherCenterY);
            if (dy <= kAlignThreshold) {
                if (!bestY.found || !bestY.isPortSnap || dy < bestY.delta ||
                    (dy == bestY.delta && otherId < bestY.otherId)) {
                    bestY = AxisMatch{true, dy, otherCenterY - size.height() / 2.0, otherCenterY, otherId, otherRect, true};
                }
            }

            const qreal candCenterX = proposed.x() + size.width() / 2.0;
            const qreal otherCenterX = otherRect.center().x();
            const qreal dx = std::abs(candCenterX - otherCenterX);
            if (dx <= kAlignThreshold) {
                if (!bestX.found || !bestX.isPortSnap || dx < bestX.delta ||
                    (dx == bestX.delta && otherId < bestX.otherId)) {
                    bestX = AxisMatch{true, dx, otherCenterX - size.width() / 2.0, otherCenterX, otherId, otherRect, true};
                }
            }
        }
    }

    // 2. Edge/center alignment with any state, on axes without a port snap.
    const qreal offsetsX[3] = {0.0, size.width() / 2.0, size.width()};
    const qreal offsetsY[3] = {0.0, size.height() / 2.0, size.height()};

    for (auto it = items_.stateItems().constBegin(); it != items_.stateItems().constEnd(); ++it) {
        if (excluded.contains(it.key())) {
            continue;
        }
        StateItem* other = it.value();
        if (other == nullptr || !other->isVisible()) {
            continue;
        }
        const QRectF otherRect = other->sceneRect();
        const qreal otherEdgesX[3] = {otherRect.left(), otherRect.center().x(), otherRect.right()};
        const qreal otherEdgesY[3] = {otherRect.top(), otherRect.center().y(), otherRect.bottom()};

        if (!bestX.isPortSnap) {
            for (qreal offset : offsetsX) {
                const qreal candCoord = proposed.x() + offset;
                for (qreal otherCoord : otherEdgesX) {
                    const qreal d = std::abs(candCoord - otherCoord);
                    if (d <= kAlignThreshold &&
                        (!bestX.found || d < bestX.delta || (d == bestX.delta && it.key() < bestX.otherId))) {
                        bestX = AxisMatch{true, d, otherCoord - offset, otherCoord, it.key(), otherRect, false};
                    }
                }
            }
        }
        if (!bestY.isPortSnap) {
            for (qreal offset : offsetsY) {
                const qreal candCoord = proposed.y() + offset;
                for (qreal otherCoord : otherEdgesY) {
                    const qreal d = std::abs(candCoord - otherCoord);
                    if (d <= kAlignThreshold &&
                        (!bestY.found || d < bestY.delta || (d == bestY.delta && it.key() < bestY.otherId))) {
                        bestY = AxisMatch{true, d, otherCoord - offset, otherCoord, it.key(), otherRect, false};
                    }
                }
            }
        }
    }

    QPointF result = proposed;
    if (bestX.found) {
        result.setX(bestX.value);
    }
    if (bestY.found) {
        result.setY(bestY.value);
    }

    ensureAlignmentGuides();
    const QRectF finalRect(result, size);
    if (bestX.found) {
        showAlignmentGuide(alignGuideV_, /*vertical=*/true, bestX.guideCoord, finalRect, bestX.otherRect, bestX.isPortSnap);
    } else if (alignGuideV_ != nullptr) {
        alignGuideV_->setVisible(false);
    }
    if (bestY.found) {
        showAlignmentGuide(alignGuideH_, /*vertical=*/false, bestY.guideCoord, finalRect, bestY.otherRect, bestY.isPortSnap);
    } else if (alignGuideH_ != nullptr) {
        alignGuideH_->setVisible(false);
    }

    return result;
}

void DragSessionDelegate::ensureAlignmentGuides() {
    if (alignGuideV_ == nullptr) {
        alignGuideV_ = scene_->addLine(QLineF(), QPen(alignmentGuideColor(), 1.0, Qt::DashLine));
        alignGuideV_->setZValue(100.0);  // interaction-overlay band
        alignGuideV_->setVisible(false);
    }
    if (alignGuideH_ == nullptr) {
        alignGuideH_ = scene_->addLine(QLineF(), QPen(alignmentGuideColor(), 1.0, Qt::DashLine));
        alignGuideH_->setZValue(100.0);
        alignGuideH_->setVisible(false);
    }
}

void DragSessionDelegate::showAlignmentGuide(QGraphicsLineItem* guide, bool vertical, qreal coordinate,
                                             const QRectF& a, const QRectF& b, bool isPortSnap) {
    if (guide == nullptr) {
        return;
    }
    if (vertical) {
        const qreal top = std::min(a.top(), b.top()) - kAlignGuideMargin;
        const qreal bottom = std::max(a.bottom(), b.bottom()) + kAlignGuideMargin;
        guide->setLine(QLineF(coordinate, top, coordinate, bottom));
    } else {
        const qreal left = std::min(a.left(), b.left()) - kAlignGuideMargin;
        const qreal right = std::max(a.right(), b.right()) + kAlignGuideMargin;
        guide->setLine(QLineF(left, coordinate, right, coordinate));
    }
    guide->setPen(QPen(alignmentGuideColor(), isPortSnap ? 1.5 : 1.0, Qt::DashLine));
    guide->setVisible(true);
}

void DragSessionDelegate::clearAlignmentGuides() {
    if (alignGuideV_ != nullptr) {
        alignGuideV_->setVisible(false);
    }
    if (alignGuideH_ != nullptr) {
        alignGuideH_->setVisible(false);
    }
}

void DragSessionDelegate::showSegmentMergeGuide(bool vertical, qreal coordinate, const QRectF& draggedSpan,
                                                const QRectF& partnerSpan) {
    ensureAlignmentGuides();
    showAlignmentGuide(vertical ? alignGuideV_ : alignGuideH_, vertical, coordinate, draggedSpan, partnerSpan,
                       /*isPortSnap=*/true);
    if (QGraphicsLineItem* other = vertical ? alignGuideH_ : alignGuideV_) {
        other->setVisible(false);
    }
}

bool DragSessionDelegate::verticalGuideVisible() const {
    return alignGuideV_ != nullptr && alignGuideV_->isVisible();
}

bool DragSessionDelegate::horizontalGuideVisible() const {
    return alignGuideH_ != nullptr && alignGuideH_->isVisible();
}

QPointF DragSessionDelegate::resolvePillSnap(quint64 transitionId, QPointF proposedCenter) {
    const Machine* machine = machine_();
    if (machine == nullptr) {
        clearAlignmentGuides();
        return proposedCenter;
    }
    const Transition* transition = nullptr;
    for (const Transition& t : machine->transitions) {
        if (t.id == transitionId) {
            transition = &t;
            break;
        }
    }
    if (transition == nullptr || transition->from == 0 || transition->to == 0 || transition->from == transition->to) {
        clearAlignmentGuides();
        return proposedCenter;
    }

    StateItem* sItem = items_.stateItems().value(transition->from, nullptr);
    StateItem* tItem = items_.stateItems().value(transition->to, nullptr);
    TransitionLabelItem* labelItem = items_.transitionLabels().value(transitionId, nullptr);
    if (sItem == nullptr || tItem == nullptr || labelItem == nullptr) {
        clearAlignmentGuides();
        return proposedCenter;
    }

    const QRectF sRect = sItem->sceneRect();
    const QRectF tRect = tItem->sceneRect();

    struct AxisSnap {
        bool found = false;
        qreal delta = 0.0;
        qreal targetCoord = 0.0;
        QRectF refRect;
    };

    AxisSnap snapY;
    AxisSnap snapX;

    // Y axis: the shared corridor when both ends are level, else the nearer port line.
    const qreal sCenterY = sRect.center().y();
    const qreal tCenterY = tRect.center().y();
    if (std::abs(sCenterY - tCenterY) <= 1.5) {
        const qreal dy = std::abs(proposedCenter.y() - sCenterY);
        if (dy <= kPillPortSnapThreshold) {
            snapY = AxisSnap{true, dy, sCenterY, sRect.united(tRect)};
        }
    } else {
        const qreal dyS = std::abs(proposedCenter.y() - sCenterY);
        if (dyS <= kPillPortSnapThreshold) {
            snapY = AxisSnap{true, dyS, sCenterY, sRect};
        }
        const qreal dyT = std::abs(proposedCenter.y() - tCenterY);
        if (dyT <= kPillPortSnapThreshold) {
            if (!snapY.found || dyT < snapY.delta) {
                snapY = AxisSnap{true, dyT, tCenterY, tRect};
            }
        }
    }

    // X axis, likewise.
    const qreal sCenterX = sRect.center().x();
    const qreal tCenterX = tRect.center().x();
    if (std::abs(sCenterX - tCenterX) <= 1.5) {
        const qreal dx = std::abs(proposedCenter.x() - sCenterX);
        if (dx <= kPillPortSnapThreshold) {
            snapX = AxisSnap{true, dx, sCenterX, sRect.united(tRect)};
        }
    } else {
        const qreal dxS = std::abs(proposedCenter.x() - sCenterX);
        if (dxS <= kPillPortSnapThreshold) {
            snapX = AxisSnap{true, dxS, sCenterX, sRect};
        }
        const qreal dxT = std::abs(proposedCenter.x() - tCenterX);
        if (dxT <= kPillPortSnapThreshold) {
            if (!snapX.found || dxT < snapX.delta) {
                snapX = AxisSnap{true, dxT, tCenterX, tRect};
            }
        }
    }

    QPointF result = proposedCenter;
    if (snapY.found) {
        result.setY(snapY.targetCoord);
    }
    if (snapX.found) {
        result.setX(snapX.targetCoord);
    }

    ensureAlignmentGuides();
    const QRectF finalPillRect = labelItem->sceneRectAt(result);

    if (snapY.found) {
        showAlignmentGuide(alignGuideH_, /*vertical=*/false, snapY.targetCoord, finalPillRect, snapY.refRect,
                           /*isPortSnap=*/true);
    } else if (alignGuideH_ != nullptr) {
        alignGuideH_->setVisible(false);
    }

    if (snapX.found) {
        showAlignmentGuide(alignGuideV_, /*vertical=*/true, snapX.targetCoord, finalPillRect, snapX.refRect,
                           /*isPortSnap=*/true);
    } else if (alignGuideV_ != nullptr) {
        alignGuideV_->setVisible(false);
    }

    return result;
}

void DragSessionDelegate::forgetSceneItems() {
    alignGuideV_ = nullptr;
    alignGuideH_ = nullptr;
    clearSessionState();
}

DragSessionDelegate::NodeCommitPlan DragSessionDelegate::planNodeCommit(quint64 stateId, QPointF pos,
                                                                        quint64 dropTargetId) const {
    NodeCommitPlan plan;
    const Machine* machine = machine_();
    const State* state = machine != nullptr ? findState(*machine, stateId) : nullptr;
    const quint64 currentParentId = state != nullptr ? state->parentId : 0;
    plan.reparent = dropTargetId != currentParentId;
    plan.reparentTo = dropTargetId;
    if (plan.reparent) {
        // Logged so an unintended escape shows up as "... -> 0".
        qDebug() << "sd.drag reparent: state" << stateId << "parent" << currentParentId << "->" << dropTargetId
                 << "at" << pos;
    }
    const bool subtreeDrag = subtreeDragRootId_ == stateId;
    plan.needsBatch = subtreeDrag || plan.reparent;

    plan.moves.push_back(NodeCommitPlan::Move{stateId, pos});
    if (subtreeDrag) {
        // Each descendant needs its own move to persist its shifted position.
        for (auto it = subtreeDragDescendantStart_.constBegin(); it != subtreeDragDescendantStart_.constEnd(); ++it) {
            if (StateItem* item = items_.stateItems().value(it.key(), nullptr)) {
                plan.moves.push_back(NodeCommitPlan::Move{it.key(), item->pos()});
            }
        }
    }
    return plan;
}

}  // namespace app
