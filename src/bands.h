#pragma once
#include <functional>
class QThreadPool;
namespace Bands {
// Work split into independent pieces: runs tasks [0, count) on the process's
// one pool (engine::RowPool, sized from the machine) and the calling thread
// together, returning once all are done and whether every one succeeded. The
// caller takes pieces too, so a busy pool only slows it down; a call from
// inside a piece runs its pieces in place.
bool run(int count, const std::function<bool(int)> &task);
// How many helpers there are, as its maxThreadCount(): nothing runs on it.
QThreadPool *pool();
} // namespace Bands
