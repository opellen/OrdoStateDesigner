#pragma once

#include <QHash>
#include <QSet>
#include <QVector>
#include <QtGlobal>

class QObject;
class QGraphicsEllipseItem;
class QGraphicsPathItem;
class QGraphicsPolygonItem;
class QVariantAnimation;

namespace app {

class StateItem;
class TransitionItem;
class TransitionLabelItem;
class MachineDocAgent;

// Simulation look of the canvas: dim/glow/fireable/waiting flags on every
// item, plus the firing animations. `animationOwner` parents every
// animation, so they die with the presenter.
class SimPresentation {
public:
    explicit SimPresentation(QObject* animationOwner);

    // Mirrors ConfigurationChanged::activeIds; every member glows, containers
    // included. Empty means nothing is active.
    void setConfiguration(const QVector<quint64>& activeIds);
    const QSet<quint64>& configuration() const { return configuration_; }

    // The initial-marker items are only dimmed here, never created or moved.
    struct Scene {
        const QHash<quint64, StateItem*>& stateItems;
        const QHash<quint64, TransitionItem*>& transitionItems;
        const QHash<quint64, TransitionLabelItem*>& transitionLabels;
        const MachineDocAgent* doc = nullptr;
        bool simulateRunning = false;
        QGraphicsEllipseItem* initialDot = nullptr;
        QGraphicsPathItem* initialStub = nullptr;
        QGraphicsPolygonItem* initialArrow = nullptr;
    };
    void apply(const Scene& scene);

    // The animations look their target up by id in the passed hash on every
    // tick, so the hash is captured by reference and must outlive them.
    void playStatePop(quint64 stateId, const QHash<quint64, StateItem*>& stateItems);
    void pulseTransition(quint64 transitionId, const QHash<quint64, TransitionItem*>& transitionItems);

    // Stops every animation and resets each target to its rest value.
    void stopAll(const QHash<quint64, StateItem*>& stateItems, const QHash<quint64, TransitionItem*>& transitionItems);

    // Call just before deleting the target; no rest value is restored.
    void stopStatePop(quint64 stateId);
    void stopTransitionPulse(quint64 transitionId);

private:
    QObject* owner_;
    QSet<quint64> configuration_;

    // Targets are looked up by id on every tick, never held, so a deleted
    // target is a no-op. Removal paths still stop the animation first.
    QHash<quint64, QVariantAnimation*> pulseAnimations_;  // by transitionId
    QHash<quint64, QVariantAnimation*> popAnimations_;    // by stateId
};

}  // namespace app
