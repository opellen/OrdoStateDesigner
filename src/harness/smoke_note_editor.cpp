#include <QPlainTextEdit>
#include <QString>
#include <QStringList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>
#include <QTextListFormat>
#include <cstdio>
#include <functional>

#include "view/canvas/note_editor_rules.h"
#include "view/generated/note_editor_core.h"

#include "harness/harness.h"

// ---- Note-editor machine smoke --------------------------------------------
// The generated core driven row by row: scripted guard answers, recorded
// action firings, asserted configurations. Pure dispatch semantics, no widget;
// also covers within-group ordering (row 7 beats row 8 when both guards pass)
// and bare-QTextDocument fixtures for list rules real keys don't reach.

namespace {

struct ScriptedNoteEditorHooks final : app::generated::note_editor::NoteEditorGuards,
                                       app::generated::note_editor::NoteEditorActions {
    bool heading = false;
    bool bullet = false;
    bool number = false;
    bool inlinePair = false;
    bool emptyItem = false;
    bool list = false;
    bool atStart = false;
    QStringList fired;

    bool blockIsHeadingMarker() override { return heading; }
    bool blockIsBulletMarker() override { return bullet; }
    bool blockIsNumberMarker() override { return number; }
    bool completesInlinePair() override { return inlinePair; }
    bool onEmptyListItem() override { return emptyItem; }
    bool inList() override { return list; }
    bool atFormattedBlockStart() override { return atStart; }

    void applyHeadingFormat() override { fired << QStringLiteral("applyHeadingFormat"); }
    void applyBulletFormat() override { fired << QStringLiteral("applyBulletFormat"); }
    void applyNumberFormat() override { fired << QStringLiteral("applyNumberFormat"); }
    void applyInlineFormat() override { fired << QStringLiteral("applyInlineFormat"); }
    void insertSpace() override { fired << QStringLiteral("insertSpace"); }
    void insertMarker() override { fired << QStringLiteral("insertMarker"); }
    void insertNewline() override { fired << QStringLiteral("insertNewline"); }
    void continueList() override { fired << QStringLiteral("continueList"); }
    void exitList() override { fired << QStringLiteral("exitList"); }
    void deleteBackward() override { fired << QStringLiteral("deleteBackward"); }
    void clearBlockFormat() override { fired << QStringLiteral("clearBlockFormat"); }
    void indentListItem() override { fired << QStringLiteral("indentListItem"); }
    void outdentListItem() override { fired << QStringLiteral("outdentListItem"); }
    void insertTab() override { fired << QStringLiteral("insertTab"); }
    void passthroughKey() override { fired << QStringLiteral("passthroughKey"); }
    void revertAutoformat() override { fired << QStringLiteral("revertAutoformat"); }
};

}  // namespace

int runNoteEditorMachineSmoke() {
    using Core = app::generated::note_editor::NoteEditorCore;
    using State = app::generated::note_editor::NoteEditorState;
    namespace rules = app::note_editor_rules;

    struct Row {
        const char* name;
        std::function<void(ScriptedNoteEditorHooks&)> arm;
        std::function<void(Core&)> fire;
        const char* expectAction;  // nullptr == the row has no action
        State expectState;
    };
    const std::vector<Row> rows = {
        {"1 SpaceTyped[heading]", [](ScriptedNoteEditorHooks& h) { h.heading = true; },
         [](Core& c) { c.spaceTypedRequested(); }, "applyHeadingFormat", State::ArmedRevert},
        {"2 SpaceTyped[bullet]", [](ScriptedNoteEditorHooks& h) { h.bullet = true; },
         [](Core& c) { c.spaceTypedRequested(); }, "applyBulletFormat", State::ArmedRevert},
        {"3 SpaceTyped[number]", [](ScriptedNoteEditorHooks& h) { h.number = true; },
         [](Core& c) { c.spaceTypedRequested(); }, "applyNumberFormat", State::ArmedRevert},
        {"4 SpaceTyped[fallback]", nullptr, [](Core& c) { c.spaceTypedRequested(); }, "insertSpace", State::Ready},
        {"5 MarkerTyped[pair]", [](ScriptedNoteEditorHooks& h) { h.inlinePair = true; },
         [](Core& c) { c.markerTypedRequested(); }, "applyInlineFormat", State::ArmedRevert},
        {"6 MarkerTyped[fallback]", nullptr, [](Core& c) { c.markerTypedRequested(); }, "insertMarker",
         State::Ready},
        {"7 EnterTyped[emptyItem beats inList]",
         [](ScriptedNoteEditorHooks& h) {
             h.emptyItem = true;
             h.list = true;  // BOTH pass -- physical row order must pick exitList
         },
         [](Core& c) { c.enterTypedRequested(); }, "exitList", State::Ready},
        {"8 EnterTyped[inList]", [](ScriptedNoteEditorHooks& h) { h.list = true; },
         [](Core& c) { c.enterTypedRequested(); }, "continueList", State::Ready},
        {"9 EnterTyped[fallback]", nullptr, [](Core& c) { c.enterTypedRequested(); }, "insertNewline",
         State::Ready},
        {"10 BackspaceTyped[atStart]", [](ScriptedNoteEditorHooks& h) { h.atStart = true; },
         [](Core& c) { c.backspaceTypedRequested(); }, "clearBlockFormat", State::Ready},
        {"11 BackspaceTyped[fallback]", nullptr, [](Core& c) { c.backspaceTypedRequested(); }, "deleteBackward",
         State::Ready},
        {"12 TabTyped[inList]", [](ScriptedNoteEditorHooks& h) { h.list = true; },
         [](Core& c) { c.tabTypedRequested(); }, "indentListItem", State::Ready},
        {"13 TabTyped[fallback]", nullptr, [](Core& c) { c.tabTypedRequested(); }, "insertTab", State::Ready},
        {"14 ShiftTabTyped[inList]", [](ScriptedNoteEditorHooks& h) { h.list = true; },
         [](Core& c) { c.shiftTabTypedRequested(); }, "outdentListItem", State::Ready},
        {"15 ShiftTabTyped[fallback]", nullptr, [](Core& c) { c.shiftTabTypedRequested(); }, nullptr,
         State::Ready},
        {"16 OtherKeyTyped", nullptr, [](Core& c) { c.otherKeyTypedRequested(); }, "passthroughKey",
         State::Ready},
        {"17 CompositionStarted", nullptr, [](Core& c) { c.compositionStartedRequested(); }, nullptr,
         State::Composing},
    };
    for (const Row& row : rows) {
        ScriptedNoteEditorHooks hooks;
        if (row.arm) {
            row.arm(hooks);
        }
        Core core(hooks, hooks);
        if (!core.isActive(State::Editing) || !core.isActive(State::Ready)) {
            std::fprintf(stderr, "FAIL: note-editor smoke: fresh core did not descend Editing -> Ready\n");
            return 1;
        }
        row.fire(core);
        const QStringList expected =
            row.expectAction != nullptr ? QStringList{QString::fromLatin1(row.expectAction)} : QStringList();
        if (hooks.fired != expected || !core.isActive(row.expectState)) {
            std::fprintf(stderr, "FAIL: note-editor smoke row %s (fired='%s')\n", row.name,
                          qUtf8Printable(hooks.fired.join(QLatin1Char(','))));
            return 1;
        }
    }
    {
        // Row 18: ArmedRevert's Backspace override wins over both Editing
        // Backspace rows even with the parent's guard armed (child first).
        ScriptedNoteEditorHooks hooks;
        hooks.heading = true;
        Core core(hooks, hooks);
        core.spaceTypedRequested();
        hooks.atStart = true;
        hooks.fired.clear();
        core.backspaceTypedRequested();
        if (hooks.fired != QStringList{QStringLiteral("revertAutoformat")} || !core.isActive(State::Ready)) {
            std::fprintf(stderr, "FAIL: note-editor smoke row 18 (ArmedRevert Backspace override)\n");
            return 1;
        }
    }
    {
        // Bubbling disarm: any OTHER key in ArmedRevert reaches the parent's
        // own row and its Ready target drops the latch.
        ScriptedNoteEditorHooks hooks;
        hooks.bullet = true;
        Core core(hooks, hooks);
        core.spaceTypedRequested();
        hooks.fired.clear();
        core.otherKeyTypedRequested();
        if (hooks.fired != QStringList{QStringLiteral("passthroughKey")} || !core.isActive(State::Ready) ||
            core.isActive(State::ArmedRevert)) {
            std::fprintf(stderr, "FAIL: note-editor smoke: ArmedRevert did not disarm through the parent row\n");
            return 1;
        }
    }
    {
        // Row 19 -- composition round trip: Composing suspends (no rows for
        // typing events there), CompositionEnded re-enters Editing and the
        // initial descent lands Ready with the rules live again.
        ScriptedNoteEditorHooks hooks;
        Core core(hooks, hooks);
        core.compositionStartedRequested();
        if (core.isActive(State::Editing) || !core.isActive(State::Composing)) {
            std::fprintf(stderr, "FAIL: note-editor smoke: CompositionStarted did not leave Editing\n");
            return 1;
        }
        core.compositionEndedRequested();
        if (!core.isActive(State::Editing) || !core.isActive(State::Ready)) {
            std::fprintf(stderr, "FAIL: note-editor smoke row 19 (composition re-entry descent)\n");
            return 1;
        }
        hooks.heading = true;
        hooks.fired.clear();
        core.spaceTypedRequested();
        if (hooks.fired != QStringList{QStringLiteral("applyHeadingFormat")}) {
            std::fprintf(stderr, "FAIL: note-editor smoke: rules dead after the composition round trip\n");
            return 1;
        }
    }

    // ---- rules-unit fixtures (bare QTextDocument -- the list/number rows the
    // probe's real-key scenario does not reach) --------------------------------
    {
        QTextDocument document;
        QTextCursor cursor(&document);
        cursor.insertText(QStringLiteral("-"));
        if (!rules::isBulletMarker(cursor)) {
            std::fprintf(stderr, "FAIL: note-editor rules: '-' did not read as a bullet marker\n");
            return 1;
        }
        rules::applyBullet(cursor);
        if (cursor.currentList() == nullptr || !cursor.block().text().isEmpty() ||
            cursor.currentList()->format().style() != QTextListFormat::ListDisc) {
            std::fprintf(stderr, "FAIL: note-editor rules: applyBullet did not produce an empty disc item\n");
            return 1;
        }
        cursor.insertText(QStringLiteral("item"));
        rules::continueList(cursor);
        if (cursor.currentList() == nullptr || !rules::onEmptyListItem(cursor)) {
            std::fprintf(stderr, "FAIL: note-editor rules: continueList did not open a fresh empty item\n");
            return 1;
        }
        // Depth-cycled bullets: indent walks disc -> circle -> square and back.
        rules::indentListItem(cursor, +1);
        const int indented = cursor.currentList() != nullptr ? cursor.currentList()->format().indent() : -1;
        const bool circleAt2 =
            cursor.currentList() != nullptr && cursor.currentList()->format().style() == QTextListFormat::ListCircle;
        rules::indentListItem(cursor, +1);
        const bool squareAt3 =
            cursor.currentList() != nullptr && cursor.currentList()->format().style() == QTextListFormat::ListSquare;
        rules::indentListItem(cursor, -1);
        rules::indentListItem(cursor, -1);
        const int outdented = cursor.currentList() != nullptr ? cursor.currentList()->format().indent() : -1;
        const bool discAt1 =
            cursor.currentList() != nullptr && cursor.currentList()->format().style() == QTextListFormat::ListDisc;
        if (indented != 2 || !circleAt2 || !squareAt3 || outdented != 1 || !discAt1) {
            std::fprintf(stderr,
                          "FAIL: note-editor rules: depth walk %d/%d did not cycle disc/circle/square bullets\n",
                          indented, outdented);
            return 1;
        }
        rules::exitList(cursor);
        if (cursor.currentList() != nullptr || rules::inList(cursor)) {
            std::fprintf(stderr, "FAIL: note-editor rules: exitList left the item in a list\n");
            return 1;
        }
    }
    {
        QTextDocument document;
        QTextCursor cursor(&document);
        cursor.insertText(QStringLiteral("12."));
        if (!rules::isNumberMarker(cursor)) {
            std::fprintf(stderr, "FAIL: note-editor rules: '12.' did not read as a number marker\n");
            return 1;
        }
        rules::applyNumber(cursor);
        if (cursor.currentList() == nullptr ||
            cursor.currentList()->format().style() != QTextListFormat::ListDecimal) {
            std::fprintf(stderr, "FAIL: note-editor rules: applyNumber did not produce a decimal list\n");
            return 1;
        }
    }
    {
        QTextDocument document;
        QTextCursor cursor(&document);
        cursor.insertText(QStringLiteral("##"));
        if (rules::headingMarkerLevel(cursor) != 2) {
            std::fprintf(stderr, "FAIL: note-editor rules: '##' did not read as heading level 2\n");
            return 1;
        }
        rules::applyHeading(cursor, 2);
        if (cursor.blockFormat().headingLevel() != 2 || !cursor.block().text().isEmpty() ||
            !rules::atFormattedBlockStart(cursor)) {
            std::fprintf(stderr, "FAIL: note-editor rules: applyHeading(2) shape is wrong\n");
            return 1;
        }
        rules::clearBlockFormat(cursor);
        if (cursor.blockFormat().headingLevel() != 0) {
            std::fprintf(stderr, "FAIL: note-editor rules: clearBlockFormat kept the heading\n");
            return 1;
        }
    }
    {
        // Document-level presentation: setMarkdown's nested lists get the
        // indentWidth + depth-cycled bullets that both renderers apply after loading.
        QTextDocument document;
        document.setMarkdown(QStringLiteral("- a\n  - b\n    - c"),
                             QTextDocument::MarkdownDialectGitHub);
        app::note_editor_rules::applyListPresentation(&document);
        if (document.indentWidth() > 16.5 || document.indentWidth() < 15.5) {
            std::fprintf(stderr, "FAIL: note-editor rules: applyListPresentation did not set the indent width\n");
            return 1;
        }
        bool sawDisc = false;
        bool sawCircle = false;
        bool sawSquare = false;
        for (QTextBlock block = document.begin(); block != document.end(); block = block.next()) {
            QTextList* list = block.textList();
            if (list == nullptr) {
                continue;
            }
            const QTextListFormat format = list->format();
            sawDisc = sawDisc || (format.indent() == 1 && format.style() == QTextListFormat::ListDisc);
            sawCircle = sawCircle || (format.indent() == 2 && format.style() == QTextListFormat::ListCircle);
            sawSquare = sawSquare || (format.indent() == 3 && format.style() == QTextListFormat::ListSquare);
        }
        if (!sawDisc || !sawCircle || !sawSquare) {
            std::fprintf(stderr,
                          "FAIL: note-editor rules: nested markdown lists did not restyle disc/circle/square\n");
            return 1;
        }
    }
    {
        QTextDocument document;
        QTextCursor cursor(&document);
        cursor.insertText(QStringLiteral("*it"));
        const rules::InlineMatch italic = rules::findInlinePair(cursor, QLatin1Char('*'));
        if (italic.kind != rules::InlineMatch::Kind::Italic || italic.start != 0 || italic.length != 2) {
            std::fprintf(stderr, "FAIL: note-editor rules: '*it' + '*' did not match italic\n");
            return 1;
        }
        cursor.select(QTextCursor::Document);
        cursor.removeSelectedText();
        cursor.insertText(QStringLiteral("`x"));
        const rules::InlineMatch code = rules::findInlinePair(cursor, QLatin1Char('`'));
        if (code.kind != rules::InlineMatch::Kind::Code || code.length != 1) {
            std::fprintf(stderr, "FAIL: note-editor rules: '`x' + '`' did not match code\n");
            return 1;
        }
    }

    std::printf("PASS: state-designer note-editor machine smoke (19 table rows + child override + bubbling disarm "
                "+ composition round trip + rules-unit list/number/heading/inline fixtures)\n");
    return 0;
}
