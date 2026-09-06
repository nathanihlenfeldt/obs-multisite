#pragma once
//
// auth.h — who is allowed to change what a church broadcasts.
//
// The relay started life shaped like the campus appliance, which sits on a
// church LAN and is guarded by the building's own network. This is not that:
// it runs on a VPS with a port open, and anyone who can reach that port can
// point a service at their own server. So it needs a login, and the login is
// not optional.
//
// Deliberately small: one operator, one password. A church has one person
// doing this, and per-user accounts would mean account management, password
// resets and a story about who owns them — all cost, no safety gained here.
//
// Passwords are stored as PBKDF2-HMAC-SHA256 over a random salt, so the
// database is not a list of passwords even though it sits on a volume the
// church's host provider can read. Sessions live in memory only: a restart
// signs everyone out, which is the right default for something that is
// usually left open in a tab.
//
#include "config_store.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace multisite_relay {

class Auth {
public:
    explicit Auth(ConfigStore& cfg) : m_cfg(cfg) {}

    // Whether anyone has set a login yet. Until they have, the interface shows
    // nothing but the form that sets one — an unclaimed relay is exactly as
    // dangerous as an unprotected one.
    bool configured() const;

    // Sets (or replaces) the single operator login. Returns a message for the
    // person reading it, or empty on success.
    std::string set_credentials(const std::string& user,
                                const std::string& password);

    // True if these are right. Takes the same time either way.
    bool verify(const std::string& user, const std::string& password) const;

    // ── Sessions ─────────────────────────────────────────────────────────────
    std::string create_session();
    bool        valid_session(const std::string& token);
    void        destroy_session(const std::string& token);

    // The value for a Set-Cookie header. `secure` adds the Secure attribute,
    // which a browser will then refuse to send back over plain HTTP — correct
    // behind TLS, and quietly fatal without it, which is why it is decided by
    // the caller from the actual request rather than assumed.
    static std::string cookie_for(const std::string& token, bool secure);
    static std::string clear_cookie();

    // Pulls our cookie out of a Cookie header.
    static std::string session_from_cookies(const std::string& header);

private:
    ConfigStore& m_cfg;
    mutable std::mutex m_mtx;
    std::map<std::string, int64_t> m_sessions;   // token -> expiry (ms)
};

// Whether this request arrived over a connection that protects a password.
// True behind a TLS-terminating proxy that sets X-Forwarded-Proto, and for a
// connection to localhost — which is what an SSH tunnel looks like from here.
// Anything else is reported to the operator rather than refused: locking
// someone out of their own relay mid-service would be the worse failure.
bool connection_is_private(const std::map<std::string, std::string>& headers);

} // namespace multisite_relay
