#pragma once

// --gui-probe guard asserting every scene stays on NoIndex. Canvas scenes opt
// out of Qt's BSP index because a graphics effect's per-view device-pixel
// margin leaves the index holding dangling item pointers across a zoom.

class QGraphicsScene;

// Checks the scene of every QGraphicsView in the application; prints one FAIL
// line to stderr per offending scene and returns false if any is not NoIndex.
bool probeSceneIndexCheckAllScenes(const char* scenario);
