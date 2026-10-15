#include "mxl_source.hpp"

#include <filesystem>
#include <sys/stat.h>

namespace cc
{

SourceReader::~SourceReader()
{
    close();
}

void SourceReader::release()
{
    if (reader_ && instance_) mxlReleaseFlowReader(instance_, reader_);
    reader_ = nullptr;
    dataPath_.clear();
    dataInode_ = 0;
}

void SourceReader::close()
{
    release();
    if (instance_) mxlDestroyInstance(instance_);
    instance_ = nullptr;
    dir_.clear();
    dirInode_ = 0;
    dirDevice_ = 0;
}

SourceReader::Open SourceReader::open(std::string const& domainDir, std::string const& flowId)
{
    release();
    struct stat dirStat{};
    if (::stat(domainDir.c_str(), &dirStat) != 0 || !S_ISDIR(dirStat.st_mode))
    {
        close();
        return Open::DomainMissing;
    }
    if (instance_ && (dir_ != domainDir || dirStat.st_ino != dirInode_ || dirStat.st_dev != dirDevice_)) close();
    if (instance_ == nullptr)
    {
        instance_ = mxlCreateInstance(domainDir.c_str(), nullptr);
        if (instance_ == nullptr) return Open::Error;
        dir_ = domainDir;
        dirInode_ = dirStat.st_ino;
        dirDevice_ = dirStat.st_dev;
    }
    auto const dataPath = (std::filesystem::path(domainDir) / (flowId + ".mxl-flow") / "data").string();
    struct stat before{};
    if (::stat(dataPath.c_str(), &before) != 0) return Open::FlowMissing;
    mxlFlowReader reader = nullptr;
    auto const status = mxlCreateFlowReader(instance_, flowId.c_str(), nullptr, &reader);
    if (status != MXL_STATUS_OK || reader == nullptr)
    {
        return status == MXL_ERR_FLOW_NOT_FOUND || status == MXL_ERR_NOT_FOUND ? Open::FlowMissing : Open::Error;
    }
    reader_ = reader;
    dataPath_ = dataPath;
    // A flow re-created between the stat and the open leaves the inodes different; 0 then makes
    // valid() report the reader stale until it has a head index, when MXL decides.
    struct stat after{};
    dataInode_ = ::stat(dataPath.c_str(), &after) == 0 && after.st_ino == before.st_ino ? after.st_ino : 0;
    return Open::Ok;
}

bool SourceReader::valid() const
{
    if (reader_ == nullptr) return false;
    mxlFlowRuntimeInfo runtime{};
    if (mxlFlowReaderGetRuntimeInfo(reader_, &runtime) == MXL_STATUS_OK && runtime.headIndex != MXL_UNDEFINED_INDEX)
    {
        // One past the head is always too early; MXL then checks the data file's inode.
        mxlGrainInfo grain{};
        std::uint8_t* payload = nullptr;
        return mxlFlowReaderGetGrainNonBlocking(reader_, runtime.headIndex + 1, &grain, &payload) != MXL_ERR_FLOW_INVALID;
    }
    struct stat now{};
    return dataInode_ != 0 && ::stat(dataPath_.c_str(), &now) == 0 && now.st_ino == dataInode_;
}

FlowFiles flowFiles(std::string const& domainDir, std::string const& flowId)
{
    auto inode = [](std::string const& path) -> ino_t {
        struct stat st{};
        return ::stat(path.c_str(), &st) == 0 ? st.st_ino : 0;
    };
    FlowFiles files;
    if (domainDir.empty()) return files;
    auto const flowDir = domainDir + "/" + flowId + ".mxl-flow";
    files.domainDef = inode(domainDir + "/domain_def.json");
    files.dir = inode(flowDir);
    files.data = inode(flowDir + "/data");
    return files;
}

double ReopenSchedule::delayAfter(int failures)
{
    if (failures <= 1) return 0.1;
    if (failures == 2) return 0.2;
    if (failures == 3) return 0.5;
    return 1.0;
}

void ReopenSchedule::start(double now)
{
    active_ = true;
    attempts_ = 0;
    started_ = now;
    next_ = now;
}

void ReopenSchedule::reset()
{
    active_ = false;
    attempts_ = 0;
    started_ = 0;
    next_ = 0;
}

double ReopenSchedule::failed(double now)
{
    ++attempts_;
    auto const delay = delayAfter(attempts_);
    next_ = now + delay;
    return delay;
}

} // namespace cc
