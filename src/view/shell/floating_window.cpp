#include "view/shell/floating_window.h"

#include <QCloseEvent>
#include <QVBoxLayout>

#include "constants/design_tokens.h"
#include "view/shell/document_session.h"
#include "view/shell/editor_group.h"

namespace app {

FloatingEditorWindow::FloatingEditorWindow(EditorGroup* group) : QWidget(nullptr), group_(group) {
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(design::resolveRoles(QStringLiteral("background: {surface-0};")));
    resize(900, 640);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(group_);

    connect(group_, &EditorGroup::currentViewChanged, this, [this](EditorGroup*) { refreshTitle(); });
    refreshTitle();
}

void FloatingEditorWindow::closeEvent(QCloseEvent* event) {
    emit closing(this);
    QWidget::closeEvent(event);
}

void FloatingEditorWindow::refreshTitle() {
    DocumentSession* session = group_->currentSession();
    setWindowTitle(session != nullptr ? session->machineName() + QStringLiteral(" — Ordo State Designer")
                                       : QStringLiteral("Ordo State Designer"));
}

}  // namespace app
