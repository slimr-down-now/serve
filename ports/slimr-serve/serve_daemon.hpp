#pragma once
#include "serve.hpp"

#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <grp.h>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace slimr {

/** Opt-in listener, Unix socket, and ThreadPool configuration for DaemonServer. */
struct DaemonListenOptions {
    std::string host{"127.0.0.1"};
    std::optional<int> port;

    std::string socketPath;
    mode_t socketMode{0600};
    std::string socketGroup;

    uint32_t maxConnections{64};
    size_t minWorkerThreads{8};
    int readTimeoutSec{5};
    int writeTimeoutSec{1};
    int keepaliveTimeoutSec{30};
    int shutdownTimeoutMs{5000};
    std::chrono::milliseconds cancelPollInterval{50};
};

namespace serve_daemon_detail {

inline bool resolve_group_gid(std::string_view groupName, gid_t &outGid, std::string &errOut) {
    if (groupName.empty()) {
        return false;
    }
    long initSize = ::sysconf(_SC_GETGR_R_SIZE_MAX);
    size_t bufSize = (initSize > 0) ? static_cast<size_t>(initSize) : 16384;
    std::string nameStr(groupName);
    for (int attempt = 0; attempt < 4; ++attempt) {
        std::vector<char> buf(bufSize);
        struct group grp{};
        struct group *res = nullptr;
        int rc = ::getgrnam_r(nameStr.c_str(), &grp, buf.data(), buf.size(), &res);
        if (rc == 0) {
            if (!res) {
                errOut = "Unknown socket group: " + nameStr;
                return false;
            }
            outGid = res->gr_gid;
            return true;
        }
        if (rc == ERANGE) {
            bufSize *= 2;
            continue;
        }
        errOut = "Failed to resolve socket group '" + nameStr + "': " + std::strerror(rc);
        return false;
    }
    errOut = "Failed to resolve socket group '" + nameStr + "': buffer limit exceeded";
    return false;
}

inline bool validate_sun_path(std::string_view socketPath, std::string &errOut) {
    if (socketPath.empty()) {
        errOut = "Unix socket path must not be empty";
        return false;
    }
    constexpr size_t kMaxSunPath = sizeof(sockaddr_un::sun_path) - 1;
    if (socketPath.size() > kMaxSunPath) {
        errOut = "Unix socket path exceeds sun_path limit (" +
                 std::to_string(socketPath.size()) + " > " + std::to_string(kMaxSunPath) + ")";
        return false;
    }
    return true;
}

} // namespace serve_daemon_detail

/** RAII flock single-instance guard and socket file lifecycle manager. */
class SocketLockGuard {
  public:
    SocketLockGuard() = default;
    SocketLockGuard(const SocketLockGuard &) = delete;
    SocketLockGuard &operator=(const SocketLockGuard &) = delete;

    SocketLockGuard(SocketLockGuard &&other) noexcept
        : fd_(std::exchange(other.fd_, -1)),
          socketPath_(std::move(other.socketPath_)),
          unlinkOnClose_(std::exchange(other.unlinkOnClose_, false)) {}

    SocketLockGuard &operator=(SocketLockGuard &&other) noexcept {
        if (this != &other) {
            release();
            fd_ = std::exchange(other.fd_, -1);
            socketPath_ = std::move(other.socketPath_);
            unlinkOnClose_ = std::exchange(other.unlinkOnClose_, false);
        }
        return *this;
    }

    ~SocketLockGuard() { release(); }

    bool acquire(const std::string &socketPath, std::string_view socketGroup, std::string &errOut) {
        release();
        if (!serve_daemon_detail::validate_sun_path(socketPath, errOut)) {
            return false;
        }

        std::filesystem::path sockP(socketPath);
        std::filesystem::path parent = sockP.parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                errOut = "Failed to create socket parent directory: " + ec.message();
                return false;
            }
            mode_t dirMode = socketGroup.empty() ? static_cast<mode_t>(0700) : static_cast<mode_t>(0750);
            if (::chmod(parent.c_str(), dirMode) != 0) {
                errOut = "Failed to chmod socket parent directory: " + std::string(std::strerror(errno));
                return false;
            }
            if (!socketGroup.empty()) {
                gid_t gid{};
                if (!serve_daemon_detail::resolve_group_gid(socketGroup, gid, errOut)) {
                    return false;
                }
                if (::chown(parent.c_str(), static_cast<uid_t>(-1), gid) != 0) {
                    errOut = "Failed to chgrp socket parent directory: " + std::string(std::strerror(errno));
                    return false;
                }
            }
        }

        std::string lockPath = socketPath + ".lock";
        int fd = ::open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (fd < 0) {
            errOut = "Failed to open socket lock file '" + lockPath + "': " + std::strerror(errno);
            return false;
        }
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            int savedErr = errno;
            ::close(fd);
            if (savedErr == EWOULDBLOCK || savedErr == EAGAIN) {
                errOut = "Another daemon instance holds lock: " + lockPath;
            } else {
                errOut = "Failed to flock '" + lockPath + "': " + std::strerror(savedErr);
            }
            return false;
        }

        struct stat st{};
        if (::lstat(socketPath.c_str(), &st) == 0) {
            if (!S_ISSOCK(st.st_mode)) {
                ::flock(fd, LOCK_UN);
                ::close(fd);
                errOut = "Socket path exists and is not a socket: " + socketPath;
                return false;
            }
            if (::unlink(socketPath.c_str()) != 0) {
                int savedErr = errno;
                ::flock(fd, LOCK_UN);
                ::close(fd);
                errOut = "Failed to remove stale socket '" + socketPath + "': " + std::strerror(savedErr);
                return false;
            }
        }

        fd_ = fd;
        socketPath_ = socketPath;
        unlinkOnClose_ = false;
        return true;
    }

    void markBound() noexcept { unlinkOnClose_ = true; }

    void release() noexcept {
        if (unlinkOnClose_ && !socketPath_.empty()) {
            ::unlink(socketPath_.c_str());
        }
        unlinkOnClose_ = false;
        socketPath_.clear();
        if (fd_ >= 0) {
            ::flock(fd_, LOCK_UN);
            ::close(fd_);
            fd_ = -1;
        }
    }

  private:
    int fd_{-1};
    std::string socketPath_;
    bool unlinkOnClose_{false};
};

/** Shared ~50 ms poll watcher that triggers CancellationSource when a client disconnects. */
class DisconnectWatcher {
  public:
    explicit DisconnectWatcher(std::chrono::milliseconds pollInterval = std::chrono::milliseconds{50})
        : pollInterval_(pollInterval.count() > 0 ? pollInterval : std::chrono::milliseconds{50}) {}

    DisconnectWatcher(const DisconnectWatcher &) = delete;
    DisconnectWatcher &operator=(const DisconnectWatcher &) = delete;
    ~DisconnectWatcher() { stop(); }

    class ScopedWatch {
      public:
        ScopedWatch() = default;
        ScopedWatch(DisconnectWatcher &watcher, uint64_t id) noexcept
            : watcher_(&watcher), id_(id) {}
        ScopedWatch(const ScopedWatch &) = delete;
        ScopedWatch &operator=(const ScopedWatch &) = delete;
        ScopedWatch(ScopedWatch &&other) noexcept
            : watcher_(std::exchange(other.watcher_, nullptr)),
              id_(std::exchange(other.id_, 0)) {}
        ScopedWatch &operator=(ScopedWatch &&other) noexcept {
            if (this != &other) {
                reset();
                watcher_ = std::exchange(other.watcher_, nullptr);
                id_ = std::exchange(other.id_, 0);
            }
            return *this;
        }
        ~ScopedWatch() { reset(); }

        void reset() noexcept {
            if (watcher_ && id_ != 0) {
                watcher_->untrack(id_);
            }
            watcher_ = nullptr;
            id_ = 0;
        }

      private:
        DisconnectWatcher *watcher_{nullptr};
        uint64_t id_{0};
    };

    void start() {
        std::lock_guard lock(startMu_);
        if (thread_.joinable()) {
            return;
        }
        thread_ = std::jthread([this](std::stop_token st) { run(std::move(st)); });
    }

    void stop() noexcept {
        std::lock_guard lock(startMu_);
        if (!thread_.joinable()) {
            return;
        }
        thread_.request_stop();
        cv_.notify_all();
        thread_.join();
        std::lock_guard entryLock(mu_);
        entries_.clear();
    }

    ScopedWatch watch(const httplib::Request &req, std::shared_ptr<CancellationSource> cancel) {
        if (!req.is_connection_closed || !cancel) {
            return ScopedWatch{};
        }
        uint64_t id = 0;
        {
            std::lock_guard lock(mu_);
            id = nextId_++;
            entries_.push_back(Entry{id, req.is_connection_closed, std::move(cancel)});
        }
        return ScopedWatch(*this, id);
    }

    void untrack(uint64_t id) noexcept {
        if (id == 0) {
            return;
        }
        std::lock_guard lock(mu_);
        std::erase_if(entries_, [id](const Entry &e) { return e.id == id; });
    }

  private:
    struct Entry {
        uint64_t id{0};
        std::function<bool()> isClosed;
        std::shared_ptr<CancellationSource> cancel;
    };

    void run(std::stop_token st) {
        std::unique_lock waitLk(waitMu_);
        while (!st.stop_requested()) {
            cv_.wait_for(waitLk, st, pollInterval_, [&st] { return st.stop_requested(); });
            if (st.stop_requested()) {
                break;
            }
            std::vector<Entry> snapshot;
            {
                std::lock_guard lock(mu_);
                snapshot = entries_;
            }
            std::vector<uint64_t> closedIds;
            for (const auto &entry : snapshot) {
                bool closed = false;
                try {
                    closed = entry.isClosed && entry.isClosed();
                } catch (...) { // NOLINT(bugprone-empty-catch)
                    closed = true;
                }
                if (closed) {
                    if (entry.cancel) {
                        entry.cancel->cancel();
                    }
                    closedIds.push_back(entry.id);
                }
            }
            if (!closedIds.empty()) {
                std::lock_guard lock(mu_);
                std::erase_if(entries_, [&closedIds](const Entry &e) {
                    return std::find(closedIds.begin(), closedIds.end(), e.id) != closedIds.end();
                });
            }
        }
    }

    std::chrono::milliseconds pollInterval_;
    std::mutex startMu_;
    std::mutex waitMu_;
    std::condition_variable_any cv_;
    std::mutex mu_;
    uint64_t nextId_{1};
    std::vector<Entry> entries_;
    std::jthread thread_;
};

/** Manages opt-in TCP and Unix socket httplib::Server listeners, flock guard, onServe, and cancel watcher. */
class DaemonServer {
  public:
    using RouteConfiguratorFn = std::function<void(httplib::Server &, DisconnectWatcher &, bool isUnixSocket)>;

    DaemonServer(ServeApp &app, DaemonListenOptions opts, RouteConfiguratorFn configureRoutes)
        : app_(app),
          opts_(std::move(opts)),
          configureRoutes_(std::move(configureRoutes)),
          watcher_(opts_.cancelPollInterval) {}

    DaemonServer(const DaemonServer &) = delete;
    DaemonServer &operator=(const DaemonServer &) = delete;
    ~DaemonServer() { stop(); }

    bool start(std::string *errOut = nullptr) {
        std::string localErr;
        auto fail = [&](std::string msg) -> bool {
            if (errOut) {
                *errOut = msg;
            }
            stop();
            return false;
        };

        const bool wantTcp = opts_.port.has_value();
        const bool wantUnix = !opts_.socketPath.empty();
        if (!wantTcp && !wantUnix) {
            return fail("No listener configured: specify --port or --socket");
        }
        if (wantTcp && (*opts_.port < 0 || *opts_.port > 65535)) {
            return fail("Invalid TCP port: " + std::to_string(*opts_.port));
        }

        shutdownRequested_.store(false, std::memory_order_release);
        stopping_.store(false, std::memory_order_release);
        watcher_.start();

        if (wantUnix) {
            if (!lockGuard_.acquire(opts_.socketPath, opts_.socketGroup, localErr)) {
                return fail(std::move(localErr));
            }
            unixSvr_ = makeServer();
            unixSvr_->set_address_family(AF_UNIX);
            if (configureRoutes_) {
                configureRoutes_(*unixSvr_, watcher_, true);
            }
            if (!unixSvr_->bind_to_port(opts_.socketPath, 80)) {
                return fail("Failed to bind Unix socket: " + opts_.socketPath);
            }
            lockGuard_.markBound();
            if (::chmod(opts_.socketPath.c_str(), opts_.socketMode) != 0) {
                return fail("Failed to chmod Unix socket: " + std::string(std::strerror(errno)));
            }
            if (!opts_.socketGroup.empty()) {
                gid_t gid{};
                if (!serve_daemon_detail::resolve_group_gid(opts_.socketGroup, gid, localErr)) {
                    return fail(std::move(localErr));
                }
                if (::chown(opts_.socketPath.c_str(), static_cast<uid_t>(-1), gid) != 0) {
                    return fail("Failed to chgrp Unix socket: " + std::string(std::strerror(errno)));
                }
            }
            unixThread_ = std::jthread([this](std::stop_token) {
                if (unixSvr_) {
                    unixSvr_->listen_after_bind();
                }
            });
            unixSvr_->wait_until_ready();
            if (!unixSvr_->is_running()) {
                return fail("Unix socket listener failed to enter running state");
            }
        }

        if (wantTcp) {
            tcpSvr_ = makeServer();
            if (configureRoutes_) {
                configureRoutes_(*tcpSvr_, watcher_, false);
            }
            if (*opts_.port == 0) {
                int assigned = tcpSvr_->bind_to_any_port(opts_.host);
                if (assigned <= 0) {
                    return fail("Failed to bind TCP ephemeral port on " + opts_.host);
                }
                boundTcpPort_.store(assigned, std::memory_order_release);
            } else {
                if (!tcpSvr_->bind_to_port(opts_.host, *opts_.port)) {
                    return fail("Failed to bind TCP " + opts_.host + ":" + std::to_string(*opts_.port));
                }
                boundTcpPort_.store(*opts_.port, std::memory_order_release);
            }
            tcpThread_ = std::jthread([this](std::stop_token) {
                if (tcpSvr_) {
                    tcpSvr_->listen_after_bind();
                }
            });
            tcpSvr_->wait_until_ready();
            if (!tcpSvr_->is_running()) {
                return fail("TCP listener failed to enter running state");
            }
        }

        running_.store(true, std::memory_order_release);

        if (app_.onServe) {
            onServeThread_ = std::jthread([this](std::stop_token st) {
                try {
                    app_.onServe(std::move(st));
                } catch (const std::exception &e) {
                    std::string safeMsg = redact_sensitive(e.what());
                    if (auto *bs = app_.bugsink()) {
                        bs->reportError(std::runtime_error(safeMsg));
                    }
                    requestShutdown();
                } catch (...) {
                    if (auto *bs = app_.bugsink()) {
                        bs->reportError(std::runtime_error("Unknown exception in onServe"));
                    }
                    requestShutdown();
                }
            });
        }

        return true;
    }

    void requestShutdown() noexcept {
        shutdownRequested_.store(true, std::memory_order_release);
    }

    bool isShutdownRequested() const noexcept {
        return shutdownRequested_.load(std::memory_order_acquire);
    }

    bool isRunning() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    int boundTcpPort() const noexcept {
        return boundTcpPort_.load(std::memory_order_acquire);
    }

    const std::string &socketPath() const noexcept { return opts_.socketPath; }

    void stop() noexcept {
        if (stopping_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        running_.store(false, std::memory_order_release);

        if (onServeThread_.joinable()) {
            onServeThread_.request_stop();
            onServeThread_.join();
        }

        if (tcpSvr_) {
            tcpSvr_->stop();
        }
        if (unixSvr_) {
            unixSvr_->stop();
        }
        if (tcpThread_.joinable()) {
            tcpThread_.join();
        }
        if (unixThread_.joinable()) {
            unixThread_.join();
        }

        tcpSvr_.reset();
        unixSvr_.reset();
        boundTcpPort_.store(0, std::memory_order_release);

        watcher_.stop();
        lockGuard_.release();
    }

    int runUntilSignal() {
        static std::atomic<bool> *sStopFlag = nullptr;
        sStopFlag = &shutdownRequested_;

        auto sigHandler = [](int) {
            if (sStopFlag) {
                sStopFlag->store(true, std::memory_order_release);
            }
        };
        auto prevInt = std::signal(SIGINT, sigHandler);
        auto prevTerm = std::signal(SIGTERM, sigHandler);

        while (running_.load(std::memory_order_acquire) &&
               !shutdownRequested_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        std::signal(SIGINT, prevInt);
        std::signal(SIGTERM, prevTerm);
        sStopFlag = nullptr;
        stop();
        return 0;
    }

  private:
    std::unique_ptr<httplib::Server> makeServer() const {
        auto svr = std::make_unique<httplib::Server>();
        size_t baseThreads = std::max<size_t>(8, opts_.minWorkerThreads);
        size_t maxThreads = std::max<size_t>(baseThreads, static_cast<size_t>(opts_.maxConnections) + 16);
        svr->new_task_queue = [baseThreads, maxThreads]() {
            return new httplib::ThreadPool(baseThreads, maxThreads, /*mqr=*/0);
        };
        svr->set_read_timeout(opts_.readTimeoutSec, 0);
        svr->set_write_timeout(opts_.writeTimeoutSec, 0);
        svr->set_keep_alive_timeout(opts_.keepaliveTimeoutSec);
        return svr;
    }

    ServeApp &app_;
    DaemonListenOptions opts_;
    RouteConfiguratorFn configureRoutes_;
    DisconnectWatcher watcher_;
    SocketLockGuard lockGuard_;
    std::unique_ptr<httplib::Server> tcpSvr_;
    std::unique_ptr<httplib::Server> unixSvr_;
    std::jthread tcpThread_;
    std::jthread unixThread_;
    std::jthread onServeThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> shutdownRequested_{false};
    std::atomic<int> boundTcpPort_{0};
};

} // namespace slimr
