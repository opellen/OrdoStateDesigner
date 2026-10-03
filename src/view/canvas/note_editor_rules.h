#pragma once

#include <QChar>
#include <QString>
#include <QTextCursor>

class QTextDocument;

namespace app {

// Stateless markdown input rules for the note editor: predicates for the
// machine's guards and document transforms for its actions.
// Predicates see the block text BEFORE the pending key is inserted, so for
// "# " the block text is "#" and Space is the pending key.
// Every transform is one edit block, so a single undo() reverts it.
namespace note_editor_rules {

// 1-4 when the cursor's block text up to the cursor is that many '#' and
// nothing else, on a still-unformatted block; 0 otherwise.
int headingMarkerLevel(const QTextCursor& cursor);
// Block text before the cursor is exactly "-" or "*" on an unformatted block.
bool isBulletMarker(const QTextCursor& cursor);
// Block text before the cursor is "<digits>." (1-3 digits) on an unformatted block.
bool isNumberMarker(const QTextCursor& cursor);

// A completed inline pair, found at guard time and applied at action time.
struct InlineMatch {
    enum class Kind { None, Bold, Italic, Code };
    Kind kind = Kind::None;
    int start = -1;   // block-relative position of the OPENING marker's first char
    int length = 0;   // content length between the markers
};
// The just-typed `typed` marker would close a pair in the cursor's block:
// '*' closes **bold** (text before cursor ends with the bold pair's 3rd
// '*') or *italic*; '`' closes `code`. Conservative: content non-empty,
// no nested marker inside, non-space against both markers.
InlineMatch findInlinePair(const QTextCursor& cursor, QChar typed);

bool inList(const QTextCursor& cursor);
bool onEmptyListItem(const QTextCursor& cursor);
// Cursor sits at position 0 of a heading or list block, no selection.
bool atFormattedBlockStart(const QTextCursor& cursor);

void applyHeading(QTextCursor cursor, int level);
void applyBullet(QTextCursor cursor);
void applyNumber(QTextCursor cursor);
void applyInline(QTextCursor cursor, const InlineMatch& match);
// Enter outside a list: new block; leaving a heading resets the fresh
// block to a plain paragraph (a heading does not continue).
void insertPlainBlock(QTextCursor cursor);
// Enter inside a non-empty list item: next item (block format inherited).
void continueList(QTextCursor cursor);
// Enter on an EMPTY list item: the item leaves the list, plain paragraph.
void exitList(QTextCursor cursor);
// Backspace at a formatted block's start: heading/list membership drops,
// text stays.
void clearBlockFormat(QTextCursor cursor);
// delta +1 / -1, indent clamped at 1; bullets cycle disc -> circle -> square by depth.
void indentListItem(QTextCursor cursor, int delta);

// Narrow indent steps and depth-cycled bullets. Apply to both the editor and
// NoteItem's document so they render identically. Idempotent.
void applyListPresentation(QTextDocument* document);

}  // namespace note_editor_rules

}  // namespace app
