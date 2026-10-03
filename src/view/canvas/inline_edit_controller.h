#pragma once

#include <functional>

#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>

#include "view/canvas/note_editor_fsm.h"

class QEvent;
class QGraphicsProxyWidget;
class QGraphicsScene;
class QLineEdit;
class QTextEdit;

namespace app {

// In-place canvas editors: a QLineEdit for state/transition names and a
// WYSIWYG markdown QTextEdit for note bodies, whose input rules come from
// NoteEditorFsm. A note commits on every close path, Esc included, and its
// Ctrl+Z/Y is the editor's own undo. Widget mechanics only: the presenter
// owns the FSM session and sends the intents via the callbacks below.
class InlineEditController : public QObject {
    Q_OBJECT

public:
    // `scene` is not owned.
    explicit InlineEditController(QGraphicsScene* scene);

    // Line edit: editingFinished. Note: Esc or focus-out.
    std::function<void()> onCommitRequested;
    // Esc in the line edit.
    std::function<void()> onAbortRequested;
    // Ctrl+Z (redo=false) / Ctrl+Y or Ctrl+Shift+Z (redo=true) in the line
    // edit; these act on the document, not the field's own text undo.
    std::function<void(bool redo)> onUndoRedoKey;

    // Where the editor sits over its target. The note editor has no formula:
    // it opens at the note's top-left and autosizes to its content.
    static QRectF editorRectForState(const QRectF& stateBox);
    static QRectF editorRectForTransition(QPointF labelPos, qreal labelWidth);

    // Builds the needed editor lazily, then shows and focuses it. A note
    // (`multiline`) loads markdown with the cursor at the end; a line edit
    // starts with its text selected.
    void begin(bool multiline, const QString& initialText, const QRectF& editorRect);
    // Hiding re-fires editingFinished/FocusOut; the presenter must have
    // cleared its session first so that commit is a no-op.
    void hide();
    // After scene_->clear() deleted both proxies: drop the dangling pointers.
    void forgetSceneOwnedWidgets();

    // Empty if no editor is open. A note serializes back to GitHub markdown
    // with trailing newlines chopped.
    QString currentText() const;

    // Probe levers.
    void debugSetText(const QString& text);      // fills the OPEN editor's text (no-op if none open)
    QTextEdit* debugNoteEditorWidget() const;    // nullptr unless the Note pair is the open one

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QGraphicsScene* scene_;
    // True while the note editor is the open one.
    bool multilineOpen_ = false;

    // Sizes the note editor to its laid-out document, at the same wrap column
    // NoteItem uses so both lay the content out identically.
    void applyNoteAutosize();

    QGraphicsProxyWidget* lineProxy_ = nullptr;
    QLineEdit* lineField_ = nullptr;  // owned by lineProxy_

    QGraphicsProxyWidget* areaProxy_ = nullptr;
    QTextEdit* area_ = nullptr;  // owned by areaProxy_
    NoteEditorFsm noteFsm_;
};

}  // namespace app
