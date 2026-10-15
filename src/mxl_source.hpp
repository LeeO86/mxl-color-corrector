#pragma once

#include <mxl/flow.h>
#include <mxl/mxl.h>

#include <string>
#include <sys/types.h>

namespace cc
{

// One channel's input: an MXL instance of the channel's own on the source domain, and the
// reader on it.
//
// The instance is the channel's own because MXL hands out one reader per flow id and instance
// and gives the same reader back while anyone holds it (Instance::getFlowReader in MXL 218ddaa).
// With an instance shared by the channels, a channel re-opening a re-created flow that another
// channel also reads would get the stale reader back. The instance follows the route: a route to
// another domain, or a domain directory that was removed and made again, gets a new instance.
class SourceReader
{
public:
    enum class Open
    {
        Ok,
        DomainMissing, // the domain directory is not there
        FlowMissing,   // the flow's data file is not there (yet)
        Error,         // the instance or the reader could not be created
    };

    SourceReader() = default;
    ~SourceReader();
    SourceReader(SourceReader const&) = delete;
    SourceReader& operator=(SourceReader const&) = delete;

    // Releases the current reader and opens the flow again; always a new reader.
    Open open(std::string const& domainDir, std::string const& flowId);
    // Releases the reader and keeps the instance.
    void release();
    // Releases the reader and the instance.
    void close();

    [[nodiscard]] mxlFlowReader reader() const { return reader_; }
    [[nodiscard]] mxlInstance instance() const { return instance_; }

    // False once the open reader's flow was deleted or re-created under the same id (its data
    // file is gone or has another inode). With a head index MXL decides
    // (MXL_ERR_FLOW_INVALID on a read past the head); before the first grain the data file's
    // inode is compared with the one seen at open.
    [[nodiscard]] bool valid() const;

private:
    std::string dir_;
    ino_t dirInode_ = 0;
    dev_t dirDevice_ = 0;
    mxlInstance instance_ = nullptr;
    mxlFlowReader reader_ = nullptr;
    std::string dataPath_;
    ino_t dataInode_ = 0;
};

// What says whether a flow can be opened: the inodes of the domain's domain_def.json, the flow
// directory and its data file (0 when missing). MXL publishes a new flow directory with a rename,
// so a flow that is back shows as a new directory inode. Three stat calls: cheap enough to look
// every 20 ms while a flow is missing.
struct FlowFiles
{
    ino_t domainDef = 0;
    ino_t dir = 0;
    ino_t data = 0;
    [[nodiscard]] bool present() const { return domainDef != 0 && data != 0; }
    bool operator==(FlowFiles const&) const = default;
};
[[nodiscard]] FlowFiles flowFiles(std::string const& domainDir, std::string const& flowId);

// When to try again to open a flow that was deleted or re-created: at once, then after 100, 200
// and 500 ms, then every second, counted from the first attempt's failure.
class ReopenSchedule
{
public:
    void start(double now);
    void reset();
    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] bool due(double now) const { return active_ && now >= next_; }
    [[nodiscard]] double next() const { return next_; }
    // Records a failed attempt and returns the delay to the next one in seconds.
    double failed(double now);
    [[nodiscard]] int attempts() const { return attempts_; }
    [[nodiscard]] double startedAt() const { return started_; }

    // Delay after the n-th failed attempt (n from 1): 0.1, 0.2, 0.5, then 1 s.
    [[nodiscard]] static double delayAfter(int failures);

private:
    bool active_ = false;
    int attempts_ = 0;
    double started_ = 0;
    double next_ = 0;
};

} // namespace cc
