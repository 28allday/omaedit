#include "compute.h"
namespace SuiteProcessing {
QString pointShader(const QString &functionText) {
    return QStringLiteral(R"GLSL(#version 450
layout(local_size_x=256) in;
layout(set=0,binding=0,std430) readonly buffer InputPixels { vec4 pixels[]; } source;
layout(set=0,binding=1,std430) writeonly buffer OutputPixels { vec4 pixels[]; } target;
layout(set=0,binding=2,std430) buffer ResultFlags { uint invalid; } resultFlags;
layout(push_constant) uniform Parameters { uint count; } parameters;
)GLSL")+functionText+QStringLiteral(R"GLSL(
void main() {
    uint i=gl_GlobalInvocationID.x;
    if(i>=parameters.count) return;
    vec4 value=suitePixel(source.pixels[i]);
    value.a=source.pixels[i].a;
    if(any(isnan(value)) || any(isinf(value)) || value.a<0.0 || value.a>1.0)
        atomicOr(resultFlags.invalid,1u);
    target.pixels[i]=value;
}
)GLSL");
}
}
