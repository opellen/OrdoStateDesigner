#include "view/canvas/sim_presentation.h"

#include <QColor>
#include <QGraphicsEllipseItem>
#include <QGraphicsPathItem>
#include <QGraphicsPolygonItem>
#include <QVariant>
#include <QVariantAnimation>

#include "constants/design_tokens.h"
#include "infra/expression.h"
#include "model/machine_doc.h"
#include "view/items/state_item.h"
#include "view/items/transition_item.h"

namespace app {

namespace {

// The initial marker's plain items have no setDimmed(), so they use opacity.
constexpr qreal kMarkerDimmedOpacity = 0.3;

constexpr int kPulseDurationMs = 300;
constexpr int kPopDurationMs = 150;
constexpr qreal kPopPeakScale = 1.06;
QColor pulseStartColor() { return design::color(design::kAccentPulse); }
// Must match transition_item.cpp's edge color.
QColor pulseRestColor() { return design::color(design::kEdge); }

}  // namespace

SimPresentation::SimPresentation(QObject* animationOwner) : owner_(animationOwner) {}

void SimPresentation::setConfiguration(const QVector<quint64>& activeIds) {
    configuration_.clear();
    for (quint64 id : activeIds) {
        configuration_.insert(id);
    }
}

void SimPresentation::apply(const Scene& scene) {
    for (auto it = scene.stateItems.begin(); it != scene.stateItems.end(); ++it) {
        const bool isActive = scene.simulateRunning && configuration_.contains(it.key());
        it.value()->setActiveGlow(isActive);
        it.value()->setDimmed(scene.simulateRunning && !isActive);
    }

    for (auto it = scene.transitionItems.begin(); it != scene.transitionItems.end(); ++it) {
        const quint64 id = it.key();
        const Transition* transition = scene.doc != nullptr ? scene.doc->findTransition(id) : nullptr;
        // Live when its source is active, or always for a root transition
        // (from == 0), which fires from any active state.
        const bool leavesActive = scene.simulateRunning && transition != nullptr &&
                                   (transition->from == 0 || configuration_.contains(transition->from));
        const bool isTimeTrigger = transition != nullptr && (expr::parseTimeTrigger(transition->event).ok || (transition->event.trimmed().isEmpty() && transition->delayMs > 0));
        const bool fireable = leavesActive && !isTimeTrigger && !transition->event.trimmed().isEmpty();
        const bool waiting = leavesActive && isTimeTrigger;
        const bool inActiveSet = fireable || waiting;

        it.value()->setDimmed(scene.simulateRunning && !inActiveSet);
        if (TransitionLabelItem* label = scene.transitionLabels.value(id, nullptr)) {
            label->setFireable(fireable);
            label->setWaitingAccent(waiting);
            label->setDimmed(scene.simulateRunning && !inActiveSet);
        }
    }

    // The initial marker is a design-time affordance: dim it whenever running.
    const qreal markerOpacity = scene.simulateRunning ? kMarkerDimmedOpacity : 1.0;
    if (scene.initialDot != nullptr) {
        scene.initialDot->setOpacity(markerOpacity);
    }
    if (scene.initialStub != nullptr) {
        scene.initialStub->setOpacity(markerOpacity);
    }
    if (scene.initialArrow != nullptr) {
        scene.initialArrow->setOpacity(markerOpacity);
    }
}

void SimPresentation::playStatePop(quint64 stateId, const QHash<quint64, StateItem*>& stateItems) {
    StateItem* item = stateItems.value(stateId, nullptr);
    if (item == nullptr) {
        return;
    }
    // A second fire mid-animation replaces the first.
    if (QVariantAnimation* existing = popAnimations_.take(stateId)) {
        existing->stop();
        existing->deleteLater();
    }
    item->setTransformOriginPoint(item->boundingRect().center());

    auto* anim = new QVariantAnimation(owner_);
    anim->setDuration(kPopDurationMs);
    anim->setKeyValueAt(0.0, 1.0);
    anim->setKeyValueAt(0.5, kPopPeakScale);
    anim->setKeyValueAt(1.0, 1.0);
    // Look the target up by id, never capture the item pointer.
    QObject::connect(anim, &QVariantAnimation::valueChanged, owner_, [&stateItems, stateId](const QVariant& value) {
        if (StateItem* target = stateItems.value(stateId, nullptr)) {
            target->setScale(value.toReal());
        }
    });
    QObject::connect(anim, &QVariantAnimation::finished, owner_, [this, &stateItems, stateId] {
        if (StateItem* target = stateItems.value(stateId, nullptr)) {
            target->setScale(1.0);
        }
        popAnimations_.remove(stateId);
    });
    popAnimations_.insert(stateId, anim);
    anim->start();
}

void SimPresentation::pulseTransition(quint64 transitionId, const QHash<quint64, TransitionItem*>& transitionItems) {
    TransitionItem* edge = transitionItems.value(transitionId, nullptr);
    if (edge == nullptr) {
        return;
    }
    if (QVariantAnimation* existing = pulseAnimations_.take(transitionId)) {
        existing->stop();
        existing->deleteLater();
    }

    auto* anim = new QVariantAnimation(owner_);
    anim->setDuration(kPulseDurationMs);
    anim->setStartValue(pulseStartColor());
    anim->setEndValue(pulseRestColor());
    QObject::connect(anim, &QVariantAnimation::valueChanged, owner_,
                      [&transitionItems, transitionId](const QVariant& value) {
                          if (TransitionItem* target = transitionItems.value(transitionId, nullptr)) {
                              target->setPulseColor(value.value<QColor>());
                          }
                      });
    QObject::connect(anim, &QVariantAnimation::finished, owner_, [this, &transitionItems, transitionId] {
        if (TransitionItem* target = transitionItems.value(transitionId, nullptr)) {
            target->clearPulse();
        }
        pulseAnimations_.remove(transitionId);
    });
    pulseAnimations_.insert(transitionId, anim);
    anim->start();
}

void SimPresentation::stopStatePop(quint64 stateId) {
    if (QVariantAnimation* anim = popAnimations_.take(stateId)) {
        anim->stop();
        anim->deleteLater();
    }
}

void SimPresentation::stopTransitionPulse(quint64 transitionId) {
    if (QVariantAnimation* anim = pulseAnimations_.take(transitionId)) {
        anim->stop();
        anim->deleteLater();
    }
}

void SimPresentation::stopAll(const QHash<quint64, StateItem*>& stateItems,
                               const QHash<quint64, TransitionItem*>& transitionItems) {
    for (auto it = pulseAnimations_.begin(); it != pulseAnimations_.end(); ++it) {
        it.value()->stop();
        it.value()->deleteLater();
        if (TransitionItem* edge = transitionItems.value(it.key(), nullptr)) {
            edge->clearPulse();
        }
    }
    pulseAnimations_.clear();

    for (auto it = popAnimations_.begin(); it != popAnimations_.end(); ++it) {
        it.value()->stop();
        it.value()->deleteLater();
        if (StateItem* state = stateItems.value(it.key(), nullptr)) {
            state->setScale(1.0);
        }
    }
    popAnimations_.clear();
}

}  // namespace app
