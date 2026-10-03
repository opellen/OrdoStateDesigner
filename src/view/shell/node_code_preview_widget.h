#pragma once

#include <QWidget>
#include <QString>

#include "infra/node_code_projector.h"

class QPlainTextEdit;
class QPushButton;
class QButtonGroup;

namespace app {

// Contextual Dev-Mode live code projection preview widget
// Displays sliced C++ production code with sub-filters ([All], [State Enum], [Hooks], [Core Handler])
// and instant clipboard copying.
class NodeCodePreviewWidget : public QWidget {
    Q_OBJECT

public:
    enum class Filter {
        All = 0,
        StateEnum = 1,
        Hooks = 2,
        CoreHandler = 3
    };

    explicit NodeCodePreviewWidget(bool isTransition = false, QWidget* parent = nullptr);

    void setProjection(const NodeCodeProjection& projection);
    void clear();

    QString currentSnippet() const;

    // Probe / Test levers
    void debugSelectFilter(Filter filter);
    QString debugDisplayedText() const;
    void debugClickCopy();

private:
    void updateDisplayedText();
    void onCopyClicked();

    bool isTransition_ = false;
    NodeCodeProjection currentProjection_;
    Filter activeFilter_ = Filter::All;

    QPushButton* allBtn_ = nullptr;
    QPushButton* enumBtn_ = nullptr;
    QPushButton* hooksBtn_ = nullptr;
    QPushButton* coreBtn_ = nullptr;
    QPushButton* copyBtn_ = nullptr;
    QPlainTextEdit* codeEdit_ = nullptr;
};

}  // namespace app
