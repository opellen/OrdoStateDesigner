#pragma once

#include <functional>

#include <QObject>
#include <QTimer>

namespace app {

// The simulation's single time source. It never reads SimulationAgent; it only
// calls `onTick` (wired to events::TickElapsed). Real mode ticks a ~50ms QTimer,
// manual mode ticks only via advanceTicks(); all ticks run on the owning UI thread.
// RunCommand/PauseCommand/ResetCommand sync start()/stop() to running().
// SetModeCommand does not stop the clock on leaving Simulate mid-run; the extra
// ticks are no-ops, only wasted wakeups.
class SimClock : public QObject {
public:
    explicit SimClock(QObject* parent = nullptr);

    // Called with kRealTickMs from the real-mode timer, or with the caller's ms
    // from advanceTicks().
    std::function<void(int)> onTick;

    // Manual mode: start() becomes a no-op and advanceTicks() is the only way
    // time moves. Set it once, before any start(); no event loop is then needed.
    void setManualMode(bool manual);
    bool manualMode() const { return manualMode_; }

    // Real mode only: starts the ~50ms QTimer. No-op in manual mode or if started.
    void start();

    // Stops the QTimer; no-op if not running.
    void stop();

    // Manual mode: calls onTick(elapsedMs) synchronously on the caller's stack.
    void advanceTicks(int elapsedMs);

    void setTimeScale(double scale);
    double timeScale() const { return timeScale_; }

private:
    static constexpr int kRealTickMs = 50;

    QTimer timer_;
    bool manualMode_ = false;
    double timeScale_ = 1.0;
};

}  // namespace app
