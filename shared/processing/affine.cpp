#include "affine.h"
#include <cmath>
namespace SuiteProcessing {
bool RgbAffine::valid() const {for(double value:values)if(!std::isfinite(value))return false;return true;}
bool applyAffineCpu(float *rgba,qsizetype count,const RgbAffine &transform) {
    if(count<0 || (!rgba && count) || !transform.valid())return false;
    const auto &m=transform.values;
    for(qsizetype i=0;i<count;++i,rgba+=4) {
        const double r=rgba[0],g=rgba[1],b=rgba[2];
        rgba[0]=m[0]*r+m[1]*g+m[2]*b+m[3];
        rgba[1]=m[4]*r+m[5]*g+m[6]*b+m[7];
        rgba[2]=m[8]*r+m[9]*g+m[10]*b+m[11];
    }
    return true;
}
}
