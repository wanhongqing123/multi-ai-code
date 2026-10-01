#pragma once

#include "MaiGraphicsPresenter.h"

struct MaiVideoColorParameters {
    float vectors[3][4];
    float rangeMin[3];
    float rangeMax[3];
};

MaiVideoColorParameters maiVideoColorParameters(MaiVideoColorSpace space, MaiVideoColorRange range);
