#pragma once

#include <QObject>
#include <QRunnable>
#include <QString>
#include <QThreadPool>
#include <any>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "model/io_events.h"

namespace app {

enum class ExecutionPolicy {
    Async,
    DirectSynchronous  // For headless smoke tests and deterministic probes
};

class IoDispatcher : public QObject {
    Q_OBJECT

public:
    using ProgressCallback = std::function<void(int percentage, const QString& message)>;
    using CancellationToken = std::shared_ptr<std::atomic<bool>>;

    explicit IoDispatcher(QObject* parent = nullptr);
    ~IoDispatcher() override;

    ExecutionPolicy executionPolicy() const { return policy_; }
    void setExecutionPolicy(ExecutionPolicy policy) { policy_ = policy; }

    // Submits a typed worker task.
    // `work` executes in a background worker thread (or caller thread if DirectSynchronous).
    // `onSuccess` and `onError` callbacks are ALWAYS invoked on the main GUI thread.
    template <typename ResultT>
    quint64 submitTask(
        events::IoOperationKind kind,
        const QString& targetPath,
        const QString& title,
        bool isModal,
        std::function<std::optional<ResultT>(const std::atomic<bool>& cancelToken, ProgressCallback progress, QString* error)> work,
        std::function<void(ResultT result)> onSuccess = nullptr,
        std::function<void(const QString& error)> onError = nullptr
    );

    // Convenience overload for actions without return payload (e.g. Save, Generate)
    quint64 submitAction(
        events::IoOperationKind kind,
        const QString& targetPath,
        const QString& title,
        bool isModal,
        std::function<bool(const std::atomic<bool>& cancelToken, ProgressCallback progress, QString* error)> work,
        std::function<void()> onSuccess = nullptr,
        std::function<void(const QString& error)> onError = nullptr
    );

    // Requests cancellation of an active task
    bool cancel(quint64 opId);

    // True if task with opId is currently executing
    bool isRunning(quint64 opId) const;

    // Wait until all pending tasks complete
    void waitForDone(int msecs = -1);

signals:
    void ioStarted(const app::events::IoStarted& event);
    void ioProgress(const app::events::IoProgress& event);
    void ioCompleted(const app::events::IoCompleted& event);
    void ioFailed(const app::events::IoFailed& event);

private:
    quint64 allocateOpId();
    CancellationToken registerTask(quint64 opId);
    void unregisterTask(quint64 opId);

    void dispatchProgress(quint64 opId, int percentage, const QString& message);
    void dispatchCompleted(quint64 opId, events::IoOperationKind kind, const QString& targetPath, std::any payload);
    void dispatchFailed(quint64 opId, events::IoOperationKind kind, const QString& targetPath, const QString& error);

    ExecutionPolicy policy_ = ExecutionPolicy::Async;
    std::atomic<quint64> nextOpId_{1};
    mutable std::mutex tasksMutex_;
    std::unordered_map<quint64, CancellationToken> activeTasks_;
    QThreadPool pool_;
};

// ---- Template Implementation ------------------------------------------------

template <typename ResultT>
quint64 IoDispatcher::submitTask(
    events::IoOperationKind kind,
    const QString& targetPath,
    const QString& title,
    bool isModal,
    std::function<std::optional<ResultT>(const std::atomic<bool>& cancelToken, ProgressCallback progress, QString* error)> work,
    std::function<void(ResultT result)> onSuccess,
    std::function<void(const QString& error)> onError
) {
    const quint64 opId = allocateOpId();
    CancellationToken cancelToken = registerTask(opId);

    events::IoStarted startedEvent{opId, kind, targetPath, title, isModal};
    emit ioStarted(startedEvent);

    if (policy_ == ExecutionPolicy::DirectSynchronous) {
        QString error;
        ProgressCallback progressFn = [this, opId](int percent, const QString& msg) {
            dispatchProgress(opId, percent, msg);
        };
        auto result = work(*cancelToken, progressFn, &error);
        unregisterTask(opId);
        if (cancelToken->load()) {
            dispatchFailed(opId, kind, targetPath, QStringLiteral("Operation canceled"));
            if (onError) onError(QStringLiteral("Operation canceled"));
        } else if (result.has_value()) {
            dispatchCompleted(opId, kind, targetPath, std::make_any<ResultT>(*result));
            if (onSuccess) onSuccess(std::move(*result));
        } else {
            dispatchFailed(opId, kind, targetPath, error);
            if (onError) onError(error);
        }
        return opId;
    }

    // Async execution via worker thread
    ProgressCallback progressFn = [this, opId](int percent, const QString& msg) {
        QMetaObject::invokeMethod(this, [this, opId, percent, msg]() {
            dispatchProgress(opId, percent, msg);
        }, Qt::QueuedConnection);
    };

    struct WorkerContext {
        std::function<std::optional<ResultT>(const std::atomic<bool>&, ProgressCallback, QString*)> work;
        std::function<void(ResultT)> onSuccess;
        std::function<void(const QString&)> onError;
    };
    auto ctx = std::make_shared<WorkerContext>(WorkerContext{std::move(work), std::move(onSuccess), std::move(onError)});

    class TaskRunnable : public QRunnable {
    public:
        TaskRunnable(IoDispatcher* dispatcher, quint64 opId, events::IoOperationKind kind, QString path,
                     CancellationToken token, ProgressCallback pfn, std::shared_ptr<WorkerContext> c)
            : dispatcher_(dispatcher), opId_(opId), kind_(kind), path_(std::move(path)),
              token_(std::move(token)), pfn_(std::move(pfn)), ctx_(std::move(c)) {
            setAutoDelete(true);
        }
        void run() override {
            QString error;
            auto result = ctx_->work(*token_, pfn_, &error);
            QMetaObject::invokeMethod(dispatcher_, [dispatcher = dispatcher_, opId = opId_, kind = kind_, path = path_,
                                                    token = token_, res = std::move(result), err = std::move(error), ctx = ctx_]() mutable {
                dispatcher->unregisterTask(opId);
                if (token->load()) {
                    dispatcher->dispatchFailed(opId, kind, path, QStringLiteral("Operation canceled"));
                    if (ctx->onError) ctx->onError(QStringLiteral("Operation canceled"));
                } else if (res.has_value()) {
                    dispatcher->dispatchCompleted(opId, kind, path, std::make_any<ResultT>(*res));
                    if (ctx->onSuccess) ctx->onSuccess(std::move(*res));
                } else {
                    dispatcher->dispatchFailed(opId, kind, path, err);
                    if (ctx->onError) ctx->onError(err);
                }
            }, Qt::QueuedConnection);
        }
    private:
        IoDispatcher* dispatcher_;
        quint64 opId_;
        events::IoOperationKind kind_;
        QString path_;
        CancellationToken token_;
        ProgressCallback pfn_;
        std::shared_ptr<WorkerContext> ctx_;
    };

    pool_.start(new TaskRunnable(this, opId, kind, targetPath, cancelToken, progressFn, ctx));
    return opId;
}

}  // namespace app
