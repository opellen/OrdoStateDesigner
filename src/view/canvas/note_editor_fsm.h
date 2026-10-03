#pragma once

#include <optional>

#include <QChar>
#include <QString>

#include "view/canvas/note_editor_rules.h"
#include "view/generated/note_editor_core.h"

class QKeyEvent;
class QInputMethodEvent;
class QTextEdit;

namespace app {

// Adapter over the generated note-editor core, which decides which input rule
// fires, when rules are suspended (IME composition), and when Backspace
// reverts an autoformat. This class implements the guards and actions over
// the live QTextEdit via note_editor_rules, and holds per-keystroke payloads.
// Every routed key's candidate group is total, so a routed key is always
// fully handled by the fired action. One core per editing session.
class NoteEditorFsm : public generated::note_editor::NoteEditorGuards,
                      public generated::note_editor::NoteEditorActions {
public:
    using State = generated::note_editor::NoteEditorState;

    // `editor` is not owned; must outlive this object or be re-attached.
    void attach(QTextEdit* editor);
    // Fresh core for a fresh editing session.
    void reset();

    // True: the key was handled and the caller must swallow the event.
    // False: not a routed key; let Qt handle it.
    bool handleKeyPress(const QKeyEvent* event);
    // Reports IME preedit start/end so rules are suspended while composing.
    // Never consumes the event.
    void handleInputMethod(const QInputMethodEvent* event);

    bool isActive(State state) const { return core_.has_value() && core_->isActive(state); }

    // ---- generated::note_editor::NoteEditorGuards ----
    bool blockIsHeadingMarker() override;
    bool blockIsBulletMarker() override;
    bool blockIsNumberMarker() override;
    bool completesInlinePair() override;
    bool onEmptyListItem() override;
    bool inList() override;
    bool atFormattedBlockStart() override;

    // ---- generated::note_editor::NoteEditorActions ----
    void applyHeadingFormat() override;
    void applyBulletFormat() override;
    void applyNumberFormat() override;
    void applyInlineFormat() override;
    void insertSpace() override;
    void insertMarker() override;
    void insertNewline() override;
    void continueList() override;
    void exitList() override;
    void deleteBackward() override;
    void clearBlockFormat() override;
    void indentListItem() override;
    void outdentListItem() override;
    void insertTab() override;
    void passthroughKey() override;
    void revertAutoformat() override;

private:
    // Call after a block transform: QTextEdit's insertion format does not
    // follow edits made through cursor copies, so the next typed text would
    // come out plain inside a heading.
    void syncInsertionFormat();

    QTextEdit* editor_ = nullptr;
    std::optional<generated::note_editor::NoteEditorCore> core_;

    // Written before each dispatch, read by the fired guard/action.
    QString pendingText_;
    QChar pendingMarker_;
    note_editor_rules::InlineMatch pendingInlineMatch_;  // cached at guard time, applied at action time
    bool composing_ = false;  // preedit edge detector; the mode itself lives in the machine
};

}  // namespace app
