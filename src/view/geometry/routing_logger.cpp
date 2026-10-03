#include "view/geometry/routing_logger.h"

#if !defined(SD_ENABLE_LOGGING) || (SD_ENABLE_LOGGING != 0)

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>

namespace app {

namespace {

// Silence depth for RoutingLogSilencer; GUI thread only, so a plain int.
int silenced = 0;

void appendToFile(const QString& line) {
    static const QString logPath = routingTraceLogPath();
    QFile file(logPath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream out(&file);
        out << line << "\n";
    }
}

}  // namespace

QString routingTraceLogPath() {
    return QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
           QStringLiteral("/state-designer-routing-trace-%1.log").arg(QLatin1String(kAppVersion));
}

void logRoutingTrace(const QString& tag, const QString& message) {
    if (silenced > 0) {
        return;
    }
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    const QString formatted = QStringLiteral("[%1] [%2] %3").arg(timestamp, tag, message);
    qDebug().noquote() << formatted;
    appendToFile(formatted);
}

void logPortRun(const QString& context, QPointF from, QPointF fromDir, QPointF to, QPointF toDir,
                const QString& branch, const QVector<QPointF>& waypoints) {
    QString ptsStr;
    for (int i = 0; i < waypoints.size(); ++i) {
        if (i > 0) ptsStr += QStringLiteral(" -> ");
        ptsStr += QStringLiteral("(%1, %2)").arg(waypoints[i].x(), 0, 'f', 1).arg(waypoints[i].y(), 0, 'f', 1);
    }

    // A route of >= 5 points is a defect only if a forward corridor existed:
    // parallel end directions with the target at or behind the source admit no
    // straight/L/Z route, so the wrap is the honest fallback there.
    const bool parallelEnds = QPointF::dotProduct(fromDir, toDir) > 0.99;
    const bool noForwardCorridor =
        parallelEnds && QPointF::dotProduct(to - from, fromDir) <= 0.01;
    const bool isDefect = waypoints.size() >= 5 && !noForwardCorridor;
    const QString tag = isDefect ? QStringLiteral("ROUTING_DEFECT") : QStringLiteral("ROUTING");
    const QString msg = QStringLiteral("%1: from=(%2, %3) dir=(%4, %5) -> to=(%6, %7) dir=(%8, %9) | %10 | pts[%11]: %12")
                            .arg(context)
                            .arg(from.x(), 0, 'f', 1).arg(from.y(), 0, 'f', 1)
                            .arg(fromDir.x(), 0, 'f', 1).arg(fromDir.y(), 0, 'f', 1)
                            .arg(to.x(), 0, 'f', 1).arg(to.y(), 0, 'f', 1)
                            .arg(toDir.x(), 0, 'f', 1).arg(toDir.y(), 0, 'f', 1)
                            .arg(branch)
                            .arg(waypoints.size())
                            .arg(ptsStr);

    logRoutingTrace(tag, msg);
}

RoutingLogSilencer::RoutingLogSilencer() { ++silenced; }

RoutingLogSilencer::~RoutingLogSilencer() { --silenced; }

}  // namespace app

#endif
