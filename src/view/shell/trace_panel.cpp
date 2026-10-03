#include "view/shell/trace_panel.h"

#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTime>
#include <QVBoxLayout>

#include "constants/design_tokens.h"

namespace app {

namespace {

constexpr const char* kPanelBackground = design::kSurface1;

QString nowStamp() { return QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")); }

}  // namespace

// ---- TracePanel ---------------------------------------------------------------

TracePanel::TracePanel(QWidget* parent) : QWidget(parent) {
    setStyleSheet(QStringLiteral("background: %1;").arg(QString::fromLatin1(kPanelBackground)));

    table_ = new QTableWidget(0, 3, this);
    table_->setHorizontalHeaderLabels({QStringLiteral("Time"), QStringLiteral("Machine"), QStringLiteral("Entry")});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setStyleSheet(design::resolveRoles(QStringLiteral("color: {text-primary}; background: %1; gridline-color: {outline};"))
                               .arg(QString::fromLatin1(kPanelBackground)));

    auto* headerRow = new QHBoxLayout();
    auto* heading = new QLabel(QStringLiteral("EVENT TRACE"), this);
    heading->setStyleSheet(design::resolveRoles(QStringLiteral("color: {text-secondary}; font-weight: 600;")));
    auto* clearButton = new QPushButton(QStringLiteral("Clear"), this);
    connect(clearButton, &QPushButton::clicked, this, &TracePanel::clearClicked);
    headerRow->addWidget(heading);
    headerRow->addStretch(1);
    headerRow->addWidget(clearButton);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->addLayout(headerRow);
    layout->addWidget(table_, 1);
}

void TracePanel::clearRows() { table_->setRowCount(0); }

void TracePanel::appendRow(const QString& time, const QString& machine, const QString& entry) {
    const int row = table_->rowCount();
    table_->insertRow(row);
    table_->setItem(row, 0, new QTableWidgetItem(time));
    table_->setItem(row, 1, new QTableWidgetItem(machine));
    table_->setItem(row, 2, new QTableWidgetItem(entry));
    table_->scrollToBottom();  // auto-scroll
}

int TracePanel::rowCount() const { return table_->rowCount(); }

// ---- TraceAdapter -------------------------------------------------------------

TraceAdapter::TraceAdapter(TracePanel* panel, QString machineName)
    : Presenter(QStringLiteral("TraceAdapter"), panel), panel_(panel), machineName_(std::move(machineName)) {}

void TraceAdapter::onRegister() {
    sim_ = context().agentAs<SimulationAgent>(SimulationAgent::kName);

    subscribe<events::TraceAppended>(&TraceAdapter::onTraceAppended);
    subscribe<events::SimulationReset>(&TraceAdapter::onSimulationReset);
    connect(panel_, &TracePanel::clearClicked, this, &TraceAdapter::onClearClicked);

    // Seed from lines recorded before this adapter existed; they get "(history)"
    // instead of a timestamp.
    panel_->clearRows();
    if (sim_ != nullptr) {
        for (const QString& line : sim_->trace()) {
            panel_->appendRow(QStringLiteral("(history)"), machineName_, line);
        }
        knownCount_ = static_cast<int>(sim_->trace().size());
    }
}

void TraceAdapter::onTraceAppended(const events::TraceAppended&) { reconcile(); }
void TraceAdapter::onSimulationReset(const events::SimulationReset&) { reconcile(); }

void TraceAdapter::onClearClicked() {
    // View-local clear: the agent's trace is kept. Resetting knownCount_ makes
    // later reconciles show only new lines.
    panel_->clearRows();
    knownCount_ = sim_ != nullptr ? static_cast<int>(sim_->trace().size()) : 0;
}

void TraceAdapter::reconcile() {
    if (sim_ == nullptr) {
        return;
    }
    const QStringList trace = sim_->trace();
    const int traceSize = static_cast<int>(trace.size());
    if (traceSize < knownCount_) {
        // Trace shrank: a Reset/Run/Back wipe-and-replay happened. Rebuild, stamped now.
        panel_->clearRows();
        for (const QString& line : trace) {
            panel_->appendRow(nowStamp(), machineName_, line);
        }
        knownCount_ = traceSize;
        return;
    }
    for (int i = knownCount_; i < traceSize; ++i) {
        panel_->appendRow(nowStamp(), machineName_, trace.at(i));
    }
    knownCount_ = traceSize;
}

}  // namespace app
