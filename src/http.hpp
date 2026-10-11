#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace cc
{

struct HttpRequest
{
    std::string method;
    std::string path;
    std::string query;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    [[nodiscard]] std::string header(std::string const& name) const;
};

struct HttpResponse
{
    int status = 200;
    std::string contentType = "application/json; charset=utf-8";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    // The API's CORS headers (any origin). The widget routes answer with their own.
    bool cors = true;
};

class HttpServer
{
public:
    using Handler = std::function<void(HttpRequest const&, HttpResponse&)>;
    using SocketHandler = std::function<void(std::string const& message)>;

    explicit HttpServer(Handler handler);
    ~HttpServer();

    HttpServer(HttpServer const&) = delete;
    HttpServer& operator=(HttpServer const&) = delete;

    void setWebSocket(std::string path, SocketHandler onMessage, std::function<std::string()> hello);
    bool start(int port, std::string& error);
    void stop();
    [[nodiscard]] int port() const;
    void broadcast(std::string const& text);

private:
    struct Impl;
    Impl* impl_;
};

} // namespace cc
