#include "fd_control.hpp"

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <memory>
#include <stdexcept>

#include <userver/engine/task/cancel.hpp>
#include <userver/logging/log.hpp>
#include <userver/utils/assert.hpp>

#include <engine/task/task_context.hpp>
#include <utils/check_syscall.hpp>

USERVER_NAMESPACE_BEGIN

namespace engine::io::impl {
namespace {

int SetNonblock(int fd) {
    const int oldflags =
        utils::CheckSyscallCustomException<IoSystemError>(::fcntl(fd, F_GETFL), "getting file status flags, fd={}", fd);
    if (!(oldflags & O_NONBLOCK)) {
        utils::CheckSyscallCustomException<IoSystemError>(
            ::fcntl(fd, F_SETFL, oldflags | O_NONBLOCK), "setting file status flags, fd=", fd
        );
    }
    return fd;
}

int SetCloexec(int fd) {
    const int oldflags =
        utils::CheckSyscallCustomException<IoSystemError>(::fcntl(fd, F_GETFD), "getting file status flags, fd={}", fd);
    if (!(oldflags & FD_CLOEXEC)) {
        utils::CheckSyscallCustomException<IoSystemError>(
            ::fcntl(fd, F_SETFD, oldflags | FD_CLOEXEC), "setting file status flags, fd={}", fd
        );
    }
    return fd;
}

int ReduceSigpipe(int fd) {
#ifdef F_SETNOSIGPIPE
    // may fail for all we care, SIGPIPE is ignored anyway
    ::fcntl(fd, F_SETNOSIGPIPE, 1);
#endif
    return fd;
}

}  // namespace

void FdControlDeleter::operator()(FdControl* ptr) const noexcept { std::default_delete<FdControl>{}(ptr); }

#ifndef NDEBUG
Direction::SingleUserGuard::SingleUserGuard(Direction& dir) : dir_(dir) { dir_.poller_.SwitchStateToInUse(); }

Direction::SingleUserGuard::~SingleUserGuard() { dir_.poller_.SwitchStateToReadyToUse(); }
#endif  // #ifndef NDEBUG

// Write operations on socket usually do not block, so it makes sense to reuse
// the same ThreadControl for the sake of better balancing of ev threads.
FdControl::FdControl(const ev::ThreadControl& control) : read_(control), write_(control) {}

FdControl::~FdControl() {
    try {
        Close();
    } catch (const std::exception& e) {
        LOG_ERROR() << "Exception while destructing: " << e;
    }
}

FdControlHolder FdControl::Adopt(int fd) {
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
    FdControlHolder fd_control{new FdControl(current_task::GetEventThread())};
    // TODO: add conditional CLOEXEC set
    SetCloexec(fd);
    SetNonblock(fd);
    ReduceSigpipe(fd);
    fd_control->read_.Reset(fd, Direction::Kind::kRead);
    fd_control->write_.Reset(fd, Direction::Kind::kWrite);
    return fd_control;
}

void FdControl::Close() {
    if (!IsValid()) return;
    Invalidate();

    const auto fd = Fd();
    if (::close(fd) == -1) {
        const auto error_code = errno;
        const std::error_code ec(error_code, std::system_category());
        UASSERT_MSG(!error_code, "Failed to close fd=" + std::to_string(fd));
        LOG_ERROR() << "Cannot close fd " << fd << ": " << ec.message();
    }

    read_.WakeupWaiters();
    write_.WakeupWaiters();
}

void FdControl::Invalidate() {
    read_.Invalidate();
    write_.Invalidate();
}

}  // namespace engine::io::impl

USERVER_NAMESPACE_END
