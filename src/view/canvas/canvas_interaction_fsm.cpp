#include "view/canvas/canvas_interaction_fsm.h"

#include <string_view>

#include <QDebug>

namespace app {

CanvasInteractionFsm::CanvasInteractionFsm(Hooks hooks)
    : hooks_(std::move(hooks)), actions_(hooks_), core_(guards_, actions_) {
    // One trace line per committed transition, then the optional observer.
    core_.setOnStateChanged([this](State previous, State next) {
        qDebug().noquote() << QStringLiteral("[interaction] %1 -> %2")
                                  .arg(stateName(previous), stateName(next));
        if (stateObserver_) {
            stateObserver_(previous, next);
        }
    });
}

QLatin1String CanvasInteractionFsm::stateName(State state) {
    const std::string_view text = generated::canvas_interaction::toString(state);
    return QLatin1String(text.data(), static_cast<qsizetype>(text.size()));
}

}  // namespace app
