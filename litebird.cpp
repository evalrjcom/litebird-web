// LiteBird - Lightweight browser inspired by Ladybird's modular design
// Pure C++ + FLTK + libcurl. Minimal footprint for Ubuntu i386 live / v86.
//
// Ladybird ideas used (simplified):
// - Separation of networking from UI/rendering (NetworkFetcher ≈ RequestServer)
// - Thin BrowserEngine abstraction (so a stronger renderer can be swapped later)
// - Event-driven callbacks
// - Controlled loading of untrusted content with clear error paths
//
// AUDIT NOTE: every change made during the code audit is marked with a
// "// FIX:" (bug/security/edge case) or "// CHANGED:" (behaviour/efficiency)
// comment. See AUDIT.md for the full changelog.

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Group.H>      // CHANGED: toolbar group so buttons don't stretch on resize
#include <FL/Fl_Input.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Help_View.H>
#include <FL/Fl_Box.H>
#include <curl/curl.h>
#include <atomic>             // CHANGED: cancellation / completion flags for worker threads
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>             // CHANGED: network fetch now runs off the UI thread
#include <vector>
// CHANGED: <fstream>, <unistd.h> and <windows.h> are no longer needed: the page
// is handed to Fl_Help_View in memory, so the temp-file code (and its leaks,
// unchecked rename()/ofstream errors and Windows ANSI-path issues) is gone.

namespace {

// ---------------------------------------------------------------------------
// Small string helpers
// ---------------------------------------------------------------------------

// FIX: lower-casing helper; the original strncmp("http://") check missed "HTTP://" links
// and nothing else (scheme, host, content type) was compared case-insensitively.
std::string to_lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) b++;
    while (e > b && std::isspace((unsigned char)s[e - 1])) e--;
    return s.substr(b, e - b);
}

// FIX: the original error page concatenated text straight into HTML.
// Anything that can contain '<', '&' or quotes is now escaped.
std::string html_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

// FIX: FLTK treats '@' in widget labels as a symbol prefix, so URLs such as
// https://medium.com/@user rendered as garbage in the status bar. "@@" draws '@'.
std::string escape_label(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '@') out += "@@";
        else out += c;
    }
    return out;
}

void append_utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

// Windows-1252 code points for bytes 0x80..0x9F (0xFFFD = undefined).
const unsigned kCp1252[32] = {
    0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
    0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};

unsigned cp1252_to_cp(unsigned char b) {
    if (b >= 0x80 && b <= 0x9F) return kCp1252[b - 0x80];
    return b;
}

// Length of a valid UTF-8 sequence starting at s[i], or 0 if invalid.
size_t utf8_seq_len(const std::string& s, size_t i) {
    unsigned char c = (unsigned char)s[i];
    size_t n;
    unsigned cp;
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) { n = 2; cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 3; cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 4; cp = c & 0x07; }
    else return 0;
    if (i + n > s.size()) return 0;
    for (size_t k = 1; k < n; k++) {
        unsigned char cc = (unsigned char)s[i + k];
        if ((cc & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (cc & 0x3F);
    }
    if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
        (cp >= 0xD800 && cp <= 0xDFFF))
        return 0;
    return n;
}

// FIX: Fl_Help_View expects UTF-8, but the original passed the raw bytes of
// ISO-8859-1 / Windows-1252 pages through, which displayed as mojibake.
// single_byte=true: decode every byte as Windows-1252 (a superset of Latin-1).
// single_byte=false: keep valid UTF-8, repair stray invalid bytes as 1252.
std::string to_utf8(const std::string& in, bool single_byte) {
    std::string out;
    out.reserve(in.size() + in.size() / 8);
    for (size_t i = 0; i < in.size();) {
        unsigned char c = (unsigned char)in[i];
        if (c == 0) { i++; continue; }  // FIX: embedded NULs truncated the page in value(c_str())
        if (!single_byte) {
            size_t n = utf8_seq_len(in, i);
            if (n > 0) { out.append(in, i, n); i += n; continue; }
        }
        append_utf8(out, cp1252_to_cp(c));
        i++;
    }
    return out;
}

bool is_single_byte_charset(const std::string& cs) {
    return cs == "iso-8859-1" || cs == "latin1" || cs == "latin-1" || cs == "iso8859-1" ||
           cs == "windows-1252" || cs == "cp1252" || cs == "us-ascii" || cs == "ascii" ||
           cs == "iso-8859-15";
}

// Extract "charset=xxx" from a Content-Type value or a chunk of HTML.
std::string find_charset(const std::string& text_lower) {
    size_t p = text_lower.find("charset=");
    if (p == std::string::npos) return "";
    p += 8;
    while (p < text_lower.size() && (text_lower[p] == '"' || text_lower[p] == '\'' || text_lower[p] == ' ')) p++;
    size_t e = p;
    while (e < text_lower.size() &&
           (std::isalnum((unsigned char)text_lower[e]) || text_lower[e] == '-' || text_lower[e] == '_'))
        e++;
    return text_lower.substr(p, e - p);
}

// Minimal HTML entity decoder for attribute values (hrefs use &amp; a lot).
std::string decode_entities(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') { out += s[i]; continue; }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) { out += s[i]; continue; }
        std::string ent = s.substr(i + 1, semi - i - 1);
        unsigned cp = 0;
        bool ok = true;
        if (ent == "amp") cp = '&';
        else if (ent == "lt") cp = '<';
        else if (ent == "gt") cp = '>';
        else if (ent == "quot") cp = '"';
        else if (ent == "apos") cp = '\'';
        else if (ent.size() > 1 && ent[0] == '#') {
            char* end = nullptr;
            unsigned long v = (ent[1] == 'x' || ent[1] == 'X')
                                  ? std::strtoul(ent.c_str() + 2, &end, 16)
                                  : std::strtoul(ent.c_str() + 1, &end, 10);
            ok = end && *end == '\0' && v > 0 && v <= 0x10FFFF;
            cp = (unsigned)v;
        } else ok = false;
        if (!ok) { out += s[i]; continue; }
        append_utf8(out, cp);
        i = semi;
    }
    return out;
}

// ---------------------------------------------------------------------------
// URL handling (RFC 3986). Hand-written rather than curl_url() so it still
// builds against the libcurl 7.58 that ships with Ubuntu 18.04 i386 (v86 target).
// ---------------------------------------------------------------------------

struct Url {
    std::string scheme, authority, path, query, fragment;
    bool has_scheme = false, has_authority = false, has_query = false, has_fragment = false;
};

bool is_scheme_char(char c, bool first) {
    return std::isalpha((unsigned char)c) ||
           (!first && (std::isdigit((unsigned char)c) || c == '+' || c == '-' || c == '.'));
}

Url parse_url(const std::string& s) {
    Url u;
    size_t i = 0;
    // scheme
    size_t colon = s.find(':');
    if (colon != std::string::npos && colon > 0) {
        bool ok = true;
        for (size_t k = 0; k < colon; k++)
            if (!is_scheme_char(s[k], k == 0)) { ok = false; break; }
        if (ok) { u.scheme = to_lower(s.substr(0, colon)); u.has_scheme = true; i = colon + 1; }
    }
    if (s.compare(i, 2, "//") == 0) {
        i += 2;
        size_t e = s.find_first_of("/?#", i);
        if (e == std::string::npos) e = s.size();
        u.authority = s.substr(i, e - i);
        u.has_authority = true;
        i = e;
    }
    size_t e = s.find_first_of("?#", i);
    if (e == std::string::npos) e = s.size();
    u.path = s.substr(i, e - i);
    i = e;
    if (i < s.size() && s[i] == '?') {
        e = s.find('#', i);
        if (e == std::string::npos) e = s.size();
        u.query = s.substr(i + 1, e - i - 1);
        u.has_query = true;
        i = e;
    }
    if (i < s.size() && s[i] == '#') {
        u.fragment = s.substr(i + 1);
        u.has_fragment = true;
    }
    return u;
}

std::string remove_dot_segments(std::string in) {
    std::string out;
    while (!in.empty()) {
        if (in.compare(0, 3, "../") == 0) in.erase(0, 3);
        else if (in.compare(0, 2, "./") == 0) in.erase(0, 2);
        else if (in.compare(0, 3, "/./") == 0) in.replace(0, 3, "/");
        else if (in == "/.") in = "/";
        else if (in.compare(0, 4, "/../") == 0 || in == "/..") {
            in = (in == "/..") ? "/" : in.substr(3);
            size_t p = out.rfind('/');
            out.erase(p == std::string::npos ? 0 : p);
        } else if (in == "." || in == "..") in.clear();
        else {
            size_t start = (in[0] == '/') ? 1 : 0;
            size_t p = in.find('/', start);
            if (p == std::string::npos) p = in.size();
            out += in.substr(0, p);
            in.erase(0, p);
        }
    }
    return out;
}

std::string url_to_string(const Url& u) {
    std::string s;
    if (u.has_scheme) s += u.scheme + ":";
    if (u.has_authority) s += "//" + u.authority;
    s += u.path;
    if (u.has_query) s += "?" + u.query;
    if (u.has_fragment) s += "#" + u.fragment;
    return s;
}

// FIX: relative links ("about.html", "/docs", "../x", "?page=2") were resolved
// by Fl_Help_View against the *temp file's directory* (e.g. /tmp/about.html),
// so they either broke or read local files. They are now resolved properly
// against the page's real (post-redirect) URL.
std::string resolve_url(const std::string& base_s, const std::string& ref_s) {
    Url base = parse_url(base_s), ref = parse_url(ref_s), t;
    if (ref.has_scheme) {
        t = ref;
        t.path = remove_dot_segments(ref.path);
    } else {
        t.scheme = base.scheme; t.has_scheme = base.has_scheme;
        if (ref.has_authority) {
            t.authority = ref.authority; t.has_authority = true;
            t.path = remove_dot_segments(ref.path);
            t.query = ref.query; t.has_query = ref.has_query;
        } else {
            t.authority = base.authority; t.has_authority = base.has_authority;
            if (ref.path.empty()) {
                t.path = base.path;
                if (ref.has_query) { t.query = ref.query; t.has_query = true; }
                else { t.query = base.query; t.has_query = base.has_query; }
            } else {
                if (ref.path[0] == '/') t.path = remove_dot_segments(ref.path);
                else {
                    std::string merged;
                    if (base.has_authority && base.path.empty()) merged = "/" + ref.path;
                    else {
                        size_t p = base.path.rfind('/');
                        merged = (p == std::string::npos ? "" : base.path.substr(0, p + 1)) + ref.path;
                    }
                    t.path = remove_dot_segments(merged);
                }
                t.query = ref.query; t.has_query = ref.has_query;
            }
        }
    }
    t.fragment = ref.fragment; t.has_fragment = ref.has_fragment;
    if (t.has_authority && t.path.empty()) t.path = "/";
    return url_to_string(t);
}

// FIX: spaces / non-ASCII bytes typed into the URL bar made curl fail with
// "URL using bad/illegal format". Unsafe bytes are now percent-encoded.
std::string percent_encode_unsafe(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (c <= 0x20 || c >= 0x7F || std::strchr("\"<>\\^`{|}", c)) {
            out += '%'; out += hex[c >> 4]; out += hex[c & 15];
        } else out += (char)c;
    }
    return out;
}

bool is_http_scheme(const std::string& url) {
    Url u = parse_url(url);
    return u.has_scheme && (u.scheme == "http" || u.scheme == "https");
}

// FIX: the original only checked url.find("://"), so "example.com/?u=http://x"
// was sent without a scheme, file:// / gopher:// / dict:// etc. were passed
// straight to curl, and an empty box became "https://".
// Returns "" and sets err for invalid input.
std::string normalize_user_url(const std::string& raw, std::string& err) {
    std::string s = trim(raw);
    if (s.empty()) { err = "Please enter an address."; return ""; }
    Url probe = parse_url(s);
    // "localhost:8080" parses as scheme "localhost"; a colon followed by a
    // digit is a port, anything else ("mailto:", "javascript:") is a scheme.
    size_t colon = s.find(':');
    bool host_port = probe.has_scheme && !probe.has_authority && colon + 1 < s.size() &&
                     std::isdigit((unsigned char)s[colon + 1]);
    if (!probe.has_scheme || host_port) s = "https://" + s;
    else if (!probe.has_authority) {
        err = "Unsupported address \"" + s + "\" (only http and https are allowed).";
        return "";
    }
    s = percent_encode_unsafe(s);
    Url u = parse_url(s);
    if (u.scheme != "http" && u.scheme != "https") {
        err = "Unsupported scheme \"" + u.scheme + "\" (only http and https are allowed).";
        return "";
    }
    if (u.authority.empty()) { err = "The address has no host name."; return ""; }
    // Lower-case the host part (keep userinfo / port as typed).
    size_t at = u.authority.rfind('@');
    size_t hs = (at == std::string::npos) ? 0 : at + 1;
    u.authority = u.authority.substr(0, hs) + to_lower(u.authority.substr(hs));
    if (u.path.empty()) u.path = "/";
    u.path = remove_dot_segments(u.path);
    return url_to_string(u);
}

std::string strip_fragment(const std::string& url, std::string* frag) {
    size_t p = url.find('#');
    if (frag) *frag = (p == std::string::npos) ? "" : url.substr(p + 1);
    return p == std::string::npos ? url : url.substr(0, p);
}

// ---------------------------------------------------------------------------
// HTML pre-processing
// ---------------------------------------------------------------------------

bool ieq_prefix(const std::string& s, size_t pos, const char* lit) {
    size_t n = std::strlen(lit);
    if (pos + n > s.size()) return false;
    for (size_t k = 0; k < n; k++)
        if (std::tolower((unsigned char)s[pos + k]) != lit[k]) return false;
    return true;
}

size_t find_tag_end(const std::string& h, size_t p) {  // p points at '<'
    char q = 0;
    for (size_t i = p + 1; i < h.size(); i++) {
        char c = h[i];
        if (q) { if (c == q) q = 0; }
        else if (c == '"' || c == '\'') q = c;
        else if (c == '>') return i;
    }
    return std::string::npos;
}

struct AttrSpan { size_t start = 0, end = 0; std::string value; bool found = false; };

// Locate attribute `name` inside tag text `tag` (which starts with '<').
AttrSpan find_attr(const std::string& tag, const char* name) {
    AttrSpan a;
    size_t i = 1;
    while (i < tag.size() && !std::isspace((unsigned char)tag[i]) && tag[i] != '>') i++;  // tag name
    while (i < tag.size()) {
        while (i < tag.size() && (std::isspace((unsigned char)tag[i]) || tag[i] == '/')) i++;
        if (i >= tag.size() || tag[i] == '>') break;
        size_t ns = i;
        while (i < tag.size() && !std::isspace((unsigned char)tag[i]) && tag[i] != '=' && tag[i] != '>') i++;
        std::string an = to_lower(tag.substr(ns, i - ns));
        while (i < tag.size() && std::isspace((unsigned char)tag[i])) i++;
        if (i >= tag.size() || tag[i] != '=') { if (an == name) { a.found = true; a.start = a.end = i; } continue; }
        i++;
        while (i < tag.size() && std::isspace((unsigned char)tag[i])) i++;
        size_t vs = i, ve;
        std::string val;
        if (i < tag.size() && (tag[i] == '"' || tag[i] == '\'')) {
            char q = tag[i];
            size_t close = tag.find(q, i + 1);
            if (close == std::string::npos) close = tag.size() - 1;
            val = tag.substr(i + 1, close - i - 1);
            ve = close + 1;
        } else {
            while (i < tag.size() && !std::isspace((unsigned char)tag[i]) && tag[i] != '>') i++;
            val = tag.substr(vs, i - vs);
            ve = i;
        }
        i = ve;
        if (an == name) { a.found = true; a.start = vs; a.end = ve; a.value = val; return a; }
    }
    return a;
}

// Removes control chars that HTML parsers strip from URLs (tab/CR/LF).
std::string clean_href(const std::string& v) {
    std::string out;
    for (char c : trim(decode_entities(v)))
        if (c != '\t' && c != '\n' && c != '\r') out += c;
    return out;
}

// FIX (critical): Fl_Help_View::load()/follow_link() hand any absolute
// http(s) link to fl_open_uri(), i.e. clicking a normal link opened the
// *system* browser instead of LiteBird, and the link callback was also called
// for every <img src> on every re-layout, so images triggered navigations.
// Every <a href> is now rewritten to an opaque per-session token
// ("lb<random>:<index>") that FLTK cannot open itself; the real, fully
// resolved URL is kept in `links` and only followed on a genuine click.
// CHANGED: <script>/<style> bodies are dropped (Fl_Help_View rendered them as text).
std::string prepare_html(const std::string& html, const std::string& doc_url,
                         const std::string& token, std::vector<std::string>& links) {
    std::string out;
    out.reserve(html.size() + 1024);
    std::string base = doc_url;
    size_t i = 0;
    while (i < html.size()) {
        size_t lt = html.find('<', i);
        if (lt == std::string::npos) { out.append(html, i, std::string::npos); break; }
        out.append(html, i, lt - i);
        if (html.compare(lt, 4, "<!--") == 0) {
            size_t ce = html.find("-->", lt + 4);
            i = (ce == std::string::npos) ? html.size() : ce + 3;
            continue;  // comments are dropped (they may contain fake tags)
        }
        size_t gt = find_tag_end(html, lt);
        if (gt == std::string::npos) { out.append(html, lt, std::string::npos); break; }
        std::string tag = html.substr(lt, gt - lt + 1);
        if (ieq_prefix(html, lt, "<script") || ieq_prefix(html, lt, "<style")) {
            const char* close = ieq_prefix(html, lt, "<script") ? "</script" : "</style";
            size_t j = gt + 1, ce = std::string::npos;
            for (; j < html.size(); j++)
                if (html[j] == '<' && ieq_prefix(html, j, close)) { ce = j; break; }
            if (ce == std::string::npos) { i = html.size(); break; }
            size_t ce_gt = html.find('>', ce);
            i = (ce_gt == std::string::npos) ? html.size() : ce_gt + 1;
            continue;
        }
        bool is_a = ieq_prefix(tag, 0, "<a") && tag.size() > 2 &&
                    (std::isspace((unsigned char)tag[2]) || tag[2] == '>');
        bool is_base = ieq_prefix(tag, 0, "<base") && tag.size() > 5 &&
                       (std::isspace((unsigned char)tag[5]) || tag[5] == '>' || tag[5] == '/');
        if (is_base) {
            AttrSpan a = find_attr(tag, "href");
            if (a.found && !a.value.empty()) base = resolve_url(doc_url, clean_href(a.value));
        } else if (is_a) {
            AttrSpan a = find_attr(tag, "href");
            if (a.found && a.end > a.start) {
                std::string href = clean_href(a.value);
                if (!href.empty() && href[0] != '#') {  // same-page anchors are left to FLTK
                    links.push_back(resolve_url(base, href));
                    tag = tag.substr(0, a.start) + "\"" + token + ":" +
                          std::to_string(links.size() - 1) + "\"" + tag.substr(a.end);
                }
            }
        }
        out += tag;
        i = gt + 1;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Networking
// ---------------------------------------------------------------------------

const size_t kMaxBodyBytes = 8 * 1024 * 1024;  // CHANGED: cap page size (was unbounded)

struct FetchResult {
    bool ok = false;
    std::string error;
    std::string body;
    std::string content_type;
    std::string effective_url;
    long http_code = 0;
};

struct WriteCtx {
    std::string* out;
    bool too_big = false;
};

class NetworkFetcher {
public:
    // CHANGED: takes a cancel flag (checked from curl's progress callback) and
    // returns a structured result instead of an HTML string, so callers can see
    // the content type and the final URL after redirects.
    static FetchResult fetch(const std::string& url, const std::atomic<bool>* cancel, bool insecure) {
        FetchResult r;
        r.effective_url = url;
        // FIX: RAII for the easy handle so it is released even if an exception
        // (e.g. std::bad_alloc) propagates.
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> h(curl_easy_init(), &curl_easy_cleanup);
        if (!h) { r.error = "curl init failed"; return r; }
        CURL* curl = h.get();

        WriteCtx ctx{&r.body};
        // FIX: check the return value of the one setopt that can fail on bad input.
        if (curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) != CURLE_OK) {
            r.error = "Invalid URL";
            return r;
        }
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &NetworkFetcher::on_write);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);        // FIX: redirect loops were unbounded on older libcurl
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);   // CHANGED: fail fast on dead hosts
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);          // FIX: required when libcurl is used from threads
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");   // CHANGED: accept gzip/deflate/br -> less data
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "LiteBird/1.1 (Ladybird-inspired)");
        // FIX (security): only http/https, for the first request and for redirects.
        // Without this a page could redirect into other curl protocols.
#if LIBCURL_VERSION_NUM >= 0x075500  // 7.85.0
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
        // FIX (critical security): TLS certificate and host-name verification
        // were disabled (VERIFYPEER/VERIFYHOST = 0), so any network attacker
        // could impersonate any HTTPS site. Now on by default; opt out only
        // with the explicit --insecure command-line flag.
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, insecure ? 0L : 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, insecure ? 0L : 2L);
        // CHANGED: progress callback lets a newer navigation cancel this one.
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &NetworkFetcher::on_progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, const_cast<std::atomic<bool>*>(cancel));

        CURLcode res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.http_code);
        // FIX: copy curl-owned strings before the handle is cleaned up.
        char* ct = nullptr;
        if (curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct) == CURLE_OK && ct) r.content_type = ct;
        char* eff = nullptr;
        if (curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff) == CURLE_OK && eff) r.effective_url = eff;

        if (res != CURLE_OK) {
            if (ctx.too_big) r.error = "Page is larger than " + std::to_string(kMaxBodyBytes / (1024 * 1024)) + " MB";
            else if (res == CURLE_ABORTED_BY_CALLBACK) r.error = "Cancelled";
            else if (res == CURLE_PEER_FAILED_VERIFICATION
#if LIBCURL_VERSION_NUM < 0x073e00  // CURLE_SSL_CACERT was merged into the above in 7.62
                     || res == CURLE_SSL_CACERT
#endif
            )
                // FIX: actionable message now that TLS is verified.
                r.error = std::string("TLS certificate check failed (") + curl_easy_strerror(res) +
                          "). Install the ca-certificates package, or run with --insecure at your own risk.";
            else r.error = std::string("Network: ") + curl_easy_strerror(res);
            return r;
        }
        if (r.http_code >= 400) { r.error = "HTTP " + std::to_string(r.http_code); return r; }
        if (r.body.empty()) { r.error = "Empty body"; return r; }
        r.ok = true;
        return r;
    }

private:
    // FIX: the old lambda let std::bad_alloc unwind through libcurl's C frames
    // (undefined behaviour) and accepted unlimited data.
    static size_t on_write(char* ptr, size_t size, size_t nmemb, void* user) {
        auto* ctx = static_cast<WriteCtx*>(user);
        size_t n = size * nmemb;
        try {
            if (ctx->out->size() + n > kMaxBodyBytes) { ctx->too_big = true; return 0; }
            ctx->out->append(ptr, n);
        } catch (...) {
            return 0;  // aborts the transfer with CURLE_WRITE_ERROR
        }
        return n;
    }
    static int on_progress(void* p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
        auto* cancel = static_cast<std::atomic<bool>*>(p);
        return (cancel && cancel->load()) ? 1 : 0;
    }
};

std::string error_page(const std::string& msg, const std::string& url) {
    // FIX: message and URL are HTML-escaped (page text can no longer inject markup).
    return "<html><head><title>Error</title></head><body>"
           "<h1>LiteBird</h1><p>" + html_escape(msg) + "</p>" +
           (url.empty() ? std::string() : "<p><tt>" + html_escape(url) + "</tt></p>") +
           "<p><a href='https://example.com'>example.com</a></p></body></html>";
}

// What the UI needs to display a page. Built on the worker thread.
struct Page {
    std::string html;
    std::string url;               // final URL after redirects (fragment restored)
    std::string fragment;
    std::vector<std::string> links;
    bool is_error = false;
};

// Thin engine abstraction (inspired by the boundary between Ladybird's
// networking helpers and the WebContent / LibWeb renderer).
class BrowserEngine {
public:
    // CHANGED: the engine now owns content handling: content-type check,
    // charset conversion and link rewriting all happen here (off the UI thread).
    static Page load(const std::string& url, const std::atomic<bool>* cancel, bool insecure,
                     const std::string& token) {
        Page pg;
        std::string fetch_url = strip_fragment(url, &pg.fragment);  // FIX: never send #fragment to the server
        FetchResult r = NetworkFetcher::fetch(fetch_url, cancel, insecure);
        pg.url = r.effective_url + (pg.fragment.empty() ? "" : "#" + pg.fragment);
        if (!r.ok) {
            pg.is_error = true;
            pg.html = error_page(r.error, pg.url);
            return pg;
        }
        std::string ct = to_lower(r.content_type);
        std::string mime = trim(ct.substr(0, ct.find(';')));
        std::string charset = find_charset(ct);
        bool is_html = mime.empty() || mime.find("html") != std::string::npos;
        bool is_text = mime.compare(0, 5, "text/") == 0 || mime == "application/json" ||
                       mime == "application/xml" || mime == "application/javascript";
        if (!is_html && !is_text) {
            // FIX: images, PDFs, zips... were dumped into the HTML view as binary garbage.
            pg.is_error = true;
            pg.html = error_page("LiteBird cannot display content of type \"" + mime + "\" (" +
                                     std::to_string(r.body.size()) + " bytes).", pg.url);
            return pg;
        }
        if (charset.empty() && is_html) {
            std::string head = to_lower(r.body.substr(0, 4096));
            charset = find_charset(head);
        }
        std::string text = to_utf8(r.body, is_single_byte_charset(charset));
        if (is_html) {
            pg.html = prepare_html(text, r.effective_url, token, pg.links);
        } else {
            // FIX: plain text was parsed as HTML (lost line breaks, '<' swallowed text).
            pg.html = "<html><body><pre>" + html_escape(text) + "</pre></body></html>";
        }
        return pg;
    }
};

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

class BrowserUI;

struct Job {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> cancel;
    std::shared_ptr<std::atomic<bool>> done;
};

enum class NavMode { Push, Traverse };

struct NavResult {
    BrowserUI* ui;
    unsigned long gen;
    NavMode mode;
    Page page;
};

class BrowserUI : public Fl_Window {
public:
    BrowserUI(int w, int h, bool insecure)
        : Fl_Window(w, h, "LiteBird"), hist_idx_(-1), insecure_(insecure) {
        // Random per-session token for rewritten links (see prepare_html()).
        std::random_device rd;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "lb%08x%08x", (unsigned)rd(), (unsigned)rd());
        token_ = buf;

        begin();
        // CHANGED: toolbar in its own group with the URL field as the resizable
        // widget; previously the buttons stretched/moved when the window resized.
        auto* bar = new Fl_Group(0, 0, w, 44);
        new Fl_Box(10, 8, 65, 28, "URL:");
        url_ = new Fl_Input(80, 8, w - 280, 28);
        url_->value("https://example.com");
        url_->when(FL_WHEN_ENTER_KEY_ALWAYS);
        url_->callback(cb_go, this);
        auto* go = new Fl_Button(w - 190, 8, 55, 28, "Go");
        go->callback(cb_go, this);
        back_ = new Fl_Button(w - 130, 8, 55, 28, "Back");
        back_->callback(cb_back, this);
        fwd_ = new Fl_Button(w - 70, 8, 55, 28, "Fwd");
        fwd_->callback(cb_fwd, this);
        bar->resizable(url_);
        bar->end();

        view_ = new Fl_Help_View(5, 45, w - 10, h - 75);
        view_->color(FL_WHITE);
        view_->textsize(14);
        view_->link(link_cb);
        view_->user_data(this);

        status_ = new Fl_Box(5, h - 25, w - 10, 20, "Ready");
        status_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        status_->labelsize(11);

        end();
        resizable(view_);
        size_range(420, 240);  // CHANGED: stop the window collapsing to an unusable size
        // FIX: FLTK's default window callback closes the window on Escape, so
        // pressing Esc in the address bar quit the browser. Only the window
        // manager's close button (or Alt+F4) closes it now.
        callback([](Fl_Widget* w, void*) {
            if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape) return;
            w->hide();
        });
        update_nav_buttons();
        show();
    }

    // FIX: worker threads are cancelled and joined before the window (and
    // later libcurl) goes away; the original had no shutdown path at all.
    ~BrowserUI() override { shutdown(); }

    void shutdown() {
        std::lock_guard<std::mutex> lk(jobs_mu_);
        for (auto& j : jobs_) j.cancel->store(true);
        for (auto& j : jobs_) if (j.thread.joinable()) j.thread.join();
        jobs_.clear();
    }

    // CHANGED: navigation is asynchronous. The original fetched on the UI
    // thread, freezing the window for up to 25 s (no redraw, no close).
    void navigate(const std::string& raw, NavMode mode = NavMode::Push) {
        std::string err;
        std::string url = normalize_user_url(raw, err);
        if (url.empty()) {  // FIX: invalid input is reported instead of fetched
            set_document(error_page(err, trim(raw)));
            set_status(err);
            copy_label("Error - LiteBird");
            return;
        }
        unsigned long gen = ++gen_;
        reap_jobs(/*cancel_all=*/true);  // FIX: a newer navigation cancels the older one

        set_status("Loading " + url + " ...");
        url_->value(url.c_str());
        cursor(FL_CURSOR_WAIT);

        Job job;
        job.cancel = std::make_shared<std::atomic<bool>>(false);
        job.done = std::make_shared<std::atomic<bool>>(false);
        auto cancel = job.cancel;
        auto done = job.done;
        std::string token = token_;
        bool insecure = insecure_;
        BrowserUI* self = this;
        job.thread = std::thread([self, url, gen, mode, cancel, done, token, insecure]() {
            // Worker thread: no FLTK calls here except Fl::awake().
            auto* res = new NavResult{self, gen, mode, Page()};
            try {
                res->page = BrowserEngine::load(url, cancel.get(), insecure, token);
            } catch (const std::exception& e) {  // FIX: an exception in a std::thread calls std::terminate
                res->page.is_error = true;
                res->page.url = url;
                res->page.html = error_page(std::string("Internal error: ") + e.what(), url);
            }
            // FIX: hand the result to the UI thread safely; free it if it was
            // superseded or if FLTK's awake queue is full (Fl::awake returns -1).
            if (cancel->load() || Fl::awake(&BrowserUI::on_loaded, res) != 0) delete res;
            done->store(true);
        });
        std::lock_guard<std::mutex> lk(jobs_mu_);
        jobs_.push_back(std::move(job));
    }

private:
    Fl_Input* url_;
    Fl_Help_View* view_;
    Fl_Box* status_;
    Fl_Button* back_;
    Fl_Button* fwd_;
    std::vector<std::string> hist_;
    int hist_idx_;
    bool insecure_;
    std::string token_;
    std::vector<std::string> links_;      // real URLs behind the rewritten hrefs
    std::string pending_link_;
    unsigned long gen_ = 0;              // only results for the latest navigation are shown
    bool in_set_value_ = false;          // true while we hand a document to view_
    std::mutex jobs_mu_;
    std::vector<Job> jobs_;

    // Layout (format()) calls the link callback for every <img>; the guard
    // lets link_cb tell those calls apart from clicks.
    void set_document(const std::string& html) {
        in_set_value_ = true;
        view_->value(html.c_str());
        in_set_value_ = false;
    }

    void set_status(const std::string& s) { status_->copy_label(escape_label(s).c_str()); }

    void update_nav_buttons() {  // CHANGED: grey out Back/Fwd when unusable
        if (hist_idx_ > 0) back_->activate(); else back_->deactivate();
        if (hist_idx_ + 1 < (int)hist_.size()) fwd_->activate(); else fwd_->deactivate();
    }

    void reap_jobs(bool cancel_all) {
        std::lock_guard<std::mutex> lk(jobs_mu_);
        for (auto& j : jobs_) if (cancel_all) j.cancel->store(true);
        // FIX: finished threads are joined so std::thread objects never leak
        // (destroying a joinable std::thread calls std::terminate).
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            if (it->done->load()) { if (it->thread.joinable()) it->thread.join(); it = jobs_.erase(it); }
            else ++it;
        }
    }

    // Runs on the UI thread (via Fl::awake).
    static void on_loaded(void* p) {
        std::unique_ptr<NavResult> r(static_cast<NavResult*>(p));
        BrowserUI* self = r->ui;
        if (r->gen != self->gen_) return;  // FIX: stale result from a superseded navigation
        self->show_page(r->page, r->mode);
        self->reap_jobs(false);
    }

    void show_page(Page& pg, NavMode mode) {
        links_.swap(pg.links);
        set_document(pg.html);
        if (!pg.fragment.empty()) view_->topline(pg.fragment.c_str());  // FIX: #fragment was ignored
        cursor(FL_CURSOR_DEFAULT);

        // FIX: history logic. The original Back handler re-pushed the page and
        // then popped it, and Fwd popped the entry it was about to visit, so
        // forward history was destroyed and Back/Fwd jumped to the wrong page.
        if (mode == NavMode::Push) {
            if (hist_idx_ >= 0 && hist_[hist_idx_] == pg.url) {
                // reload of the same page: do not add a duplicate entry
            } else {
                hist_.resize(hist_idx_ + 1);  // drop forward entries
                hist_.push_back(pg.url);
                hist_idx_ = (int)hist_.size() - 1;
            }
        } else if (hist_idx_ >= 0 && hist_idx_ < (int)hist_.size()) {
            hist_[hist_idx_] = pg.url;  // keep the post-redirect URL
        }
        // FIX: the URL bar now shows the final URL after redirects.
        url_->value(pg.url.c_str());
        set_status(pg.is_error ? "Error: " + pg.url : pg.url);
        const char* t = view_->title();  // CHANGED: page <title> in the window title
        // Fl_Help_View reports "Untitled" when the page has no <title>; show the URL then.
        std::string name = (t && *t && std::strcmp(t, "Untitled") != 0) ? std::string(t) : pg.url;
        std::string title = name + " - LiteBird";
        copy_label(title.c_str());
        update_nav_buttons();
    }

    static void cb_go(Fl_Widget*, void* v) {
        auto* self = static_cast<BrowserUI*>(v);
        self->navigate(self->url_->value(), NavMode::Push);
    }
    static void cb_back(Fl_Widget*, void* v) {
        auto* self = static_cast<BrowserUI*>(v);
        if (self->hist_idx_ > 0) {
            self->hist_idx_--;
            self->update_nav_buttons();
            self->navigate(self->hist_[self->hist_idx_], NavMode::Traverse);
        }
    }
    static void cb_fwd(Fl_Widget*, void* v) {
        auto* self = static_cast<BrowserUI*>(v);
        if (self->hist_idx_ + 1 < (int)self->hist_.size()) {
            self->hist_idx_++;
            self->update_nav_buttons();
            self->navigate(self->hist_[self->hist_idx_], NavMode::Traverse);
        }
    }

    static void cb_deferred_nav(void* v) {
        auto* self = static_cast<BrowserUI*>(v);
        std::string u;
        u.swap(self->pending_link_);
        if (!u.empty()) self->navigate(u, NavMode::Push);
    }

    // FIX: the link callback is also called by Fl_Help_View for every <img>
    // during layout and for its own load() calls. Only real clicks on our
    // rewritten links navigate now; everything else returns nullptr, which also
    // stops pages from making FLTK read local image files ("/etc/...", "file:").
    static const char* link_cb(Fl_Widget* w, const char* uri) {
        auto* self = static_cast<BrowserUI*>(w->user_data());
        // Fl_Help_View::handle() is private in FLTK 1.3, so a click is detected
        // as "the current event is a mouse release and we are not laying out".
        // The unguessable token below is the real gate; this is defence in depth.
        // FIX (crash): for <img> requests FLTK 1.3/1.4 dereference the result of
        // get_image() without a NULL check, so returning nullptr here segfaults
        // (the original crashed on any page with an absolute http(s) <img>).
        // Returning "" makes FLTK fall back to its built-in "broken image" icon
        // without touching the network or the local disk.
        static const char kBlocked[] = "";
        if (!self || !uri) return kBlocked;
        if (self->in_set_value_ || Fl::event() != FL_RELEASE) return kBlocked;  // image / layout request: blocked
        std::string u(uri);
        std::string prefix = self->token_ + ":";
        // Anything that is not one of our tokens is never followed. It must
        // get kBlocked (not nullptr): draw() also calls this for images and the
        // last event may still be the FL_RELEASE of the previous click.
        if (u.compare(0, prefix.size(), prefix) != 0) return kBlocked;
        char* end = nullptr;
        unsigned long idx = std::strtoul(u.c_str() + prefix.size(), &end, 10);
        if (!end || *end != '\0' || idx >= self->links_.size()) return nullptr;
        const std::string& target = self->links_[idx];
        if (!is_http_scheme(target)) {
            self->set_status("Unsupported link: " + target);
            return nullptr;
        }
        // FIX: navigation is deferred to the event loop instead of re-entering
        // the widget (replacing its document) from inside its own handle().
        self->pending_link_ = target;
        Fl::add_timeout(0.0, cb_deferred_nav, self);
        return nullptr;
    }
};

void print_usage(const char* argv0) {
    std::cout << "Usage: " << argv0 << " [--insecure] [URL]\n"
              << "  --insecure   disable TLS certificate verification (unsafe)\n";
}

}  // namespace

int main(int argc, char** argv) {
    // FIX: curl_global_init's result was ignored.
    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
        std::cerr << "LiteBird: libcurl initialisation failed\n";
        return 1;
    }
    // FIX: command-line parsing; argv[1] was always treated as a URL.
    bool insecure = false;
    std::string start = "https://example.com";
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--insecure") insecure = true;
        else if (a == "-h" || a == "--help") { print_usage(argv[0]); curl_global_cleanup(); return 0; }
        else start = a;
    }
    Fl::lock();  // FIX: enables Fl::awake() so worker threads can post results safely
    int rc;
    {
        // FIX: the window (and its worker threads) is destroyed before
        // curl_global_cleanup(); before, cleanup ran while `win` was alive.
        BrowserUI win(920, 680, insecure);
        win.navigate(start);
        rc = Fl::run();
        win.shutdown();
    }
    curl_global_cleanup();
    return rc;
}
