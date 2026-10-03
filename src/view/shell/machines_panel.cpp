#include "view/shell/machines_panel.h"

#include <QAction>
#include <QColor>
#include <QFont>
#include <QFrame>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPolygonF>
#include <QSet>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include "constants/design_tokens.h"
#include "view/shell/document_session.h"
#include "view/shell/editor_group.h"  // kMachineMimeType -- the drag contract the editor area accepts

namespace app {

namespace {

// Every row stores its owning session as a quintptr so a click anywhere in
// a machine's subtree resolves to that machine without walking parents.
constexpr int kSessionRole = Qt::UserRole;
// State rows also carry their state id (to restore collapse state and to
// reveal the state); event rows carry their transition id.
constexpr int kStateIdRole = Qt::UserRole + 1;
constexpr int kTransitionIdRole = Qt::UserRole + 2;

// A small filled circle -- green while the machine is actively simulating,
// dim gray otherwise.
QIcon statusDotIcon(bool active) {
    QPixmap pixmap(10, 10);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(active ? QColor(QString::fromLatin1(design::kStatusLive))
                             : QColor(QString::fromLatin1(design::kStatusIdle)));
    painter.drawEllipse(1, 1, 8, 8);
    return QIcon(pixmap);
}

// State rows: hollow circle normally, filled green while this state is the
// live one; the machine's initial state keeps a center dot, Final a second
// ring -- the canvas vocabulary shrunk to glyph size. Parallel/History have
// no glyph of their own.
QIcon stateDotIcon(bool active, StateKind kind, bool isInitial) {
    QPixmap pixmap(12, 12);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    // Inactive glyphs use text-primary to match the inspector's icon tint.
    const QColor color =
        active ? QColor(QString::fromLatin1(design::kStatusLive)) : QColor(QString::fromLatin1(design::kTextPrimary));
    painter.setPen(QPen(color, 1.4));
    painter.setBrush(active ? QBrush(color) : Qt::NoBrush);
    painter.drawEllipse(QRectF(2, 2, 8, 8));
    if (isInitial) {
        painter.setPen(Qt::NoPen);
        // The punched hole shows the panel background (surface-1).
        painter.setBrush(active ? design::color(design::kSurface1) : color);
        painter.drawEllipse(QRectF(4.75, 4.75, 2.5, 2.5));
    } else if (kind == StateKind::Final) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(color, 1.0));
        painter.drawEllipse(QRectF(0.5, 0.5, 11, 11));
    }
    return QIcon(pixmap);
}

// Event rows: a small right arrow, Stately's transition-event glyph.
QIcon eventArrowIcon() {
    QPixmap pixmap(12, 12);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    // text-primary, matching the inspector's icon tint.
    const QColor color(QString::fromLatin1(design::kTextPrimary));
    painter.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(QPointF(1.5, 6.0), QPointF(8.5, 6.0));
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    QPolygonF head;
    head << QPointF(7.0, 3.0) << QPointF(11.0, 6.0) << QPointF(7.0, 9.0);
    painter.drawPolygon(head);
    return QIcon(pixmap);
}

DocumentSession* sessionOf(const QTreeWidgetItem* item) {
    return reinterpret_cast<DocumentSession*>(item->data(0, kSessionRole).value<quintptr>());
}

// Drag source for the explorer -> editor-area drop: only machine rows start a
// drag (nullptr aborts it), carrying the session's identity token under
// kMachineMimeType; EditorGroup/MainWindow validate it on drop.
class MachinesTree : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;

protected:
    QStringList mimeTypes() const override { return {QLatin1String(kMachineMimeType)}; }

    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override {
        for (QTreeWidgetItem* item : items) {
            if (item->parent() != nullptr) {
                continue;  // not a machine row
            }
            auto* mime = new QMimeData();
            mime->setData(QLatin1String(kMachineMimeType),
                          QByteArray::number(static_cast<qulonglong>(item->data(0, kSessionRole).value<quintptr>())));
            return mime;
        }
        return nullptr;
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_F2) {
            QTreeWidgetItem* item = currentItem();
            if (item != nullptr && item->parent() == nullptr) {
                if (auto* panel = qobject_cast<MachinesPanel*>(parent())) {
                    DocumentSession* session = sessionOf(item);
                    if (session != nullptr) {
                        emit panel->machineRenameRequested(session);
                        return;
                    }
                }
            }
        } else if (event->key() == Qt::Key_Delete) {
            QTreeWidgetItem* item = currentItem();
            if (item != nullptr && item->parent() == nullptr) {
                if (auto* panel = qobject_cast<MachinesPanel*>(parent())) {
                    DocumentSession* session = sessionOf(item);
                    if (session != nullptr) {
                        emit panel->machineDeleteRequested(session);
                        return;
                    }
                }
            }
        }
        QTreeWidget::keyPressEvent(event);
    }

    // Paints machine separators, then a right-edge disclosure chevron (the
    // native left branch indicator is hidden by the panel's stylesheet) over
    // the finished base pass.
    void paintEvent(QPaintEvent* event) override {
        QTreeWidget::paintEvent(event);
        QPainter painter(viewport());

        // Hairline above every machine row after the first, filled inside the
        // row's top edge: a drawLine across it vanished at 150% scaling.
        const QColor separator(QString::fromLatin1(design::kOutlineStrong));
        for (int i = 1; i < topLevelItemCount(); ++i) {
            const QRect rect = visualItemRect(topLevelItem(i));
            if (rect.isValid() && rect.intersects(viewport()->rect())) {
                painter.fillRect(QRect(0, rect.top(), viewport()->width(), design::kHairline), separator);
            }
        }

        painter.setRenderHint(QPainter::Antialiasing, true);
        // Same chevron color as the inspector's section headers.
        QPen pen(QColor(QString::fromLatin1(design::kTextSecondary)), 1.6, Qt::SolidLine, Qt::RoundCap,
                 Qt::RoundJoin);
        painter.setPen(pen);
        for (QTreeWidgetItemIterator it(this); *it != nullptr; ++it) {
            QTreeWidgetItem* item = *it;
            if (item->childCount() == 0) {
                continue;
            }
            const QRect rect = visualItemRect(item);
            if (!rect.isValid() || !rect.intersects(viewport()->rect())) {
                continue;
            }
            const int cx = viewport()->width() - design::kSectionChevronRightInset;
            const qreal cy = rect.center().y() + 0.5;
            QPainterPath arrow;
            if (item->isExpanded()) {
                arrow.moveTo(cx - 4, cy - 2);
                arrow.lineTo(cx, cy + 2.5);
                arrow.lineTo(cx + 4, cy - 2);
            } else {
                arrow.moveTo(cx - 2, cy - 4);
                arrow.lineTo(cx + 2.5, cy);
                arrow.lineTo(cx - 2, cy + 4);
            }
            painter.drawPath(arrow);
        }
    }

    // A press on the chevron strip toggles and is consumed, so collapsing a
    // machine row does not also select it and steal editor focus.
    void mousePressEvent(QMouseEvent* event) override {
        QTreeWidgetItem* item = itemAt(event->pos());
        if (item != nullptr && item->childCount() > 0 &&
            event->pos().x() >= viewport()->width() - 2 * design::kSectionChevronRightInset) {
            item->setExpanded(!item->isExpanded());
            return;
        }
        QTreeWidget::mousePressEvent(event);
    }
};

}  // namespace

MachinesPanel::MachinesPanel(QWidget* parent) : QWidget(parent) {
    // Panel title, not a section header: inspector header type ramp, but no
    // chevron or icon.
    auto* heading = new QLabel(QStringLiteral("Machines"), this);
    heading->setStyleSheet(design::kTypeSection + QStringLiteral(" padding: 6px 8px 4px 8px;"));

    tree_ = new MachinesTree(this);
    tree_->setFrameShape(QFrame::NoFrame);
    tree_->setHeaderHidden(true);
    tree_->setIndentation(14);
    // Hide the native left branch indicator (MachinesTree::paintEvent draws a
    // right-edge chevron). Scoped to this tree; other trees still need theirs.
    tree_->setStyleSheet(QStringLiteral("QTreeWidget::branch { image: none; border-image: none; }"));
    // Double-click is Zoom to Selection; the chevron already toggles.
    tree_->setExpandsOnDoubleClick(false);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setDragEnabled(true);
    tree_->setDragDropMode(QAbstractItemView::DragOnly);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(heading);
    layout->addWidget(tree_, 1);

    connect(tree_, &QTreeWidget::itemClicked, this, &MachinesPanel::onItemClicked);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) { requestZoomTo(item); });
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, &MachinesPanel::onCustomContextMenuRequested);
}

void MachinesPanel::addMachine(DocumentSession* session) {
    auto* item = new QTreeWidgetItem(tree_);
    item->setData(0, kSessionRole, QVariant::fromValue(reinterpret_cast<quintptr>(session)));
    rows_.push_back(Row{session, item});

    refreshMachineRow(session);
    rebuildSubtree(session);
    item->setExpanded(true);

    // The badge drives the machine row's own dot/text; the outline drives
    // the subtree. Both lambdas re-search rows_ by session pointer, so they
    // are safe no-ops the instant clear() empties rows_.
    connect(session->badge(), &StatusBadgeAdapter::changed, this,
            [this, session] { refreshMachineRow(session); });
    connect(session->outlineAdapter(), &MachineOutlineAdapter::outlineChanged, this,
            [this, session] { rebuildSubtree(session); });
}

void MachinesPanel::removeMachine(DocumentSession* session) {
    for (auto it = rows_.begin(); it != rows_.end(); ++it) {
        if (it->session == session) {
            disconnect(session->badge(), nullptr, this, nullptr);
            disconnect(session->outlineAdapter(), nullptr, this, nullptr);
            delete it->item;
            rows_.erase(it);
            return;
        }
    }
}

void MachinesPanel::clear() {
    for (const Row& row : rows_) {
        disconnect(row.session->badge(), nullptr, this, nullptr);
        disconnect(row.session->outlineAdapter(), nullptr, this, nullptr);
    }
    tree_->clear();  // deletes every QTreeWidgetItem
    rows_.clear();
}

void MachinesPanel::onCustomContextMenuRequested(const QPoint& pos) {
    QTreeWidgetItem* item = tree_->itemAt(pos);
    if (item == nullptr) {
        return;
    }
    DocumentSession* session = sessionOf(item);
    if (session == nullptr) {
        return;
    }

    // The right-clicked row becomes current (tree-local only); the machine is
    // activated only by Zoom, so Delete/Export never rebind a pane.
    tree_->setCurrentItem(item);

    // Every row carries Zoom to Selection; machine rows add their own verbs.
    QMenu menu(this);
    QAction* zoomAct = menu.addAction(QStringLiteral("Zoom to Selection"));
    QAction* renameAct = nullptr;
    QAction* deleteAct = nullptr;
    QAction* exportAct = nullptr;
    if (item->parent() == nullptr) {
        menu.addSeparator();
        renameAct = menu.addAction(QStringLiteral("Rename..."));
        renameAct->setShortcut(QKeySequence(Qt::Key_F2));
        deleteAct = menu.addAction(QStringLiteral("Delete Machine..."));
        deleteAct->setShortcut(QKeySequence(Qt::Key_Delete));
        menu.addSeparator();
        exportAct = menu.addAction(QStringLiteral("Export Machine..."));
    }

    QAction* selected = menu.exec(tree_->viewport()->mapToGlobal(pos));
    if (selected == nullptr) {
        return;
    }
    if (selected == zoomAct) {
        // Select first, as a left click would, then zoom (like a double-click).
        onItemClicked(item);
        requestZoomTo(item);
    } else if (selected == renameAct) {
        emit machineRenameRequested(session);
    } else if (selected == deleteAct) {
        emit machineDeleteRequested(session);
    } else if (selected == exportAct) {
        emit machineExportRequested(session);
    }
}

QTreeWidgetItem* MachinesPanel::debugRow(DocumentSession* session, quint64 stateId, quint64 transitionId) const {
    for (QTreeWidgetItemIterator it(tree_); *it != nullptr; ++it) {
        QTreeWidgetItem* item = *it;
        if (sessionOf(item) != session) {
            continue;
        }
        const bool machineRow = item->parent() == nullptr;
        const bool stateRow = !machineRow && item->parent()->parent() == nullptr;
        if (stateId == 0 && transitionId == 0 && machineRow) {
            return item;
        }
        if (stateId != 0 && stateRow && item->data(0, kStateIdRole).value<quint64>() == stateId) {
            return item;
        }
        if (transitionId != 0 && !machineRow && !stateRow &&
            item->data(0, kTransitionIdRole).value<quint64>() == transitionId) {
            return item;
        }
    }
    return nullptr;
}

// Row depth names the target: machine row = whole machine, child = state,
// grandchild = transition.
void MachinesPanel::requestZoomTo(QTreeWidgetItem* item) {
    DocumentSession* session = item != nullptr ? sessionOf(item) : nullptr;
    if (session == nullptr) {
        return;
    }
    quint64 stateId = 0;
    quint64 transitionId = 0;
    if (item->parent() != nullptr) {
        if (item->parent()->parent() == nullptr) {
            stateId = item->data(0, kStateIdRole).value<quint64>();
        } else {
            transitionId = item->data(0, kTransitionIdRole).value<quint64>();
        }
    }
    emit zoomToSelectionRequested(session, stateId, transitionId);
}

void MachinesPanel::onItemClicked(QTreeWidgetItem* item) {
    DocumentSession* session = sessionOf(item);
    for (const Row& row : rows_) {
        if (row.session != session) {
            continue;
        }
        // machineActivated first, then the element signal, so the canvas is visible.
        emit machineActivated(session);
        if (item->parent() != nullptr) {
            if (item->parent()->parent() == nullptr) {
                emit stateActivated(session, item->data(0, kStateIdRole).value<quint64>());
            } else {
                emit transitionActivated(session, item->data(0, kTransitionIdRole).value<quint64>());
            }
        }
        return;
    }
}

void MachinesPanel::refreshMachineRow(DocumentSession* session) {
    for (const Row& row : rows_) {
        if (row.session == session) {
            row.item->setText(0, machineRowLabel(session));
            row.item->setIcon(0, statusDotIcon(session->badge()->text() != QStringLiteral("idle")));
            return;
        }
    }
}

void MachinesPanel::rebuildSubtree(DocumentSession* session) {
    QTreeWidgetItem* machineItem = nullptr;
    for (const Row& row : rows_) {
        if (row.session == session) {
            machineItem = row.item;
            break;
        }
    }
    if (machineItem == nullptr) {
        return;
    }

    // Wholesale rebuild per change; collapsed state rows are remembered by state id.
    QSet<quint64> collapsedStates;
    for (int i = 0; i < machineItem->childCount(); ++i) {
        const QTreeWidgetItem* stateItem = machineItem->child(i);
        if (!stateItem->isExpanded() && stateItem->childCount() > 0) {
            collapsedStates.insert(stateItem->data(0, kStateIdRole).value<quint64>());
        }
    }

    // takeChildren() detaches and returns the rows ownerless -- delete them
    // explicitly or they leak for the window's lifetime.
    qDeleteAll(machineItem->takeChildren());

    const auto outline = session->outlineAdapter()->outline();
    const quintptr sessionTag = reinterpret_cast<quintptr>(session);
    for (const auto& state : outline) {
        auto* stateItem = new QTreeWidgetItem(machineItem);
        stateItem->setText(0, state.name);
        stateItem->setIcon(0, stateDotIcon(state.active, state.kind, state.isInitial));
        stateItem->setData(0, kSessionRole, QVariant::fromValue(sessionTag));
        stateItem->setData(0, kStateIdRole, QVariant::fromValue(state.stateId));
        if (state.active) {
            QFont font = stateItem->font(0);
            font.setBold(true);
            stateItem->setFont(0, font);
        }
        for (const auto& event : state.events) {
            auto* eventItem = new QTreeWidgetItem(stateItem);
            eventItem->setText(0, event.label);
            eventItem->setIcon(0, eventArrowIcon());
            eventItem->setData(0, kSessionRole, QVariant::fromValue(sessionTag));
            eventItem->setData(0, kTransitionIdRole, QVariant::fromValue(event.transitionId));
            eventItem->setForeground(0, design::color(design::kTextSecondary));
        }
        stateItem->setExpanded(!collapsedStates.contains(state.stateId));
    }
}

QString MachinesPanel::machineRowLabel(DocumentSession* session) const {
    return session->machineName() + QStringLiteral("   ") + session->badge()->text();
}

}  // namespace app
