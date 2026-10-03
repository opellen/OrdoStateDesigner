#include "view/shell/editor_view.h"

#include <QEvent>
#include <QGraphicsScene>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QVBoxLayout>

#include "view/canvas/canvas_view.h"
#include "view/shell/document_session.h"
#include "view/shell/minimap_view.h"

namespace app {

namespace {
constexpr int kMinimapMargin = 8;  // corner inset
}  // namespace

EditorView::EditorView(QWidget* parent) : QWidget(parent) {
    canvasView_ = new CanvasView(this);
    // The viewport is the widget QGraphicsView delivers mouse events to.
    canvasView_->installEventFilter(this);
    canvasView_->viewport()->installEventFilter(this);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(canvasView_);

    // The minimap floats over canvasView_'s corner instead of taking a layout
    // row. The source view is fixed; only its scene changes (bindScene()).
    minimap_ = new MinimapView(this);
    minimap_->setSourceView(canvasView_);
    minimap_->raise();
    minimap_->onNavigate = [this](QPointF sceneCenter) { canvasView_->centerOn(sceneCenter); };
    repositionMinimap();
}

EditorView::~EditorView() { unbind(); }

void EditorView::bindSession(DocumentSession* session) {
    if (session_ == session) {
        return;
    }
    unbind();
    session_ = session;
    if (session_ != nullptr) {
        presenter_ = session_->attachView(canvasView_);
        if (presenter_ != nullptr) {
            connect(presenter_, &CanvasPresenter::selectionChanged, this, &EditorView::selectionChanged);
            connect(presenter_, &CanvasPresenter::inspectorFieldFocusRequested, this,
                    &EditorView::inspectorFieldFocusRequested);
            connect(presenter_, &CanvasPresenter::autoLayoutRequested, this, &EditorView::autoLayoutRequested);
        }
        // The scene is already fully rebuilt when attachView() returns.
        if (QGraphicsScene* scene = canvasView_->scene()) {
            canvasView_->centerAndFit(scene->itemsBoundingRect());
        }
        // Unrealized at bind time: redo the fit once real geometry arrives.
        pendingFit_ = canvasView_->viewport()->width() < 50 || canvasView_->viewport()->height() < 50;

        // The minimap shares the scene attachView() just set; unbind() clears
        // it before detachView() destroys the scene.
        minimap_->bindScene(canvasView_->scene());
    }
}

void EditorView::unbind() {
    if (session_ == nullptr) {
        return;
    }
    minimap_->bindScene(nullptr);  // before detachView() destroys the scene
    session_->detachView(canvasView_);
    session_ = nullptr;
    presenter_ = nullptr;  // detachView() above just destroyed it
    pendingFit_ = false;
}


bool EditorView::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress) {
        emit activated(this);  // non-consuming
    }
    // Resolves on the viewport's resize, not resizeEvent: children keep their
    // old geometry until the layout pass runs.
    if (event->type() == QEvent::Resize && watched == canvasView_->viewport() && pendingFit_ &&
        session_ != nullptr) {
        const QSize size = static_cast<QResizeEvent*>(event)->size();
        if (size.width() >= 50 && size.height() >= 50) {
            pendingFit_ = false;
            if (QGraphicsScene* scene = canvasView_->scene()) {
                canvasView_->centerAndFit(scene->itemsBoundingRect());
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void EditorView::mousePressEvent(QMouseEvent* event) {
    emit activated(this);
    QWidget::mousePressEvent(event);
}

void EditorView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    repositionMinimap();
}

void EditorView::repositionMinimap() {
    const QSize size = minimap_->size();  // fixed size (~180x120)
    minimap_->setGeometry(width() - size.width() - kMinimapMargin, height() - size.height() - kMinimapMargin,
                           size.width(), size.height());
}

}  // namespace app
