// See harness/probe_scene_index_check.h.

#include "harness/probe_scene_index_check.h"

#include <QApplication>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <cstdio>

bool probeSceneIndexCheckAllScenes(const char* scenario) {
    QList<QGraphicsScene*> scenes;
    QHash<QGraphicsScene*, QStringList> viewClasses;
    for (QWidget* widget : QApplication::allWidgets()) {
        auto* view = qobject_cast<QGraphicsView*>(widget);
        if (view == nullptr || view->scene() == nullptr) {
            continue;
        }
        viewClasses[view->scene()] << QString::fromLatin1(view->metaObject()->className());
        if (!scenes.contains(view->scene())) {
            scenes << view->scene();
        }
    }

    QStringList badReports;
    for (QGraphicsScene* scene : scenes) {
        if (scene->itemIndexMethod() != QGraphicsScene::NoIndex) {
            badReports << QStringLiteral("{views=%1}").arg(viewClasses.value(scene).join(QLatin1Char('+')));
        }
    }
    if (!badReports.isEmpty()) {
        std::fprintf(stderr, "FAIL: scene uses a BSP item index after %s %s\n", scenario,
                     qPrintable(badReports.join(QStringLiteral(" || "))));
    }
    std::fflush(stderr);
    return badReports.isEmpty();
}
