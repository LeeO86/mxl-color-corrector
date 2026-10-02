#include "http.hpp"

#include "util/sha1.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace cc
{
namespace
{
std::string base64(std::uint8_t const* data, std::size_t len)
{
    static char const* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (std::size_t i = 0; i < len; i += 3)
    {
        unsigned n = data[i] << 16;
        if (i + 1 < len) n |= data[i + 1] << 8;
        if (i + 2 < len) n |= data[i + 2];
        out.push_back(table[(n >> 18) & 63]);
        out.push_back(table[(n >> 12) & 63]);
        out.push_back(i + 1 < len ? table[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < len ? table[n & 63] : '=');
    }
    return out;
}

std::string wsFrame(std::string const& payload, unsigned opcode = 0x1)
{
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | opcode));
    if (payload.size() < 126)
    {
        frame.push_back(static_cast<char>(payload.size()));
    }
    else if (payload.size() <= 65535)
    {
        frame.push_back(126);
        frame.push_back(static_cast<char>((payload.size() >> 8) & 0xff));
        frame.push_back(static_cast<char>(payload.size() & 0xff));
    }
    else
    {
        frame.push_back(127);
        auto n = static_cast<std::uint64_t>(payload.size());
        for (int i = 7; i >= 0; --i) frame.push_back(static_cast<char>((n >> (i * 8)) & 0xff));
    }
    frame += payload;
    return frame;
}

bool writeAll(int fd, std::string const& data)
{
    std::size_t off = 0;
    while (off < data.size())
    {
        ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

std::string statusText(int status)
{
    switch (status)
    {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 101: return "Switching Protocols";
        default: return "Error";
    }
}
} // namespace

std::string HttpRequest::header(std::string const& name) const
{
    for (auto const& [key, value] : headers)
    {
        if (key.size() != name.size()) continue;
        bool match = true;
        for (std::size_t i = 0; i < key.size(); ++i)
        {
            if (std::tolower(static_cast<unsigned char>(key[i])) != std::tolower(static_cast<unsigned char>(name[i])))
            {
                match = false;
                break;
            }
        }
        if (match) return value;
    }
    return {};
}

struct HttpServer::Impl
{
    Handler handler;
    std::string wsPath;
    SocketHandler onMessage;
    std::function<std::string()> hello;
    int listenFd = -1;
    int boundPort = 0;
    std::atomic<bool> stop{false};
    std::thread acceptThread;
    std::mutex clientsMu;
    std::vector<int> clients;
    struct Job
    {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::vector<Job> jobs;
    std::mutex threadsMu;

    void reap()
    {
        std::lock_guard lock(threadsMu);
        std::vector<Job> live;
        for (auto& job : jobs)
        {
            if (job.done && job.done->load())
            {
                if (job.thread.joinable()) job.thread.join();
            }
            else
            {
                live.push_back(std::move(job));
            }
        }
        jobs.swap(live);
    }

    ~Impl()
    {
        stop.store(true);
        if (listenFd >= 0)
        {
            ::shutdown(listenFd, SHUT_RDWR);
            ::close(listenFd);
            listenFd = -1;
        }
        if (acceptThread.joinable()) acceptThread.join();
        std::vector<int> fds;
        {
            std::lock_guard lock(clientsMu);
            fds.swap(clients);
        }
        for (int fd : fds)
        {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
        std::vector<Job> local;
        {
            std::lock_guard lock(threadsMu);
            local.swap(jobs);
        }
        for (auto& job : local)
        {
            if (job.thread.joinable()) job.thread.join();
        }
    }

    void addClient(int fd)
    {
        std::lock_guard lock(clientsMu);
        clients.push_back(fd);
    }

    void dropClient(int fd)
    {
        std::lock_guard lock(clientsMu);
        clients.erase(std::remove(clients.begin(), clients.end(), fd), clients.end());
    }

    void broadcast(std::string const& text)
    {
        auto frame = wsFrame(text);
        std::vector<int> dead;
        std::lock_guard lock(clientsMu);
        for (int fd : clients)
        {
            if (!writeAll(fd, frame)) dead.push_back(fd);
        }
        for (int fd : dead)
        {
            clients.erase(std::remove(clients.begin(), clients.end(), fd), clients.end());
            ::close(fd);
        }
    }

    bool readSome(int fd, std::string& data, int timeoutMs)
    {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int rc = ::poll(&pfd, 1, timeoutMs);
        if (rc <= 0) return false;
        char buf[8192];
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        data.append(buf, buf + n);
        return true;
    }

    bool readHttp(int fd, HttpRequest& req)
    {
        std::string data;
        while (data.find("\r\n\r\n") == std::string::npos)
        {
            if (stop.load()) return false;
            if (!readSome(fd, data, 500)) return false;
            if (data.size() > 1024 * 1024) return false;
        }
        auto headerEnd = data.find("\r\n\r\n");
        std::string head = data.substr(0, headerEnd);
        std::string rest = data.substr(headerEnd + 4);
        std::size_t lineEnd = head.find("\r\n");
        std::string start = lineEnd == std::string::npos ? head : head.substr(0, lineEnd);
        std::size_t s1 = start.find(' ');
        std::size_t s2 = start.find(' ', s1 == std::string::npos ? 0 : s1 + 1);
        if (s1 == std::string::npos || s2 == std::string::npos) return false;
        req.method = start.substr(0, s1);
        auto target = start.substr(s1 + 1, s2 - s1 - 1);
        auto q = target.find('?');
        if (q == std::string::npos) req.path = target;
        else
        {
            req.path = target.substr(0, q);
            req.query = target.substr(q + 1);
        }
        std::size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
        while (pos < head.size())
        {
            auto next = head.find("\r\n", pos);
            auto line = head.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
            auto colon = line.find(':');
            if (colon != std::string::npos)
            {
                auto value = line.substr(colon + 1);
                while (!value.empty() && value.front() == ' ') value.erase(value.begin());
                req.headers.emplace_back(line.substr(0, colon), value);
            }
            if (next == std::string::npos) break;
            pos = next + 2;
        }
        std::size_t length = 0;
        auto declared = req.header("Content-Length");
        if (!declared.empty()) length = static_cast<std::size_t>(std::strtoul(declared.c_str(), nullptr, 10));
        if (length > 2 * 1024 * 1024) return false;
        while (rest.size() < length)
        {
            if (!readSome(fd, rest, 500)) return false;
        }
        req.body = rest.substr(0, length);
        return true;
    }

    void serveHttp(int fd, HttpRequest const& req)
    {
        HttpResponse res;
        try
        {
            handler(req, res);
        }
        catch (std::exception const& ex)
        {
            res.status = 500;
            res.contentType = "application/json";
            res.body = std::string("{\"error\":\"") + ex.what() + "\"}";
        }
        std::string out = "HTTP/1.1 " + std::to_string(res.status) + " " + statusText(res.status) + "\r\n";
        out += "Content-Type: " + res.contentType + "\r\n";
        out += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
        out += "Connection: close\r\n";
        out += "Access-Control-Allow-Origin: *\r\n";
        out += "Access-Control-Allow-Methods: GET, POST, PATCH, DELETE, OPTIONS\r\n";
        out += "Access-Control-Allow-Headers: Content-Type\r\n";
        for (auto const& [key, value] : res.headers) out += key + ": " + value + "\r\n";
        out += "\r\n";
        out += res.body;
        writeAll(fd, out);
    }

    void serveWebSocket(int fd, HttpRequest const& req)
    {
        auto key = req.header("Sec-WebSocket-Key");
        if (key.empty())
        {
            std::string body = "{\"error\":\"missing websocket key\"}";
            std::string raw = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                              "\r\nConnection: close\r\n\r\n" + body;
            writeAll(fd, raw);
            ::close(fd);
            return;
        }
        auto digest = sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
        std::string accept = base64(digest.data(), digest.size());
        std::string out = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept +
                          "\r\n\r\n";
        if (!writeAll(fd, out)) return;
        addClient(fd);
        if (hello)
        {
            auto greeting = hello();
            if (!greeting.empty()) writeAll(fd, wsFrame(greeting));
        }
        std::string buffer;
        while (!stop.load())
        {
            if (!readSome(fd, buffer, 200))
            {
                if (stop.load()) break;
                continue;
            }
            while (buffer.size() >= 2)
            {
                auto const* bytes = reinterpret_cast<unsigned char const*>(buffer.data());
                unsigned opcode = bytes[0] & 0x0f;
                bool masked = (bytes[1] & 0x80) != 0;
                std::uint64_t len = bytes[1] & 0x7f;
                std::size_t header = 2;
                if (len == 126)
                {
                    if (buffer.size() < 4) break;
                    len = (std::uint64_t(bytes[2]) << 8) | bytes[3];
                    header = 4;
                }
                else if (len == 127)
                {
                    if (buffer.size() < 10) break;
                    len = 0;
                    for (int i = 0; i < 8; ++i) len = (len << 8) | bytes[2 + i];
                    header = 10;
                }
                std::size_t maskLen = masked ? 4 : 0;
                if (buffer.size() < header + maskLen + len) break;
                std::string payload(len, '\0');
                for (std::uint64_t i = 0; i < len; ++i)
                {
                    unsigned char c = bytes[header + maskLen + i];
                    if (masked) c ^= bytes[header + (i % 4)];
                    payload[static_cast<std::size_t>(i)] = static_cast<char>(c);
                }
                buffer.erase(0, header + maskLen + static_cast<std::size_t>(len));
                if (opcode == 0x8)
                {
                    dropClient(fd);
                    ::close(fd);
                    return;
                }
                if (opcode == 0x9)
                {
                    writeAll(fd, wsFrame(payload, 0xA));
                }
                else if (opcode == 0x1 && onMessage)
                {
                    onMessage(payload);
                }
            }
        }
        dropClient(fd);
        ::close(fd);
    }

    void connection(int fd)
    {
        HttpRequest req;
        if (!readHttp(fd, req))
        {
            ::close(fd);
            return;
        }
        if (req.method == "GET" && !wsPath.empty() && req.path == wsPath && req.header("Upgrade") == "websocket")
        {
            serveWebSocket(fd, req);
            return;
        }
        serveHttp(fd, req);
        ::close(fd);
    }

    void acceptLoop()
    {
        while (!stop.load())
        {
            reap();
            pollfd pfd{};
            pfd.fd = listenFd;
            pfd.events = POLLIN;
            int rc = ::poll(&pfd, 1, 200);
            if (rc <= 0) continue;
            sockaddr_storage addr{};
            socklen_t len = sizeof(addr);
            int fd = ::accept(listenFd, reinterpret_cast<sockaddr*>(&addr), &len);
            if (fd < 0) continue;
            auto done = std::make_shared<std::atomic<bool>>(false);
            std::lock_guard lock(threadsMu);
            Job job;
            job.done = done;
            job.thread = std::thread([this, fd, done] {
                connection(fd);
                done->store(true);
            });
            jobs.push_back(std::move(job));
        }
    }
};

HttpServer::HttpServer(Handler handler)
    : impl_(new Impl)
{
    impl_->handler = std::move(handler);
}

HttpServer::~HttpServer()
{
    delete impl_;
}

void HttpServer::setWebSocket(std::string path, SocketHandler onMessage, std::function<std::string()> hello)
{
    impl_->wsPath = std::move(path);
    impl_->onMessage = std::move(onMessage);
    impl_->hello = std::move(hello);
}

bool HttpServer::start(int port, std::string& error)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        error = "socket failed";
        return false;
    }
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        error = "bind failed on port " + std::to_string(port);
        ::close(fd);
        return false;
    }
    if (::listen(fd, 64) != 0)
    {
        error = "listen failed";
        ::close(fd);
        return false;
    }
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len);
    impl_->listenFd = fd;
    impl_->boundPort = ntohs(bound.sin_port);
    impl_->stop.store(false);
    impl_->acceptThread = std::thread([this] { impl_->acceptLoop(); });
    return true;
}

void HttpServer::stop()
{
    if (impl_ == nullptr) return;
    impl_->stop.store(true);
    if (impl_->listenFd >= 0)
    {
        ::shutdown(impl_->listenFd, SHUT_RDWR);
    }
}

int HttpServer::port() const
{
    return impl_->boundPort;
}

void HttpServer::broadcast(std::string const& text)
{
    impl_->broadcast(text);
}

} // namespace cc
