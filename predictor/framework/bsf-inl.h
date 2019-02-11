#pragma once

#include <boost/bind.hpp>
#include <base/atomicops.h>

#include <comlog/comlog.h>
#include "common/inner_common.h"

#include <sys/syscall.h>

namespace im {
namespace bsf {

template<typename TaskT>
void* TaskExecutor<TaskT>::thread_entry(void* args) {
    ComlogGuard logging_guard;

    ThreadContext<TaskT>* context = static_cast<ThreadContext<TaskT>*>(args);
    TaskExecutor<TaskT>* executor = static_cast<TaskExecutor<TaskT>*>(context->executor);
    executor->work(context);

    return NULL;
}

template<typename TaskT>
int TaskExecutor<TaskT>::start(uint32_t thread_num, uint32_t init_timeout_sec) {
    _stop = false;
    if (!_thread_contexts.empty()) {
        CWARNING_LOG("BSF has started");
        return 0;
    }

    if (thread_num == 0) {
        CFATAL_LOG("cannot init BSF with zero thread");
        return -1;
    }

    ThreadContext<TaskT>* contexts = new ThreadContext<TaskT>[thread_num];
    for (uint32_t i = 0; i < thread_num; ++i) {
        contexts[i].executor = this;
        if (_user_thread_contexts != NULL) {
            contexts[i].user_thread_context = _user_thread_contexts[i];
        }

        int rc = THREAD_CREATE(
                &contexts[i].tid, NULL, &TaskExecutor::thread_entry, &contexts[i]);
        if (rc != 0) {
            CFATAL_LOG("failed to create BSF worker thread: index=%u, rc=%d, errno=%d:%m",
                    i, rc, errno);
            return -1;
        }

        _thread_contexts.push_back(&contexts[i]);
    }

    int init_timeout = init_timeout_sec * 1000 * 1000;
    bool has_error = false;

    bool has_timeout = true;
    if (init_timeout == 0) {
        has_timeout = false;
    }

    while (!has_timeout || init_timeout > 0) {
        bool done = true;
        for (size_t i = 0; i < _thread_contexts.size(); ++i) {
            if (_thread_contexts[i]->init_status < 0) {
                has_error = true;
                break;
            }

            if (_thread_contexts[i]->init_status == 0) {
                done = false;
            }
        }

        if (has_error) {
            CFATAL_LOG("BSF thread init error");
            return -1;
        }

        if (done) {
            CDEBUG_LOG("BSF thread init done");
            return 0;
        }

        // 100ms
        const int sleep_interval = 100 * 1000;
        usleep(sleep_interval);
        init_timeout -= sleep_interval;
    }

    CFATAL_LOG("BSF thread init timed out");
    return -1;
}

template<typename TaskT>
void TaskExecutor<TaskT>::stop() {
    _stop = true;
    for (size_t i = 0; i < _thread_contexts.size(); ++i) {
        THREAD_CANCEL(_thread_contexts[i]->tid);
    }

    for (size_t i = 0; i < _thread_contexts.size(); ++i) {
        THREAD_JOIN(_thread_contexts[i]->tid, NULL);
    }
    _thread_contexts.clear();
}

template<typename TaskT>
TaskHandler<TaskT> TaskExecutor<TaskT>::schedule(
        const InArrayT& in, OutArrayT& out) {
    TaskT* task = base::get_object<TaskT>();
    if (!task) {
        LOG(FATAL) << "Failed get TaskT from object pool";
        return TaskHandler<TaskT>::valid_handle();
    }

    if (!BatchTasks<TaskT>::check_valid(in, out, _batch_align)) {
        LOG(FATAL) << "Invalid input & output";
        return TaskHandler<TaskT>::valid_handle();
    }

    int fds[2];
    int rc = pipe(fds);
    if (rc != 0) {
        CFATAL_LOG("call pipe() failed, errno=%d:%m", errno);
        return TaskHandler<TaskT>::valid_handle();
    }

    task->read_fd = fds[0];
    task->write_fd = fds[1];
    task->owner_tid = ::syscall(SYS_gettid);

    task->in = &in;
    task->out = &out;
    task->rem = in.size();
    task->size = in.size();
    task->index.store(0, base::memory_order_relaxed);

    AutoMutex lock(_mut);
    _task_queue.push_back(task);
    THREAD_COND_SIGNAL(&_cond);

    return TaskHandler<TaskT>(*task);
}

template<typename TaskT>
bool TaskExecutor<TaskT>::fetch_batch(BatchTasks<TaskT>& batch) {
    AutoMutex lock(_mut);
    while (_task_queue.empty()) {
        THREAD_COND_WAIT(&_cond, &_mut);
    }

    if (_task_queue.empty()) {
        CFATAL_LOG("invalid task queue!");
        return false;
    }

    while (!_task_queue.empty()) {
        TaskT* task = _task_queue.front();
        size_t rem = batch.append_task(task);
        if (task->rem <= 0) {
            _task_queue.pop_front();
        }
        if (rem <= 0) break;
    }

    return true;
}

template<typename TaskT>
int TaskExecutor<TaskT>::work(ThreadContext<TaskT>* context) {
    if (_thread_init_fn != NULL) {
        if (_thread_init_fn(context->user_thread_context) != 0) {
            CFATAL_LOG("execute thread init thunk failed, BSF thread will exit");
            context->init_status = -1;
            return -1;
        } else {
            CDEBUG_LOG("execute thread init thunk succeed");
        }
    }

    context->init_status = 1;
    while (!_stop) {
        if (_thread_reset_fn != NULL) {
            if (_thread_reset_fn(context->user_thread_context) != 0) {
                CFATAL_LOG("execute user thread reset failed");
            }
        }

        BatchTasks<TaskT> batch(_batch_size, _batch_align);
        if (fetch_batch(batch)) {
            batch.merge_tasks();
            _fn(batch.in(), batch.out());
            batch.notify_tasks();
        }
    }

    return 0;
}

template<typename InItemT, typename OutItemT>
bool TaskManager<InItemT, OutItemT>::schedule(const InArrayT& in, 
        OutArrayT& out) {
    TaskHandler<TaskT> handler = _executor.schedule(in, out);

    if (handler.valid()) {
        _task_owned = handler;
        return true;
    } else {
        CFATAL_LOG("failed to schedule task");
        return false;
    }
}

template<typename InItemT, typename OutItemT>
void TaskManager<InItemT, OutItemT>::wait() {
    char buffer[128];
    while (read(_task_owned.read_fd, buffer, sizeof(buffer)) < 0
            && errno == EINTR) {
        ;
    }

    close(_task_owned.read_fd);
    close(_task_owned.write_fd);

    _task_owned.read_fd = -1;
    _task_owned.write_fd = -1;
    return;
}

}
}
