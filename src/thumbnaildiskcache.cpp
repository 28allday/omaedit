#include "thumbnaildiskcache.h"
#include "workingframe.h"
#include "storagepreferences.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QMutex>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <cstdio>
#include <QUuid>
#include <QRegularExpression>
#include <list>
#include <memory>
#include <atomic>

namespace {
// A cache file written whole under a name of its own, then given its name: a
// reader meanwhile (another thread, another running editor) has the old file
// or the new, never a part. Not forced to the disk first, as a saved document
// is: that took milliseconds a thumbnail, and what a power cut leaves of a
// cache file is told by its reader and made again.
bool writeWhole(const QString &path,const QByteArray &bytes){
    QTemporaryFile file(path+QStringLiteral(".XXXXXX"));
    file.setAutoRemove(false);
    if(!file.open())return false;
    const QString written=file.fileName();
    const bool whole=file.write(bytes)==bytes.size();
    file.close();
    if(whole && ::rename(QFile::encodeName(written).constData(),QFile::encodeName(path).constData())==0)return true;
    QFile::remove(written);
    return false;
}
std::atomic<qint64> budget{512ll*1024*1024};
std::atomic<bool> budgetChosen{false}; // setLimit() wins over the preference
struct Store {
    QMutex mutex;
    QString directory;           // this run's own folder under the thumbnail root
    QByteArray session=QUuid::createUuid().toByteArray();
    std::unique_ptr<QLockFile> alive; // held for the run: tells the next one these files are in use
    QHash<QString,qint64> sizes;
    QHash<QString,qint64> revisions;
    std::list<QString> order;
    QHash<QString,std::list<QString>::iterator> positions;
    qint64 used=0;
    bool initialized=false;
    void touch(const QString &name){auto it=positions.find(name);if(it!=positions.end())order.erase(it.value());order.push_front(name);positions[name]=order.begin();}
    // An entry that cannot be removed (permissions, a read-only mount) leaves
    // the books too: kept counted, it could never be evicted and every later
    // trim would empty the rest of the cache.
    void trim(){while((used>ThumbnailDiskCache::limit() || sizes.size()>8192) && !order.empty()){const auto name=order.back();order.pop_back();positions.remove(name);QFile::remove(directory+'/'+name);used-=sizes.take(name);revisions.remove(name);}}
    void initialize(){
        if(initialized)return;initialized=true;
        if(!budgetChosen.load())budget.store(qint64(StoragePreferences::session().value("thumbnailDiskMiB").toInt())*1024*1024);
        const QString root=StoragePreferences::thumbnailRoot();
        if(!QDir().mkpath(root))return;
        // A file's name carries its run's own ID, so no run can read what
        // another wrote: the thumbnails of a run that has ended are dead
        // weight, and are cleared here instead of being counted against the
        // budget. A run still going holds the lock beside its folder.
        const QRegularExpression generated("^[0-9a-f]{64}\\.png$"),runFolder("^run-[0-9a-f]{32}$");
        for(const auto &f:QDir(root).entryInfoList({"*.png"},QDir::Files|QDir::NoSymLinks))
            if(generated.match(f.fileName()).hasMatch())QFile::remove(f.filePath()); // from before runs had folders
        for(const auto &d:QDir(root).entryInfoList({"run-*"},QDir::Dirs|QDir::NoDotAndDotDot|QDir::NoSymLinks)){
            if(!runFolder.match(d.fileName()).hasMatch())continue;
            QLockFile theirs(d.filePath()+".lock");theirs.setStaleLockTime(0);
            if(theirs.tryLock(0))QDir(d.filePath()).removeRecursively();
        }
        const QString mine=root+"/run-"+QString::fromLatin1(QCryptographicHash::hash(session,QCryptographicHash::Md5).toHex());
        alive=std::make_unique<QLockFile>(mine+".lock");alive->setStaleLockTime(0);
        if(!alive->tryLock(0)||!QDir().mkpath(mine)){alive.reset();return;}
        directory=mine;
    }
    QString name(const QString &key)const{return QString::fromLatin1(QCryptographicHash::hash(session+'\0'+key.toUtf8(),QCryptographicHash::Sha256).toHex())+".png";}
};
Store &store(){static Store value;return value;}
}
qint64 ThumbnailDiskCache::limit(){return budget.load();}
void ThumbnailDiskCache::setLimit(qint64 bytes){budgetChosen.store(true);budget.store(qBound(0ll,bytes,16ll*1024*1024*1024));}
namespace {
// The kept folder's books. `used` is what the folder held when last counted
// plus what this run has written since: another running editor writes and
// removes there too, so it is counted again before anything is removed and
// every so many writes.
struct Kept {
    QMutex mutex;
    QString directory;
    bool counted=false;
    qint64 used=0;
    int entries=0;               // files, as last counted and written since
    int writes=0;
    QString folder(){
        if(directory.isEmpty()){
            const QString path=StoragePreferences::thumbnailRoot()+QStringLiteral("/kept-v1");
            if(QDir().mkpath(path))directory=path;
        }
        return directory;
    }
    // Every kind keep() is given: tiles, sound peaks and beat maps.
    static bool ours(const QFileInfo &f){return f.suffix()==QLatin1String("tile")||f.suffix()==QLatin1String("peaks")||f.suffix()==QLatin1String("beats");}
    // Oldest first: a file read lately has had its date moved on (kept()).
    QFileInfoList files(){
        if(folder().isEmpty())return {};
        QFileInfoList all;
        const QDateTime stale=QDateTime::currentDateTime().addSecs(-3600);
        static const QRegularExpression unfinished(QStringLiteral("^[0-9a-f]{64}\\.(tile|peaks|beats)\\.[A-Za-z0-9]+$"));
        for(const auto &f:QDir(directory).entryInfoList(QDir::Files|QDir::NoSymLinks,QDir::Time|QDir::Reversed)){
            if(ours(f))all.append(f);
            // What a write that never finished left (a run that was killed):
            // one of our names with the save file's own ending. Anything
            // else in the folder is not the cache's to remove.
            else if(f.lastModified()<stale && unfinished.match(f.fileName()).hasMatch())QFile::remove(f.filePath());
        }
        return all;
    }
    void count(){const auto all=files();used=0;for(const auto &f:all)used+=f.size();entries=int(all.size());counted=true;writes=0;}
    // No more bytes than `allowance`, and no more files than a folder lists quickly.
    static constexpr int kMostFiles=32768;
    void trim(qint64 allowance){
        if(!counted)count();
        if(used<=allowance && entries<=kMostFiles)return;
        auto all=files();
        used=0;for(const auto &f:std::as_const(all))used+=f.size();
        entries=int(all.size());
        for(const auto &f:std::as_const(all)){
            if(used<=allowance && entries<=kMostFiles)break;
            // Gone already (another editor's trim) or removed here: off the books either way.
            QFile::remove(f.filePath());used-=f.size();--entries;
        }
        counted=true;writes=0;
    }
};
Kept &keptStore(){static Kept value;return value;}
std::atomic<bool> keepingOn{qEnvironmentVariable("OMAEDIT_KEEP_DECOR")!=QLatin1String("0")};
QString keptName(const QString &key,const char *kind){
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(),QCryptographicHash::Sha256).toHex())+QLatin1Char('.')+QLatin1String(kind);
}
// What the kept files may fill: the limit less this run's own thumbnails.
qint64 keptAllowance(){
    auto &s=store();QMutexLocker lock(&s.mutex);s.initialize();
    return qMax<qint64>(0,ThumbnailDiskCache::limit()-s.used);
}
}
void ThumbnailDiskCache::setKeeping(bool on){keepingOn.store(on);}
bool ThumbnailDiskCache::keeping(){return keepingOn.load();}
qint64 ThumbnailDiskCache::keptBytes(){
    const qint64 allowance=keptAllowance();
    auto &k=keptStore();QMutexLocker lock(&k.mutex);k.trim(allowance);return k.used;
}
void ThumbnailDiskCache::clearKept(){
    auto &k=keptStore();QMutexLocker lock(&k.mutex);k.trim(0);
}
QByteArray ThumbnailDiskCache::kept(const QString &key,const char *kind){
    if(!keeping()||limit()<=0)return {};
    QString path;
    {auto &k=keptStore();QMutexLocker lock(&k.mutex);if(k.folder().isEmpty())return {};path=k.directory+QLatin1Char('/')+keptName(key,kind);}
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly))return {};
    const QByteArray bytes=file.readAll();
    // Used today: not among the first to go. (Once a day is often enough to say so.)
    if(file.fileTime(QFileDevice::FileModificationTime).secsTo(QDateTime::currentDateTime())>86400){
        file.close();
        if(QFile touched(path);touched.open(QIODevice::ReadWrite))touched.setFileTime(QDateTime::currentDateTime(),QFileDevice::FileModificationTime);
    }
    return bytes;
}
void ThumbnailDiskCache::keep(const QString &key,const char *kind,const QByteArray &bytes){
    if(!keeping()||bytes.isEmpty())return;
    const qint64 allowance=keptAllowance();
    if(bytes.size()>allowance)return;
    auto &k=keptStore();
    QString path;
    {QMutexLocker lock(&k.mutex);if(k.folder().isEmpty())return;path=k.directory+QLatin1Char('/')+keptName(key,kind);}
    const qint64 before=QFileInfo(path).size(); // 0 when it is not there
    if(!writeWhole(path,bytes))return;
    QMutexLocker lock(&k.mutex);
    if(!k.counted)k.count();else{k.used+=bytes.size()-before;if(before==0)++k.entries;}
    if(++k.writes>=256){k.count();}
    k.trim(allowance);
}
QImage ThumbnailDiskCache::keptImage(const QString &key,QByteArray *packedAs){
    const QByteArray bytes=kept(key,"tile");
    QImage image;
    if(!bytes.isEmpty())image.loadFromData(bytes,"PNG");
    if(packedAs && !image.isNull())*packedAs=bytes;
    return image;
}
void ThumbnailDiskCache::keepImage(const QString &key,const QImage &image,const QByteArray &packedAlready){
    if(image.isNull()||!keeping()||limit()<=0)return;
    keep(key,"tile",packedAlready.isEmpty()?packed(image):packedAlready);
}
// This run's thumbnails and the kept ones together: what the limit is a limit of.
qint64 ThumbnailDiskCache::bytes(){
    qint64 mine=0;
    {auto &s=store();QMutexLocker lock(&s.mutex);s.initialize();mine=s.used;}
    return mine+keptBytes();
}
QImage ThumbnailDiskCache::read(const QString &key){
    auto &s=store();QMutexLocker lock(&s.mutex);s.initialize();if(s.directory.isEmpty())return {};
    s.trim();const auto name=s.name(key);if(!s.sizes.contains(name))return {};
    QImage image(s.directory+'/'+name);if(!image.isNull())s.touch(name);return image;
}
QByteArray ThumbnailDiskCache::packed(const QImage &image){
    QImage preview=image;if(WorkingFrame::isTagged(preview))preview=WorkingFrame::fromTagged(preview).display({}).convertToFormat(QImage::Format_RGBA8888);
    QByteArray bytes;QBuffer buffer(&bytes);
    if(preview.isNull()||!buffer.open(QIODevice::WriteOnly)||!preview.save(&buffer,"PNG"))return {};
    return bytes;
}
void ThumbnailDiskCache::write(const QString &key,const QImage &image,const QByteArray &packedAlready){
    if(image.isNull())return;
    auto &s=store();QMutexLocker lock(&s.mutex);s.initialize();if(s.directory.isEmpty())return;
    s.trim();const auto name=s.name(key);
    if(s.sizes.contains(name) && s.revisions.value(name)==image.cacheKey()){s.touch(name);return;}
    const QByteArray bytes=packedAlready.isEmpty()?packed(image):packedAlready;
    if(bytes.isEmpty())return;
    const auto size=bytes.size();if(size>limit())return;
    if(!writeWhole(s.directory+'/'+name,bytes))return;
    s.used+=size-s.sizes.value(name);s.sizes[name]=size;s.revisions[name]=image.cacheKey();s.touch(name);s.trim();
}
void ThumbnailDiskCache::reopen(){
    auto &s=store();QMutexLocker lock(&s.mutex);
    s.alive.reset();s.directory.clear();
    s.session=QUuid::createUuid().toByteArray();s.sizes.clear();s.revisions.clear();s.order.clear();s.positions.clear();s.used=0;s.initialized=false;
    lock.unlock();
    // The kept folder is counted again, as a new run would count it.
    auto &k=keptStore();QMutexLocker keptLock(&k.mutex);k.counted=false;k.used=0;k.entries=0;k.writes=0;
}
