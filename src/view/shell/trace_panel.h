#pragma once

#include <memory>

#include <QString>
#include <QWidget>
#include <QtGlobal>

#include <ordo/qt/presenter.h>

#include "model/sim_agent.h"
#include "model/sim_events.h"

class QTableWidget;
class QPushButton;

namespace app {

// Bottom event-trace panel: a Time | Machine | Entry table following the focused
// pane's session. A dumb view (table + Clear button, Clear reported via signal);
// TraceAdapter below is its Presenter. MainWindow owns one panel and rebuilds one
// adapter per ViewHost on every focus or machine-rebind change.
class TracePanel : public QWidget {
    Q_OBJECT

public:
    explicit TracePanel(QWidget* parent = nullptr);

    void clearRows();
    void appendRow(const QString& time, const QString& machine, const QString& entry);
    int rowCount() const;

signals:
    void clearClicked();

private:
    QTableWidget* table_ = nullptr;
};

// Presenter over TracePanel, one per focused-session binding. It reconciles
// against SimulationAgent::trace() rather than blindly appending: no fact says
// "trace cleared", yet reset()/run()/back() clear and replay it. A trace shorter
// than knownCount_ means a wipe, so rebuild; otherwise append. Lines present at
// attach time show "(history)".
class TraceAdapter : public ordo::qt::Presenter {
    Q_OBJECT

public:
    // Does not take ownership of `panel`. `machineName` is the session's display
    // name, captured once at construction.
    TraceAdapter(TracePanel* panel, QString machineName);

    void onRegister() override;

private:
    void onTraceAppended(const events::TraceAppended&);
    void onSimulationReset(const events::SimulationReset&);
    void onClearClicked();

    void reconcile();

    TracePanel* panel_;
    QString machineName_;
    std::shared_ptr<SimulationAgent> sim_;
    // Trace lines already shown or dismissed by Clear; reconcile()'s baseline.
    // Not rowCount(): Clear empties the table without the agent knowing.
    int knownCount_ = 0;
};

}  // namespace app
