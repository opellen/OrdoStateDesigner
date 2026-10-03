// --gui-probe hierarchy scenarios: canvas annotation notes (single- and
// multi-line), the derived action box, and the compound/parallel family:
// container creation, reparent drag, cross-hierarchy edges, hierarchical
// Simulate.

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QHash>
#include <QImage>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QPair>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextEdit>
#include <QPointF>
#include <QRect>
#include <QElapsedTimer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <ordo/core/kernel.h>
#include <ordo/qt/view_host.h>

#include "controller/edit_commands.h"
#include "controller/sim_commands.h"
#include "controller/undo_capture.h"
#include "controller/undo_commands.h"
#include "infra/code_generator.h"
#include "infra/machine_validator.h"
#include "infra/project_io.h"
#include "infra/sim_clock.h"
#include "infra/xstate_v5_io.h"
#include "model/machine.h"
#include "model/machine_doc.h"
#include "model/machine_events.h"
#include "model/sim_agent.h"
#include "model/sim_events.h"
#include "model/undo_events.h"
#include "model/undo_store.h"
#include "view/canvas/canvas_presenter.h"
#include "view/canvas/canvas_view.h"
#include "view/generated/canvas_interaction_core.h"  // drives the machine directly
#include "view/shell/document_session.h"
#include "view/shell/editor_view.h"
#include "view/shell/inspector_panel.h"
#include "view/shell/main_window.h"
#include "view/items/machine_frame_item.h"
#include "view/shell/minimap_view.h"
#include "view/items/note_item.h"
#include "view/geometry/pill_port_resolver.h"  // asserts the rule table 1:1
#include "view/items/state_item.h"
#include "view/shell/theme.h"
#include "view/shell/trace_panel.h"
#include "view/items/transition_item.h"

#include "harness/harness.h"
#include "harness/probe_scenarios.h"

// ---- the unified-wire invariants ----
// A Normal transition is one canonical Manhattan polyline from node anchor to
// node anchor, the pill a pure annotation. Read off
// canonicalWaypoints(debugRoute()): continuity/orthogonality with no U-turn
// wrap, both ends meeting their own node's border along that side's outward
// normal, the label law, and the <= 4 point corner bound.
namespace {

// edge_router.cpp's offsets, restated because they are file-local there: a wire
// starts kProbeStubGap off the source rect and stops kProbeStubGap +
// kProbeArrowApproach off the target's.
constexpr qreal kProbeStubGap = 5.0;
constexpr qreal kProbeArrowApproach = 10.0;

struct UnifiedWire {
    QVector<QPointF> points;
    QPointF departure;     // unit tangent leaving the source node
    QPointF arrival;       // unit tangent entering the target node
    QPointF sourceAnchor;  // where the wire meets the source node's border
    QPointF targetAnchor;  // where its arrowhead lands on the target's
};

QPointF unitStep(QPointF from, QPointF to) {
    const QPointF d = to - from;
    const qreal norm = std::hypot(d.x(), d.y());
    return norm > 1e-6 ? d / norm : QPointF();
}

app::PortSide sideOfOutwardNormal(QPointF normal) {
    if (normal.x() > 0.5) {
        return app::PortSide::Right;
    }
    if (normal.x() < -0.5) {
        return app::PortSide::Left;
    }
    if (normal.y() > 0.5) {
        return app::PortSide::Bottom;
    }
    return app::PortSide::Top;
}

bool pointOnRectSide(const QRectF& rect, app::PortSide side, QPointF p) {
    switch (side) {
        case app::PortSide::Top:
            return std::abs(p.y() - rect.top()) < 0.01 && p.x() >= rect.left() - 0.01 &&
                   p.x() <= rect.right() + 0.01;
        case app::PortSide::Bottom:
            return std::abs(p.y() - rect.bottom()) < 0.01 && p.x() >= rect.left() - 0.01 &&
                   p.x() <= rect.right() + 0.01;
        case app::PortSide::Left:
            return std::abs(p.x() - rect.left()) < 0.01 && p.y() >= rect.top() - 0.01 &&
                   p.y() <= rect.bottom() + 0.01;
        case app::PortSide::Right:
            return std::abs(p.x() - rect.right()) < 0.01 && p.y() >= rect.top() - 0.01 &&
                   p.y() <= rect.bottom() + 0.01;
    }
    return false;
}

void dumpWaypoints(const QVector<QPointF>& points) {
    for (const QPointF& pt : points) {
        std::fprintf(stderr, "  wp (%.1f, %.1f)\n", pt.x(), pt.y());
    }
}

// The skeleton's law, plus the ends the border checks below need, derived from
// the drawn geometry.
bool unifiedWireHolds(const app::RoutedEdge& route, const char* what, UnifiedWire* out) {
    const QVector<QPointF> points = app::canonicalWaypoints(route);
    if (points.size() < 2) {
        std::fprintf(stderr, "FAIL: %s has no unified wire (%d waypoint(s))\n", what,
                     static_cast<int>(points.size()));
        return false;
    }
    QVector<QPointF> directions;
    for (int i = 0; i + 1 < points.size(); ++i) {
        const qreal dx = points[i + 1].x() - points[i].x();
        const qreal dy = points[i + 1].y() - points[i].y();
        if (std::abs(dx) > 0.01 && std::abs(dy) > 0.01) {
            std::fprintf(stderr, "FAIL: %s segment %d is diagonal (dx=%.2f dy=%.2f)\n", what, i, dx, dy);
            dumpWaypoints(points);
            return false;
        }
        if (std::hypot(dx, dy) <= 0.01) {
            std::fprintf(stderr, "FAIL: %s kept a zero-length segment at %d through the collapse\n", what, i);
            dumpWaypoints(points);
            return false;
        }
        directions.push_back(unitStep(points[i], points[i + 1]));
    }
    for (int i = 1; i < directions.size(); ++i) {
        if (QPointF::dotProduct(directions[i - 1], directions[i]) < -0.99) {
            std::fprintf(stderr, "FAIL: %s wraps back on itself at segment %d (a U-turn, not a corner)\n", what, i);
            dumpWaypoints(points);
            return false;
        }
    }
    out->points = points;
    out->departure = directions.front();
    out->arrival = directions.back();
    out->sourceAnchor = points.front() - directions.front() * kProbeStubGap;
    out->targetAnchor = points.back() + directions.back() * (kProbeStubGap + kProbeArrowApproach);
    return true;
}

// Both ends on their own node's border, on the side their tangent names. A
// parent->child edge departs inward at the container's end, so that anchor sits
// on the opposite border. Anchors are recovered by projecting each end onto the
// expected side's line along its tangent (not a fixed stub offset, which the
// router shrinks in short corridors) and written back into `wire`.
bool unifiedWireMeetsNodes(UnifiedWire* wire, const QRectF& sourceRect, const QRectF& targetRect,
                           const char* what) {
    app::PortSide sourceSide = sideOfOutwardNormal(wire->departure);
    app::PortSide targetSide = sideOfOutwardNormal(-wire->arrival);
    if (sourceRect.contains(targetRect.center()) && !targetRect.contains(sourceRect.center())) {
        sourceSide = app::oppositePortSide(sourceSide);
    }
    if (targetRect.contains(sourceRect.center()) && !sourceRect.contains(targetRect.center())) {
        targetSide = app::oppositePortSide(targetSide);
    }
    // Slide `tip` backward along -dir to the side's coordinate line, within the
    // largest stub the router uses.
    const auto projectToSide = [](const QRectF& rect, app::PortSide side, QPointF tip, QPointF dir,
                                  qreal maxSlide, QPointF* anchorOut) {
        const bool horiz = side == app::PortSide::Left || side == app::PortSide::Right;
        const qreal coord = side == app::PortSide::Left    ? rect.left()
                            : side == app::PortSide::Right ? rect.right()
                            : side == app::PortSide::Top   ? rect.top()
                                                           : rect.bottom();
        const qreal axisDir = horiz ? dir.x() : dir.y();
        if (std::abs(axisDir) < 0.5) {
            return false;  // tangent runs along the side: no perpendicular contact
        }
        const qreal t = ((horiz ? tip.x() : tip.y()) - coord) / axisDir;
        if (t < -0.01 || t > maxSlide + 0.6) {
            return false;
        }
        const QPointF anchor = tip - dir * t;
        *anchorOut = anchor;
        return horiz ? (anchor.y() >= rect.top() - 0.01 && anchor.y() <= rect.bottom() + 0.01)
                     : (anchor.x() >= rect.left() - 0.01 && anchor.x() <= rect.right() + 0.01);
    };
    if (!projectToSide(sourceRect, sourceSide, wire->points.front(), wire->departure, kProbeStubGap,
                       &wire->sourceAnchor)) {
        std::fprintf(stderr, "FAIL: %s does not leave the source node's own border (first %.1f, %.1f side %d)\n",
                     what, wire->points.front().x(), wire->points.front().y(), static_cast<int>(sourceSide));
        return false;
    }
    if (!projectToSide(targetRect, targetSide, wire->points.back(), -wire->arrival,
                       kProbeStubGap + kProbeArrowApproach, &wire->targetAnchor)) {
        std::fprintf(stderr, "FAIL: %s does not arrive on the target node's own border (last %.1f, %.1f side %d)\n",
                     what, wire->points.back().x(), wire->points.back().y(), static_cast<int>(targetSide));
        return false;
    }
    return true;
}

// The corner bound: at most 4 points (straight, L or Z).
bool unifiedWireCornerBoundHolds(const UnifiedWire& wire, const char* what) {
    if (wire.points.size() > 4) {
        std::fprintf(stderr,
                     "FAIL: %s bends more than twice (%d waypoints, want <= 4 -- straight, L or Z)\n", what,
                     static_cast<int>(wire.points.size()));
        dumpWaypoints(wire.points);
        return false;
    }
    return true;
}

// The pill sits on the wire, displaced by exactly the persisted offset.
bool unifiedLabelLawHolds(const app::RoutedEdge& route, QPointF labelOffset, const char* what) {
    const QPointF want = route.labelBase + labelOffset;
    if (std::hypot(route.labelAnchor.x() - want.x(), route.labelAnchor.y() - want.y()) > 0.01) {
        std::fprintf(stderr,
                     "FAIL: %s pill is not its wire's own base plus the persisted offset (%.1f, %.1f vs %.1f, %.1f)\n",
                     what, route.labelAnchor.x(), route.labelAnchor.y(), want.x(), want.y());
        return false;
    }
    return true;
}

}  // namespace

// Scenario "note-element": add via the empty-canvas context menu's verb
// (CanvasPresenter::debugAddNote, no menu exec()), assert it exists and is
// selected, inline-edit its text, drag it (one undo restores), delete + undo
// restores, and assert the XState v5 export is byte-identical with and without
// the note (notes are native-only).
int runNoteElementScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::CanvasPresenter* presenter = loginPane->presenter();
    app::CanvasView* view = loginPane->canvasView();
    QGraphicsScene* scene = view != nullptr ? view->scene() : nullptr;
    if (doc == nullptr || presenter == nullptr || view == nullptr || scene == nullptr) {
        std::fprintf(stderr, "FAIL: note-element scenario preconditions (doc/presenter/view/scene) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    scene->clearSelection();
    QApplication::processEvents();

    // Baseline export: a note's existence must never change it.
    const app::XStateExportResult exportBefore = app::machineToXStateJson(doc->machine());

    // ---- add via the menu verb -------------------------------------------------
    const QPointF addPos(-900.0, -500.0);  // far outside the fixture states/frame -- guaranteed empty
    const quint64 noteId = doc->machine().nextId;
    presenter->debugAddNote(addPos);
    QApplication::processEvents();
    if (doc->machine().notes.size() != 1 || doc->findNote(noteId) == nullptr) {
        std::fprintf(stderr, "FAIL: debugAddNote did not add exactly one note\n");
        return 1;
    }
    if (presenter->currentSelection().kind != app::SelectionKind::Note ||
        presenter->currentSelection().id != noteId) {
        std::fprintf(stderr, "FAIL: the fresh note was not selected (kind=%d id=%llu)\n",
                     static_cast<int>(presenter->currentSelection().kind),
                     static_cast<unsigned long long>(presenter->currentSelection().id));
        return 1;
    }

    // ---- inline-edit its text ---------------------------------------------------
    presenter->debugCommitInlineEditText(QStringLiteral("Reminder"));
    QApplication::processEvents();
    if (doc->findNote(noteId)->text != QStringLiteral("Reminder")) {
        std::fprintf(stderr, "FAIL: inline edit did not set the note's text (got '%s')\n",
                     qUtf8Printable(doc->findNote(noteId)->text));
        return 1;
    }
    if (!saveSceneRegionCapture(loginPane, presenter->debugNoteRect(noteId).adjusted(-24.0, -24.0, 24.0, 24.0),
                                 "probe-note-element-edited")) {
        return 1;
    }

    // ---- content sizing: the box hugs the text; a short note sits at the
    // ---- floors, a long one wraps at the max width and grows DOWN ---------------
    const QRectF shortRect = presenter->debugNoteRect(noteId);
    if (shortRect.width() > 264.5 || shortRect.height() < 39.5) {
        std::fprintf(stderr, "FAIL: a short note's content-sized box is out of range (w=%.1f h=%.1f)\n",
                     shortRect.width(), shortRect.height());
        return 1;
    }
    kernel.send(app::events::SetNoteTextRequested{
        .id = noteId,
        .text = QStringLiteral("A long enough annotation that must word-wrap at the note's max width and "
                                "therefore grow the box vertically, Stately style, rather than clipping at a "
                                "fixed sticky-note size.")});
    QApplication::processEvents();
    const QRectF longRect = presenter->debugNoteRect(noteId);
    if (longRect.width() <= shortRect.width() || longRect.height() <= shortRect.height()) {
        std::fprintf(stderr, "FAIL: a long note did not grow its content-sized box (w=%.1f h=%.1f)\n",
                     longRect.width(), longRect.height());
        return 1;
    }
    kernel.send(app::events::UndoRequested{});  // back to "Reminder" for the drag below
    QApplication::processEvents();

    // ---- drag it: moved + one-undo restores -------------------------------------
    const QPointF posBefore = doc->findNote(noteId)->pos;
    const QRectF noteRect = presenter->debugNoteRect(noteId);
    const QPointF dragTo = noteRect.center() + QPointF(120.0, 72.0);
    view->debugMousePress(noteRect.center());
    view->debugMouseMove(dragTo);
    view->debugMouseRelease(dragTo);
    QApplication::processEvents();
    if (doc->findNote(noteId)->pos == posBefore) {
        std::fprintf(stderr, "FAIL: dragging the note did not move it\n");
        return 1;
    }
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();
    if (doc->findNote(noteId)->pos != posBefore) {
        std::fprintf(stderr, "FAIL: one undo did not restore the note's pre-drag position\n");
        return 1;
    }

    // ---- delete + undo restores --------------------------------------------------
    scene->clearSelection();
    if (app::NoteItem* item = findNoteItemById(scene, noteId)) {
        item->setSelected(true);
    }
    QApplication::processEvents();
    presenter->debugDeleteSelection();
    QApplication::processEvents();
    if (doc->findNote(noteId) != nullptr) {
        std::fprintf(stderr, "FAIL: deleting the selected note did not remove it\n");
        return 1;
    }
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();
    if (doc->findNote(noteId) == nullptr || doc->findNote(noteId)->text != QStringLiteral("Reminder")) {
        std::fprintf(stderr, "FAIL: undo did not restore the deleted note\n");
        return 1;
    }

    // ---- the note never reaches the XState export --------------------------------
    const app::XStateExportResult exportAfter = app::machineToXStateJson(doc->machine());
    if (exportAfter.json != exportBefore.json) {
        std::fprintf(stderr, "FAIL: the note's existence changed the machine's XState v5 export\n");
        return 1;
    }

    scene->clearSelection();
    QApplication::processEvents();
    std::printf("PASS: gui-probe scenario note-element (add + select, inline edit, content sizing, drag + undo, "
                "delete + undo, "
                "xstate export unchanged)\n");
    return 0;
}

// Scenario "multiline-note": the note's rich QTextEdit editor, driven through
// real key events. Enter is a newline (never a commit), every close path
// commits, one undo reverts a whole editing session, "# " becomes an H1 (an
// immediate Backspace reverts it), **b** lands a bold span, and the committed
// text round-trips as markdown. Uses a relative note count, since
// runNoteElementScenario leaves one note behind.
int runMultilineNoteScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::CanvasPresenter* presenter = loginPane->presenter();
    app::CanvasView* view = loginPane->canvasView();
    QGraphicsScene* scene = view != nullptr ? view->scene() : nullptr;
    if (doc == nullptr || presenter == nullptr || view == nullptr || scene == nullptr) {
        std::fprintf(stderr, "FAIL: multiline-note scenario preconditions (doc/presenter/view/scene) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    scene->clearSelection();
    QApplication::processEvents();

    // ---- add via the menu verb, selected + inline editor already open ------------
    const qsizetype notesBefore = doc->machine().notes.size();
    const QPointF addPos(-900.0, -200.0);  // far outside every fixture -- guaranteed empty
    const quint64 noteId = doc->machine().nextId;
    presenter->debugAddNote(addPos);
    QApplication::processEvents();
    if (doc->machine().notes.size() != notesBefore + 1 || doc->findNote(noteId) == nullptr) {
        std::fprintf(stderr, "FAIL: debugAddNote did not add exactly one note\n");
        return 1;
    }

    // ---- helpers: real key events + type-through (each char rides the
    // machine's own event routing) ---------------------------------------------
    const auto sendKey = [](QWidget* target, int key, const QString& text) {
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, text);
        QCoreApplication::sendEvent(target, &press);
        QApplication::processEvents();
    };
    const auto typeText = [&sendKey](QWidget* target, const QString& text) {
        for (const QChar& c : text) {
            sendKey(target, c == QLatin1Char(' ') ? static_cast<int>(Qt::Key_Space) : 0, QString(c));
        }
    };

    QTextEdit* editorWidget = presenter->debugInlineEditAreaWidget();
    if (editorWidget == nullptr) {
        std::fprintf(stderr, "FAIL: adding a note did not open its multi-line inline editor\n");
        return 1;
    }
    // ---- the editor opens within the note column, no scrollbars (size comes
    // ---- from the editor's own laid-out document) ------------------------------
    const int emptyHeight = editorWidget->height();
    if (editorWidget->width() > 264 || editorWidget->width() < 96 || emptyHeight < 40 ||
        editorWidget->verticalScrollBar()->maximum() != 0) {
        std::fprintf(stderr, "FAIL: the fresh note editor did not open note-column-sized and scroll-free\n");
        return 1;
    }
    const QString multilineText = QStringLiteral("Line one\nLine two");
    editorWidget->setPlainText(multilineText);
    QApplication::processEvents();
    // ---- live autosize (no-scroll growth) -------------------------------------
    if (editorWidget->height() <= emptyHeight || editorWidget->verticalScrollBar()->maximum() != 0) {
        std::fprintf(stderr, "FAIL: typing did not live-autosize the note editor (or it scrolled)\n");
        return 1;
    }
    // ---- Enter is a NEWLINE (EnterTyped -> insertNewline); Backspace merges the
    // ---- fresh empty block back (deleteBackward) --------------------------------
    const int blocksBefore = editorWidget->document()->blockCount();
    sendKey(editorWidget, Qt::Key_Return, QString());
    if (presenter->debugInlineEditAreaWidget() == nullptr ||
        editorWidget->document()->blockCount() != blocksBefore + 1) {
        std::fprintf(stderr, "FAIL: Enter did not insert a newline (or closed the editor)\n");
        return 1;
    }
    sendKey(editorWidget, Qt::Key_Backspace, QString());
    if (editorWidget->document()->blockCount() != blocksBefore) {
        std::fprintf(stderr, "FAIL: Backspace did not merge the fresh empty block back\n");
        return 1;
    }
    // ---- Esc commits and closes (no abort path) ---------------------------------
    sendKey(editorWidget, Qt::Key_Escape, QString());
    const QString committedMultiline = QStringLiteral("Line one\n\nLine two");  // two blocks == two md paragraphs
    if (presenter->debugInlineEditAreaWidget() != nullptr || doc->findNote(noteId) == nullptr ||
        doc->findNote(noteId)->text != committedMultiline) {
        std::fprintf(stderr, "FAIL: Esc did not commit-and-close the multi-line note text\n");
        return 1;
    }
    app::NoteItem* noteItem = findNoteItemById(scene, noteId);
    if (noteItem == nullptr || noteItem->text() != committedMultiline) {
        std::fprintf(stderr, "FAIL: the note item did not render the committed multi-line text\n");
        return 1;
    }
    if (!saveSceneRegionCapture(loginPane, presenter->debugNoteRect(noteId).adjusted(-24.0, -24.0, 24.0, 24.0),
                                 "probe-multiline-note")) {
        return 1;
    }

    // ---- re-open: a stationary click on the already-selected note -- the
    // markdown renders back into the editor (setMarkdown), and its plain
    // text still carries the line structure -------------------------------------
    const QPointF noteCenter = presenter->debugNoteRect(noteId).center();
    view->debugMousePress(noteCenter);
    view->debugMouseRelease(noteCenter);
    QApplication::processEvents();
    editorWidget = presenter->debugInlineEditAreaWidget();
    if (editorWidget == nullptr || editorWidget->toPlainText() != multilineText) {
        std::fprintf(stderr, "FAIL: re-opening the note's editor did not preserve the line structure\n");
        return 1;
    }
    // Closing again without a change commits nothing (the presenter's
    // no-change guard) -- so ONE undo below still reverts the whole session.
    sendKey(editorWidget, Qt::Key_Escape, QString());

    // ---- one undo reverts the editing session's single commit -----------------
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();
    if (doc->findNote(noteId) == nullptr || !doc->findNote(noteId)->text.isEmpty()) {
        std::fprintf(stderr, "FAIL: ONE undo did not revert the note-editing session's commit\n");
        return 1;
    }

    // ---- LIVE input rules: each round reopens the editor via the
    // ---- stationary-click re-edit gesture, drives real key events, and closes
    // ---- with Esc (commit) -----------------------------------------------------
    const auto openEditor = [&]() -> QTextEdit* {
        app::NoteItem* item = findNoteItemById(scene, noteId);
        if (item == nullptr) {
            return nullptr;
        }
        scene->clearSelection();
        item->setSelected(true);
        QApplication::processEvents();
        const QPointF center = presenter->debugNoteRect(noteId).center();
        view->debugMousePress(center);
        view->debugMouseRelease(center);
        QApplication::processEvents();
        return presenter->debugInlineEditAreaWidget();
    };

    // Round 1 -- plain baseline for the height comparison below.
    QTextEdit* editor = openEditor();
    if (editor == nullptr) {
        std::fprintf(stderr, "FAIL: reopening the note editor for the plain round failed\n");
        return 1;
    }
    editor->selectAll();
    typeText(editor, QStringLiteral("Heading"));
    sendKey(editor, Qt::Key_Escape, QString());
    if (doc->findNote(noteId) == nullptr || doc->findNote(noteId)->text != QStringLiteral("Heading")) {
        std::fprintf(stderr, "FAIL: the typed plain text did not commit as-is\n");
        return 1;
    }
    const qreal plainHeight = presenter->debugNoteRect(noteId).height();

    // Round 2 -- "# " becomes a live H1; an immediate Backspace reverts the
    // auto-format (ArmedRevert); Space re-applies; the commit round-trips as
    // markdown.
    editor = openEditor();
    if (editor == nullptr) {
        std::fprintf(stderr, "FAIL: reopening the note editor for the heading round failed\n");
        return 1;
    }
    editor->selectAll();
    typeText(editor, QStringLiteral("# "));
    if (editor->textCursor().blockFormat().headingLevel() != 1 ||
        !editor->textCursor().block().text().isEmpty()) {
        std::fprintf(stderr, "FAIL: typing '# ' did not become a LIVE H1 block in the editor\n");
        return 1;
    }
    sendKey(editor, Qt::Key_Backspace, QString());
    if (editor->textCursor().blockFormat().headingLevel() != 0 ||
        editor->textCursor().block().text() != QStringLiteral("#")) {
        std::fprintf(stderr, "FAIL: Backspace right after the auto-format did not revert it (ArmedRevert)\n");
        return 1;
    }
    typeText(editor, QStringLiteral(" "));
    if (editor->textCursor().blockFormat().headingLevel() != 1) {
        std::fprintf(stderr, "FAIL: re-applying the heading after the revert did not fire\n");
        return 1;
    }
    typeText(editor, QStringLiteral("Heading"));
    sendKey(editor, Qt::Key_Escape, QString());
    if (doc->findNote(noteId) == nullptr || doc->findNote(noteId)->text != QStringLiteral("# Heading")) {
        std::fprintf(stderr, "FAIL: the live H1 did not commit as '# Heading' markdown\n");
        return 1;
    }
    const qreal headingHeight = presenter->debugNoteRect(noteId).height();
    if (headingHeight <= plainHeight + 2.0) {
        std::fprintf(stderr, "FAIL: the H1 note did not render taller than the same text plain\n");
        return 1;
    }

    // Round 3 -- typing **b** lands a live bold span, and the commit re-emits
    // the markdown pair.
    editor = openEditor();
    if (editor == nullptr) {
        std::fprintf(stderr, "FAIL: reopening the note editor for the bold round failed\n");
        return 1;
    }
    QTextCursor endCursor = editor->textCursor();
    endCursor.movePosition(QTextCursor::End);
    editor->setTextCursor(endCursor);
    // A fresh plain block first (Enter leaves the heading): bold inside an H1
    // is invisible to the markdown writer.
    sendKey(editor, Qt::Key_Return, QString());
    typeText(editor, QStringLiteral("**b**"));
    // Probe the document character (a fresh cursor reads the char before its
    // position); the widget cursor's charFormat() is the reset insertion format.
    QTextCursor boldProbe(editor->document());
    boldProbe.setPosition(editor->textCursor().position());
    if (boldProbe.charFormat().fontWeight() <= QFont::Normal) {
        std::fprintf(stderr, "FAIL: typing **b** did not land a live bold span\n");
        return 1;
    }
    sendKey(editor, Qt::Key_Escape, QString());
    const QString bolded = doc->findNote(noteId) != nullptr ? doc->findNote(noteId)->text : QString();
    if (!bolded.startsWith(QStringLiteral("# Heading")) || !bolded.contains(QStringLiteral("**b**"))) {
        std::fprintf(stderr, "FAIL: the live bold did not round-trip as **b** markdown (got '%s')\n",
                     qUtf8Printable(bolded));
        return 1;
    }
    if (!saveSceneRegionCapture(loginPane, presenter->debugNoteRect(noteId).adjusted(-24.0, -24.0, 24.0, 24.0),
                                 "probe-note-markdown")) {
        return 1;
    }

    scene->clearSelection();
    QApplication::processEvents();
    std::printf("PASS: gui-probe scenario multiline-note (WYSIWYG: Enter=newline + Esc=commit-and-close, no-scroll "
                "autosize, one-undo per session, live '# '->H1 + ArmedRevert Backspace + **b** bold through real "
                "keys, markdown round trips)\n");
    return 0;
}
// Scenario "action-box": the floating action box is a derived transient, shown
// iff Design mode, the interaction FSM is Idle and the selection is a
// Frame/State/Transition, with a variant per kind. It is dismissed by firing a
// verb or deselecting, and hidden during a session and in Simulate. The
// onboarding hint exists only while the machine has zero states.
int runActionBoxScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::CanvasPresenter* presenter = loginPane->presenter();
    app::CanvasView* view = loginPane->canvasView();
    QGraphicsScene* scene = view != nullptr ? view->scene() : nullptr;
    if (doc == nullptr || presenter == nullptr || scene == nullptr) {
        std::fprintf(stderr, "FAIL: action-box scenario preconditions (doc/presenter/scene) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    scene->clearSelection();
    QApplication::processEvents();
    if (presenter->debugActionBoxVisible()) {
        std::fprintf(stderr, "FAIL: the action box is visible with nothing selected\n");
        return 1;
    }

    // ---- State variant: [+][self][more] --------------------------------------
    presenter->selectState(1);
    QApplication::processEvents();
    // Icon segments carry asset slugs with empty labels: assert the slug roster.
    std::vector<QString> slugs = presenter->debugActionBoxIconSlugs();
    if (!presenter->debugActionBoxVisible() || slugs.size() != 3 || slugs[0] != QStringLiteral("add") ||
        slugs[1] != QStringLiteral("self-loop") || slugs[2] != QStringLiteral("more")) {
        std::fprintf(stderr, "FAIL: selecting a state did not show the [+][self][more] variant\n");
        return 1;
    }
    app::StateItem* loggedOut = findStateItemById(scene, 1);
    if (loggedOut == nullptr ||
        !saveSceneRegionCapture(
            loginPane, (loggedOut->sceneRect() | presenter->debugActionBoxRect()).adjusted(-14.0, -14.0, 14.0, 14.0),
            "probe-action-box-state")) {
        return 1;
    }

    // ---- the anchor FOLLOWS geometry: adding a child grows the still-selected
    // ---- state into a container; the box must re-derive below the derived
    // ---- rect, never overlap the interior; one undo restores the fixture --------
    {
        const QRectF beforeRect = loggedOut->sceneRect();
        const quint64 childId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = beforeRect.center() + QPointF(0.0, 110.0), .parentId = 1});
        QApplication::processEvents();
        app::StateItem* grown = findStateItemById(scene, 1);
        const QRectF grownRect = grown != nullptr ? grown->sceneRect() : QRectF();
        const QRectF boxRect = presenter->debugActionBoxRect();
        if (doc->findState(childId) == nullptr || grown == nullptr || grownRect.height() <= beforeRect.height() ||
            !presenter->debugActionBoxVisible() || boxRect.bottom() > grownRect.top() + 0.5) {
            std::fprintf(stderr,
                          "FAIL: the action box did not re-anchor above the grown container (box top %.1f vs "
                          "container bottom %.1f)\n",
                          boxRect.top(), grownRect.bottom());
            return 1;
        }
        kernel.send(app::events::UndoRequested{});
        QApplication::processEvents();
        if (doc->findState(childId) != nullptr) {
            std::fprintf(stderr, "FAIL: undoing the task-24 fixture child did not restore the machine\n");
            return 1;
        }
    }

    // ---- Transition variant: [Guard][Action][reverse][more] -------------------
    presenter->selectTransition(5);
    QApplication::processEvents();
    const std::vector<QString> labels = presenter->debugActionBoxLabels();
    if (!presenter->debugActionBoxVisible() || labels.size() != 4 || labels[0] != QStringLiteral("Guard") ||
        labels[1] != QStringLiteral("Action")) {
        std::fprintf(stderr, "FAIL: selecting a transition did not show the [Guard][Action][reverse][more] variant\n");
        return 1;
    }

    // ---- Frame (machine) variant: [+ State][Note] -----------------------------
    app::MachineFrameItem* frame = findFrameItem(scene);
    if (frame == nullptr || !frame->isVisible()) {
        std::fprintf(stderr, "FAIL: no visible machine frame for the action box's machine variant\n");
        return 1;
    }
    scene->clearSelection();
    frame->setSelected(true);
    QApplication::processEvents();
    slugs = presenter->debugActionBoxIconSlugs();
    // Icon-only segments (SVG asset slugs); the verb names ride the hover
    // tooltips instead of segment text.
    if (!presenter->debugActionBoxVisible() || slugs.size() != 4 || slugs[0] != QStringLiteral("add") ||
        slugs[1] != QStringLiteral("self-loop") || slugs[2] != QStringLiteral("note") ||
        slugs[3] != QStringLiteral("more")) {
        std::fprintf(stderr, "FAIL: selecting the frame did not show the icon-only [add][self-loop][note][more] variant\n");
        return 1;
    }
    const std::vector<QString> tooltips = presenter->debugActionBoxTooltips();
    if (tooltips.size() != 4 || tooltips[0] != QStringLiteral("Event") ||
        tooltips[1] != QStringLiteral("Self transition") || tooltips[2] != QStringLiteral("Note") ||
        tooltips[3] != QStringLiteral("More")) {
        std::fprintf(stderr, "FAIL: the frame box's icon-only segments did not carry their verb-name tooltips\n");
        return 1;
    }

    // ---- dismissal on deselect -----------------------------------------------
    scene->clearSelection();
    QApplication::processEvents();
    if (presenter->debugActionBoxVisible()) {
        std::fprintf(stderr, "FAIL: deselecting did not dismiss the action box\n");
        return 1;
    }

    // ---- firing a verb dismisses the box AND lands the verb's effect ----------
    const int transitionCountBefore = doc->machine().transitions.size();
    presenter->selectState(1);
    QApplication::processEvents();
    presenter->debugFireActionBoxVerb(app::ActionBoxVerb::AddSelfTransition);
    QApplication::processEvents();
    if (presenter->debugActionBoxVisible()) {
        std::fprintf(stderr, "FAIL: firing a verb did not dismiss the action box\n");
        return 1;
    }
    if (doc->machine().transitions.size() != transitionCountBefore + 1) {
        std::fprintf(stderr, "FAIL: the self-transition verb did not add a transition\n");
        return 1;
    }
    view->escapePressed();  // Q_SIGNALS is public -- close the drained inline editor without committing
    QApplication::processEvents();
    kernel.send(app::events::UndoRequested{});  // the quick-add's ONE batch
    QApplication::processEvents();
    if (doc->machine().transitions.size() != transitionCountBefore) {
        std::fprintf(stderr, "FAIL: one undo did not remove the verb's self-transition batch\n");
        return 1;
    }

    // ---- a live session hides the box; it returns on commit -------------------
    presenter->selectState(1);
    QApplication::processEvents();
    const QPointF grab = frame->sceneFrameRect().topLeft() + QPointF(2.0, 40.0);  // border band
    presenter->debugFrameDragStarted(grab);
    presenter->debugFrameDragMoved(grab + QPointF(24.0, 0.0));  // grid multiple -- no soft-snap surprises
    QApplication::processEvents();
    if (presenter->debugActionBoxVisible()) {
        std::fprintf(stderr, "FAIL: the action box stayed visible during a FrameDrag session\n");
        return 1;
    }
    presenter->debugFrameDragFinished(grab + QPointF(24.0, 0.0));
    QApplication::processEvents();
    if (!presenter->debugActionBoxVisible()) {
        std::fprintf(stderr, "FAIL: the action box did not return after the session committed\n");
        return 1;
    }
    kernel.send(app::events::UndoRequested{});  // the frame move's ONE batch
    QApplication::processEvents();

    // ---- Simulate hides it ----------------------------------------------------
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
    QApplication::processEvents();
    if (presenter->debugActionBoxVisible()) {
        std::fprintf(stderr, "FAIL: the action box survived into Simulate mode\n");
        return 1;
    }
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Design});
    QApplication::processEvents();

    // ---- onboarding hint: zero states in, restored machine out ----------------
    if (presenter->debugOnboardingHintVisible()) {
        std::fprintf(stderr, "FAIL: the onboarding hint is visible while states exist\n");
        return 1;
    }
    const int stateCountBefore = doc->machine().states.size();
    const QRectF frameRect = frame->sceneFrameRect();
    view->debugMousePress(frameRect.topLeft() - QPointF(80.0, 80.0), Qt::ShiftModifier);
    view->debugMouseMove(frameRect.bottomRight() + QPointF(80.0, 80.0));
    view->debugMouseRelease(frameRect.bottomRight() + QPointF(80.0, 80.0));
    QApplication::processEvents();
    presenter->debugDeleteSelection();
    QApplication::processEvents();
    if (!doc->machine().states.isEmpty()) {
        std::fprintf(stderr, "FAIL: band-deleting everything left states behind\n");
        return 1;
    }
    if (!presenter->debugOnboardingHintVisible() || presenter->debugActionBoxVisible()) {
        std::fprintf(stderr, "FAIL: an empty machine did not show the onboarding hint (or the box lingered)\n");
        return 1;
    }
    kernel.send(app::events::UndoRequested{});  // the batch delete, one step
    QApplication::processEvents();
    if (doc->machine().states.size() != stateCountBefore || presenter->debugOnboardingHintVisible()) {
        std::fprintf(stderr, "FAIL: undo did not restore the machine / hide the onboarding hint\n");
        return 1;
    }
    scene->clearSelection();
    QApplication::processEvents();

    std::printf("PASS: gui-probe scenario action-box (variants per selection kind, dismiss on verb + deselect, "
                "session/Simulate hiding, onboarding hint on empty machine)\n");
    return 0;
}

// Scenario "hierarchy-canvas": builds a compound P{A,B} and a Parallel Q{R1,R2}
// and asserts container rendering: padded boxes containing the children, the
// container z-band below edges/leaves, a click-through interior with a
// selecting header, and an initial-child marker for P. Cleans up by deleting
// P and Q as one undo batch.
int runHierarchyCanvasScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::CanvasPresenter* presenter = loginPane->presenter();
    app::CanvasView* view = loginPane->canvasView();
    QGraphicsScene* scene = view != nullptr ? view->scene() : nullptr;
    if (doc == nullptr || presenter == nullptr || view == nullptr || scene == nullptr) {
        std::fprintf(stderr, "FAIL: hierarchy-canvas scenario preconditions (doc/presenter/view/scene) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    scene->clearSelection();
    QApplication::processEvents();

    const QVector<app::State> originalStates = doc->machine().states;
    const QVector<app::Transition> originalTransitions = doc->machine().transitions;

    // ---- build P{A,B} and a Parallel Q{R1,R2}, far outside the fixture --------
    const quint64 kP = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(900.0, -520.0)});
    const quint64 kA = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(860.0, -380.0)});
    const quint64 kB = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(1060.0, -380.0)});
    kernel.send(app::events::ReparentStateRequested{.id = kA, .parentId = kP});
    kernel.send(app::events::ReparentStateRequested{.id = kB, .parentId = kP});

    const quint64 kQ = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(1400.0, -520.0)});
    kernel.send(app::events::SetStateKindRequested{.id = kQ, .kind = app::StateKind::Parallel});
    const quint64 kR1 = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(1360.0, -380.0)});
    const quint64 kR2 = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(1560.0, -380.0)});
    kernel.send(app::events::ReparentStateRequested{.id = kR1, .parentId = kQ});
    kernel.send(app::events::ReparentStateRequested{.id = kR2, .parentId = kQ});
    QApplication::processEvents();  // settle

    app::StateItem* pItem = findStateItemById(scene, kP);
    app::StateItem* aItem = findStateItemById(scene, kA);
    app::StateItem* bItem = findStateItemById(scene, kB);
    app::StateItem* qItem = findStateItemById(scene, kQ);
    app::StateItem* r1Item = findStateItemById(scene, kR1);
    app::StateItem* r2Item = findStateItemById(scene, kR2);
    if (pItem == nullptr || aItem == nullptr || bItem == nullptr || qItem == nullptr || r1Item == nullptr ||
        r2Item == nullptr) {
        std::fprintf(stderr, "FAIL: hierarchy-canvas scenario's P/A/B/Q/R1/R2 StateItems were not all created\n");
        return 1;
    }

    // ---- (a) P is a container whose padded box contains both children ---------
    // Model-layer sanity first, to separate a view bug from a reparent that
    // never landed (a command missing from the session's own bootstrap).
    if (doc->findState(kA)->parentId != kP || doc->findState(kB)->parentId != kP) {
        std::fprintf(stderr, "FAIL: A/B's reparent under P did not take (A.parentId=%llu B.parentId=%llu, want %llu)\n",
                     static_cast<unsigned long long>(doc->findState(kA)->parentId),
                     static_cast<unsigned long long>(doc->findState(kB)->parentId),
                     static_cast<unsigned long long>(kP));
        return 1;
    }
    if (!pItem->isContainerMode()) {
        std::fprintf(stderr, "FAIL: P did not switch to container mode after gaining children\n");
        return 1;
    }
    const QRectF pRect = pItem->sceneRect();
    if (!pRect.contains(aItem->sceneRect()) || !pRect.contains(bItem->sceneRect())) {
        std::fprintf(stderr, "FAIL: P's container rect does not contain both children's rects\n");
        return 1;
    }
    const QRectF childUnion = aItem->sceneRect().united(bItem->sceneRect());
    if (pRect.width() <= childUnion.width() || pRect.height() <= childUnion.height()) {
        std::fprintf(stderr, "FAIL: P's container rect is not padded beyond its children's union\n");
        return 1;
    }

    // ---- (b) z-band order: container < edge(0) < leaf(10) ---------------------
    if (doc->machine().transitions.isEmpty()) {
        std::fprintf(stderr, "FAIL: hierarchy-canvas scenario needs an existing transition for the z-band check\n");
        return 1;
    }
    const app::TransitionItem* edge = presenter->debugTransitionItem(doc->machine().transitions.first().id);
    if (edge == nullptr) {
        std::fprintf(stderr, "FAIL: no TransitionItem found for the z-band check\n");
        return 1;
    }
    if (!(pItem->zValue() < edge->zValue() && edge->zValue() < aItem->zValue())) {
        std::fprintf(stderr, "FAIL: z-band order is not container < edge < leaf (got %.2f, %.2f, %.2f)\n",
                     pItem->zValue(), edge->zValue(), aItem->zValue());
        return 1;
    }

    // ---- (c) a click on P's empty interior selects nothing --------------------
    const QPointF interiorPoint((aItem->sceneRect().right() + bItem->sceneRect().left()) / 2.0,
                                 aItem->sceneRect().center().y());
    if (!pRect.contains(interiorPoint) || aItem->sceneRect().contains(interiorPoint) ||
        bItem->sceneRect().contains(interiorPoint)) {
        std::fprintf(stderr, "FAIL: the chosen interior probe point is not empty P interior\n");
        return 1;
    }
    view->debugMousePress(interiorPoint);
    view->debugMouseRelease(interiorPoint);
    QApplication::processEvents();
    if (presenter->currentSelection().kind != app::SelectionKind::None) {
        std::fprintf(stderr, "FAIL: clicking P's empty interior selected something (kind=%d)\n",
                     static_cast<int>(presenter->currentSelection().kind));
        return 1;
    }

    // ---- (d) a click on P's header band selects P ------------------------------
    const QPointF headerPoint(pRect.left() + 12.0, pRect.top() + 10.0);
    view->debugMousePress(headerPoint);
    view->debugMouseRelease(headerPoint);
    QApplication::processEvents();
    if (presenter->currentSelection().kind != app::SelectionKind::State || presenter->currentSelection().id != kP) {
        std::fprintf(stderr, "FAIL: clicking P's header band did not select (State, %llu) (got kind=%d id=%llu)\n",
                     static_cast<unsigned long long>(kP), static_cast<int>(presenter->currentSelection().kind),
                     static_cast<unsigned long long>(presenter->currentSelection().id));
        return 1;
    }

    // ---- (e) Q renders as a container too --------------------------------------
    if (!qItem->isContainerMode()) {
        std::fprintf(stderr, "FAIL: Q did not switch to container mode after gaining children\n");
        return 1;
    }
    const QRectF qRect = qItem->sceneRect();
    if (!qRect.contains(r1Item->sceneRect()) || !qRect.contains(r2Item->sceneRect())) {
        std::fprintf(stderr, "FAIL: Q's container rect does not contain both regions' rects\n");
        return 1;
    }

    // ---- (f) a per-compound initial-child marker exists for P -----------------
    if (doc->findState(kP)->initialChildId != kA) {
        std::fprintf(stderr, "FAIL: P's initialChildId was not auto-assigned to A\n");
        return 1;
    }
    if (!presenter->debugHasCompoundInitialMarker(kP)) {
        std::fprintf(stderr, "FAIL: no per-compound initial-child marker for P\n");
        return 1;
    }

    // ---- (g) entry actions on an ALREADY-container P grow its dynamic ---------
    // ---- header band instead of vanishing off it -------------------------------
    const qreal pBareHeaderHeight = pItem->containerHeaderHeight();
    kernel.send(app::events::SetEntryActionsRequested{
        .id = kP, .entryActions = QStringList{QStringLiteral("logEnter()"), QStringLiteral("startTimer()")}});
    QApplication::processEvents();
    if (pItem->containerHeaderHeight() <= pBareHeaderHeight) {
        std::fprintf(stderr,
                      "FAIL: P's container header height did not grow for its new entry actions (bare=%.1f "
                      "now=%.1f)\n",
                      pBareHeaderHeight, pItem->containerHeaderHeight());
        return 1;
    }
    if (pItem->sceneRect().height() <= pRect.height()) {
        std::fprintf(stderr, "FAIL: P's pushed container rect did not grow taller for its new entry actions\n");
        return 1;
    }

    // ---- (h) a long name with only a small child must not clip: the header's
    // ---- min-width floor (containerMinHeaderWidth()) wins over the children's
    // ---- union ---------------------------------------------------------------
    kernel.send(app::events::ReparentStateRequested{.id = kB, .parentId = 0});  // P keeps only its small child A
    QApplication::processEvents();
    kernel.send(app::events::RenameStateRequested{
        .id = kP, .name = QStringLiteral("A Very Long Container Name That Must Never Clip The Header Band")});
    QApplication::processEvents();
    if (pItem->sceneRect().width() < pItem->containerMinHeaderWidth()) {
        std::fprintf(stderr,
                      "FAIL: P's pushed container rect width (%.1f) is narrower than its own "
                      "containerMinHeaderWidth (%.1f) -- the long name would clip\n",
                      pItem->sceneRect().width(), pItem->containerMinHeaderWidth());
        return 1;
    }
    // Restore P{A,B} so the cleanup below deletes exactly what this scenario added.
    kernel.send(app::events::ReparentStateRequested{.id = kB, .parentId = kP});
    QApplication::processEvents();

    scene->clearSelection();
    QApplication::processEvents();
    if (!saveSceneRegionCapture(loginPane, (pItem->sceneRect() | qRect).adjusted(-24.0, -24.0, 24.0, 24.0),
                                 "probe-hierarchy-canvas")) {
        return 1;
    }

    // ---- clean up: delete P and Q (cascades A/B and R1/R2) as ONE batch -------
    pItem->setSelected(true);
    qItem->setSelected(true);
    QApplication::processEvents();
    presenter->debugDeleteSelection();
    QApplication::processEvents();
    if (doc->machine().states != originalStates || doc->machine().transitions != originalTransitions) {
        std::fprintf(stderr, "FAIL: the hierarchy-canvas scenario did not restore the machine it borrowed\n");
        return 1;
    }
    scene->clearSelection();
    QApplication::processEvents();

    std::printf("PASS: gui-probe scenario hierarchy-canvas (P{A,B} + Parallel Q{R1,R2} containers, z-band order, "
                "header selects/interior click-through, per-compound initial marker, dynamic container header "
                "entry-action growth + long-name min-width)\n");
    return 0;
}

// Scenario "reparent-drag": a real mouse-driven NodeDrag (debugMousePress/Move/
// Release, not setPos) into a container commits its move and reparent as one
// undo batch, highlighting the drop candidate mid-gesture. With leaves L1/L2
// and P{A}: (a)+(d) drag L1 into P and undo it whole; (b) a plain drag of A
// never detaches it; (c) drag P's header, moving A and L2 by the same
// grid-multiple delta. Cleans up by deleting P and L1 as one batch.
int runReparentDragScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::CanvasPresenter* presenter = loginPane->presenter();
    app::CanvasView* view = loginPane->canvasView();
    QGraphicsScene* scene = view != nullptr ? view->scene() : nullptr;
    if (doc == nullptr || presenter == nullptr || view == nullptr || scene == nullptr) {
        std::fprintf(stderr, "FAIL: reparent-drag scenario preconditions (doc/presenter/view/scene) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    scene->clearSelection();
    QApplication::processEvents();

    const QVector<app::State> originalStates = doc->machine().states;
    const QVector<app::Transition> originalTransitions = doc->machine().transitions;

    // ---- build L1, L2 and compound P{A}, far outside the fixture --------------
    // Authored positions sit a full 12px off the 24px grid, away from the
    // kSnapRadius=4.0 boundary: a leaf's first setPos runs before addItem and
    // never snaps, but a later in-scene setPos of the same value (undo's
    // restore echo) would, settling onto the snapped neighbor.
    const quint64 kP = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(2412.0, -516.0)});
    const quint64 kA = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(2364.0, -372.0)});
    kernel.send(app::events::ReparentStateRequested{.id = kA, .parentId = kP});
    const quint64 kL1 = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(2796.0, -516.0)});
    const quint64 kL2 = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(2796.0, -372.0)});
    QApplication::processEvents();  // settle

    app::StateItem* pItem = findStateItemById(scene, kP);
    app::StateItem* aItem = findStateItemById(scene, kA);
    app::StateItem* l1Item = findStateItemById(scene, kL1);
    app::StateItem* l2Item = findStateItemById(scene, kL2);
    if (pItem == nullptr || aItem == nullptr || l1Item == nullptr || l2Item == nullptr) {
        std::fprintf(stderr, "FAIL: reparent-drag scenario's P/A/L1/L2 StateItems were not all created\n");
        return 1;
    }
    if (doc->findState(kA)->parentId != kP || !pItem->isContainerMode()) {
        std::fprintf(stderr, "FAIL: reparent-drag scenario's initial P{A} setup did not take\n");
        return 1;
    }

    // ---- (a)+(d): drag L1 into P's interior; P highlights mid-gesture; one --
    // ---- undo reverts BOTH the move and the reparent -------------------------
    const QPointF l1PosBefore = doc->findState(kL1)->pos;
    const QRectF pRectBeforeDrop = pItem->sceneRect();
    // A point well inside P's padding moat (kContainerPadding=16), clear of A's
    // own leaf rect.
    const QPointF dropPoint(pRectBeforeDrop.right() - 8.0, pRectBeforeDrop.bottom() - 8.0);
    view->debugMousePress(l1Item->sceneRect().center());
    view->debugMouseMove(dropPoint);
    // Mid-gesture (d): the drop-candidate resolution runs synchronously inside
    // the mouse-move dispatch, so no processEvents() is needed.
    if (!pItem->isDropHighlighted()) {
        std::fprintf(stderr, "FAIL: P was not drop-highlighted mid-drag with L1 over its interior\n");
        return 1;
    }
    view->debugMouseRelease(dropPoint);
    QApplication::processEvents();
    if (doc->findState(kL1)->parentId != kP) {
        std::fprintf(stderr, "FAIL: dropping L1 on P did not reparent it (parentId=%llu, want %llu)\n",
                     static_cast<unsigned long long>(doc->findState(kL1)->parentId),
                     static_cast<unsigned long long>(kP));
        return 1;
    }
    if (!pItem->sceneRect().contains(l1Item->sceneRect())) {
        std::fprintf(stderr, "FAIL: P's container rect does not contain L1 after the reparenting drop\n");
        return 1;
    }
    if (pItem->isDropHighlighted()) {
        std::fprintf(stderr, "FAIL: P's drop-highlight was not cleared once the NodeDrag session committed\n");
        return 1;
    }
    if (!saveSceneRegionCapture(loginPane, pItem->sceneRect().adjusted(-24.0, -24.0, 24.0, 24.0),
                                 "probe-reparent-drag")) {
        return 1;
    }
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();
    if (doc->findState(kL1)->parentId != 0 || doc->findState(kL1)->pos != l1PosBefore) {
        std::fprintf(stderr,
                      "FAIL: one undo did not revert BOTH L1's reparent and its position (parentId=%llu pos=%.1f,"
                      "%.1f want 0, %.1f,%.1f)\n",
                      static_cast<unsigned long long>(doc->findState(kL1)->parentId), doc->findState(kL1)->pos.x(),
                      doc->findState(kL1)->pos.y(), l1PosBefore.x(), l1PosBefore.y());
        return 1;
    }

    // ---- (b): a plain drag PLACES, it never detaches: P's hull follows A live
    // ---- onto empty canvas, the release keeps A a child, and only the explicit
    // ---- verb takes it out --------------------------------------------------------
    const QRectF pRectAtExitDragStart = pItem->sceneRect();
    view->debugMousePress(aItem->sceneRect().center());
    view->debugMouseMove(QPointF(2360.0, 200.0));  // clearly empty canvas, well below P
    // The box follows the drag live, growing to keep holding A.
    if (!pItem->sceneRect().contains(aItem->sceneRect())) {
        std::fprintf(stderr, "FAIL: (b) P's hull did not follow A live -- container %.1f,%.1f %.1fx%.1f\n",
                     pItem->sceneRect().x(), pItem->sceneRect().y(), pItem->sceneRect().width(),
                     pItem->sceneRect().height());
        return 1;
    }
    // A is P's only child here (L1 went back to root on undo), so the hull
    // tracks it at constant size; assert that it moved, not that it grew. The
    // multi-child stretch is (d)'s case.
    if (pItem->sceneRect().topLeft() == pRectAtExitDragStart.topLeft()) {
        std::fprintf(stderr, "FAIL: (b) P's hull did not move with A (still at %.1f,%.1f)\n",
                     pRectAtExitDragStart.x(), pRectAtExitDragStart.y());
        return 1;
    }
    if (!pItem->isDropHighlighted()) {
        std::fprintf(stderr, "FAIL: (b) P is not lit as the owner while it still owns the dragged A\n");
        return 1;
    }
    if (!saveSceneRegionCapture(loginPane, pItem->sceneRect().adjusted(-24.0, -24.0, 24.0, 24.0),
                                 "probe-drag-live-growth")) {
        return 1;
    }
    view->debugMouseRelease(QPointF(2360.0, 200.0));
    QApplication::processEvents();
    if (doc->findState(kA)->parentId != kP) {
        std::fprintf(stderr, "FAIL: (b) an empty-canvas release detached A from P (parentId=%llu, want %llu)\n",
                     static_cast<unsigned long long>(doc->findState(kA)->parentId),
                     static_cast<unsigned long long>(kP));
        return 1;
    }
    // The explicit exit verb, driven through the real menu action: the only
    // manual path that changes a state's parent.
    QMenu* aMenu = presenter->debugBuildStateContextMenu(kA);
    if (aMenu == nullptr) {
        std::fprintf(stderr, "FAIL: (b) debugBuildStateContextMenu(A) returned no menu\n");
        return 1;
    }
    QAction* moveOut = nullptr;
    for (QAction* action : aMenu->actions()) {
        if (action->text() == QStringLiteral("Move Out One Level")) {
            moveOut = action;
        }
    }
    if (moveOut == nullptr) {
        std::fprintf(stderr, "FAIL: (b) a child state's context menu does not offer \"Move Out One Level\"\n");
        return 1;
    }
    moveOut->trigger();
    aMenu->deleteLater();
    QApplication::processEvents();
    if (doc->findState(kA)->parentId != 0) {
        std::fprintf(stderr, "FAIL: (b) the Move Out One Level verb did not detach A (parentId=%llu)\n",
                     static_cast<unsigned long long>(doc->findState(kA)->parentId));
        return 1;
    }
    if (pItem->isContainerMode()) {
        std::fprintf(stderr, "FAIL: P did not fall back to leaf mode once childless\n");
        return 1;
    }
    // A root state has nothing to leave: the verb is absent, not disabled.
    QMenu* rootMenu = presenter->debugBuildStateContextMenu(kA);
    bool rootOffersMoveOut = false;
    if (rootMenu != nullptr) {
        for (QAction* action : rootMenu->actions()) {
            rootOffersMoveOut = rootOffersMoveOut || action->text() == QStringLiteral("Move Out One Level");
        }
        rootMenu->deleteLater();
    }
    if (rootOffersMoveOut) {
        std::fprintf(stderr, "FAIL: (b) a root state's context menu offers \"Move Out One Level\"\n");
        return 1;
    }

    // ---- (c): rebuild P{A, L2} and drag P's own header -- both children move
    // ---- by the SAME delta (subtree move), one undo reverts all -------------
    kernel.send(app::events::ReparentStateRequested{.id = kA, .parentId = kP});
    kernel.send(app::events::ReparentStateRequested{.id = kL2, .parentId = kP});
    QApplication::processEvents();
    if (doc->findState(kA)->parentId != kP || doc->findState(kL2)->parentId != kP || !pItem->isContainerMode()) {
        std::fprintf(stderr, "FAIL: rebuilding P{A, L2} did not take\n");
        return 1;
    }
    const QPointF aPosBefore = aItem->pos();
    const QPointF l2PosBefore = l2Item->pos();
    const QRectF pRectRebuilt = pItem->sceneRect();
    const QPointF grabPoint(pRectRebuilt.left() + 12.0, pRectRebuilt.top() + 10.0);  // header band, not the interior
    const QPointF delta(48.0, -24.0);  // multiples of the 24px grid: no soft-snap surprises
    view->debugMousePress(grabPoint);
    view->debugMouseMove(grabPoint + delta);
    view->debugMouseRelease(grabPoint + delta);
    QApplication::processEvents();
    // debugMouseMove round-trips through mapFromScene's integer viewport pixels
    // (unlike debugFrameDragMoved), so the shared delta may carry sub-pixel
    // rounding: assert A and L2 moved by exactly the same delta as each other,
    // and only loosely against the requested one.
    const QPointF aDelta = aItem->pos() - aPosBefore;
    const QPointF l2Delta = l2Item->pos() - l2PosBefore;
    const bool sameDelta = aDelta == l2Delta;
    // Tolerance covers the integer-pixel rounding and the dragged container's
    // own grid magnet (up to kSnapRadius, 4px). Descendants do not snap on
    // their own, so the same-delta half above is exact.
    const bool closeToRequested = std::hypot(aDelta.x() - delta.x(), aDelta.y() - delta.y()) <= 5.0;
    if (!sameDelta || !closeToRequested) {
        std::fprintf(stderr,
                      "FAIL: dragging P's header did not move A and L2 by the SAME delta "
                      "(A %.2f,%.2f L2 %.2f,%.2f want ~+%.1f,%.1f)\n",
                      aDelta.x(), aDelta.y(), l2Delta.x(), l2Delta.y(), delta.x(), delta.y());
        return 1;
    }
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();
    if (aItem->pos() != aPosBefore || l2Item->pos() != l2PosBefore) {
        std::fprintf(stderr, "FAIL: one undo did not restore P's whole subtree (A and L2) to its pre-drag position\n");
        return 1;
    }

    // ---- (d): live growth over a whole round trip on a multi-child P: the hull
    // ---- tracks A out, back, and at rest, always holds BOTH children, ownership
    // ---- never changes, and one undo restores -----------------------------------
    {
        const QRectF pStartRect = pItem->sceneRect();
        const QPointF aStart = aItem->pos();
        const QPointF aStartCenter = aItem->sceneRect().center();
        const QPointF farPoint(pStartRect.right() + 220.0, aStartCenter.y());
        view->debugMousePress(aStartCenter);
        view->debugMouseMove(farPoint);
        if (!pItem->sceneRect().contains(aItem->sceneRect()) || !pItem->sceneRect().contains(l2Item->sceneRect())) {
            std::fprintf(stderr, "FAIL: (d) P's hull does not hold BOTH children while A is dragged far out\n");
            return 1;
        }
        // Assert the hull re-derived, not that it grew: A crosses to the far
        // side of L2, so the union can legitimately narrow.
        if (pItem->sceneRect() == pStartRect) {
            std::fprintf(stderr, "FAIL: (d) P's hull held still while A moved (A %.1f,%.1f -> %.1f,%.1f)\n",
                         aStart.x(), aStart.y(), aItem->pos().x(), aItem->pos().y());
            return 1;
        }
        if (pItem->sceneRect().right() < aItem->sceneRect().right()) {
            std::fprintf(stderr, "FAIL: (d) P's hull does not reach past A's right edge at the far position\n");
            return 1;
        }
        // No other container claims A, so a release at any point of this drag
        // commits "still P's child".
        if (!pItem->isDropHighlighted()) {
            std::fprintf(stderr, "FAIL: (d) P stopped being lit as the owner while A was far outside\n");
            return 1;
        }
        view->debugMouseMove(aStartCenter + QPointF(24.0, 0.0));  // back near its origin
        if (!pItem->sceneRect().contains(aItem->sceneRect())) {
            std::fprintf(stderr, "FAIL: (d) P's hull lost A on the way back in\n");
            return 1;
        }
        view->debugMouseRelease(aStartCenter + QPointF(24.0, 0.0));
        QApplication::processEvents();
        if (doc->findState(kA)->parentId != kP) {
            std::fprintf(stderr, "FAIL: (d) the release did not keep A a child of P\n");
            return 1;
        }
        kernel.send(app::events::UndoRequested{});  // revert the small move -- restore the borrowed fixture
        QApplication::processEvents();
        if (aItem->pos() != aStart) {
            std::fprintf(stderr, "FAIL: (d) one undo did not restore A's pre-drag position\n");
            return 1;
        }
    }

    // ---- (e): an OVERLAPPING foreign container does not steal a child
    // ---- (dragging a child must not light up a root container its parent
    // ---- merely overlaps) --------------------------------------------------------
    const quint64 kF = doc->machine().nextId;
    {
        // F: a root container built right on top of P's area, so P's box and
        // F's box overlap the way two hand-placed compounds on one canvas do.
        const QRectF pRect = pItem->sceneRect();
        // F's own pos is irrelevant: a container's rect derives from its
        // children, so the child has to straddle P's right edge.
        kernel.send(app::events::AddStateRequested{.pos = QPointF(pRect.right() + 240.0, pRect.bottom() - 140.0)});
        kernel.send(app::events::AddStateRequested{.pos = QPointF(pRect.right() - 60.0, pRect.bottom() - 140.0),
                                                    .parentId = kF});
        QApplication::processEvents();
        app::StateItem* fItem = findStateItemById(scene, kF);
        if (fItem == nullptr || !fItem->isContainerMode()) {
            std::fprintf(stderr, "FAIL: (e) the foreign container F was not built\n");
            return 1;
        }
        const QRectF fRect = fItem->sceneRect();
        const QRectF overlap = pRect.intersected(fRect);
        if (overlap.width() < 48.0 || overlap.height() < 48.0) {
            std::fprintf(stderr, "FAIL: (e) fixture: P and F do not overlap enough (%.1fx%.1f)\n", overlap.width(),
                         overlap.height());
            return 1;
        }

        // Drag A into the overlap (inside F, still inside its starting box):
        // P keeps it, and F must not even light up.
        const QPointF aStart = aItem->pos();
        const QPointF overlapPoint = overlap.center();
        view->debugMousePress(aItem->sceneRect().center());
        view->debugMouseMove(overlapPoint);
        if (fItem->isDropHighlighted()) {
            std::fprintf(stderr, "FAIL: (e) F lit up while A was still inside its own parent's start box\n");
            return 1;
        }
        if (!pItem->isDropHighlighted()) {
            std::fprintf(stderr, "FAIL: (e) P stopped being lit as the owner inside its own start box\n");
            return 1;
        }
        view->debugMouseRelease(overlapPoint);
        QApplication::processEvents();
        if (doc->findState(kA)->parentId != kP) {
            std::fprintf(stderr, "FAIL: (e) an overlapping foreign container stole A (parentId=%llu, want %llu)\n",
                         static_cast<unsigned long long>(doc->findState(kA)->parentId),
                         static_cast<unsigned long long>(kP));
            return 1;
        }
        kernel.send(app::events::UndoRequested{});
        QApplication::processEvents();

        // ...and drag-into-a-container still works: past P's start box, F
        // claims A and the release re-parents. Halfway between the two right
        // edges gives the widest margin on both sides (inside F, clear of P),
        // so snap and alignment nudges cannot decide the outcome.
        const QPointF intoF((pRect.right() + fRect.right()) / 2.0, fRect.top() + 16.0);
        if (pRect.contains(intoF) || !fRect.contains(intoF)) {
            std::fprintf(stderr, "FAIL: (e) fixture: no point sits inside F and clear of P (P right %.1f, F right %.1f)\n",
                         pRect.right(), fRect.right());
            return 1;
        }
        view->debugMousePress(aItem->sceneRect().center());
        view->debugMouseMove(intoF);
        if (!fItem->isDropHighlighted()) {
            std::fprintf(stderr, "FAIL: (e) F did not claim A once A left P's start box\n");
            return 1;
        }
        view->debugMouseRelease(intoF);
        QApplication::processEvents();
        if (doc->findState(kA)->parentId != kF) {
            std::fprintf(stderr, "FAIL: (e) dropping A inside F did not re-parent it (parentId=%llu, want %llu)\n",
                         static_cast<unsigned long long>(doc->findState(kA)->parentId),
                         static_cast<unsigned long long>(kF));
            return 1;
        }
        kernel.send(app::events::UndoRequested{});  // back to P, pre-drag position
        QApplication::processEvents();
        if (doc->findState(kA)->parentId != kP || aItem->pos() != aStart) {
            std::fprintf(stderr, "FAIL: (e) one undo did not restore A to P at its pre-drag position\n");
            return 1;
        }
    }

    // ---- (f): drag compound P onto leaf L1: cursor at P's header over L1
    // ---- highlights L1; release reparents P into L1 (making L1 a compound);
    // ---- one undo restores P to root and L1 to leaf mode.
    {
        const QPointF pHeaderPress = QPointF(pItem->sceneRect().left() + 20.0, pItem->sceneRect().top() + 10.0);
        const QPointF l1Center = l1Item->sceneRect().center();

        view->debugMousePress(pHeaderPress);
        view->debugMouseMove(l1Center);
        if (!l1Item->isDropHighlighted()) {
            std::fprintf(stderr, "FAIL: (f) dragging compound P over leaf L1 did not highlight L1\n");
            return 1;
        }
        view->debugMouseRelease(l1Center);
        QApplication::processEvents();
        if (doc->findState(kP)->parentId != kL1) {
            std::fprintf(stderr, "FAIL: (f) dropping compound P onto leaf L1 did not reparent P into L1 (parentId=%llu, want %llu)\n",
                         static_cast<unsigned long long>(doc->findState(kP)->parentId),
                         static_cast<unsigned long long>(kL1));
            return 1;
        }
        if (!l1Item->isContainerMode()) {
            std::fprintf(stderr, "FAIL: (f) L1 did not switch to container mode after adopting P\n");
            return 1;
        }
        kernel.send(app::events::UndoRequested{});
        QApplication::processEvents();
        if (doc->findState(kP)->parentId != 0) {
            std::fprintf(stderr, "FAIL: (f) undo did not restore P to root (parentId=%llu)\n",
                         static_cast<unsigned long long>(doc->findState(kP)->parentId));
            return 1;
        }
        if (l1Item->isContainerMode()) {
            std::fprintf(stderr, "FAIL: (f) L1 did not restore to leaf mode after undo\n");
            return 1;
        }
    }

    // ---- clean up: delete P (cascades A/L2), L1 and F (cascades FC) -----------
    scene->clearSelection();
    pItem->setSelected(true);
    l1Item->setSelected(true);
    if (app::StateItem* fItem = findStateItemById(scene, kF)) {
        fItem->setSelected(true);
    }
    QApplication::processEvents();
    presenter->debugDeleteSelection();
    QApplication::processEvents();
    if (doc->machine().states != originalStates || doc->machine().transitions != originalTransitions) {
        std::fprintf(stderr, "FAIL: the reparent-drag scenario did not restore the machine it borrowed\n");
        return 1;
    }
    scene->clearSelection();
    QApplication::processEvents();

    std::printf("PASS: gui-probe scenario reparent-drag (drag-into-parent reparent + one-undo, drop-target "
                "highlight, a plain drag never detaches + live hull growth, Move Out One Level verb + "
                "leaf fallback, compound subtree drag + one-undo, an overlapping foreign container neither "
                "lights up nor steals while the child is home)\n");
    return 0;
}

// Scenario "nested-container-drag": five states nested one inside the next,
// the outermost dragged by its header. Every level is a container whose box
// derives from its child, so one drag moves five derived rects; this keeps
// that cascade finite (it once overflowed the stack). Also asserts that every
// state in the chain moves by the same delta and one undo restores the chain.
int runNestedContainerDragScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::CanvasPresenter* presenter = loginPane->presenter();
    app::CanvasView* view = loginPane->canvasView();
    QGraphicsScene* scene = view != nullptr ? view->scene() : nullptr;
    if (doc == nullptr || presenter == nullptr || view == nullptr || scene == nullptr) {
        std::fprintf(stderr, "FAIL: nested-container-drag preconditions (doc/presenter/view/scene) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    scene->clearSelection();
    QApplication::processEvents();

    const QVector<app::State> originalStates = doc->machine().states;
    const QVector<app::Transition> originalTransitions = doc->machine().transitions;

    // Five levels, each born inside the previous one. Positions stay 12px off
    // the 24px grid and far from every other fixture.
    constexpr int kLevels = 5;
    quint64 ids[kLevels] = {0};
    for (int level = 0; level < kLevels; ++level) {
        ids[level] = doc->machine().nextId;
        const QPointF pos(4212.0 + 36.0 * level, -516.0 + 36.0 * level);
        kernel.send(app::events::AddStateRequested{.pos = pos, .parentId = level == 0 ? 0 : ids[level - 1]});
    }
    QApplication::processEvents();

    app::StateItem* items[kLevels] = {nullptr};
    for (int level = 0; level < kLevels; ++level) {
        items[level] = findStateItemById(scene, ids[level]);
        if (items[level] == nullptr) {
            std::fprintf(stderr, "FAIL: nested-container-drag level %d has no StateItem\n", level);
            return 1;
        }
    }
    for (int level = 0; level < kLevels - 1; ++level) {
        if (!items[level]->isContainerMode()) {
            std::fprintf(stderr, "FAIL: nested-container-drag level %d did not become a container\n", level);
            return 1;
        }
        if (!items[level]->sceneRect().contains(items[level + 1]->sceneRect())) {
            std::fprintf(stderr, "FAIL: nested-container-drag level %d does not wrap level %d\n", level, level + 1);
            return 1;
        }
    }

    // Drag the outermost container by its header band. Reaching the assertions
    // below at all is the crash regression: this call once recursed until the
    // stack was gone.
    QPointF before[kLevels];
    for (int level = 0; level < kLevels; ++level) {
        before[level] = items[level]->pos();
    }
    const QRectF outerRect = items[0]->sceneRect();
    const QPointF grabPoint(outerRect.left() + 12.0, outerRect.top() + 10.0);
    const QPointF delta(48.0, -24.0);  // grid multiples: no soft-snap surprises
    // Several small steps, not one jump: each move of a real drag re-derives
    // all five boxes, the cascade under test. The last step lands on `delta`.
    constexpr int kSteps = 8;
    view->debugMousePress(grabPoint);
    presenter->debugResetContainerRefreshPasses();
    for (int step = 1; step <= kSteps; ++step) {
        view->debugMouseMove(grabPoint + delta * (static_cast<qreal>(step) / kSteps));
    }
    const int passes = presenter->debugContainerRefreshPasses();
    view->debugMouseRelease(grabPoint + delta);
    QApplication::processEvents();

    // Cost assertion: a refreshContainers box write must not re-enter the
    // item's onMoved_ as a user move, or the subtree delta re-applies in a cycle
    // and one drag step costs exponentially in nesting depth. The law is one
    // pass per moved item per move; the bound is calibrated (24 passes fixed, 86
    // with the derived-move gate removed) and the gap widens with depth.
    const int maxPasses = kSteps * kLevels;
    if (passes > maxPasses) {
        std::fprintf(stderr, "FAIL: nested-container-drag cost %d container passes over %d moves (max %d) -- the "
                              "derived-geometry cycle is back\n",
                     passes, kSteps, maxPasses);
        return 1;
    }

    // Same delta as each other is the invariant; the requested delta is only
    // loosely checked (debugMouseMove rounds to integer viewport pixels).
    const QPointF innermostDelta = items[kLevels - 1]->pos() - before[kLevels - 1];
    for (int level = 1; level < kLevels; ++level) {
        const QPointF levelDelta = items[level]->pos() - before[level];
        if (levelDelta != innermostDelta) {
            std::fprintf(stderr, "FAIL: nested-container-drag level %d moved %.2f,%.2f but the innermost moved %.2f,%.2f\n",
                         level, levelDelta.x(), levelDelta.y(), innermostDelta.x(), innermostDelta.y());
            return 1;
        }
    }
    // Exact between items, loose against the requested delta (rounding + magnet).
    if (std::hypot(innermostDelta.x() - delta.x(), innermostDelta.y() - delta.y()) > 5.0) {
        std::fprintf(stderr, "FAIL: nested-container-drag moved the chain by %.2f,%.2f (want ~%.1f,%.1f)\n",
                     innermostDelta.x(), innermostDelta.y(), delta.x(), delta.y());
        return 1;
    }
    for (int level = 0; level < kLevels - 1; ++level) {
        if (!items[level]->sceneRect().contains(items[level + 1]->sceneRect())) {
            std::fprintf(stderr, "FAIL: nested-container-drag level %d stopped wrapping level %d after the drag\n",
                         level, level + 1);
            return 1;
        }
    }
    if (!saveSceneRegionCapture(loginPane, items[0]->sceneRect().adjusted(-24.0, -24.0, 24.0, 24.0),
                                 "probe-nested-drag")) {
        return 1;
    }

    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();
    for (int level = 0; level < kLevels; ++level) {
        if (items[level]->pos() != before[level]) {
            std::fprintf(stderr, "FAIL: one undo did not restore nested level %d to its pre-drag position\n", level);
            return 1;
        }
    }

    // Clean up: deleting the outermost cascades the whole chain.
    scene->clearSelection();
    items[0]->setSelected(true);
    QApplication::processEvents();
    presenter->debugDeleteSelection();
    QApplication::processEvents();
    if (doc->machine().states != originalStates || doc->machine().transitions != originalTransitions) {
        std::fprintf(stderr, "FAIL: the nested-container-drag scenario did not restore the machine it borrowed\n");
        return 1;
    }
    scene->clearSelection();
    QApplication::processEvents();

    std::printf("PASS: gui-probe scenario nested-container-drag (5-level nesting, outermost header drag stays "
                "finite, whole chain moves by one delta, one undo restores)\n");
    return 0;
}

// Scenario "cross-hierarchy-edges": with P{A,B} and an external leaf L, authors
// P -> A, A -> L (crosses P's border) and A -> B through AddTransitionRequested
// (a container hides its wire-drag handles) and holds each drawn edge to the
// one-polyline law above; the router sees rects, not a hierarchy. Also checks
// A->L's endpoints against the borders, and that growing P by dragging A moves
// the P->A anchors with the new rects. Cleans up by deleting P and L.
int runCrossHierarchyEdgesScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::CanvasPresenter* presenter = loginPane->presenter();
    QGraphicsScene* scene = loginPane->canvasView() != nullptr ? loginPane->canvasView()->scene() : nullptr;
    if (doc == nullptr || presenter == nullptr || scene == nullptr) {
        std::fprintf(stderr, "FAIL: cross-hierarchy-edges scenario preconditions (doc/presenter/scene) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    scene->clearSelection();
    QApplication::processEvents();

    const QVector<app::State> originalStates = doc->machine().states;
    const QVector<app::Transition> originalTransitions = doc->machine().transitions;

    // ---- build P{A,B} and an external leaf L, far outside the fixture ---------
    const quint64 kP = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(3200.0, -520.0)});
    const quint64 kA = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(3160.0, -380.0)});
    const quint64 kB = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(3360.0, -380.0)});
    kernel.send(app::events::ReparentStateRequested{.id = kA, .parentId = kP});
    kernel.send(app::events::ReparentStateRequested{.id = kB, .parentId = kP});
    const quint64 kL = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(3640.0, -520.0)});
    QApplication::processEvents();  // settle

    app::StateItem* pItem = findStateItemById(scene, kP);
    app::StateItem* aItem = findStateItemById(scene, kA);
    app::StateItem* bItem = findStateItemById(scene, kB);
    app::StateItem* lItem = findStateItemById(scene, kL);
    if (pItem == nullptr || aItem == nullptr || bItem == nullptr || lItem == nullptr) {
        std::fprintf(stderr, "FAIL: cross-hierarchy-edges scenario's P/A/B/L StateItems were not all created\n");
        return 1;
    }
    if (doc->findState(kA)->parentId != kP || doc->findState(kB)->parentId != kP || !pItem->isContainerMode()) {
        std::fprintf(stderr, "FAIL: cross-hierarchy-edges scenario's P{A,B} setup did not take\n");
        return 1;
    }

    // ---- author the three cross-boundary transitions --------------------------
    kernel.send(app::events::AddTransitionRequested{.from = kP, .to = kA});  // parent -> own child
    QApplication::processEvents();
    const quint64 kPA = doc->machine().transitions.last().id;
    kernel.send(app::events::AddTransitionRequested{.from = kA, .to = kL});  // inside -> outside
    QApplication::processEvents();
    const quint64 kAL = doc->machine().transitions.last().id;
    kernel.send(app::events::AddTransitionRequested{.from = kA, .to = kB});  // siblings inside one container
    QApplication::processEvents();
    const quint64 kAB = doc->machine().transitions.last().id;

    // ---- the unified-wire invariant, numerically, on the DRAWN geometry -------
    // All three edges are Normal: one polyline each, at most two corners, no
    // U-turn wrap, both ends on their own node's border along the outward
    // normal. Containment is invisible to the router.
    const auto rectOf = [&](quint64 stateId) {
        app::StateItem* item = findStateItemById(scene, stateId);
        return item != nullptr ? item->sceneRect() : QRectF();
    };
    const auto checkUnifiedEdge = [&](quint64 id, const QRectF& sourceRect, const QRectF& targetRect,
                                       bool cornerBound, const char* what) -> std::optional<UnifiedWire> {
        const app::TransitionItem* edge = presenter->debugTransitionItem(id);
        const app::Transition* transition = doc->findTransition(id);
        if (edge == nullptr || transition == nullptr) {
            std::fprintf(stderr, "FAIL: %s has no edge visual/document row\n", what);
            return std::nullopt;
        }
        UnifiedWire wire;
        if (!unifiedWireHolds(edge->debugRoute(), what, &wire) ||
            !unifiedWireMeetsNodes(&wire, sourceRect, targetRect, what) ||
            !unifiedLabelLawHolds(edge->debugRoute(), transition->labelOffset, what)) {
            return std::nullopt;
        }
        if (cornerBound && !unifiedWireCornerBoundHolds(wire, what)) {
            return std::nullopt;
        }
        return wire;
    };

    // P->A holds the full bound too: node_anchors.cpp's containment branch
    // routes a parent->child edge through the parent's border nearest the
    // child, departing inward at the child's corridor coordinate, so both end
    // directions agree and orthogonalPortRun stays in its straight/L/Z family.
    const auto paWire =
        checkUnifiedEdge(kPA, rectOf(kP), rectOf(kA), /*cornerBound=*/true, "P->A (parent-to-child)");
    const auto alWire =
        checkUnifiedEdge(kAL, rectOf(kA), rectOf(kL), /*cornerBound=*/true, "A->L (inside-to-outside)");
    const auto abWire = checkUnifiedEdge(kAB, rectOf(kA), rectOf(kB), /*cornerBound=*/true, "A->B (siblings)");
    if (!paWire.has_value() || !alWire.has_value() || !abWire.has_value()) {
        return 1;
    }

    // ---- A->L's two endpoints sit exactly on A's and L's own borders ---------
    // What is left beyond checkUnifiedEdge is the hierarchy-specific half: the
    // source end belongs to the child, not the container; the wire starts inside
    // P and crosses out.
    if (!rectOf(kP).contains(alWire->sourceAnchor)) {
        std::fprintf(stderr, "FAIL: A->L starts outside P -- it left the container's border, not A's own\n");
        return 1;
    }
    if (rectOf(kP).contains(alWire->targetAnchor)) {
        std::fprintf(stderr, "FAIL: A->L arrives inside P -- it never crossed the container boundary\n");
        return 1;
    }

    if (!saveSceneRegionCapture(loginPane, (rectOf(kP) | rectOf(kL)).adjusted(-24.0, -24.0, 24.0, 24.0),
                                 "probe-cross-hierarchy-edges")) {
        return 1;
    }

    // ---- container stretch: dragging A reroutes P->A with the new rects -------
    const QRectF pRectBefore = rectOf(kP);
    const QPointF paSourceAnchorBefore = paWire->sourceAnchor;
    kernel.send(app::events::MoveStateRequested{.id = kA, .pos = QPointF(4200.0, 400.0)});
    QApplication::processEvents();
    const QRectF pRectAfter = rectOf(kP);
    if (pRectAfter == pRectBefore || !pRectAfter.contains(rectOf(kA))) {
        std::fprintf(stderr, "FAIL: dragging A far away did not grow P's container box around it\n");
        return 1;
    }
    const auto paAfter =
        checkUnifiedEdge(kPA, rectOf(kP), rectOf(kA), /*cornerBound=*/true, "P->A after the container stretch");
    if (!paAfter.has_value()) {
        return 1;
    }
    if (paAfter->sourceAnchor == paSourceAnchorBefore) {
        std::fprintf(stderr, "FAIL: P->A's anchor did not move with P's stretched container box\n");
        return 1;
    }

    // ---- clean up: delete P (cascades A/B) and L as ONE batch ------------------
    scene->clearSelection();
    pItem->setSelected(true);
    lItem->setSelected(true);
    QApplication::processEvents();
    presenter->debugDeleteSelection();
    QApplication::processEvents();
    if (doc->machine().states != originalStates || doc->machine().transitions != originalTransitions) {
        std::fprintf(stderr, "FAIL: the cross-hierarchy-edges scenario did not restore the machine it borrowed\n");
        return 1;
    }
    scene->clearSelection();
    QApplication::processEvents();

    std::printf("PASS: gui-probe scenario cross-hierarchy-edges (parent-to-child/inside-to-outside/sibling unified "
                "wires, A->L endpoints on their own borders across P's boundary, container-stretch reroute)\n");
    return 0;
}

// Scenario "hierarchy-simulate": Q{R1{a,b},R2{c,d}} (Parallel) plus P{A,B}, run
// through the real SetModeRequested/RunRequested path. Both regions and their
// containers glow together, one "Go" updates both in one macrostep, the trace
// gains rows for both, Back restores the pre-Go picture, and leaving Simulate
// clears every glow. Runs right before runXStateInteropScenario.
int runHierarchySimulateScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
    app::CanvasPresenter* presenter = loginPane->presenter();
    QGraphicsScene* scene = loginPane->canvasView() != nullptr ? loginPane->canvasView()->scene() : nullptr;
    app::TracePanel* trace = window.findChild<app::TracePanel*>();
    if (doc == nullptr || sim == nullptr || presenter == nullptr || scene == nullptr || trace == nullptr) {
        std::fprintf(stderr,
                      "FAIL: hierarchy-simulate scenario preconditions (doc/sim/presenter/scene/trace) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    scene->clearSelection();
    QApplication::processEvents();

    const QVector<app::State> originalStates = doc->machine().states;
    const QVector<app::Transition> originalTransitions = doc->machine().transitions;
    const quint64 originalInitialStateId = doc->machine().initialStateId;

    // ---- build Q{R1{a,b},R2{c,d}} (Parallel) + P{A,B}, far outside the fixture -
    const quint64 kQ = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(2000.0, -900.0)});
    const quint64 kR1 = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(1940.0, -760.0)});
    const quint64 kA1 = doc->machine().nextId;  // R1's own child "a"
    kernel.send(app::events::AddStateRequested{.pos = QPointF(1900.0, -620.0)});
    const quint64 kB1 = doc->machine().nextId;  // R1's own child "b"
    kernel.send(app::events::AddStateRequested{.pos = QPointF(1980.0, -620.0)});
    const quint64 kR2 = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(2160.0, -760.0)});
    const quint64 kC2 = doc->machine().nextId;  // R2's own child "c"
    kernel.send(app::events::AddStateRequested{.pos = QPointF(2120.0, -620.0)});
    const quint64 kD2 = doc->machine().nextId;  // R2's own child "d"
    kernel.send(app::events::AddStateRequested{.pos = QPointF(2200.0, -620.0)});
    const quint64 kP = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(2440.0, -900.0)});
    const quint64 kPA = doc->machine().nextId;  // P's own child "A"
    kernel.send(app::events::AddStateRequested{.pos = QPointF(2400.0, -760.0)});
    const quint64 kPB = doc->machine().nextId;  // P's own child "B"
    kernel.send(app::events::AddStateRequested{.pos = QPointF(2480.0, -760.0)});
    // Reparent order: region -> Q, each leaf -> its own region, then
    // SetStateKindRequested{Q, Parallel}.
    kernel.send(app::events::ReparentStateRequested{.id = kR1, .parentId = kQ});
    kernel.send(app::events::ReparentStateRequested{.id = kA1, .parentId = kR1});
    kernel.send(app::events::ReparentStateRequested{.id = kB1, .parentId = kR1});
    kernel.send(app::events::ReparentStateRequested{.id = kR2, .parentId = kQ});
    kernel.send(app::events::ReparentStateRequested{.id = kC2, .parentId = kR2});
    kernel.send(app::events::ReparentStateRequested{.id = kD2, .parentId = kR2});
    kernel.send(app::events::SetStateKindRequested{.id = kQ, .kind = app::StateKind::Parallel});
    kernel.send(app::events::ReparentStateRequested{.id = kPA, .parentId = kP});
    kernel.send(app::events::ReparentStateRequested{.id = kPB, .parentId = kP});
    QApplication::processEvents();  // settle

    const quint64 kGo1 = doc->machine().nextId;
    kernel.send(app::events::AddTransitionRequested{.from = kA1, .to = kB1});  // a -Go-> b, inside R1
    kernel.send(app::events::SetTransitionEventRequested{.id = kGo1, .event = QStringLiteral("Go")});
    const quint64 kGo2 = doc->machine().nextId;
    kernel.send(app::events::AddTransitionRequested{.from = kC2, .to = kD2});  // c -Go-> d, inside R2
    kernel.send(app::events::SetTransitionEventRequested{.id = kGo2, .event = QStringLiteral("Go")});
    const quint64 kWide = doc->machine().nextId;
    kernel.send(app::events::AddTransitionRequested{.from = kQ, .to = kP});  // Q -Wide-> P (authored, never fired)
    kernel.send(app::events::SetTransitionEventRequested{.id = kWide, .event = QStringLiteral("Wide")});
    QApplication::processEvents();
    // +3, not an absolute count: loginPane's machine already carries the
    // login-flow demo's transitions.
    if (doc->machine().transitions.size() != originalTransitions.size() + 3) {
        std::fprintf(stderr, "FAIL: hierarchy-simulate scenario topology build did not add 3 transitions\n");
        return 1;
    }

    app::StateItem* qItem = findStateItemById(scene, kQ);
    app::StateItem* r1Item = findStateItemById(scene, kR1);
    app::StateItem* r2Item = findStateItemById(scene, kR2);
    app::StateItem* pItem = findStateItemById(scene, kP);
    if (qItem == nullptr || r1Item == nullptr || r2Item == nullptr || pItem == nullptr ||
        findStateItemById(scene, kA1) == nullptr || findStateItemById(scene, kB1) == nullptr ||
        findStateItemById(scene, kC2) == nullptr || findStateItemById(scene, kD2) == nullptr ||
        findStateItemById(scene, kPA) == nullptr || findStateItemById(scene, kPB) == nullptr) {
        std::fprintf(stderr, "FAIL: hierarchy-simulate scenario's StateItems were not all created\n");
        return 1;
    }

    // ---- switch to Simulate through the REAL command front door ---------------
    // A hierarchical+parallel machine (Q Parallel, R1/R2 nested inside it) is
    // accepted here, not refused.
    kernel.send(app::events::SetInitialStateRequested{.id = kQ});
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
    if (sim->mode() != app::events::Mode::Simulate) {
        std::fprintf(stderr, "FAIL: SimulationAgent refused Simulate mode on a hierarchical+parallel machine\n");
        return 1;
    }
    kernel.send(app::events::RunRequested{});
    QApplication::processEvents();

    // ---- multiple StateItems glow simultaneously: parallel regions + their
    // containers, both at once ---------------------------------------------
    if (!presenter->debugStateGlowing(kQ) || !presenter->debugStateGlowing(kR1) ||
        !presenter->debugStateGlowing(kA1) || !presenter->debugStateGlowing(kR2) ||
        !presenter->debugStateGlowing(kC2) || presenter->debugStateGlowing(kB1) ||
        presenter->debugStateGlowing(kD2) || presenter->debugStateGlowing(kP) ||
        presenter->debugStateGlowing(kPA) || presenter->debugStateGlowing(kPB)) {
        std::fprintf(stderr,
                      "FAIL: Run did not light up Q+R1+a+R2+c simultaneously (parallel regions + containers)\n");
        return 1;
    }
    // Run() clears trace() and the trace adapter rebuilds the whole panel when
    // trace() shrinks, so a row count from before Run is not a baseline; assert
    // only that the panel shows something.
    if (trace->rowCount() <= 0) {
        std::fprintf(stderr, "FAIL: the trace panel is empty after Run's activation\n");
        return 1;
    }
    if (!saveSceneRegionCapture(loginPane, qItem->sceneRect().adjusted(-24.0, -24.0, 24.0, 24.0),
                                 "probe-hierarchy-simulate")) {
        return 1;
    }

    // ---- one event, both regions: ONE macrostep updates both glows ------------
    // rowCountBeforeGo is a sound baseline here: sendEvent() only appends.
    const int rowCountBeforeGo = trace->rowCount();
    kernel.send(app::events::SendEventRequested{.name = QStringLiteral("Go")});
    QApplication::processEvents();
    if (presenter->debugStateGlowing(kA1) || presenter->debugStateGlowing(kC2) ||
        !presenter->debugStateGlowing(kB1) || !presenter->debugStateGlowing(kD2) ||
        !presenter->debugStateGlowing(kQ) || !presenter->debugStateGlowing(kR1) ||
        !presenter->debugStateGlowing(kR2)) {
        std::fprintf(stderr, "FAIL: Go did not update BOTH regions' glow (a->b, c->d) in one macrostep\n");
        return 1;
    }
    if (trace->rowCount() <= rowCountBeforeGo) {
        std::fprintf(stderr, "FAIL: the trace panel did not gain a row for Go's firing\n");
        return 1;
    }

    // ---- Back restores the previous glow picture -------------------------------
    kernel.send(app::events::BackRequested{});
    QApplication::processEvents();
    if (!presenter->debugStateGlowing(kA1) || !presenter->debugStateGlowing(kC2) ||
        presenter->debugStateGlowing(kB1) || presenter->debugStateGlowing(kD2)) {
        std::fprintf(stderr, "FAIL: Back did not restore the pre-Go glow picture (a, c active again)\n");
        return 1;
    }

    // ---- switching back to Design clears every glow ----------------------------
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Design});
    QApplication::processEvents();
    if (presenter->debugStateGlowing(kQ) || presenter->debugStateGlowing(kR1) || presenter->debugStateGlowing(kA1) ||
        presenter->debugStateGlowing(kB1) || presenter->debugStateGlowing(kR2) || presenter->debugStateGlowing(kC2) ||
        presenter->debugStateGlowing(kD2) || presenter->debugStateGlowing(kP) || presenter->debugStateGlowing(kPA) ||
        presenter->debugStateGlowing(kPB)) {
        std::fprintf(stderr, "FAIL: leaving Simulate did not clear every StateItem's active glow\n");
        return 1;
    }

    // ---- clean up: restore initialStateId, delete Q and P (cascades everything) -
    kernel.send(app::events::SetInitialStateRequested{.id = originalInitialStateId});
    scene->clearSelection();
    qItem->setSelected(true);
    pItem->setSelected(true);
    QApplication::processEvents();
    presenter->debugDeleteSelection();
    QApplication::processEvents();
    if (doc->machine().states != originalStates || doc->machine().transitions != originalTransitions) {
        std::fprintf(stderr, "FAIL: the hierarchy-simulate scenario did not restore the machine it borrowed\n");
        return 1;
    }
    scene->clearSelection();
    QApplication::processEvents();

    std::printf("PASS: gui-probe scenario hierarchy-simulate (front-door Simulate acceptance, parallel regions + "
                "containers glow together, one macrostep updates both, trace panel rows, Back restores, Design "
                "clears every glow)\n");
    return 0;
}

// Scenario "canvas-context-bridge": the interaction machine's context
// (designMode, dropTargetId) is read off CanvasInteractionFsm, not the
// presenter's currentMode_ mirror. Design: a drag on L1 opens NodeDrag;
// Simulate: designMode is false and it opens none; dragging L1 over P sets
// dropTargetId==P and releasing reparents to exactly that id; a view split off
// mid-Simulate reads designMode==false, and back in Design both views flip.
int runCanvasContextBridgeScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::CanvasPresenter* presenter = loginPane->presenter();
    app::CanvasView* view = loginPane->canvasView();
    QGraphicsScene* scene = view != nullptr ? view->scene() : nullptr;
    if (doc == nullptr || presenter == nullptr || view == nullptr || scene == nullptr) {
        std::fprintf(stderr,
                      "FAIL: canvas-context-bridge scenario preconditions (doc/presenter/view/scene) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    scene->clearSelection();
    QApplication::processEvents();

    const QVector<app::State> originalStates = doc->machine().states;
    const QVector<app::Transition> originalTransitions = doc->machine().transitions;

    // ---- build L1 (root leaf) and compound P{A}, far outside every other ------
    // ---- scenario's fixture (+4800 x) -------------------------------------------
    const quint64 kP = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(7212.0, -516.0)});
    const quint64 kA = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(7164.0, -372.0)});
    kernel.send(app::events::ReparentStateRequested{.id = kA, .parentId = kP});
    const quint64 kL1 = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(7596.0, -516.0)});
    QApplication::processEvents();  // settle

    app::StateItem* pItem = findStateItemById(scene, kP);
    app::StateItem* l1Item = findStateItemById(scene, kL1);
    if (pItem == nullptr || l1Item == nullptr) {
        std::fprintf(stderr, "FAIL: canvas-context-bridge scenario's P/L1 StateItems were not all created\n");
        return 1;
    }
    if (doc->findState(kA)->parentId != kP || !pItem->isContainerMode()) {
        std::fprintf(stderr, "FAIL: canvas-context-bridge scenario's initial P{A} setup did not take\n");
        return 1;
    }
    const QPointF l1PosBefore = doc->findState(kL1)->pos;

    // ---- (1) Design: fsm context designMode==true, a real drag opens NodeDrag -
    if (!presenter->debugFsmDesignMode()) {
        std::fprintf(stderr, "FAIL: (1) fsm context designMode was not true at scenario start (Design mode)\n");
        return 1;
    }
    const QPointF l1Press1 = l1Item->sceneRect().center();
    const QPointF l1Target1 = l1Press1 + QPointF(48.0, 48.0);  // grid multiple, away from P: no snap surprises
    view->debugMousePress(l1Press1);
    view->debugMouseMove(l1Target1);
    if (presenter->debugFsmState() != app::CanvasInteractionFsm::State::NodeDrag) {
        std::fprintf(stderr, "FAIL: (1) a real drag on L1 in Design mode did not open NodeDrag (state=%s)\n",
                     qUtf8Printable(app::CanvasInteractionFsm::stateName(presenter->debugFsmState())));
        return 1;
    }
    view->debugMouseRelease(l1Target1);
    QApplication::processEvents();
    if (doc->findState(kL1)->pos == l1PosBefore) {
        std::fprintf(stderr, "FAIL: (1) the Design-mode drag did not move L1's document position\n");
        return 1;
    }
    const QPointF l1PosAfterDesignDrag = doc->findState(kL1)->pos;

    // ---- (2) Simulate through the real command front door: context flips, ---
    // ---- the SAME drag attempt opens no session and commits nothing ----------
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
    QApplication::processEvents();
    if (presenter->debugFsmDesignMode()) {
        std::fprintf(stderr, "FAIL: (2) fsm context designMode stayed true after switching to Simulate\n");
        return 1;
    }
    const QPointF l1Press2 = l1Item->sceneRect().center();
    const QPointF l1Target2 = l1Press2 + QPointF(48.0, 48.0);
    view->debugMousePress(l1Press2);
    view->debugMouseMove(l1Target2);
    if (presenter->debugFsmState() != app::CanvasInteractionFsm::State::Idle) {
        std::fprintf(stderr, "FAIL: (2) a drag attempt in Simulate opened a session (state=%s)\n",
                     qUtf8Printable(app::CanvasInteractionFsm::stateName(presenter->debugFsmState())));
        return 1;
    }
    view->debugMouseRelease(l1Target2);
    QApplication::processEvents();
    if (doc->findState(kL1)->pos != l1PosAfterDesignDrag) {
        std::fprintf(stderr, "FAIL: (2) a refused Simulate-mode drag still moved L1's document position\n");
        return 1;
    }

    // ---- (3) back to Design: context true again, a drag opens NodeDrag again -
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Design});
    QApplication::processEvents();
    if (!presenter->debugFsmDesignMode()) {
        std::fprintf(stderr,
                      "FAIL: (3) fsm context designMode did not return to true after switching back to Design\n");
        return 1;
    }
    const QPointF l1Press3 = l1Item->sceneRect().center();
    const QPointF l1Target3 = l1Press3 + QPointF(-48.0, -48.0);
    view->debugMousePress(l1Press3);
    view->debugMouseMove(l1Target3);
    if (presenter->debugFsmState() != app::CanvasInteractionFsm::State::NodeDrag) {
        std::fprintf(stderr, "FAIL: (3) a real drag on L1 back in Design mode did not open NodeDrag (state=%s)\n",
                     qUtf8Printable(app::CanvasInteractionFsm::stateName(presenter->debugFsmState())));
        return 1;
    }
    view->debugMouseRelease(l1Target3);
    QApplication::processEvents();

    // ---- (4) the drop verdict IS the machine's own context (a point well ------
    // ---- inside P's padding moat) ------------------------------------------------
    const QPointF l1StartCenter = l1Item->sceneRect().center();
    const QRectF pRectForDrop = pItem->sceneRect();
    const QPointF dropPoint(pRectForDrop.right() - 8.0, pRectForDrop.bottom() - 8.0);
    view->debugMousePress(l1StartCenter);
    view->debugMouseMove(dropPoint);
    if (presenter->debugFsmDropTargetId() != kP || !pItem->isDropHighlighted()) {
        std::fprintf(stderr,
                      "FAIL: (4) dragging L1 over P did not make the machine's dropTargetId == P (got %llu)\n",
                      static_cast<unsigned long long>(presenter->debugFsmDropTargetId()));
        return 1;
    }
    // Move off P, back to L1's start point (parent root): the context follows
    // the verdict back to 0, live.
    view->debugMouseMove(l1StartCenter);
    if (presenter->debugFsmDropTargetId() != 0 || pItem->isDropHighlighted()) {
        std::fprintf(stderr,
                      "FAIL: (4) moving L1 off P did not follow the verdict back to root (dropTargetId=%llu)\n",
                      static_cast<unsigned long long>(presenter->debugFsmDropTargetId()));
        return 1;
    }
    // Back onto P: the commit reads exactly the id the machine held, not a
    // hit test re-derived at release.
    view->debugMouseMove(dropPoint);
    if (presenter->debugFsmDropTargetId() != kP) {
        std::fprintf(stderr, "FAIL: (4) moving L1 back onto P did not restore dropTargetId == P\n");
        return 1;
    }
    if (!saveSceneRegionCapture(loginPane, pItem->sceneRect().adjusted(-24.0, -24.0, 24.0, 24.0),
                                 "probe-canvas-context-bridge")) {
        return 1;
    }
    view->debugMouseRelease(dropPoint);
    QApplication::processEvents();
    if (doc->findState(kL1)->parentId != kP) {
        std::fprintf(stderr,
                      "FAIL: (4) releasing L1 over P did not commit the reparent the machine held "
                      "(parentId=%llu, want %llu)\n",
                      static_cast<unsigned long long>(doc->findState(kL1)->parentId),
                      static_cast<unsigned long long>(kP));
        return 1;
    }
    // One undo reverts both the move and the reparent. Assert Design mode
    // first: Undo/Redo silently no-op while simulating.
    if (!presenter->debugFsmDesignMode()) {
        std::fprintf(stderr, "FAIL: (4) not in Design mode before the reparent undo\n");
        return 1;
    }
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();
    if (doc->findState(kL1)->parentId != 0) {
        std::fprintf(stderr, "FAIL: (4) one undo did not revert L1's reparent onto P\n");
        return 1;
    }

    // ---- (5) mid-Simulate attach: a second view on the SAME document ---------
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
    QApplication::processEvents();
    if (presenter->debugFsmDesignMode()) {
        std::fprintf(stderr, "FAIL: (5) fsm context designMode stayed true switching to Simulate before the split\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    QApplication::processEvents();
    app::EditorView* secondPane = window.debugSplitRight();
    if (secondPane == nullptr) {
        std::fprintf(stderr, "FAIL: (5) debugSplitRight did not produce a second group\n");
        return 1;
    }
    QApplication::processEvents();
    app::CanvasPresenter* presenter2 = secondPane->presenter();
    if (presenter2 == nullptr) {
        std::fprintf(stderr, "FAIL: (5) the split pane has no presenter\n");
        return 1;
    }
    if (presenter2->debugFsmDesignMode()) {
        std::fprintf(stderr, "FAIL: (5) a view opened mid-Simulate reported designMode==true\n");
        return 1;
    }
    app::StateItem* l1Item2 = presenter2->debugStateItem(kL1);
    if (l1Item2 == nullptr) {
        std::fprintf(stderr, "FAIL: (5) the split pane's own StateItem for L1 was not created\n");
        return 1;
    }
    if (l1Item2->flags().testFlag(QGraphicsItem::ItemIsMovable)) {
        std::fprintf(stderr, "FAIL: (5) the split pane's L1 item is movable while the kernel is in Simulate\n");
        return 1;
    }

    // ---- switching back to Design flips BOTH views' context ------------------
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Design});
    QApplication::processEvents();
    if (!presenter->debugFsmDesignMode() || !presenter2->debugFsmDesignMode()) {
        std::fprintf(stderr, "FAIL: (5) switching back to Design did not flip BOTH views' fsm context\n");
        return 1;
    }

    // ---- clean up: delete L1 and P (cascades A) as one batch ------------------
    // Re-fetch: the second presenter's onRegister() requested a snapshot, and
    // the first presenter's rebuildAll() recreated every item in `scene`, so
    // pItem/l1Item from before step (1) are dangling.
    window.debugFocusView(loginPane);
    QApplication::processEvents();
    app::StateItem* pItemFinal = findStateItemById(scene, kP);
    app::StateItem* l1ItemFinal = findStateItemById(scene, kL1);
    if (pItemFinal == nullptr || l1ItemFinal == nullptr) {
        std::fprintf(stderr, "FAIL: canvas-context-bridge scenario's P/L1 StateItems were gone before cleanup\n");
        return 1;
    }
    scene->clearSelection();
    pItemFinal->setSelected(true);
    l1ItemFinal->setSelected(true);
    QApplication::processEvents();
    presenter->debugDeleteSelection();
    QApplication::processEvents();
    if (doc->machine().states != originalStates || doc->machine().transitions != originalTransitions) {
        std::fprintf(stderr, "FAIL: the canvas-context-bridge scenario did not restore the machine it borrowed\n");
        return 1;
    }
    scene->clearSelection();
    QApplication::processEvents();

    // Close the split this scenario opened, so later scenarios see a single
    // group (a second group halves the canvas viewport and changes which group
    // an emptied pane collapses to).
    window.debugFocusView(secondPane);
    QApplication::processEvents();
    window.debugCloseAllTabsInFocusedGroup();
    QApplication::processEvents();
    window.debugFocusView(loginPane);
    QApplication::processEvents();
    if (window.focusedView() != loginPane) {
        std::fprintf(stderr, "FAIL: closing the canvas-context-bridge split did not leave the login pane focused\n");
        return 1;
    }

    std::printf("PASS: gui-probe scenario canvas-context-bridge (fsm context designMode Design/Simulate/Design "
                "gesture-gating, dropTargetId follows a live NodeDrag's verdict on/off/onto a container and commits "
                "the id the machine held, one undo reverts it, a second view opened mid-Simulate reads the same "
                "machine context and starts unmovable, both views flip back to Design together)\n");
    return 0;
}
