#include "view/canvas/note_editor_rules.h"

#include <algorithm>

#include <QFont>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTextList>
#include <QTextListFormat>

namespace app {
namespace note_editor_rules {

namespace {

// Per-depth list indent; Qt's 40px default is too wide for a note.
constexpr qreal kListIndentWidth = 16.0;

// Keeps cycling; Qt's markdown import stays on square from depth 3 on.
QTextListFormat::Style unorderedStyleForDepth(int indent) {
    switch ((indent - 1) % 3) {
        case 1:
            return QTextListFormat::ListCircle;
        case 2:
            return QTextListFormat::ListSquare;
        default:
            return QTextListFormat::ListDisc;
    }
}

bool isOrderedListStyle(QTextListFormat::Style style) {
    return style == QTextListFormat::ListDecimal || style == QTextListFormat::ListLowerAlpha ||
           style == QTextListFormat::ListUpperAlpha || style == QTextListFormat::ListLowerRoman ||
           style == QTextListFormat::ListUpperRoman;
}

QString textBeforeCursor(const QTextCursor& cursor) {
    const QTextBlock block = cursor.block();
    return block.text().left(cursor.position() - block.position());
}

// A "#" typed inside a heading or list item is literal text.
bool blockIsPlain(const QTextCursor& cursor) {
    return cursor.blockFormat().headingLevel() == 0 && cursor.currentList() == nullptr;
}

// Matches what setMarkdown() produces, so a live heading looks like the
// committed one.
QTextCharFormat headingCharFormat(const QTextDocument* document, int level) {
    static constexpr double kScale[] = {2.0, 1.5, 1.17, 1.0};
    QTextCharFormat format;
    format.setFontWeight(QFont::Bold);
    format.setFontPointSize(document->defaultFont().pointSizeF() * kScale[std::clamp(level, 1, 4) - 1]);
    return format;
}

// Removes [block start, cursor). Call inside the transform's edit block.
void removeMarkerPrefix(QTextCursor& cursor) {
    cursor.setPosition(cursor.block().position(), QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
}

}  // namespace

int headingMarkerLevel(const QTextCursor& cursor) {
    if (!blockIsPlain(cursor)) {
        return 0;
    }
    const QString text = textBeforeCursor(cursor);
    if (text.isEmpty() || text.size() > 4) {
        return 0;
    }
    for (const QChar& c : text) {
        if (c != QLatin1Char('#')) {
            return 0;
        }
    }
    return static_cast<int>(text.size());
}

bool isBulletMarker(const QTextCursor& cursor) {
    if (!blockIsPlain(cursor)) {
        return false;
    }
    const QString text = textBeforeCursor(cursor);
    return text == QStringLiteral("-") || text == QStringLiteral("*");
}

bool isNumberMarker(const QTextCursor& cursor) {
    if (!blockIsPlain(cursor)) {
        return false;
    }
    const QString text = textBeforeCursor(cursor);
    if (text.size() < 2 || text.size() > 4 || !text.endsWith(QLatin1Char('.'))) {
        return false;
    }
    for (qsizetype i = 0; i < text.size() - 1; ++i) {
        if (!text[i].isDigit()) {
            return false;
        }
    }
    return true;
}

InlineMatch findInlinePair(const QTextCursor& cursor, QChar typed) {
    InlineMatch match;
    // No pairs inside a code run.
    if (cursor.charFormat().fontFixedPitch()) {
        return match;
    }
    const QString text = textBeforeCursor(cursor);
    if (typed == QLatin1Char('`')) {
        const qsizetype open = text.lastIndexOf(QLatin1Char('`'));
        if (open < 0 || open == text.size() - 1) {
            return match;
        }
        match.kind = InlineMatch::Kind::Code;
        match.start = static_cast<int>(open);
        match.length = static_cast<int>(text.size() - open - 1);
        return match;
    }
    if (typed != QLatin1Char('*')) {
        return match;
    }
    // Bold: "**content*" + typed '*'.
    if (text.endsWith(QLatin1Char('*')) && text.size() >= 4) {
        const QString inner = text.left(text.size() - 1);
        const qsizetype open = inner.lastIndexOf(QStringLiteral("**"));
        if (open >= 0) {
            const QString content = inner.mid(open + 2);
            if (!content.isEmpty() && !content.contains(QLatin1Char('*')) && !content.front().isSpace() &&
                !content.back().isSpace()) {
                match.kind = InlineMatch::Kind::Bold;
                match.start = static_cast<int>(open);
                match.length = static_cast<int>(content.size());
                return match;
            }
        }
    }
    // Italic: "*content" + typed '*', where the opener is not part of "**".
    if (!text.endsWith(QLatin1Char('*'))) {
        const qsizetype open = text.lastIndexOf(QLatin1Char('*'));
        if (open >= 0 && (open == 0 || text[open - 1] != QLatin1Char('*'))) {
            const QString content = text.mid(open + 1);
            if (!content.isEmpty() && !content.contains(QLatin1Char('*')) && !content.front().isSpace() &&
                !content.back().isSpace()) {
                match.kind = InlineMatch::Kind::Italic;
                match.start = static_cast<int>(open);
                match.length = static_cast<int>(content.size());
                return match;
            }
        }
    }
    return match;
}

bool inList(const QTextCursor& cursor) { return cursor.currentList() != nullptr; }

bool onEmptyListItem(const QTextCursor& cursor) {
    return cursor.currentList() != nullptr && cursor.block().text().isEmpty();
}

bool atFormattedBlockStart(const QTextCursor& cursor) {
    return !cursor.hasSelection() && cursor.atBlockStart() &&
           (cursor.blockFormat().headingLevel() > 0 || cursor.currentList() != nullptr);
}

void applyHeading(QTextCursor cursor, int level) {
    cursor.beginEditBlock();
    removeMarkerPrefix(cursor);
    QTextBlockFormat blockFormat = cursor.blockFormat();
    blockFormat.setHeadingLevel(level);
    cursor.setBlockFormat(blockFormat);
    const QTextCharFormat charFormat = headingCharFormat(cursor.document(), level);
    cursor.setBlockCharFormat(charFormat);
    cursor.setCharFormat(charFormat);
    cursor.endEditBlock();
}

void applyBullet(QTextCursor cursor) {
    cursor.beginEditBlock();
    removeMarkerPrefix(cursor);
    cursor.createList(unorderedStyleForDepth(1));
    cursor.endEditBlock();
}

void applyNumber(QTextCursor cursor) {
    cursor.beginEditBlock();
    removeMarkerPrefix(cursor);
    cursor.createList(QTextListFormat::ListDecimal);
    cursor.endEditBlock();
}

void applyInline(QTextCursor cursor, const InlineMatch& match) {
    if (match.kind == InlineMatch::Kind::None) {
        return;
    }
    const int blockPosition = cursor.block().position();
    const int markerLength = match.kind == InlineMatch::Kind::Bold ? 2 : 1;
    cursor.beginEditBlock();
    // Back to front, so removals never shift later positions. Only the
    // pending '*' was suppressed; bold's first closing star is in the text.
    if (match.kind == InlineMatch::Kind::Bold) {
        QTextCursor closer(cursor.document());
        closer.setPosition(blockPosition + match.start + markerLength + match.length);
        closer.setPosition(blockPosition + match.start + markerLength + match.length + 1,
                           QTextCursor::KeepAnchor);
        closer.removeSelectedText();
    }
    // Format the content span (positions still include the opener).
    QTextCursor span(cursor.document());
    span.setPosition(blockPosition + match.start + markerLength);
    span.setPosition(blockPosition + match.start + markerLength + match.length, QTextCursor::KeepAnchor);
    QTextCharFormat format;
    if (match.kind == InlineMatch::Kind::Bold) {
        format.setFontWeight(QFont::Bold);
    } else if (match.kind == InlineMatch::Kind::Italic) {
        format.setFontItalic(true);
    } else {
        format.setFontFixedPitch(true);
    }
    span.mergeCharFormat(format);
    // Remove the opener, then reset the insertion format so typing after the
    // pair is plain again.
    QTextCursor opener(cursor.document());
    opener.setPosition(blockPosition + match.start);
    opener.setPosition(blockPosition + match.start + markerLength, QTextCursor::KeepAnchor);
    opener.removeSelectedText();
    cursor.setCharFormat(QTextCharFormat());
    cursor.endEditBlock();
}

void insertPlainBlock(QTextCursor cursor) {
    cursor.beginEditBlock();
    const bool leavingHeading = cursor.blockFormat().headingLevel() > 0;
    cursor.insertBlock();
    if (leavingHeading) {
        QTextBlockFormat plain = cursor.blockFormat();
        plain.setHeadingLevel(0);
        cursor.setBlockFormat(plain);
        cursor.setBlockCharFormat(QTextCharFormat());
        cursor.setCharFormat(QTextCharFormat());
    }
    cursor.endEditBlock();
}

void continueList(QTextCursor cursor) {
    cursor.beginEditBlock();
    cursor.insertBlock();  // inherits the list block format -- the next item
    cursor.endEditBlock();
}

void exitList(QTextCursor cursor) {
    cursor.beginEditBlock();
    QTextList* list = cursor.currentList();
    if (list != nullptr) {
        list->remove(cursor.block());
    }
    QTextBlockFormat plain = cursor.blockFormat();
    plain.setIndent(0);
    plain.setHeadingLevel(0);
    cursor.setBlockFormat(plain);
    cursor.endEditBlock();
}

void clearBlockFormat(QTextCursor cursor) {
    cursor.beginEditBlock();
    QTextList* list = cursor.currentList();
    if (list != nullptr) {
        list->remove(cursor.block());
    }
    QTextBlockFormat plain = cursor.blockFormat();
    plain.setIndent(0);
    plain.setHeadingLevel(0);
    cursor.setBlockFormat(plain);
    cursor.setBlockCharFormat(QTextCharFormat());
    cursor.setCharFormat(QTextCharFormat());
    cursor.endEditBlock();
}

void indentListItem(QTextCursor cursor, int delta) {
    QTextList* list = cursor.currentList();
    if (list == nullptr) {
        return;
    }
    cursor.beginEditBlock();
    QTextListFormat format = list->format();
    const int indent = std::max(1, format.indent() + delta);
    // A fresh list at the new level; siblings at other levels keep theirs.
    QTextListFormat moved = format;
    moved.setIndent(indent);
    if (!isOrderedListStyle(moved.style())) {
        moved.setStyle(unorderedStyleForDepth(indent));
    }
    cursor.createList(moved);
    cursor.endEditBlock();
}

void applyListPresentation(QTextDocument* document) {
    document->setIndentWidth(kListIndentWidth);
    for (QTextBlock block = document->begin(); block != document->end(); block = block.next()) {
        QTextList* list = block.textList();
        if (list == nullptr) {
            continue;
        }
        QTextListFormat format = list->format();
        if (isOrderedListStyle(format.style())) {
            continue;
        }
        const QTextListFormat::Style want = unorderedStyleForDepth(format.indent());
        if (format.style() != want) {
            format.setStyle(want);
            list->setFormat(format);
        }
    }
}

}  // namespace note_editor_rules
}  // namespace app
