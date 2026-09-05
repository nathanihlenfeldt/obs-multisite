#include "http_server.h"
#include "log.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace multisite_player {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

const char* status_text(int code) {
    switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    }
    return "OK";
}

const char* content_type_for(const std::string& path) {
    auto ends = [&](const char* ext) {
        const size_t n = std::strlen(ext);
        return path.size() >= n && path.compare(path.size() - n, n, ext) == 0;
    };
    if (ends(".html")) return "text/html; charset=utf-8";
    if (ends(".js"))   return "application/javascript; charset=utf-8";
    if (ends(".css"))  return "text/css; charset=utf-8";
    if (ends(".json")) return "application/json; charset=utf-8";
    if (ends(".svg"))  return "image/svg+xml";
    if (ends(".png"))  return "image/png";
    if (ends(".jpg") || ends(".jpeg")) return "image/jpeg";
    if (ends(".ico"))  return "image/x-icon";
    if (ends(".woff2")) return "font/woff2";
    return "application/octet-stream";
}

// A request body is a settings form, never an upload. Anything larger is a
// mistake or an attack, and reading it would only waste the box's memory.
constexpr size_t kMaxBody = 256 * 1024;
constexpr size_t kMaxHeaderBytes = 32 * 1024;

bool write_all(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        const ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
        if (n > 0) { p += n; len -= (size_t)n; continue; }
        if (n < 0 && (errno == EINTR)) continue;
        return false;
    }
    return true;
}

} // namespace

// ── URL helpers ──────────────────────────────────────────────────────────────

std::string url_decode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') { out.push_back(' '); continue; }
        if (in[i] == '%' && i + 2 < in.size() &&
            std::isxdigit((unsigned char)in[i + 1]) &&
            std::isxdigit((unsigned char)in[i + 2])) {
            out.push_back((char)std::stoi(in.substr(i + 1, 2), nullptr, 16));
            i += 2;
            continue;
        }
        out.push_back(in[i]);
    }
    return out;
}

std::map<std::string, std::string> parse_query(const std::string& in) {
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while (pos < in.size()) {
        size_t amp = in.find('&', pos);
        if (amp == std::string::npos) amp = in.size();
        const std::string pair = in.substr(pos, amp - pos);
        const size_t eq = pair.find('=');
        if (eq == std::string::npos) {
            if (!pair.empty()) out[url_decode(pair)] = "";
        } else {
            out[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
        }
        pos = amp + 1;
    }
    return out;
}

// ── Stream ───────────────────────────────────────────────────────────────────

bool HttpStream::write(const void* data, size_t len) {
    if (!m_alive) return false;
    if (!write_all(m_fd, data, len)) m_alive = false;
    return m_alive;
}

// ── Server ───────────────────────────────────────────────────────────────────

HttpServer::HttpServer(std::string bind_address, int port)
    : m_bind(std::move(bind_address)), m_port(port) {}

HttpServer::~HttpServer() { stop(); }

void HttpServer::route(const std::string& method, const std::string& path,
                       HttpHandler handler) {
    m_routes[method + " " + path] = std::move(handler);
}

bool HttpServer::start(std::string& error) {
    error.clear();
    m_listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (m_listen_fd < 0) { error = "socket: " + std::string(strerror(errno)); return false; }

    int on = 1;
    ::setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)m_port);
    if (m_bind.empty() || m_bind == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (::inet_pton(AF_INET, m_bind.c_str(), &addr.sin_addr) != 1) {
        error = "not a valid bind address: " + m_bind;
        ::close(m_listen_fd); m_listen_fd = -1;
        return false;
    }

    if (::bind(m_listen_fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
        error = "cannot listen on port " + std::to_string(m_port) + ": " +
                strerror(errno) +
                (errno == EADDRINUSE ? " (is the service already running?)" : "");
        ::close(m_listen_fd); m_listen_fd = -1;
        return false;
    }
    if (::listen(m_listen_fd, 16) != 0) {
        error = "listen: " + std::string(strerror(errno));
        ::close(m_listen_fd); m_listen_fd = -1;
        return false;
    }

    m_running = true;
    m_accept_thread = std::thread([this] { accept_loop(); });
    return true;
}

void HttpServer::stop() {
    if (!m_running.exchange(false)) return;
    // Shutting the listening socket down releases accept() immediately, which
    // is what lets a stop finish promptly rather than after a timeout.
    if (m_listen_fd >= 0) {
        ::shutdown(m_listen_fd, SHUT_RDWR);
        ::close(m_listen_fd);
        m_listen_fd = -1;
    }
    if (m_accept_thread.joinable()) m_accept_thread.join();
}

void HttpServer::accept_loop() {
    while (m_running.load()) {
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        const int fd = ::accept(m_listen_fd, (sockaddr*)&peer, &len);
        if (fd < 0) {
            if (!m_running.load()) break;
            if (errno == EINTR) continue;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (m_connections.load() >= kMaxConnections) {
            static const char busy[] =
                "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n"
                "Content-Length: 0\r\n\r\n";
            write_all(fd, busy, sizeof(busy) - 1);
            ::close(fd);
            continue;
        }
        m_connections++;
        std::thread([this, fd] {
            serve_connection(fd);
            ::close(fd);
            m_connections--;
        }).detach();
    }
}

void HttpServer::serve_connection(int fd) {
    int on = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    // A client that opens a connection and says nothing must not hold a thread
    // for ever.
    timeval tv{};
    tv.tv_sec = 30;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    std::string buf;
    for (;;) {
        // Read up to the end of the headers.
        size_t header_end = buf.find("\r\n\r\n");
        while (header_end == std::string::npos) {
            char chunk[4096];
            const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n <= 0) return;
            buf.append(chunk, (size_t)n);
            if (buf.size() > kMaxHeaderBytes) return;
            header_end = buf.find("\r\n\r\n");
        }

        HttpRequest req;
        {
            std::istringstream hs(buf.substr(0, header_end));
            std::string line;
            if (!std::getline(hs, line)) return;
            if (!line.empty() && line.back() == '\r') line.pop_back();

            std::istringstream rl(line);
            std::string target, version;
            rl >> req.method >> target >> version;
            if (req.method.empty() || target.empty()) return;

            const size_t q = target.find('?');
            if (q == std::string::npos) {
                req.path = url_decode(target);
            } else {
                req.path  = url_decode(target.substr(0, q));
                req.query = parse_query(target.substr(q + 1));
            }

            while (std::getline(hs, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                const size_t colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string key = lower(line.substr(0, colon));
                size_t v = colon + 1;
                while (v < line.size() && line[v] == ' ') ++v;
                req.headers[key] = line.substr(v);
            }
        }

        size_t body_len = 0;
        {
            auto it = req.headers.find("content-length");
            if (it != req.headers.end()) {
                try { body_len = (size_t)std::stoul(it->second); } catch (...) {}
            }
        }
        if (body_len > kMaxBody) {
            static const char big[] =
                "HTTP/1.1 413 Payload Too Large\r\nConnection: close\r\n"
                "Content-Length: 0\r\n\r\n";
            write_all(fd, big, sizeof(big) - 1);
            return;
        }

        const size_t body_start = header_end + 4;
        while (buf.size() < body_start + body_len) {
            char chunk[4096];
            const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n <= 0) return;
            buf.append(chunk, (size_t)n);
        }
        req.body = buf.substr(body_start, body_len);
        buf.erase(0, body_start + body_len);

        if (!handle_request(fd, req)) return;

        auto conn = req.headers.find("connection");
        if (conn != req.headers.end() && lower(conn->second) == "close") return;
    }
}

bool HttpServer::handle_request(int fd, const HttpRequest& req) {
    HttpResponse res;

    auto it = m_routes.find(req.method + " " + req.path);
    if (it != m_routes.end()) {
        try {
            it->second(req, res);
        } catch (const std::exception& e) {
            // A handler throwing must produce an error page, not kill the
            // control surface on a box nobody can reach.
            plog_error("request %s %s failed: %s", req.method.c_str(),
                       req.path.c_str(), e.what());
            res = HttpResponse{};
            res.text(500, std::string("internal error: ") + e.what());
        }
    } else if (req.method == "GET" || req.method == "HEAD") {
        if (!serve_static(req, res)) res.text(404, "not found");
    } else {
        res.text(405, "method not allowed");
    }

    std::string head;
    head.reserve(256);
    head += "HTTP/1.1 " + std::to_string(res.status) + " " +
            status_text(res.status) + "\r\n";
    head += "Content-Type: " + res.content_type + "\r\n";
    // The UI is a single page polled from a phone; caching any of it would
    // only ever show an operator a stale reading.
    if (res.headers.find("Cache-Control") == res.headers.end())
        head += "Cache-Control: no-store\r\n";
    for (const auto& h : res.headers) head += h.first + ": " + h.second + "\r\n";

    if (res.stream) {
        head += "Connection: close\r\n\r\n";
        if (!write_all(fd, head.data(), head.size())) return false;
        HttpStream stream(fd);
        try {
            res.stream(stream);
        } catch (const std::exception& e) {
            plog_warn("stream %s ended: %s", req.path.c_str(), e.what());
        }
        return false;               // streamed responses always close
    }

    head += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
    head += "Connection: keep-alive\r\n\r\n";
    if (!write_all(fd, head.data(), head.size())) return false;
    if (req.method == "HEAD") return true;
    return write_all(fd, res.body.data(), res.body.size());
}

bool HttpServer::serve_static(const HttpRequest& req, HttpResponse& res) {
    if (m_static_root.empty()) return false;

    std::string rel = req.path == "/" ? "/index.html" : req.path;
    // No traversal out of the web root. The box is on a church network, not
    // behind a hardened proxy, so this check is the only thing standing
    // between a stray request and /etc/multisite-player/config.json.
    if (rel.find("..") != std::string::npos) return false;
    if (rel.empty() || rel[0] != '/') return false;

    const std::string path = m_static_root + rel;
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;

    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();

    res.status = 200;
    res.content_type = content_type_for(path);
    res.body = ss.str();
    return true;
}

} // namespace multisite_player
