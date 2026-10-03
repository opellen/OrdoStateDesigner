#pragma once

#include <QWidget>
#include <QtGlobal>

#include "view/canvas/canvas_presenter.h"  // CanvasPresenter (forwarded selectionChanged) + SelectionKind

class QResizeEvent;

namespace app {

class CanvasView;
class DocumentSession;
class MinimapView;

// One editor view: the page under a single tab of an EditorGroup, holding one
// CanvasView bound to at most one DocumentSession via bindSession()/unbind()
// (which forward to attachView()/detachView()). Owns no kernel or scene state.
//
// Click-to-focus: an event filter on the CanvasView's viewport reports any
// press via activated() and never consumes it, so scene items keep their drags.
class EditorView : public QWidget {
    Q_OBJECT

public:
    explicit EditorView(QWidget* parent = nullptr);
    ~EditorView() override;  // unbind()

    // Unbinds any previously-bound session, then session->attachView(canvasView())
    // and remembers `session`. Passing nullptr just unbinds.
    void bindSession(DocumentSession* session);

    // session()->detachView(canvasView()) and forgets the session. Safe to
    // call with no session currently bound (no-op).
    void unbind();

    DocumentSession* session() const { return session_; }
    CanvasView* canvasView() const { return canvasView_; }

    // The CanvasPresenter currently bound to canvasView() (non-owning; owned
    // by session()'s current view binding) -- nullptr while unbound.
    CanvasPresenter* presenter() const { return presenter_; }

    // The corner minimap: non-owning, alive for this view's whole lifetime.
    MinimapView* minimap() const { return minimap_; }

signals:
    void activated(EditorView* view);
    // The three below are forwarded from the currently bound CanvasPresenter,
    // reconnected on every bindSession(); the old connection dies with the
    // presenter in detachView().
    void selectionChanged(SelectionKind kind, quint64 id);
    void inspectorFieldFocusRequested(InspectorField field);
    // The empty-canvas menu's Auto Layout...; the shell opens the dialog.
    void autoLayoutRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    // Keeps the minimap in the bottom-right corner; it is an overlay outside the layout.
    void resizeEvent(QResizeEvent* event) override;

private:
    void repositionMinimap();

    CanvasView* canvasView_ = nullptr;
    DocumentSession* session_ = nullptr;    // non-owning
    CanvasPresenter* presenter_ = nullptr;  // non-owning; owned by session_'s current view binding

    // Qt child (deleted with this widget), stacked above canvasView_ and
    // positioned by repositionMinimap(), never by the layout.
    MinimapView* minimap_ = nullptr;

    // bindSession() can run before the first layout pass, fitting against a
    // bogus viewport (absurd scale). When the size is unrealized at bind, this
    // arms a one-shot re-fit on the viewport's first real resize.
    bool pendingFit_ = false;
};

}  // namespace app
