#pragma once

#include <QPointF>
#include <QStandardPaths>
#include <QString>
#include <QVector>

#include "constants/app_version.h"

namespace app {

#if defined(SD_ENABLE_LOGGING) && (SD_ENABLE_LOGGING == 0)
inline void logRoutingTrace(const QString&, const QString&) {}
inline void logPortRun(const QString&, QPointF, QPointF, QPointF, QPointF,
                       const QString&, const QVector<QPointF>&) {}

// The per-version trace file path, in the system temp directory (%TEMP% on
// Windows); the one owner of the naming rule. Inlined so callers compile the same either way.
inline QString routingTraceLogPath() {
    return QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
           QStringLiteral("/state-designer-routing-trace-%1.log").arg(QLatin1String(kAppVersion));
}

// Silences logRoutingTrace/logPortRun for its lifetime. Counter-based, so
// nested guards are safe. Used by the initial auto-layout, whose provisional
// wires must not reach the defect trace. A no-op here (logging compiled out).
class RoutingLogSilencer {
public:
    RoutingLogSilencer() {}
    ~RoutingLogSilencer() {}
    RoutingLogSilencer(const RoutingLogSilencer&) = delete;
    RoutingLogSilencer& operator=(const RoutingLogSilencer&) = delete;
};
#else
// Diagnostic logging for transition routing and port resolution. Outputs to
// qDebug() and appends to the file at routingTraceLogPath().
void logRoutingTrace(const QString& tag, const QString& message);

void logPortRun(const QString& context, QPointF from, QPointF fromDir, QPointF to, QPointF toDir,
                const QString& branch, const QVector<QPointF>& waypoints);

// The per-version trace file path, in the system temp directory (%TEMP% on
// Windows); the one owner of the naming rule.
QString routingTraceLogPath();

// Silences logRoutingTrace/logPortRun for its lifetime. Counter-based, so
// nested guards are safe. Used by the initial auto-layout, whose provisional
// wires are routed for measurement only and must not reach the defect trace.
class RoutingLogSilencer {
public:
    RoutingLogSilencer();
    ~RoutingLogSilencer();
    RoutingLogSilencer(const RoutingLogSilencer&) = delete;
    RoutingLogSilencer& operator=(const RoutingLogSilencer&) = delete;
};
#endif

}  // namespace app
