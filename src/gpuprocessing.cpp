#include "gpuprocessing.h"
#include "colourpipeline.h"
namespace GpuProcessing {
SuiteProcessing::ComputeStage affine(const SuiteProcessing::RgbAffine &matrix,SuiteProcessing::ShaderCompiler &compiler) {
    SuiteProcessing::ComputeStage stage;stage.name="RGB affine";
    stage.cpu=[matrix](const QByteArray &input,QString *error){auto out=input;if(!SuiteProcessing::applyAffineCpu(reinterpret_cast<float*>(out.data()),out.size()/16,matrix)){*error="Invalid RGB affine transform.";return QByteArray();}return out;};
    if(!matrix.valid()){stage.unavailableReason="Invalid RGB affine coefficients.";return stage;}
    // Double precision matches the shared CPU coefficients and handles poorly
    // conditioned HDR matrices. Devices lacking Float64 use the CPU path.
    const auto literal=[](double value){auto text=QString::number(value,'g',17);if(!text.contains('.') && !text.contains('e'))text+=".0";return text+"LF";};
    QString source="vec4 suitePixel(vec4 pixel) { dvec4 v=dvec4(pixel.rgb,1.0LF); dvec3 result=dvec3(";
    for(int row=0;row<3;++row){if(row)source+=',';source+="dot(v,dvec4(";for(int col=0;col<4;++col){if(col)source+=',';source+=literal(matrix.values[row*4+col]);}source+="))";}
    source+="); return vec4(vec3(result),pixel.a); }";
    stage.spirv=compiler.compile(SuiteProcessing::pointShader(source),&stage.unavailableReason);return stage;
}
SuiteProcessing::ComputeStage colour(const QString &from,const QString &to,const QString &view,SuiteProcessing::ShaderCompiler &compiler) {
    SuiteProcessing::ComputeStage stage;stage.name="OCIO "+from+" → "+to+(view.isEmpty() ? QString() : " / "+view);
    stage.cpu=[from,to,view](const QByteArray &input,QString *error){auto out=input;if(!ColourPipeline::convertInPlace(reinterpret_cast<float*>(out.data()),out.size()/16,from,to,view,error))return QByteArray();return out;};
    const auto shader=ColourPipeline::gpuShader(from,to,view);
    if(!shader.value("available").toBool()){stage.unavailableReason=shader.value("reason").toString();return stage;}
    const auto source=shader.value("source").toString()+"\nvec4 suitePixel(vec4 pixel) { return suiteOcio(pixel); }\n";
    stage.spirv=compiler.compile(SuiteProcessing::pointShader(source),&stage.unavailableReason);return stage;
}
}
