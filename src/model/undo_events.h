#pragma once

#include <string_view>

namespace app::events {

// ---- Intents ----------------------------------------------------------------

struct UndoRequested {
    static constexpr std::string_view eventName = "UndoRequested";
};

struct RedoRequested {
    static constexpr std::string_view eventName = "RedoRequested";
};

// Every mutating command committed while a batch is open merges into one undo
// entry, pushed when the outermost End closes. Always sent as a balanced pair
// within one synchronous call stack, never left open across event-loop returns.
// Nesting is depth-counted; an inner pair merges into the outermost batch.
struct BeginUndoBatchRequested {
    static constexpr std::string_view eventName = "BeginUndoBatchRequested";
};

struct EndUndoBatchRequested {
    static constexpr std::string_view eventName = "EndUndoBatchRequested";
};

// ---- Facts --------------------------------------------------------------------

// Sent by UndoStore only when either flag actually changed, so every delivery is
// a real transition.
struct UndoStateChanged {
    static constexpr std::string_view eventName = "UndoStateChanged";
    bool canUndo = false;
    bool canRedo = false;
};

}  // namespace app::events
