#include "infra/io_dispatcher.h"

#include <QThread>

namespace app {

IoDispatcher::IoDispatcher(QObject* parent)
    : QObject(parent) {
    const int threads = std::clamp(QThread::idealThreadCount(), 2, 8);
    pool_.setMaxThreadCount(threads);
}

IoDispatcher::~IoDispatcher() {
    // Wait for worker threads to finish so lambda captures stay valid
    pool_.waitForDone(3000);
}

quint64 IoDispatcher::submitAction(
    events::IoOperationKind kind,
    const QString& targetPath,
    const QString& title,
    bool isModal,
    std::function<bool(const std::atomic<bool>& cancelToken, ProgressCallback progress, QString* error)> work,
    std::function<void()> onSuccess,
    std::function<void(const QString& error)> onError
) {
    auto wrappedWork = [work = std::move(work)](const std::atomic<bool>& token, ProgressCallback progress, QString* error) -> std::optional<bool> {
        if (work(token, progress, error)) {
            return true;
        }
        return std::nullopt;
    };
    auto wrappedSuccess = [onSuccess = std::move(onSuccess)](bool) {
        if (onSuccess) {
            onSuccess();
        }
    };
    return submitTask<bool>(kind, targetPath, title, isModal, std::move(wrappedWork), std::move(wrappedSuccess), std::move(onError));
}

bool IoDispatcher::cancel(quint64 opId) {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    auto it = activeTasks_.find(opId);
    if (it != activeTasks_.end()) {
        it->second->store(true);
        return true;
    }
    return false;
}

bool IoDispatcher::isRunning(quint64 opId) const {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    return activeTasks_.find(opId) != activeTasks_.end();
}

void IoDispatcher::waitForDone(int msecs) {
    pool_.waitForDone(msecs);
}

quint64 IoDispatcher::allocateOpId() {
    return nextOpId_.fetch_add(1, std::memory_order_relaxed);
}

IoDispatcher::CancellationToken IoDispatcher::registerTask(quint64 opId) {
    auto token = std::make_shared<std::atomic<bool>>(false);
    std::lock_guard<std::mutex> lock(tasksMutex_);
    activeTasks_[opId] = token;
    return token;
}

void IoDispatcher::unregisterTask(quint64 opId) {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    activeTasks_.erase(opId);
}

void IoDispatcher::dispatchProgress(quint64 opId, int percentage, const QString& message) {
    emit ioProgress(events::IoProgress{opId, percentage, message});
}

void IoDispatcher::dispatchCompleted(quint64 opId, events::IoOperationKind kind, const QString& targetPath, std::any payload) {
    emit ioCompleted(events::IoCompleted{opId, kind, targetPath, std::move(payload)});
}

void IoDispatcher::dispatchFailed(quint64 opId, events::IoOperationKind kind, const QString& targetPath, const QString& error) {
    emit ioFailed(events::IoFailed{opId, kind, targetPath, error});
}

}  // namespace app
