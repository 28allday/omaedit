#include "shadercompiler.h"
#include <QCryptographicHash>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
namespace SuiteProcessing {
QByteArray ShaderCompiler::compile(const QString &source,QString *error) {
    if(error)error->clear();
    const auto fail=[&](const QString &message){if(error)*error=message;return QByteArray();};
    const auto bytes=source.toUtf8();if(bytes.isEmpty() || bytes.size()>1024*1024)return fail("GPU shader source is empty or exceeds the compiler limit.");
    const auto key=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);
    if(m_cache.contains(key))return m_cache.value(key);
    const auto program=m_program.isEmpty() ? QStandardPaths::findExecutable("glslangValidator") : m_program;
    if(program.isEmpty())return fail("glslangValidator is unavailable; CPU colour processing remains available.");
    QTemporaryDir directory;if(!directory.isValid())return fail("Could not create a local shader compilation directory.");
    QFile input(directory.filePath("point.comp"));if(!input.open(QIODevice::WriteOnly) || input.write(bytes)!=bytes.size())return fail("Could not write the local shader source.");input.close();
    QProcess compiler;QByteArray diagnostic;
    const auto drain=[&]{const auto output=compiler.readAllStandardOutput()+compiler.readAllStandardError();if(diagnostic.size()<65536)diagnostic.append(output.left(65536-diagnostic.size()));};
    QObject::connect(&compiler,&QProcess::readyReadStandardOutput,&compiler,drain);
    QObject::connect(&compiler,&QProcess::readyReadStandardError,&compiler,drain);
    compiler.start(program,{"-V","--target-env","vulkan1.0","-S","comp",input.fileName(),"-o",directory.filePath("point.spv")});
    if(!compiler.waitForFinished(5000)){compiler.kill();compiler.waitForFinished(1000);drain();return fail("Local GPU shader compiler failed to start or exceeded its deadline: "+compiler.errorString());}
    drain();if(compiler.exitStatus()!=QProcess::NormalExit || compiler.exitCode()!=0)return fail("GPU shader compilation failed: "+QString::fromUtf8(diagnostic));
    QFile output(directory.filePath("point.spv"));if(!output.open(QIODevice::ReadOnly) || output.size()<20 || output.size()>4*1024*1024)return fail("Local shader compiler did not produce a usable SPIR-V module.");
    const auto binary=output.readAll();if(binary.size()%4)return fail("Local shader compiler produced an invalid SPIR-V size.");
    if(m_cache.size()>=64)m_cache.erase(m_cache.begin());
    m_cache.insert(key,binary);return binary;
}
}
