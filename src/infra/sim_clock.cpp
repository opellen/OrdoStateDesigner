#include <algorithm>

#include "infra/sim_clock.h"

namespace app {

SimClock::SimClock(QObject* parent) : QObject(parent) {
    // Functor-based connect needs no Q_OBJECT on SimClock itself -- `this`
    // is only a context object for connection lifetime, not a moc-generated
    // slot (the same reason examples/todomvc/infra/db_worker.h's DbRelay
    // gets away with no Q_OBJECT either, via QMetaObject::invokeMethod).
    QObject::connect(&timer_, &QTimer::timeout, this, [this] {
        if (onTick) {
            const int scaledMs = std::max(1, static_cast<int>(kRealTickMs * timeScale_));
            onTick(scaledMs);
        }
    });
}

void SimClock::setManualMode(bool manual) {
    manualMode_ = manual;
}

void SimClock::setTimeScale(double scale) {
    timeScale_ = scale > 0.0 ? scale : 1.0;
}

void SimClock::start() {
    if (manualMode_ || timer_.isActive()) {
        return;
    }
    timer_.start(kRealTickMs);
}

void SimClock::stop() {
    timer_.stop();
}

void SimClock::advanceTicks(int elapsedMs) {
    if (onTick) {
        const int scaledMs = std::max(1, static_cast<int>(elapsedMs * timeScale_));
        onTick(scaledMs);
    }
}

}  // namespace app
