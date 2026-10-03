#include "harness/harness.h"

int runHierarchicalCodegenSmoke() {
    if (runCodegenCoreSmoke() != 0) {
        return 1;
    }
    if (runCodegenFeaturesSmoke() != 0) {
        return 1;
    }
    if (runCodegenProjectorSmoke() != 0) {
        return 1;
    }
    return 0;
}
