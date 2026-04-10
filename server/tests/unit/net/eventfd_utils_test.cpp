#include <cerrno>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include <sys/eventfd.h>
#include <unistd.h>

ssize_t nebula_test_read(int fd, void* buffer, std::size_t count);
ssize_t nebula_test_write(int fd, const void* buffer, std::size_t count);

// NOLINTNEXTLINE(readability-identifier-naming)
#define read nebula_test_read
// NOLINTNEXTLINE(readability-identifier-naming)
#define write nebula_test_write
#include "nebula/net/eventfd_utils.hpp"
#undef write
#undef read

#include "nebula_tests/test_support.hpp"

namespace {

using nebula::testsupport::expect_equal;
using nebula::testsupport::expect_true;

struct ReadFaultStep {
    enum class Kind : std::uint8_t {
        Errno,
        Value,
    };

    Kind kind = Kind::Errno;
    int err = 0;
    std::uint64_t value = 0;
};

struct WriteFaultStep {
    int err = 0;
};

struct FaultState {
    std::vector<ReadFaultStep> read_faults;
    std::size_t next_read_fault = 0;
    std::vector<WriteFaultStep> write_faults;
    std::size_t next_write_fault = 0;
    std::size_t read_call_count = 0;
    std::size_t write_call_count = 0;
};

FaultState& fault_state() {
    static FaultState state;
    return state;
}

void clear_fault_state() {
    FaultState& state = fault_state();
    state.read_faults.clear();
    state.next_read_fault = 0;
    state.write_faults.clear();
    state.next_write_fault = 0;
    state.read_call_count = 0;
    state.write_call_count = 0;
}

void set_fault_state(std::vector<ReadFaultStep> read_faults, std::vector<WriteFaultStep> write_faults) {
    FaultState& state = fault_state();
    state.read_faults = std::move(read_faults);
    state.next_read_fault = 0;
    state.write_faults = std::move(write_faults);
    state.next_write_fault = 0;
    state.read_call_count = 0;
    state.write_call_count = 0;
}

class ScopedFaultInjection {
public:
    ScopedFaultInjection(std::vector<ReadFaultStep> read_faults, std::vector<WriteFaultStep> write_faults) {
        set_fault_state(std::move(read_faults), std::move(write_faults));
    }

    ~ScopedFaultInjection() noexcept {
        clear_fault_state();
    }

    ScopedFaultInjection(const ScopedFaultInjection&) = delete;
    ScopedFaultInjection& operator=(const ScopedFaultInjection&) = delete;
    ScopedFaultInjection(ScopedFaultInjection&&) = delete;
    ScopedFaultInjection& operator=(ScopedFaultInjection&&) = delete;
};

class ScopedFd {
public:
    explicit ScopedFd(int fd) : fd_(fd) {}
    ~ScopedFd() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ScopedFd(ScopedFd&&) = delete;
    ScopedFd& operator=(ScopedFd&&) = delete;

    [[nodiscard]] int get() const {
        return fd_;
    }

private:
    int fd_ = -1;
};

ReadFaultStep read_errno_fault(int err) {
    return ReadFaultStep{
        .kind = ReadFaultStep::Kind::Errno,
        .err = err,
        .value = 0,
    };
}

ReadFaultStep read_value_fault(std::uint64_t value) {
    return ReadFaultStep{
        .kind = ReadFaultStep::Kind::Value,
        .err = 0,
        .value = value,
    };
}

WriteFaultStep write_errno_fault(int err) {
    return WriteFaultStep{.err = err};
}

void test_notify_eventfd_retries_on_eintr_then_succeeds() {
    const int event_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    expect_true(event_fd >= 0, "eventfd should be created");
    const ScopedFd guard(event_fd);

    const ScopedFaultInjection inject({}, {write_errno_fault(EINTR)});

    const int err = nebula::net::notify_eventfd(guard.get());
    expect_equal(err, 0, "notify_eventfd should retry EINTR and succeed");
    expect_equal(fault_state().write_call_count, static_cast<std::size_t>(2),
                 "notify_eventfd should invoke write twice after EINTR retry");

    std::uint64_t counter = 0;
    const ssize_t read_n = ::read(guard.get(), &counter, sizeof(counter));
    expect_equal(read_n, static_cast<ssize_t>(sizeof(counter)), "eventfd should contain one wakeup");
    expect_equal(counter, static_cast<std::uint64_t>(1), "eventfd counter should increment once");
}

void test_notify_eventfd_treats_eagain_as_success() {
    const ScopedFaultInjection inject({}, {write_errno_fault(EAGAIN)});

    const int err = nebula::net::notify_eventfd(-1);
    expect_equal(err, 0, "notify_eventfd should treat EAGAIN as success");
    expect_equal(fault_state().write_call_count, static_cast<std::size_t>(1),
                 "notify_eventfd should not retry after EAGAIN");
}

void test_notify_eventfd_propagates_nonrecoverable_error() {
    const ScopedFaultInjection inject({}, {write_errno_fault(EBADF)});

    const int err = nebula::net::notify_eventfd(-1);
    expect_equal(err, EBADF, "notify_eventfd should return nonrecoverable errno");
    expect_equal(fault_state().write_call_count, static_cast<std::size_t>(1),
                 "notify_eventfd should stop after nonrecoverable errno");
}

void test_drain_eventfd_retries_on_eintr_and_stops_on_eagain() {
    const ScopedFaultInjection inject({read_errno_fault(EINTR), read_value_fault(3), read_errno_fault(EAGAIN)}, {});

    const int err = nebula::net::drain_eventfd(-1);
    expect_equal(err, 0, "drain_eventfd should continue after EINTR and stop on EAGAIN");
    expect_equal(fault_state().read_call_count, static_cast<std::size_t>(3),
                 "drain_eventfd should consume EINTR/value/EAGAIN sequence");
}

void test_drain_eventfd_propagates_nonrecoverable_error() {
    const ScopedFaultInjection inject({read_errno_fault(EBADF)}, {});

    const int err = nebula::net::drain_eventfd(-1);
    expect_equal(err, EBADF, "drain_eventfd should return nonrecoverable errno");
    expect_equal(fault_state().read_call_count, static_cast<std::size_t>(1),
                 "drain_eventfd should stop after nonrecoverable errno");
}

void test_drain_eventfd_real_fd_empty_is_success() {
    const int event_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    expect_true(event_fd >= 0, "eventfd should be created");
    const ScopedFd guard(event_fd);

    const ScopedFaultInjection inject({}, {});

    const int err = nebula::net::drain_eventfd(guard.get());
    expect_equal(err, 0, "drain_eventfd should succeed when real eventfd is empty");
    expect_equal(fault_state().read_call_count, static_cast<std::size_t>(1),
                 "drain_eventfd should perform one read on empty real eventfd");
}

int run_eventfd_utils_tests() {
    const std::vector<nebula::testsupport::TestCase> tests = {
        {"notify eventfd retries on EINTR then succeeds", test_notify_eventfd_retries_on_eintr_then_succeeds},
        {"notify eventfd treats EAGAIN as success", test_notify_eventfd_treats_eagain_as_success},
        {"notify eventfd propagates nonrecoverable error", test_notify_eventfd_propagates_nonrecoverable_error},
        {"drain eventfd retries on EINTR and stops on EAGAIN", test_drain_eventfd_retries_on_eintr_and_stops_on_eagain},
        {"drain eventfd propagates nonrecoverable error", test_drain_eventfd_propagates_nonrecoverable_error},
        {"drain eventfd real fd empty is success", test_drain_eventfd_real_fd_empty_is_success},
    };

    return nebula::testsupport::run_tests(tests);
}

}  // namespace

ssize_t nebula_test_read(int fd, void* buffer, std::size_t count) {
    FaultState& state = fault_state();
    ++state.read_call_count;

    if (state.next_read_fault < state.read_faults.size()) {
        const ReadFaultStep fault = state.read_faults[state.next_read_fault];
        ++state.next_read_fault;

        if (fault.kind == ReadFaultStep::Kind::Errno) {
            errno = fault.err;
            return -1;
        }

        if (buffer != nullptr && count >= sizeof(std::uint64_t)) {
            std::memcpy(buffer, &fault.value, sizeof(std::uint64_t));
        }
        return static_cast<ssize_t>(sizeof(std::uint64_t));
    }

    return ::read(fd, buffer, count);
}

ssize_t nebula_test_write(int fd, const void* buffer, std::size_t count) {
    FaultState& state = fault_state();
    ++state.write_call_count;

    if (state.next_write_fault < state.write_faults.size()) {
        const WriteFaultStep fault = state.write_faults[state.next_write_fault];
        ++state.next_write_fault;
        errno = fault.err;
        return -1;
    }

    return ::write(fd, buffer, count);
}

int main() {
    return nebula::testsupport::run_main(run_eventfd_utils_tests);
}
