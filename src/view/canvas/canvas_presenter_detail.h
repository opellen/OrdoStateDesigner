#pragma once

#include <ordo/core/presenter_context.h>

#include "model/undo_events.h"

// Internal to the canvas_presenter*.cpp files; include from nowhere else.
namespace app::presenter_detail {

// Opens an undo batch on construction and closes it on destruction, so the
// pair always balances. Keep it within one synchronous call stack.
class ScopedUndoBatch {
public:
    explicit ScopedUndoBatch(ordo::core::PresenterContext& context) : context_(context) {
        context_.send(events::BeginUndoBatchRequested{});
    }
    ~ScopedUndoBatch() { context_.send(events::EndUndoBatchRequested{}); }

    ScopedUndoBatch(const ScopedUndoBatch&) = delete;
    ScopedUndoBatch& operator=(const ScopedUndoBatch&) = delete;

private:
    ordo::core::PresenterContext& context_;
};

}  // namespace app::presenter_detail
