#pragma once
//
// http_server.h — the appliance's control surface, in about as little code as
// an HTTP server can be written in.
//
// The operator's interface is a phone or tablet on the church network, so the
// box has to serve a web page and answer requests. It deliberately does NOT
// pull in a web framework: this thing has to build with one command on a
// stock Raspberry Pi OS and keep working for years without anyone updating a
// dependency tree, so it speaks the small part of HTTP/1.1 it actually needs
// and nothing else.
//
// What it supports: GET/POST/PUT, query strings, a request body, keep-alive,
// static files, and streamed responses (which is how the preview works).
// What it does not: TLS, chunked request bodies, compression, or anything
// facing the public internet. This is a LAN appliance, not a web server.
//
#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace multisite_player {

struct HttpRequest {
    std::string method;
    std::string path;                            // no query string
    std::map<std::string, std::string> query;
    std::map<std::string, std::string> headers;  // keys lowercased
    std::string body;

    std::string param(const std::string& name,
                      const std::string& fallback = "") const {
        auto it = query.find(name);
        return it == query.end() ? fallback : it->second;
    }
};

// A live connection, for responses that are produced over time rather than
// all at once — the MJPEG preview being the reason this exists.
class HttpStream {
public:
    explicit HttpStream(int fd) : m_fd(fd) {}
    // Returns false once the client has gone away, which is the signal to stop
    // producing. A browser closing a preview tab must not leave a thread
    // encoding frames forever.
    bool write(const void* data, size_t len);
    bool write(const std::string& s) { return write(s.data(), s.size()); }
    bool alive() const { return m_alive; }

private:
    int  m_fd;
    bool m_alive = true;
};

struct HttpResponse {
    int         status = 200;
    std::string content_type = "application/json; charset=utf-8";
    std::string body;
    std::map<std::string, std::string> headers;

    // When set, the body is ignored and this is called with the connection
    // after the headers have gone out. The connection closes when it returns.
    std::function<void(HttpStream&)> stream;

    void json(const std::string& text) {
        content_type = "application/json; charset=utf-8";
        body = text;
    }
    void text(int code, const std::string& message) {
        status = code;
        content_type = "text/plain; charset=utf-8";
        body = message;
    }
};

using HttpHandler = std::function<void(const HttpRequest&, HttpResponse&)>;

class HttpServer {
public:
    HttpServer(std::string bind_address, int port);
    ~HttpServer();

    // Exact-path routes. Registered before start().
    void route(const std::string& method, const std::string& path,
               HttpHandler handler);

    // Files served for any GET that matches no route. "/" serves index.html.
    // Served from disk rather than compiled in, so the interface can be edited
    // on a running box without a rebuild.
    void set_static_root(std::string dir) { m_static_root = std::move(dir); }

    // Binds and starts accepting. Returns false (with `error` set) if the port
    // is taken — worth reporting plainly, because the usual cause is a second
    // copy of the service already running.
    bool start(std::string& error);
    void stop();

    int port() const { return m_port; }

private:
    void accept_loop();
    void serve_connection(int fd);
    bool handle_request(int fd, const HttpRequest& req);
    bool serve_static(const HttpRequest& req, HttpResponse& res);

    std::string m_bind;
    int         m_port;
    int         m_listen_fd = -1;
    std::string m_static_root;

    std::map<std::string, HttpHandler> m_routes;   // "GET /api/status"
    std::thread m_accept_thread;
    std::atomic<bool> m_running{false};

    // Connections are handled on their own detached threads. A preview stream
    // occupies one for as long as it is open, so the cap is what stops a
    // browser that keeps reconnecting from exhausting the box.
    std::atomic<int> m_connections{0};
    static constexpr int kMaxConnections = 24;
};

// Percent-decoding and query parsing, exposed because the API layer needs the
// same rules for form bodies.
std::string url_decode(const std::string& in);
std::map<std::string, std::string> parse_query(const std::string& in);

} // namespace multisite_player
