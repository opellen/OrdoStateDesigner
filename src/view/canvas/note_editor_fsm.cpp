#include "view/canvas/note_editor_fsm.h"

#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QTextCursor>
#include <QTextEdit>

namespace app {

namespace {

// '_' is deliberately excluded: snake_case words would trigger italics.
bool isInlineMarkerChar(const QString& text) {
    return text == QStringLiteral("*") || text == QStringLiteral("`");
}

}  // namespace

void NoteEditorFsm::attach(QTextEdit* editor) { editor_ = editor; }

void NoteEditorFsm::reset() {
    core_.emplace(*this, *this);  // fresh session: initial descent Editing -> Ready
    pendingText_.clear();
    pendingMarker_ = QChar();
    pendingInlineMatch_ = {};
    composing_ = false;
}

bool NoteEditorFsm::handleKeyPress(const QKeyEvent* event) {
    if (editor_ == nullptr || !core_.has_value()) {
        return false;
    }
    // Shortcut chords never route.
    if (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
        return false;
    }
    const int key = event->key();
    if (key == Qt::Key_Space) {
        pendingText_ = QStringLiteral(" ");
        core_->spaceTypedRequested();
        return true;
    }
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        // Shift+Enter is a plain Enter too.
        core_->enterTypedRequested();
        return true;
    }
    if (key == Qt::Key_Backspace) {
        core_->backspaceTypedRequested();
        return true;
    }
    if (key == Qt::Key_Tab) {
        core_->tabTypedRequested();
        return true;
    }
    if (key == Qt::Key_Backtab) {
        core_->shiftTabTypedRequested();
        return true;
    }
    const QString text = event->text();
    if (isInlineMarkerChar(text)) {
        pendingText_ = text;
        pendingMarker_ = text.front();
        core_->markerTypedRequested();
        return true;
    }
    if (!text.isEmpty() && text.front().isPrint()) {
        // Routed so any content input disarms the Backspace revert.
        pendingText_ = text;
        core_->otherKeyTypedRequested();
        return true;
    }
    return false;  // navigation/function keys: Qt's default handling
}

void NoteEditorFsm::handleInputMethod(const QInputMethodEvent* event) {
    if (!core_.has_value()) {
        return;
    }
    const bool preeditLive = !event->preeditString().isEmpty();
    if (preeditLive && !composing_) {
        composing_ = true;
        core_->compositionStartedRequested();
    } else if (!preeditLive && composing_) {
        composing_ = false;
        core_->compositionEndedRequested();
    }
}

// ---- guards -- every answer derived from the live document ----------------

bool NoteEditorFsm::blockIsHeadingMarker() {
    return note_editor_rules::headingMarkerLevel(editor_->textCursor()) > 0;
}
bool NoteEditorFsm::blockIsBulletMarker() { return note_editor_rules::isBulletMarker(editor_->textCursor()); }
bool NoteEditorFsm::blockIsNumberMarker() { return note_editor_rules::isNumberMarker(editor_->textCursor()); }

bool NoteEditorFsm::completesInlinePair() {
    pendingInlineMatch_ = note_editor_rules::findInlinePair(editor_->textCursor(), pendingMarker_);
    return pendingInlineMatch_.kind != note_editor_rules::InlineMatch::Kind::None;
}

bool NoteEditorFsm::onEmptyListItem() { return note_editor_rules::onEmptyListItem(editor_->textCursor()); }
bool NoteEditorFsm::inList() { return note_editor_rules::inList(editor_->textCursor()); }
bool NoteEditorFsm::atFormattedBlockStart() {
    return note_editor_rules::atFormattedBlockStart(editor_->textCursor());
}

// ---- actions -- transforms delegate to the pure rules unit ----------------

void NoteEditorFsm::syncInsertionFormat() {
    editor_->setCurrentCharFormat(editor_->textCursor().blockCharFormat());
}

void NoteEditorFsm::applyHeadingFormat() {
    note_editor_rules::applyHeading(editor_->textCursor(),
                                    note_editor_rules::headingMarkerLevel(editor_->textCursor()));
    syncInsertionFormat();
}
void NoteEditorFsm::applyBulletFormat() {
    note_editor_rules::applyBullet(editor_->textCursor());
    syncInsertionFormat();
}
void NoteEditorFsm::applyNumberFormat() {
    note_editor_rules::applyNumber(editor_->textCursor());
    syncInsertionFormat();
}
void NoteEditorFsm::applyInlineFormat() {
    QTextCursor cursor = editor_->textCursor();
    note_editor_rules::applyInline(cursor, pendingInlineMatch_);
    // applyInline reset the insertion format on ITS cursor copy; hand the
    // widget the same plain format for the next keystroke.
    QTextCursor after = editor_->textCursor();
    after.setCharFormat(QTextCharFormat());
    editor_->setTextCursor(after);
    editor_->setCurrentCharFormat(QTextCharFormat());
}
void NoteEditorFsm::insertSpace() { editor_->textCursor().insertText(pendingText_); }
void NoteEditorFsm::insertMarker() { editor_->textCursor().insertText(pendingText_); }
void NoteEditorFsm::insertNewline() {
    note_editor_rules::insertPlainBlock(editor_->textCursor());
    syncInsertionFormat();
}
void NoteEditorFsm::continueList() { note_editor_rules::continueList(editor_->textCursor()); }
void NoteEditorFsm::exitList() {
    note_editor_rules::exitList(editor_->textCursor());
    syncInsertionFormat();
}
void NoteEditorFsm::deleteBackward() { editor_->textCursor().deletePreviousChar(); }
void NoteEditorFsm::clearBlockFormat() {
    note_editor_rules::clearBlockFormat(editor_->textCursor());
    syncInsertionFormat();
}
void NoteEditorFsm::indentListItem() { note_editor_rules::indentListItem(editor_->textCursor(), +1); }
void NoteEditorFsm::outdentListItem() { note_editor_rules::indentListItem(editor_->textCursor(), -1); }
void NoteEditorFsm::insertTab() { editor_->textCursor().insertText(QStringLiteral("\t")); }
void NoteEditorFsm::passthroughKey() { editor_->textCursor().insertText(pendingText_); }

void NoteEditorFsm::revertAutoformat() {
    // Each autoformat is one edit block, so undo() is its exact inverse.
    editor_->undo();
    syncInsertionFormat();
}

}  // namespace app
