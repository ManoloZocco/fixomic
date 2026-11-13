#include "plugins/PluginCatalog.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <queue>
#include <libproc.h>
#include <sys/sysctl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fixomic {

namespace {

// The same formats on both sides of the scan, so format names match.
void addFormats(juce::AudioPluginFormatManager& formats) {
    formats.addFormat(std::make_unique<juce::AudioUnitPluginFormat>());
    formats.addFormat(std::make_unique<juce::VST3PluginFormat>());
}

juce::AudioPluginFormat* findFormat(const juce::AudioPluginFormatManager& formats, const juce::String& name) {
    for (auto* format : formats.getFormats())
        if (format->getName() == name) return format;
    return nullptr;
}

// AU components sent to the worker: effects and music effects only (instruments, generators,
// panners, mixers and MIDI processors cannot sit in the chain).
bool isAudioUnitEffect(const juce::String& identifier) {
    return identifier.startsWith("AudioUnit:Effects/");
}

struct ScanRequest {
    juce::String formatName, identifier;
    int timeoutSeconds = 0;

    juce::MemoryBlock toBlock() const {
        juce::MemoryBlock block;
        juce::MemoryOutputStream out(block, false);
        out.writeString(formatName);
        out.writeString(identifier);
        out.writeInt(timeoutSeconds);
        out.flush();
        return block;
    }

    static std::optional<ScanRequest> fromBlock(const juce::MemoryBlock& block) {
        if (block.isEmpty()) return std::nullopt;
        juce::MemoryInputStream in(block, false);
        ScanRequest r;
        r.formatName = in.readString();
        r.identifier = in.readString();
        r.timeoutSeconds = in.readInt();
        if (r.formatName.isEmpty() || r.identifier.isEmpty()) return std::nullopt;
        return r;
    }
};

// Direct, still running children of this process whose executable is `executable`.
std::vector<pid_t> childrenRunning(const juce::File& executable) {
    int mib[3] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL};
    size_t size = 0;
    if (sysctl(mib, 3, nullptr, &size, nullptr, 0) != 0) return {};
    std::vector<kinfo_proc> procs(size / sizeof(kinfo_proc) + 16);
    size = procs.size() * sizeof(kinfo_proc);
    if (sysctl(mib, 3, procs.data(), &size, nullptr, 0) != 0) return {};
    std::vector<pid_t> out;
    char path[PROC_PIDPATHINFO_MAXSIZE];
    for (size_t i = 0; i < size / sizeof(kinfo_proc); ++i) {
        const auto& p = procs[i];
        if (p.kp_eproc.e_ppid != getpid() || p.kp_proc.p_stat == SZOMB) continue;
        if (proc_pidpath(p.kp_proc.p_pid, path, sizeof(path)) > 0
            && juce::File(juce::String::fromUTF8(path)).getLinkedTarget() == executable.getLinkedTarget())
            out.push_back(p.kp_proc.p_pid);
    }
    return out;
}

// Coordinator end of one worker process (JUCE AudioPluginHost's Superprocess).
class WorkerConnection final : private juce::ChildProcessCoordinator {
public:
    enum class State { timeout, gotResult, connectionLost };

    // Disconnects while the members the IPC callbacks use are still alive (the base destructor
    // would do it only after they are gone), then reaps the worker: JUCE's ChildProcess never
    // waits for it, which would leave a zombie per worker.
    ~WorkerConnection() override {
        killWorkerProcess();
        if (pid_ > 0) reap(pid_);
    }

    bool launch(const juce::File& executable) {
        const auto before = childrenRunning(executable);
        // streamFlags 0: the worker's stdout / stderr go to /dev/null, never to a pipe nobody reads.
        const bool launched = launchWorkerProcess(executable, PluginCatalog::kWorkerId, 0, 0);
        // The new child is the one that was not there before (this thread launches one at a time).
        for (pid_t pid : childrenRunning(executable))
            if (std::find(before.begin(), before.end(), pid) == before.end()) pid_ = pid;
        return launched;
    }

    using juce::ChildProcessCoordinator::sendMessageToWorker;

    // Waits up to 50 ms for the reply to the last request.
    State wait(std::unique_ptr<juce::XmlElement>& xml) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!condvar_.wait_for(lock, std::chrono::milliseconds(50), [this] { return gotResult_ || lost_; }))
            return State::timeout;
        const auto state = lost_ ? State::connectionLost : State::gotResult;
        gotResult_ = false;
        xml = std::move(reply_);
        return state;
    }

private:
    void handleMessageFromWorker(const juce::MemoryBlock& block) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        reply_ = juce::parseXML(block.toString());
        gotResult_ = true;
        condvar_.notify_one();
    }

    void handleConnectionLost() override {
        const std::lock_guard<std::mutex> lock(mutex_);
        lost_ = true;
        condvar_.notify_one();
    }

    // The worker exits by itself once disconnected (quit when idle, _Exit when busy); give it up to
    // 2 s, then kill it, and collect its exit status either way.
    static void reap(pid_t pid) {
        int status = 0;
        for (int i = 0; i < 200; ++i) {
            const pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid || (r < 0 && errno != EINTR)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }

    pid_t pid_ = 0;
    std::mutex mutex_;
    std::condition_variable condvar_;
    std::unique_ptr<juce::XmlElement> reply_;
    bool gotResult_ = false;
    bool lost_ = false;
};

} // namespace

// ---- coordinator side -------------------------------------------------------------------------

// Runs on the scan thread, inside KnownPluginList::scanAndAddFile(). Returning false makes the
// list blacklist the file.
class PluginCatalog::Scanner final : public juce::KnownPluginList::CustomScanner {
public:
    explicit Scanner(PluginCatalog& owner) : owner_(owner) {}

    bool findPluginTypesFor(juce::AudioPluginFormat& format, juce::OwnedArray<juce::PluginDescription>& result,
                            const juce::String& identifier) override {
        if (owner_.stop_ || workerUnavailable) return true;

        if (worker_ == nullptr) {
            worker_ = std::make_unique<WorkerConnection>();
            if (!worker_->launch(owner_.workerExecutable())) {
                // Not the plugin's fault: no blacklisting, and the scan stops.
                worker_.reset();
                workerUnavailable = true;
                return true;
            }
        }

        const ScanRequest request{format.getName(), identifier, owner_.options_.scanTimeoutSeconds};
        if (!worker_->sendMessageToWorker(request.toBlock())) {
            worker_.reset();
            return false;
        }

        for (;;) {
            if (owner_.stop_) return true;   // shutting down: the worker is killed by scanFinished()
            std::unique_ptr<juce::XmlElement> xml;
            const auto state = worker_->wait(xml);
            if (state == WorkerConnection::State::timeout) continue;
            if (state == WorkerConnection::State::connectionLost) {
                // The plugin crashed the worker (or hung it until its watchdog fired).
                worker_.reset();
                return false;
            }
            if (xml != nullptr)
                for (const auto* item : xml->getChildIterator()) {
                    auto description = std::make_unique<juce::PluginDescription>();
                    if (description->loadFromXml(*item)) result.add(std::move(description));
                }
            return true;
        }
    }

    void scanFinished() override { worker_.reset(); }

    std::atomic<bool> workerUnavailable{false};

private:
    PluginCatalog& owner_;
    std::unique_ptr<WorkerConnection> worker_;
};

PluginCatalog::PluginCatalog(juce::File cacheFile) : PluginCatalog(std::move(cacheFile), Options{}) {}

PluginCatalog::PluginCatalog(juce::File cacheFile, Options options)
    : cacheFile_(std::move(cacheFile)), options_(std::move(options)) {
    addFormats(formats_);
    if (auto xml = juce::parseXML(cacheFile_)) list_.recreateFromXml(*xml);
    blacklist_ = list_.getBlacklistedFiles();
    auto scanner = std::make_unique<Scanner>(*this);
    scanner_ = scanner.get();
    list_.setCustomScanner(std::move(scanner));
}

PluginCatalog::~PluginCatalog() {
    *alive_ = false;
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

juce::File PluginCatalog::defaultCacheLocation() {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Application Support")
        .getChildFile("fixomic")
        .getChildFile("plugins.xml");
}

juce::File PluginCatalog::workerExecutable() const {
    return options_.workerExecutable != juce::File()
               ? options_.workerExecutable
               : juce::File::getSpecialLocation(juce::File::currentExecutableFile);
}

void PluginCatalog::scanAsync(std::function<void()> onDone) {
    if (onDone) onDone_.push_back(std::move(onDone));
    if (scanning_) return;
    if (thread_.joinable()) thread_.join();
    scanning_ = true;
    thread_ = std::thread([this, alive = alive_] {
        runScan();
        juce::MessageManager::callAsync([this, alive] {
            if (*alive) scanFinished();
        });
    });
}

bool PluginCatalog::isScanning() const { return scanning_; }

void PluginCatalog::runScan() {
    scanner_->workerUnavailable = false;
    for (auto* format : formats_.getFormats()) {
        const bool audioUnit = format->getName() == "AudioUnit";
        if (stop_ || (audioUnit && !options_.scanAudioUnits)) continue;

        for (const auto& known : list_.getTypesForFormat(*format))
            if (!format->doesPluginStillExist(known)) list_.removeType(known);

        juce::FileSearchPath path;
        if (!audioUnit)
            path = options_.vst3SearchPath.getNumPaths() > 0 ? options_.vst3SearchPath
                                                             : format->getDefaultLocationsToSearch();
        juce::StringArray identifiers;
        for (const auto& id : format->searchPathsForPlugins(path, true, false))
            if (!audioUnit || isAudioUnitEffect(id)) identifiers.add(id);
        identifiers.sortNatural();

        for (const auto& identifier : identifiers) {
            if (stop_ || scanner_->workerUnavailable) break;
            if (list_.getBlacklistedFiles().contains(identifier) || list_.isListingUpToDate(identifier, *format))
                continue;
            juce::OwnedArray<juce::PluginDescription> found;
            list_.scanAndAddFile(identifier, true, found, *format);
            // KnownPluginList's blacklist is not locked: publish a copy (only this thread mutates it).
            const std::lock_guard<std::mutex> lock(blacklistMutex_);
            blacklist_ = list_.getBlacklistedFiles();
        }
        saveCache();
    }
    list_.scanFinished();   // kills the worker
    if (scanner_->workerUnavailable)
        DBG("PluginCatalog: could not launch the scan worker " << workerExecutable().getFullPathName());
}

void PluginCatalog::scanFinished() {
    if (thread_.joinable()) thread_.join();
    scanning_ = false;
    auto callbacks = std::move(onDone_);
    onDone_.clear();
    for (auto& callback : callbacks) callback();
}

void PluginCatalog::saveCache() const {
    const auto xml = list_.createXml();
    if (xml == nullptr || !cacheFile_.getParentDirectory().createDirectory()) return;
    juce::TemporaryFile temp(cacheFile_);
    if (xml->writeTo(temp.getFile())) temp.overwriteTargetFileWithTemporary();
}

std::vector<juce::PluginDescription> PluginCatalog::effects() const {
    auto types = list_.getTypes();
    std::vector<juce::PluginDescription> out;
    for (const auto& d : types)
        if (!d.isInstrument && d.numInputChannels > 0) out.push_back(d);
    std::sort(out.begin(), out.end(), [](const juce::PluginDescription& a, const juce::PluginDescription& b) {
        if (const int c = a.name.compareIgnoreCase(b.name); c != 0) return c < 0;
        return a.pluginFormatName < b.pluginFormatName;
    });
    return out;
}

juce::StringArray PluginCatalog::blacklisted() const {
    const std::lock_guard<std::mutex> lock(blacklistMutex_);
    return blacklist_;
}

PluginFactory PluginCatalog::factory() {
    return [this](const juce::PluginDescription& description, double sampleRate, int blockSize,
                  juce::String& error) -> std::unique_ptr<juce::AudioPluginInstance> {
        // `error` is only technical detail for the caller to show in parentheses; it may stay empty.
        return formats_.createPluginInstance(description, sampleRate > 0.0 ? sampleRate : 48000.0,
                                             blockSize > 0 ? blockSize : 128, error);
    };
}

// ---- worker side ------------------------------------------------------------------------------

namespace {

// A plugin that crashes the worker must not bring up a crash report: end the process at once
// (async-signal-safe), the coordinator sees the connection drop.
void exitOnFatalSignal(int signal) { _exit(128 + signal); }

void installFatalSignalHandlers() {
    for (int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP}) std::signal(signal, exitOnFatalSignal);
}

} // namespace

struct PluginScanWorker::Impl final : private juce::AsyncUpdater {
    explicit Impl(Override o) : override_(std::move(o)) {
        addFormats(formats_);
        watchdog_ = std::thread([this] { watch(); });
    }

    ~Impl() override {
        connection.reset();   // no more requests from the IPC thread
        cancelPendingUpdate();
        {
            const std::lock_guard<std::mutex> lock(watchMutex_);
            quitWatchdog_ = true;
        }
        watchCondvar_.notify_one();
        watchdog_.join();
    }

    // IPC thread: scans here only for formats that need the message thread unblocked; everything
    // else (AU v2, VST3) is queued for the message thread.
    void received(const juce::MemoryBlock& block) {
        auto request = ScanRequest::fromBlock(block);
        if (!request) return;
        auto* format = findFormat(formats_, request->formatName);
        juce::PluginDescription probe;
        probe.fileOrIdentifier = request->identifier;
        if (format != nullptr && format->requiresUnblockedMessageThreadDuringCreation(probe)) {
            reply(scan(*request));
            return;
        }
        {
            const std::lock_guard<std::mutex> lock(queueMutex_);
            pending_.push(std::move(*request));
        }
        triggerAsyncUpdate();
    }

    void connectionLost() {
        // A scan stuck on the message thread would never see a quit message.
        if (busy_) std::_Exit(0);
        juce::JUCEApplicationBase::quit();
    }

    struct Connection final : juce::ChildProcessWorker {
        explicit Connection(Impl& o) : owner(o) {}
        void handleMessageFromCoordinator(const juce::MemoryBlock& block) override { owner.received(block); }
        void handleConnectionLost() override { owner.connectionLost(); }
        Impl& owner;
    };
    std::unique_ptr<Connection> connection;   // created after the rest of Impl, destroyed first

private:
    void handleAsyncUpdate() override {
        for (;;) {
            ScanRequest request;
            {
                const std::lock_guard<std::mutex> lock(queueMutex_);
                if (pending_.empty()) return;
                request = std::move(pending_.front());
                pending_.pop();
            }
            reply(scan(request));
        }
    }

    juce::OwnedArray<juce::PluginDescription> scan(const ScanRequest& request) {
        juce::OwnedArray<juce::PluginDescription> results;
        auto* format = findFormat(formats_, request.formatName);
        if (format == nullptr) return results;

        arm(request.timeoutSeconds);
        if (!override_ || !override_(request.formatName, request.identifier, results))
            format->findAllTypesForFile(results, request.identifier);
        disarm();
        return results;
    }

    void reply(const juce::OwnedArray<juce::PluginDescription>& results) {
        juce::XmlElement xml("LIST");
        for (const auto* d : results) xml.addChildElement(d->createXml().release());
        const auto text = xml.toString();
        if (connection != nullptr) connection->sendMessageToCoordinator({text.toRawUTF8(), text.getNumBytesAsUTF8()});
    }

    // Watchdog: a scan running past its timeout ends the process (the coordinator blacklists it).
    void arm(int timeoutSeconds) {
        {
            const std::lock_guard<std::mutex> lock(watchMutex_);
            deadline_ = timeoutSeconds > 0
                            ? std::optional(std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds))
                            : std::nullopt;
        }
        busy_ = true;
        watchCondvar_.notify_one();
    }

    void disarm() {
        busy_ = false;
        {
            const std::lock_guard<std::mutex> lock(watchMutex_);
            deadline_.reset();
        }
        watchCondvar_.notify_one();
    }

    void watch() {
        std::unique_lock<std::mutex> lock(watchMutex_);
        while (!quitWatchdog_) {
            if (deadline_) {
                if (std::chrono::steady_clock::now() >= *deadline_) std::_Exit(3);
                watchCondvar_.wait_until(lock, *deadline_);
            } else {
                watchCondvar_.wait(lock);
            }
        }
    }

    Override override_;
    juce::AudioPluginFormatManager formats_;
    std::mutex queueMutex_;
    std::queue<ScanRequest> pending_;
    std::atomic<bool> busy_{false};

    std::mutex watchMutex_;
    std::condition_variable watchCondvar_;
    std::optional<std::chrono::steady_clock::time_point> deadline_;
    bool quitWatchdog_ = false;
    std::thread watchdog_;
};

bool PluginScanWorker::isWorkerCommandLine(const juce::String& commandLine) {
    return commandLine.trim().startsWith("--" + juce::String(PluginCatalog::kWorkerId) + ":");
}

std::unique_ptr<PluginScanWorker> PluginScanWorker::connect(const juce::String& commandLine, Override override) {
    if (!isWorkerCommandLine(commandLine)) return nullptr;
    installFatalSignalHandlers();
    auto impl = std::make_unique<Impl>(std::move(override));
    impl->connection = std::make_unique<Impl::Connection>(*impl);
    if (!impl->connection->initialiseFromCommandLine(commandLine, PluginCatalog::kWorkerId)) return nullptr;
    return std::unique_ptr<PluginScanWorker>(new PluginScanWorker(std::move(impl)));
}

PluginScanWorker::PluginScanWorker(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PluginScanWorker::~PluginScanWorker() = default;

} // namespace fixomic
