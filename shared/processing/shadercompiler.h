#pragma once
#include <QByteArray>
#include <QHash>
#include <QString>
namespace SuiteProcessing {
// Optional local compiler. Normal operation retains CPU implementations when
// glslangValidator is unavailable or a shader cannot be compiled.
class ShaderCompiler {
public:
    explicit ShaderCompiler(const QString &program = {}):m_program(program){}
    QByteArray compile(const QString &source,QString *error=nullptr);
    int cachedShaders() const {return m_cache.size();}
private:
    QString m_program;
    QHash<QByteArray,QByteArray> m_cache;
};
}
