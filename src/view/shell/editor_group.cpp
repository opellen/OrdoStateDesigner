#include "view/shell/editor_group.h"

#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "constants/design_tokens.h"
#include "view/shell/document_session.h"
#include "view/shell/editor_view.h"
#include "view/shell/icons.h"

namespace app {

// ---- DropOverlay ------------------------------------------------------------

// The translucent zone shading shown while a machine drag hovers a group
// (VSCode's drop shading): covers the group's stack area, mouse-transparent
// so the drag events keep landing on the group itself, and paints the
// half/full rect the current zone would claim.
class DropOverlay : public QWidget {
public:
    explicit DropOverlay(QWidget* parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        hide();
    }

    void setZone(DropZone zone) {
        if (zone_ != zone) {
            zone_ = zone;
            update();
        }
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QRectF zoneRect(rect());
        switch (zone_) {
            case DropZone::Center:
                break;  // whole area
            case DropZone::Left:
                zoneRect.setWidth(zoneRect.width() / 2);
                break;
            case DropZone::Right:
                zoneRect.setLeft(zoneRect.center().x());
                break;
            case DropZone::Top:
                zoneRect.setHeight(zoneRect.height() / 2);
                break;
            case DropZone::Bottom:
                zoneRect.setTop(zoneRect.center().y());
                break;
        }
        QPainter painter(this);
        painter.fillRect(zoneRect, design::color(design::kDropZoneFill));
        painter.setPen(QPen(design::color(design::kAccentInteractiveSoft), 1));
        painter.drawRect(zoneRect.adjusted(0.5, 0.5, -0.5, -0.5));
    }

private:
    DropZone zone_ = DropZone::Center;
};

// ---- EditorTabBar -----------------------------------------------------------

EditorTabBar::EditorTabBar(QWidget* parent) : QTabBar(parent) {
    setObjectName(QStringLiteral("editorTabBar"));
    // Custom per-tab close buttons, not setTabsClosable(): the platform's stock
    // close glyph cannot be restyled by QSS.
    setMovable(true);
    setExpanding(false);
    setDrawBase(false);
    setElideMode(Qt::ElideRight);
    setUsesScrollButtons(true);
}

void EditorTabBar::contextMenuEvent(QContextMenuEvent* event) {
    const int index = tabAt(event->pos());
    if (index >= 0) {
        emit tabContextMenuRequested(index, event->globalPos());
        event->accept();
        return;
    }
    QTabBar::contextMenuEvent(event);
}

void EditorTabBar::mousePressEvent(QMouseEvent* event) {
    if (tabAt(event->pos()) < 0) {
        emit barBackgroundClicked();  // empty strip area -- a focus request, nothing more
    }
    QTabBar::mousePressEvent(event);
}

// ---- EditorGroup ------------------------------------------------------------

EditorGroup::EditorGroup(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("editorGroup"));
    setAttribute(Qt::WA_StyledBackground, true);  // required for the focus-ring border QSS to paint
    setAcceptDrops(true);  // machine drags from the MACHINES tree (kMachineMimeType)

    tabBar_ = new EditorTabBar(this);
    stack_ = new QStackedWidget(this);
    dropOverlay_ = new DropOverlay(this);

    emptyHint_ = new QLabel(QStringLiteral("Open a machine from the MACHINES panel"), this);
    emptyHint_->setAlignment(Qt::AlignCenter);
    emptyHint_->setStyleSheet(design::resolveRoles(QStringLiteral("color: {text-disabled}; background: {surface-0};")));
    stack_->addWidget(emptyHint_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);  // room for the focus-ring border
    layout->setSpacing(0);
    layout->addWidget(tabBar_);
    layout->addWidget(stack_, 1);

    connect(tabBar_, &QTabBar::currentChanged, this, [this](int) {
        syncStackToCurrent();
        emit currentViewChanged(this);
    });
    // Keep tabs_ index-aligned through drag-reorder: QTabBar already moved
    // its own tab from -> to before this fires.
    connect(tabBar_, &QTabBar::tabMoved, this, [this](int from, int to) {
        Tab moved = tabs_[static_cast<std::size_t>(from)];
        tabs_.erase(tabs_.begin() + from);
        tabs_.insert(tabs_.begin() + to, moved);
    });
    connect(tabBar_, &EditorTabBar::tabContextMenuRequested, this,
            [this](int index, const QPoint& globalPos) { emit tabContextMenuRequested(this, index, globalPos); });
    connect(tabBar_, &EditorTabBar::barBackgroundClicked, this, [this] { emit focusRequested(this); });

    syncStackToCurrent();
}

EditorView* EditorGroup::openView(DocumentSession* session) {
    auto* view = new EditorView();
    connect(view, &EditorView::activated, this, [this] { emit focusRequested(this); });
    connect(view, &EditorView::selectionChanged, this,
            [this](SelectionKind kind, quint64 id) { emit viewSelectionChanged(this, kind, id); });
    connect(view, &EditorView::inspectorFieldFocusRequested, this,
            [this](InspectorField field) { emit viewInspectorFieldFocusRequested(this, field); });
    connect(view, &EditorView::autoLayoutRequested, this, [this, view] { emit viewAutoLayoutRequested(view); });

    Tab tab;
    tab.view = view;
    tab.session = session;
    // The badge drives this tab's title/dot. Looked up by view, not captured
    // index: drag-reorder and closes shift indices.
    tab.badgeConnection = connect(session->badge(), &StatusBadgeAdapter::changed, this, [this, view] {
        const int index = indexOfView(view);
        if (index >= 0) {
            refreshTabRow(index);
        }
    });

    tabs_.push_back(tab);
    stack_->addWidget(view);
    const int index = tabBar_->addTab(session->machineName());

    // Looked up by view on click, as above.
    auto* closeButton = new QToolButton(tabBar_);
    closeButton->setIcon(icons::closeGlyph());
    closeButton->setAutoRaise(true);
    closeButton->setFixedSize(16, 16);
    closeButton->setToolTip(QStringLiteral("Close"));
    connect(closeButton, &QToolButton::clicked, this, [this, view] {
        const int i = indexOfView(view);
        if (i >= 0) {
            closeTab(i);
        }
    });
    tabBar_->setTabButton(index, QTabBar::RightSide, closeButton);

    view->bindSession(session);
    refreshTabRow(index);
    tabBar_->setCurrentIndex(index);  // syncs the stack + emits currentViewChanged via the handler
    syncStackToCurrent();             // no-op if setCurrentIndex already fired; covers the first-tab case
    return view;
}

QWidget* EditorGroup::openCustomTab(QWidget* widget, const QString& title, const QIcon& icon) {
    Tab tab;
    tab.customWidget = widget;
    tab.title = title;

    tabs_.push_back(tab);
    stack_->addWidget(widget);
    const int index = tabBar_->addTab(icon, title);

    auto* closeButton = new QToolButton(tabBar_);
    closeButton->setIcon(icons::closeGlyph());
    closeButton->setAutoRaise(true);
    closeButton->setFixedSize(16, 16);
    closeButton->setToolTip(QStringLiteral("Close"));
    connect(closeButton, &QToolButton::clicked, this, [this, widget] {
        const int i = indexOfWidget(widget);
        if (i >= 0) {
            closeTab(i);
        }
    });
    tabBar_->setTabButton(index, QTabBar::RightSide, closeButton);

    tabBar_->setCurrentIndex(index);
    syncStackToCurrent();
    return widget;
}

bool EditorGroup::revealWidget(QWidget* widget) {
    for (std::size_t i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i].customWidget == widget) {
            tabBar_->setCurrentIndex(static_cast<int>(i));
            return true;
        }
    }
    return false;
}

int EditorGroup::indexOfWidget(const QWidget* widget) const {
    for (std::size_t i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i].customWidget == widget || tabs_[i].view == widget) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool EditorGroup::revealSession(DocumentSession* session) {
    for (std::size_t i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i].session == session) {
            tabBar_->setCurrentIndex(static_cast<int>(i));
            return true;
        }
    }
    return false;
}

void EditorGroup::updateSessionTitle(DocumentSession* session) {
    for (std::size_t i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i].session == session) {
            refreshTabRow(static_cast<int>(i));
        }
    }
}

void EditorGroup::closeTabsForSession(DocumentSession* session) {
    for (int i = static_cast<int>(tabs_.size()) - 1; i >= 0; --i) {
        if (tabs_[static_cast<std::size_t>(i)].session == session) {
            closeTab(i);
        }
    }
}

void EditorGroup::closeTab(int index) {
    if (index < 0 || index >= tabCount()) {
        return;
    }
    Tab tab = tabs_[static_cast<std::size_t>(index)];
    disconnect(tab.badgeConnection);

    // tabs_ first, so the currentChanged that removeTab() fires sees an
    // index-aligned vector.
    tabs_.erase(tabs_.begin() + index);
    tabBar_->removeTab(index);

    if (tab.view != nullptr) {
        stack_->removeWidget(tab.view);
        tab.view->unbind();
        tab.view->deleteLater();
    } else if (tab.customWidget != nullptr) {
        stack_->removeWidget(tab.customWidget);
        tab.customWidget->deleteLater();
    }

    if (tabs_.empty()) {
        syncStackToCurrent();
        emit emptied(this);
    }
}

EditorView* EditorGroup::currentView() const {
    const int index = tabBar_->currentIndex();
    if (index < 0 || index >= tabCount()) {
        return nullptr;
    }
    return tabs_[static_cast<std::size_t>(index)].view;
}

DocumentSession* EditorGroup::currentSession() const {
    EditorView* view = currentView();
    return view != nullptr ? view->session() : nullptr;
}

EditorView* EditorGroup::viewAt(int index) const {
    if (index < 0 || index >= tabCount()) {
        return nullptr;
    }
    return tabs_[static_cast<std::size_t>(index)].view;
}

int EditorGroup::indexOfView(const EditorView* view) const {
    for (std::size_t i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i].view == view) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void EditorGroup::setFocusedGroup(bool focused) {
    if (focused_ == focused) {
        return;
    }
    focused_ = focused;
    // Transparent-vs-accent border; the layout margins reserve the 1px, so
    // focus never shifts layout.
    setStyleSheet(focused_ ? design::resolveRoles(QStringLiteral("#editorGroup { border: 1px solid {accent-interactive}; }"))
                            : QStringLiteral("#editorGroup { border: 1px solid transparent; }"));
}

void EditorGroup::mousePressEvent(QMouseEvent* event) {
    emit focusRequested(this);
    QWidget::mousePressEvent(event);
}

void EditorGroup::dragEnterEvent(QDragEnterEvent* event) {
    if (!event->mimeData()->hasFormat(QLatin1String(kMachineMimeType))) {
        event->ignore();
        return;
    }
    dropOverlay_->setGeometry(stack_->geometry());
    dropOverlay_->setZone(dropZoneForPos(event->position().toPoint()));
    dropOverlay_->raise();
    dropOverlay_->show();
    event->acceptProposedAction();
}

void EditorGroup::dragMoveEvent(QDragMoveEvent* event) {
    if (!event->mimeData()->hasFormat(QLatin1String(kMachineMimeType))) {
        event->ignore();
        return;
    }
    dropOverlay_->setZone(dropZoneForPos(event->position().toPoint()));
    event->acceptProposedAction();
}

void EditorGroup::dragLeaveEvent(QDragLeaveEvent* event) {
    dropOverlay_->hide();
    QWidget::dragLeaveEvent(event);
}

void EditorGroup::dropEvent(QDropEvent* event) {
    dropOverlay_->hide();
    if (!event->mimeData()->hasFormat(QLatin1String(kMachineMimeType))) {
        event->ignore();
        return;
    }
    const quintptr sessionTag =
        static_cast<quintptr>(event->mimeData()->data(QLatin1String(kMachineMimeType)).toULongLong());
    event->acceptProposedAction();
    emit machineDropRequested(this, sessionTag, dropZoneForPos(event->position().toPoint()));
}

DropZone EditorGroup::dropZoneForPos(const QPoint& groupPos) const {
    const QRect area = stack_->geometry();
    if (area.width() <= 0 || area.height() <= 0) {
        return DropZone::Center;
    }
    const qreal x = static_cast<qreal>(groupPos.x() - area.left()) / area.width();
    const qreal y = static_cast<qreal>(groupPos.y() - area.top()) / area.height();
    // Edge bands are the outer quarter on each side; corners resolve to the
    // horizontal band first.
    if (x < 0.25) {
        return DropZone::Left;
    }
    if (x > 0.75) {
        return DropZone::Right;
    }
    if (y < 0.25) {
        return DropZone::Top;
    }
    if (y > 0.75) {
        return DropZone::Bottom;
    }
    return DropZone::Center;
}

void EditorGroup::refreshTabRow(int index) {
    const Tab& tab = tabs_[static_cast<std::size_t>(index)];
    if (tab.session != nullptr) {
        const QString badgeText = tab.session->badge()->text();
        tabBar_->setTabText(index, tab.session->machineName());
        tabBar_->setTabIcon(index, icons::statusDot(badgeText != QStringLiteral("idle")));
        tabBar_->setTabToolTip(index, tab.session->machineName() + QStringLiteral("  ") + badgeText);
    } else if (!tab.title.isEmpty()) {
        tabBar_->setTabText(index, tab.title);
    }
}

void EditorGroup::syncStackToCurrent() {
    const int index = tabBar_->currentIndex();
    if (index < 0 || index >= tabCount()) {
        stack_->setCurrentWidget(emptyHint_);
        return;
    }
    const auto& tab = tabs_[static_cast<std::size_t>(index)];
    QWidget* target = tab.view != nullptr ? static_cast<QWidget*>(tab.view) : tab.customWidget;
    if (target != nullptr) {
        stack_->setCurrentWidget(target);
    }
}

}  // namespace app
