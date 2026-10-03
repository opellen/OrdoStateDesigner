#include "view/canvas/inline_edit_controller.h"

#include <algorithm>
#include <cmath>

#include <QColor>
#include <QFont>
#include <QFrame>
#include <QGraphicsProxyWidget>
#include <QPalette>
#include <QGraphicsScene>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextOption>

#include "constants/design_tokens.h"

namespace {

// Keep in sync with note_item.cpp so editor and item wrap at the same column.
// kEditorFrame is the stylesheet's 8px padding plus 1px border.
constexpr double kNoteColumnMax = 264.0;
constexpr double kNoteMinWidth = 96.0;
constexpr double kNoteMinHeight = 40.0;
constexpr double kNoteTextPadding = 10.0;
constexpr double kEditorFrame = 9.0;

}  // namespace

namespace app {

InlineEditController::InlineEditController(QGraphicsScene* scene) : scene_(scene) {}

QRectF InlineEditController::editorRectForState(const QRectF& stateBox) {
    // Inside the name band; long names scroll rather than widen the editor.
    return QRectF(stateBox.left() + 5.0, stateBox.top() + 4.0, std::max(stateBox.width() - 10.0, 40.0), 22.0);
}

QRectF InlineEditController::editorRectForTransition(QPointF labelPos, qreal labelWidth) {
    // Centered on the pill, with typing room that may overhang it.
    const qreal width = std::max(labelWidth + 16.0, 72.0);
    return QRectF(labelPos.x() - width / 2.0, labelPos.y() - 11.0, width, 22.0);
}

void InlineEditController::begin(bool multiline, const QString& initialText, const QRectF& editorRect) {
    multilineOpen_ = multiline;

    // Two proxies rather than one with setWidget(), which reparents and
    // transfers ownership. Only one session is ever open, so only one shows.
    if (multiline) {
        if (areaProxy_ == nullptr) {
            area_ = new QTextEdit();
            // 8px padding ~= NoteItem's 10px text inset minus the 1px
            // border, so the editor's real wrap column tracks the item's.
            area_->setStyleSheet(design::resolveRoles(QStringLiteral(
                "background: {surface-2}; color: {text-primary}; border: 1px solid {outline-focus}; border-radius: 3px; padding: 8px;")));
            area_->setFrameShape(QFrame::NoFrame);
            area_->setWordWrapMode(QTextOption::WordWrap);  // matches NoteItem's own word-wrap
            area_->setAcceptRichText(false);  // pastes come in plain -- no foreign formats
            QFont editorFont = area_->font();
            editorFont.setPointSizeF(9.0);  // NoteItem's noteFont() -- heading scales derive from it
            area_->setFont(editorFont);
            area_->document()->setDefaultFont(editorFont);
            // The widget grows with its content, so there is nothing to scroll.
            area_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            area_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            // The caret is painted with the palette Text role; the stylesheet
            // `color` does not reliably reach it.
            area_->setCursorWidth(2);
            QPalette caretPalette = area_->palette();
            caretPalette.setColor(QPalette::Text, design::color(design::kTextPrimary));
            area_->setPalette(caretPalette);
            connect(area_, &QTextEdit::textChanged, this, [this] { applyNoteAutosize(); });
            area_->installEventFilter(this);  // key routing/Esc/focus-out -- see eventFilter()
            noteFsm_.attach(area_);
            areaProxy_ = scene_->addWidget(area_);  // proxy owns the widget
            areaProxy_->setZValue(100.0);  // the reserved interaction z-band
        }
        noteFsm_.reset();
        area_->document()->setDefaultFont(area_->font());
        area_->document()->setMarkdown(initialText, QTextDocument::MarkdownDialectGitHub);
        note_editor_rules::applyListPresentation(area_->document());
        QTextCursor cursor = area_->textCursor();
        cursor.movePosition(QTextCursor::End);
        area_->setTextCursor(cursor);
        applyNoteAutosize();  // open sized to the CURRENT content -- editorRect is only the anchor
        areaProxy_->setPos(editorRect.topLeft());
        areaProxy_->show();
        // clearFocus() first: on a reused proxy widget a bare setFocus() sends
        // no FocusIn, so the caret never blinks.
        area_->clearFocus();
        areaProxy_->setFocus();
        area_->setFocus();
        return;
    }

    if (lineProxy_ == nullptr) {
        lineField_ = new QLineEdit();
        lineField_->setStyleSheet(design::resolveRoles(QStringLiteral(
            "background: {surface-2}; color: {text-primary}; border: 1px solid {outline-focus}; border-radius: 3px; padding: 0 4px;")));
        lineField_->installEventFilter(this);  // Esc -- see eventFilter()
        connect(lineField_, &QLineEdit::editingFinished, this, [this] {
            if (onCommitRequested) {
                onCommitRequested();
            }
        });
        lineProxy_ = scene_->addWidget(lineField_);  // proxy owns the field
        lineProxy_->setZValue(100.0);  // the reserved interaction z-band
    }
    lineField_->setText(initialText);
    lineField_->selectAll();
    lineField_->setFixedSize(static_cast<int>(editorRect.width()), static_cast<int>(editorRect.height()));
    lineProxy_->setPos(editorRect.topLeft());
    lineProxy_->show();
    lineProxy_->setFocus();
    lineField_->setFocus();
}

void InlineEditController::hide() {
    if (multilineOpen_) {
        if (areaProxy_ != nullptr) {
            areaProxy_->hide();  // re-fires focus-out; the presenter's cleared FSM target no-ops it
        }
    } else if (lineProxy_ != nullptr) {
        lineProxy_->hide();  // re-fires editingFinished; the presenter's cleared FSM target no-ops it
    }
    multilineOpen_ = false;
}

void InlineEditController::applyNoteAutosize() {
    if (!multilineOpen_ || area_ == nullptr) {
        return;
    }
    // Ideal width capped at the note column; the fixed size makes QTextEdit
    // re-pin textWidth to the viewport at exactly that width.
    QTextDocument* document = area_->document();
    document->setTextWidth(-1.0);
    const double contentWidth =
        std::min(document->idealWidth(), kNoteColumnMax - kNoteTextPadding * 2.0);
    document->setTextWidth(contentWidth);
    const double width =
        std::clamp(contentWidth + kEditorFrame * 2.0, kNoteMinWidth, kNoteColumnMax);
    const double height = std::max(document->size().height() + kEditorFrame * 2.0, kNoteMinHeight);
    area_->setFixedSize(static_cast<int>(std::ceil(width)), static_cast<int>(std::ceil(height)));
    area_->ensureCursorVisible();
}

void InlineEditController::forgetSceneOwnedWidgets() {
    lineProxy_ = nullptr;
    lineField_ = nullptr;
    areaProxy_ = nullptr;
    area_ = nullptr;
    multilineOpen_ = false;
}

QString InlineEditController::currentText() const {
    if (multilineOpen_) {
        if (area_ == nullptr) {
            return QString();
        }
        // toMarkdown appends block-separator newlines; chop them so round
        // trips stay byte-comparable.
        QString markdown = area_->document()->toMarkdown(QTextDocument::MarkdownDialectGitHub);
        while (markdown.endsWith(QLatin1Char('\n'))) {
            markdown.chop(1);
        }
        return markdown;
    }
    return lineField_ != nullptr ? lineField_->text() : QString();
}

void InlineEditController::debugSetText(const QString& text) {
    if (multilineOpen_) {
        if (area_ == nullptr) {
            return;
        }
        area_->setPlainText(text);
    } else {
        if (lineField_ == nullptr) {
            return;
        }
        lineField_->setText(text);
    }
}

QTextEdit* InlineEditController::debugNoteEditorWidget() const { return multilineOpen_ ? area_ : nullptr; }

bool InlineEditController::eventFilter(QObject* watched, QEvent* event) {
    if (watched == lineField_ && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            if (onAbortRequested) {
                onAbortRequested();
            }
            return true;
        }
        // Undo/redo act on the document; QLineEdit would otherwise consume them.
        if (keyEvent->matches(QKeySequence::Undo)) {
            if (onUndoRedoKey) {
                onUndoRedoKey(false);
            }
            return true;
        }
        if (keyEvent->matches(QKeySequence::Redo) ||
            (keyEvent->key() == Qt::Key_Z && keyEvent->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier))) {
            if (onUndoRedoKey) {
                onUndoRedoKey(true);
            }
            return true;
        }
    }
    // Note editor: Esc commits (notes have no abort), Ctrl+B/I toggle formats,
    // other keys go to NoteEditorFsm. Ctrl+Z/Y fall through to QTextEdit's
    // own undo stack, which autoformat reverts also use.
    if (watched == area_) {
        if (event->type() == QEvent::KeyPress) {
            auto* keyEvent = static_cast<QKeyEvent*>(event);
            if (keyEvent->key() == Qt::Key_Escape) {
                if (onCommitRequested) {
                    onCommitRequested();
                }
                return true;
            }
            if (keyEvent->matches(QKeySequence::Bold)) {
                area_->setFontWeight(area_->fontWeight() > QFont::Normal ? QFont::Normal : QFont::Bold);
                return true;
            }
            if (keyEvent->matches(QKeySequence::Italic)) {
                area_->setFontItalic(!area_->fontItalic());
                return true;
            }
            if (noteFsm_.handleKeyPress(keyEvent)) {
                return true;
            }
        } else if (event->type() == QEvent::InputMethod) {
            // Tracks composition start/end only; never consumed.
            noteFsm_.handleInputMethod(static_cast<QInputMethodEvent*>(event));
        } else if (event->type() == QEvent::FocusOut) {
            if (onCommitRequested) {
                onCommitRequested();
            }
            // Not consumed: the focus change must still proceed.
        }
    }
    return QObject::eventFilter(watched, event);
}

}  // namespace app
