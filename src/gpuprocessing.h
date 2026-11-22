#pragma once
#include "affine.h"
#include "compute.h"
#include "shadercompiler.h"
namespace GpuProcessing {
SuiteProcessing::ComputeStage affine(const SuiteProcessing::RgbAffine &matrix,SuiteProcessing::ShaderCompiler &compiler);
SuiteProcessing::ComputeStage colour(const QString &from,const QString &to,const QString &view,SuiteProcessing::ShaderCompiler &compiler);
}
