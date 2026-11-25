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

// FIX (Windows CI): <windows.h> (pulled in by FLTK/curl headers) defines min/max
// macros that break std::min/std::max; NOMINMAX turns them off.
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Group.H>      // CHANGED: toolbar group so buttons don't stretch on resize
#include <FL/Fl_Input.H>
#include <FL/Fl_Secret_Input.H>    // FEATURE: form controls
#include <FL/Fl_Multiline_Input.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Round_Button.H>
#include <FL/Fl_Scroll.H>          // FEATURE: form bar
#include <FL/fl_draw.H>            // fl_width() for form-bar layout
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
#include <regex>              // FEATURE: JS redirect detection
#include <algorithm>
#include <string>
#include <thread>             // CHANGED: network fetch now runs off the UI thread
#include <vector>
#ifdef _WIN32
#include <direct.h>           // FEATURE: _wmkdir for the config dir
#else
#include <sys/stat.h>         // FEATURE: mkdir for the config dir
#include <sys/types.h>
#endif
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

// Lower-case tag name of a tag string ("<A href=..>" -> "a", "</form>" -> "/form").
std::string tag_name(const std::string& tag) {
    size_t i = 1, s = 1;
    if (i < tag.size() && tag[i] == '/') i++;
    while (i < tag.size() && (std::isalnum((unsigned char)tag[i]) || tag[i] == '-')) i++;
    return to_lower(tag.substr(s, i - s));
}

struct AttrSpan { size_t name_start = 0, start = 0, end = 0; std::string value; bool found = false; };

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
        if (i >= tag.size() || tag[i] != '=') {
            if (an == name) { a.found = true; a.name_start = ns; a.start = a.end = i; return a; }
            continue;
        }
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
        if (an == name) { a.found = true; a.name_start = ns; a.start = vs; a.end = ve; a.value = val; return a; }
    }
    return a;
}

// FEATURE: convenience accessor (entity-decoded attribute value).
std::string attr(const std::string& tag, const char* name, bool* present = nullptr) {
    AttrSpan a = find_attr(tag, name);
    if (present) *present = a.found;
    return a.found ? decode_entities(a.value) : std::string();
}

// FEATURE: remove every occurrence of an attribute (used by dark mode).
std::string remove_attr(std::string tag, const char* name) {
    for (int guard = 0; guard < 8; guard++) {
        AttrSpan a = find_attr(tag, name);
        if (!a.found) break;
        tag.erase(a.name_start, a.end - a.name_start);
    }
    return tag;
}

// Removes control chars that HTML parsers strip from URLs (tab/CR/LF).
std::string clean_href(const std::string& v) {
    std::string out;
    for (char c : trim(decode_entities(v)))
        if (c != '\t' && c != '\n' && c != '\r') out += c;
    return out;
}

// Find the start of the closing tag `close` (e.g. "</select") at or after p.
size_t find_close_tag(const std::string& h, size_t p, const char* close) {
    for (size_t j = p; j < h.size(); j++)
        if (h[j] == '<' && ieq_prefix(h, j, close)) return j;
    return std::string::npos;
}

// Index just past the '>' of the closing tag that starts at ce (npos-safe).
size_t after_close_tag(const std::string& h, size_t ce) {
    if (ce == std::string::npos) return h.size();
    size_t gt = h.find('>', ce);
    return gt == std::string::npos ? h.size() : gt + 1;
}

// Text content of an HTML fragment (tags removed, entities decoded, spaces collapsed).
std::string strip_tags(const std::string& h) {
    std::string out;
    bool in_tag = false, sp = false;
    for (char c : h) {
        if (c == '<') { in_tag = true; continue; }
        if (c == '>') { in_tag = false; continue; }
        if (in_tag) continue;
        if (std::isspace((unsigned char)c)) { sp = !out.empty(); continue; }
        if (sp) { out += ' '; sp = false; }
        out += c;
    }
    return decode_entities(out);
}

// ---------------------------------------------------------------------------
// FEATURE: web forms. Fl_Help_View cannot host widgets and silently drops
// <form>/<input>/<select>/<textarea>/<button>. The page pre-processor now
// extracts every control into plain data (Form/FormField), leaves a visible
// inline placeholder link in the page ("[Search: ____]", "[ Search ]"), and
// the UI builds real FLTK widgets for them in a form bar above the page.
// ---------------------------------------------------------------------------

struct FormOption { std::string value, label; bool selected = false; };

struct FormField {
    std::string type;   // text, password, hidden, textarea, select, checkbox, radio, submit
    std::string name, value, label;
    std::vector<FormOption> options;
    bool checked = false, disabled = false;
    int size = 0;
};

struct Form {
    std::string action;           // absolute URL
    std::string method = "get";   // "get" or "post"
    std::vector<FormField> fields;
};

struct FieldRef { int form, field; };

// application/x-www-form-urlencoded (HTML spec): space -> '+', unreserved kept.
std::string form_urlencode(const std::vector<std::pair<std::string, std::string>>& kv) {
    static const char* hex = "0123456789ABCDEF";
    auto enc = [](const std::string& s, std::string& out) {
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '*' || c == '-' || c == '.' || c == '_') out += (char)c;
            else if (c == ' ') out += '+';
            else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
        }
    };
    std::string out;
    for (size_t i = 0; i < kv.size(); i++) {
        if (i) out += '&';
        enc(kv[i].first, out);
        out += '=';
        enc(kv[i].second, out);
    }
    return out;
}

// GET submission target: action URL with its query/fragment replaced.
std::string form_get_url(const std::string& action, const std::string& encoded) {
    std::string u = strip_fragment(action, nullptr);
    size_t q = u.find('?');
    if (q != std::string::npos) u.erase(q);
    return u + "?" + encoded;
}

std::string field_caption(const FormField& f) {
    std::string c = !f.label.empty() ? f.label : f.name;
    if (c.size() > 24) c = c.substr(0, 24) + "...";
    return c;
}

// Inline placeholder shown in the page for control `idx` (flat index).
std::string field_placeholder(const FormField& f, const std::string& token, size_t idx) {
    std::string href = token + ":" + (f.type == "submit" ? "s" : "f") + std::to_string(idx);
    std::string text;
    if (f.type == "submit") text = "[ " + (f.label.empty() ? f.value : f.label) + " ]";
    else if (f.type == "checkbox") text = std::string(f.checked ? "[x] " : "[ ] ") + field_caption(f);
    else if (f.type == "radio") text = std::string(f.checked ? "(o) " : "( ) ") + field_caption(f);
    else if (f.type == "select") {
        std::string sel;
        for (auto& o : f.options) if (o.selected) sel = o.label;
        text = "[" + field_caption(f) + ": " + sel + " v]";
    } else {
        std::string v = f.type == "password" ? std::string(f.value.size(), '*') : f.value;
        if (v.size() > 20) v = v.substr(0, 20) + "...";
        text = "[" + field_caption(f) + ": " + (v.empty() ? "________" : v) + "]";
    }
    return "<a href=\"" + href + "\"><tt>" + html_escape(text) + "</tt></a> ";
}

// FIX (critical): Fl_Help_View::load()/follow_link() hand any absolute
// http(s) link to fl_open_uri(), i.e. clicking a normal link opened the
// *system* browser instead of LiteBird, and the link callback was also called
// for every <img src> on every re-layout, so images triggered navigations.
// Every <a href> is now rewritten to an opaque per-session token
// ("lb<random>:<index>") that FLTK cannot open itself; the real, fully
// resolved URL is kept in `links` and only followed on a genuine click.
// CHANGED: <script>/<style> bodies are dropped (Fl_Help_View rendered them as text).
// FEATURE: form controls are extracted into `forms`/`refs` (see above).
std::string prepare_html(const std::string& html, const std::string& doc_url,
                         const std::string& token, std::vector<std::string>& links,
                         std::vector<Form>& forms, std::vector<FieldRef>& refs) {
    std::string out;
    out.reserve(html.size() + 1024);
    std::string base = doc_url;
    int cur_form = -1, orphan_form = -1;
    auto form_for_control = [&]() -> int {
        if (cur_form >= 0) return cur_form;
        // Controls outside any <form>: collect them in one implicit GET form.
        if (orphan_form < 0) {
            Form f;
            f.action = strip_fragment(doc_url, nullptr);
            forms.push_back(f);
            orphan_form = (int)forms.size() - 1;
        }
        return orphan_form;
    };
    auto add_field = [&](const FormField& f) {
        int fi = form_for_control();
        forms[fi].fields.push_back(f);
        refs.push_back({fi, (int)forms[fi].fields.size() - 1});
        if (f.type != "hidden") out += field_placeholder(f, token, refs.size() - 1);
    };
    auto label_of = [](const std::string& tag) {
        std::string l = attr(tag, "placeholder");
        if (l.empty()) l = attr(tag, "aria-label");
        if (l.empty()) l = attr(tag, "title");
        return trim(l);
    };

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
        std::string name = tag_name(tag);
        i = gt + 1;

        if (name == "script" || name == "style") {
            i = after_close_tag(html, find_close_tag(html, i, name == "script" ? "</script" : "</style"));
            continue;
        }
        if (name == "base") {
            std::string h = attr(tag, "href");
            if (!h.empty()) base = resolve_url(doc_url, clean_href(h));
        } else if (name == "a") {
            AttrSpan a = find_attr(tag, "href");
            if (a.found && a.end > a.start) {
                std::string href = clean_href(a.value);
                if (!href.empty() && href[0] != '#') {  // same-page anchors are left to FLTK
                    links.push_back(resolve_url(base, href));
                    tag = tag.substr(0, a.start) + "\"" + token + ":" +
                          std::to_string(links.size() - 1) + "\"" + tag.substr(a.end);
                }
            }
        } else if (name == "form") {
            Form f;
            std::string act = clean_href(attr(tag, "action"));
            f.action = act.empty() ? strip_fragment(doc_url, nullptr) : resolve_url(base, act);
            f.method = to_lower(trim(attr(tag, "method"))) == "post" ? "post" : "get";
            forms.push_back(f);
            cur_form = (int)forms.size() - 1;
            continue;  // the tag itself is dropped
        } else if (name == "/form") {
            cur_form = -1;
            continue;
        } else if (name == "input") {
            FormField f;
            std::string type = to_lower(trim(attr(tag, "type")));
            bool has_value = false;
            f.name = attr(tag, "name");
            f.value = attr(tag, "value", &has_value);
            f.checked = find_attr(tag, "checked").found;
            f.disabled = find_attr(tag, "disabled").found;
            f.label = label_of(tag);
            f.size = std::atoi(attr(tag, "size").c_str());
            if (type == "hidden") f.type = "hidden";
            else if (type == "password") f.type = "password";
            else if (type == "checkbox" || type == "radio") {
                f.type = type;
                if (!has_value) f.value = "on";
            } else if (type == "submit" || type == "image") {
                f.type = "submit";
                if (!has_value) f.value = "Submit";
                f.label.clear();
            } else if (type == "button" || type == "reset" || type == "file") {
                continue;  // no scripting / no uploads: nothing useful to render
            } else f.type = "text";  // text, search, email, url, tel, number, date, ...
            add_field(f);
            continue;
        } else if (name == "textarea") {
            FormField f;
            f.type = "textarea";
            f.name = attr(tag, "name");
            f.label = label_of(tag);
            f.disabled = find_attr(tag, "disabled").found;
            size_t ce = find_close_tag(html, i, "</textarea");
            std::string inner = html.substr(i, (ce == std::string::npos ? html.size() : ce) - i);
            if (!inner.empty() && inner[0] == '\n') inner.erase(0, 1);
            f.value = decode_entities(inner);
            i = after_close_tag(html, ce);
            add_field(f);
            continue;
        } else if (name == "select") {
            FormField f;
            f.type = "select";
            f.name = attr(tag, "name");
            f.label = label_of(tag);
            f.disabled = find_attr(tag, "disabled").found;
            size_t ce = find_close_tag(html, i, "</select");
            size_t end = (ce == std::string::npos) ? html.size() : ce;
            size_t j = i;
            while (j < end) {
                size_t o = html.find('<', j);
                if (o == std::string::npos || o >= end) break;
                size_t og = find_tag_end(html, o);
                if (og == std::string::npos || og >= end) break;
                std::string ot = html.substr(o, og - o + 1);
                j = og + 1;
                if (tag_name(ot) != "option") continue;
                size_t next = html.find('<', j);
                if (next == std::string::npos || next > end) next = end;
                FormOption opt;
                opt.label = strip_tags(html.substr(j, next - j));
                bool hv = false;
                opt.value = attr(ot, "value", &hv);
                if (!hv) opt.value = opt.label;
                opt.selected = find_attr(ot, "selected").found;
                f.options.push_back(opt);
            }
            bool any = false;  // exactly one option selected (first wins, default first)
            for (auto& o : f.options) if (o.selected) { if (any) o.selected = false; any = true; }
            if (!any && !f.options.empty()) f.options[0].selected = true;
            i = after_close_tag(html, ce);
            add_field(f);
            continue;
        } else if (name == "button") {
            size_t ce = find_close_tag(html, i, "</button");
            std::string inner = html.substr(i, (ce == std::string::npos ? html.size() : ce) - i);
            i = after_close_tag(html, ce);
            std::string type = to_lower(trim(attr(tag, "type")));
            if (!type.empty() && type != "submit") continue;
            FormField f;
            f.type = "submit";
            f.name = attr(tag, "name");
            f.value = attr(tag, "value");
            f.label = strip_tags(inner);  // visible text; `value` is what gets submitted
            if (f.label.empty()) f.label = f.value.empty() ? "Submit" : f.value;
            add_field(f);
            continue;
        }
        out += tag;
    }
    return out;
}

// ---------------------------------------------------------------------------
// FEATURE: redirects that are not HTTP 3xx
// ---------------------------------------------------------------------------

std::string percent_decode(const std::string& s, bool plus_as_space) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '+' && plus_as_space) out += ' ';
        else if (c == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) &&
                 std::isxdigit((unsigned char)s[i + 2])) {
            out += (char)std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else out += c;
    }
    return out;
}

// Value of query parameter `key` (form-decoded), or "" when absent.
std::string query_param(const std::string& url, const std::string& key) {
    Url u = parse_url(url);
    if (!u.has_query) return "";
    size_t p = 0;
    while (p <= u.query.size()) {
        size_t amp = u.query.find('&', p);
        if (amp == std::string::npos) amp = u.query.size();
        std::string kv = u.query.substr(p, amp - p);
        size_t eq = kv.find('=');
        if (percent_decode(kv.substr(0, eq), true) == key)
            return eq == std::string::npos ? "" : percent_decode(kv.substr(eq + 1), true);
        p = amp + 1;
    }
    return "";
}

std::string url_host(const std::string& url) {
    std::string a = parse_url(url).authority;
    size_t at = a.rfind('@');
    if (at != std::string::npos) a = a.substr(at + 1);
    if (!a.empty() && a[0] == '[') return to_lower(a.substr(0, a.find(']') + 1));
    return to_lower(a.substr(0, a.find(':')));
}

// FIX: DuckDuckGo result links go through https://duckduckgo.com/l/?uddg=<target>,
// which answers with an HTML page that redirects via <meta refresh> + JavaScript.
// LiteBird showed that page as blank. The target is now decoded and visited
// directly (saves a round trip and does not depend on the interstitial page).
std::string unwrap_redirector(const std::string& url) {
    std::string host = url_host(url);
    const std::string suffix = ".duckduckgo.com";
    bool ddg = host == "duckduckgo.com" ||
               (host.size() > suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0);
    if (!ddg) return "";
    std::string path = parse_url(url).path;
    if (path != "/l/" && path != "/l") return "";
    std::string target = trim(query_param(url, "uddg"));
    return is_http_scheme(target) ? target : "";
}

// FIX: <meta http-equiv="refresh" content="0; url=..."> was ignored.
// Returns true when a refresh tag with a URL is found.
bool parse_meta_refresh(const std::string& html, int& delay, std::string& target) {
    size_t i = 0;
    while ((i = html.find('<', i)) != std::string::npos) {
        if (!ieq_prefix(html, i, "<meta")) { i++; continue; }
        size_t gt = find_tag_end(html, i);
        if (gt == std::string::npos) return false;
        std::string tag = html.substr(i, gt - i + 1);
        i = gt + 1;
        if (to_lower(trim(attr(tag, "http-equiv"))) != "refresh") continue;
        std::string c = trim(attr(tag, "content"));
        size_t k = 0;
        while (k < c.size() && (std::isdigit((unsigned char)c[k]) || c[k] == '.')) k++;
        int d = std::atoi(c.substr(0, k).c_str());
        while (k < c.size() && (std::isspace((unsigned char)c[k]) || c[k] == ';' || c[k] == ',')) k++;
        if (ieq_prefix(c, k, "url")) {
            size_t m = k + 3;
            while (m < c.size() && std::isspace((unsigned char)c[m])) m++;
            if (m < c.size() && c[m] == '=') {
                k = m + 1;
                while (k < c.size() && std::isspace((unsigned char)c[k])) k++;
            }
        }
        std::string u = trim(c.substr(k));
        if (!u.empty() && (u[0] == '\'' || u[0] == '"')) {
            char q = u[0];
            size_t e = u.find(q, 1);
            u = u.substr(1, e == std::string::npos ? std::string::npos : e - 1);
        }
        if (u.empty()) continue;  // plain "refresh this page": ignored
        delay = d;
        target = u;
        return true;
    }
    return false;
}

// FEATURE: simple JavaScript redirects (window.location = "...",
// location.href = '...', location.replace("..."), location.assign(...)).
// Only the contents of <script> blocks are searched.
std::string find_js_redirect(const std::string& html) {
    static const std::regex assign_re(R"(location(?:\.href)?\s*=\s*(['"])([^'"]+)\1)");
    static const std::regex call_re(R"(location\.(?:replace|assign)\(\s*(['"])([^'"]+)\1)");
    size_t i = 0;
    while ((i = html.find('<', i)) != std::string::npos) {
        if (!ieq_prefix(html, i, "<script")) { i++; continue; }
        size_t gt = find_tag_end(html, i);
        if (gt == std::string::npos) break;
        size_t ce = find_close_tag(html, gt + 1, "</script");
        std::string js = html.substr(gt + 1, (ce == std::string::npos ? html.size() : ce) - gt - 1);
        std::smatch m;
        std::string u;
        if (std::regex_search(js, m, call_re) || std::regex_search(js, m, assign_re)) u = m[2].str();
        if (!u.empty()) {
            std::string r;  // undo the usual JS string escapes
            for (size_t k = 0; k < u.size(); k++) {
                if (u[k] == '\\' && k + 1 < u.size() && u[k + 1] == '/') { r += '/'; k++; }
                else if (u.compare(k, 6, "\\u0026") == 0) { r += '&'; k += 5; }
                else r += u[k];
            }
            return r;
        }
        if (ce == std::string::npos) break;
        i = ce + 1;
    }
    return "";
}

// Number of visible, non-space characters (scripts/styles/tags removed).
size_t visible_text_len(const std::string& html) {
    std::vector<std::string> l;
    std::vector<Form> f;
    std::vector<FieldRef> r;
    std::string body = prepare_html(html, "http://x/", "t", l, f, r);
    size_t n = 0;
    for (char c : strip_tags(body)) if (!std::isspace((unsigned char)c)) n++;
    return n;
}

// ---------------------------------------------------------------------------
// FEATURE: dark mode for page content. Fl_Help_View takes its colours from
// <body bgcolor/text/link> and <font color>/bgcolor attributes, so in dark mode
// those are replaced/removed and a dark <body> is injected.
// ---------------------------------------------------------------------------

const char* const kDarkBody = "<body bgcolor=\"#1e1e1e\" text=\"#e0e0e0\" link=\"#8ab4f8\">";

std::string apply_dark_theme(const std::string& html) {
    std::string out;
    out.reserve(html.size() + 64);
    bool body_seen = false;
    size_t i = 0;
    while (i < html.size()) {
        size_t lt = html.find('<', i);
        if (lt == std::string::npos) { out.append(html, i, std::string::npos); break; }
        out.append(html, i, lt - i);
        size_t gt = find_tag_end(html, lt);
        if (gt == std::string::npos) { out.append(html, lt, std::string::npos); break; }
        std::string tag = html.substr(lt, gt - lt + 1);
        std::string name = tag_name(tag);
        if (name == "body") { tag = kDarkBody; body_seen = true; }
        else {
            tag = remove_attr(tag, "bgcolor");
            if (name == "font") tag = remove_attr(tag, "color");
        }
        out += tag;
        i = gt + 1;
    }
    return body_seen ? out : std::string(kDarkBody) + out;
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
    // FEATURE: optional POST body (application/x-www-form-urlencoded) for forms.
    static FetchResult fetch(const std::string& url, const std::atomic<bool>* cancel, bool insecure,
                             const std::string* post_body = nullptr) {
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
        curl_easy_setopt(curl, CURLOPT_AUTOREFERER, 1L);       // CHANGED: send Referer when following redirects
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);   // CHANGED: fail fast on dead hosts
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);          // FIX: required when libcurl is used from threads
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");   // CHANGED: accept gzip/deflate/br -> less data
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "LiteBird/1.2 (Ladybird-inspired)");
        if (post_body) {
            // FEATURE: form POST. COPYPOSTFIELDS gives curl its own copy; curl sets the
            // urlencoded Content-Type and switches to GET after a 301/302/303 redirect.
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)post_body->size());
            curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, post_body->c_str());
        }
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
    std::string html;              // theme-neutral; dark mode is applied at display time
    std::string url;               // final URL after redirects (fragment restored)
    std::string fragment;
    std::vector<std::string> links;
    std::vector<Form> forms;       // FEATURE: forms on the page
    std::vector<FieldRef> refs;    // flat control index -> (form, field)
    std::string redirect_to;       // FEATURE: meta-refresh / JS redirect target, if any
    bool from_post = false;        // result of a form POST (always a new history entry)
    bool is_error = false;
};

const int kMaxSoftRedirects = 5;

// Thin engine abstraction (inspired by the boundary between Ladybird's
// networking helpers and the WebContent / LibWeb renderer).
class BrowserEngine {
public:
    // CHANGED: the engine now owns content handling: content-type check,
    // charset conversion and link rewriting all happen here (off the UI thread).
    // FEATURE: follows DDG /l/?uddg= links, <meta refresh> (delay <= 5 s) and
    // simple JS redirects on near-empty pages, up to kMaxSoftRedirects hops.
    static Page load(const std::string& url, const std::string* post, const std::atomic<bool>* cancel,
                     bool insecure, const std::string& token) {
        std::string cur = url;
        Page pg;
        for (int hop = 0;; hop++) {
            std::string unwrapped = unwrap_redirector(cur);
            if (!unwrapped.empty()) cur = unwrapped;
            pg = load_once(cur, hop == 0 ? post : nullptr, cancel, insecure, token);
            if (pg.is_error || pg.redirect_to.empty() || hop >= kMaxSoftRedirects) break;
            if (cancel && cancel->load()) break;
            if (strip_fragment(pg.redirect_to, nullptr) == strip_fragment(pg.url, nullptr)) break;  // self-refresh
            cur = pg.redirect_to;
        }
        pg.from_post = post != nullptr;
        return pg;
    }

private:
    static Page load_once(const std::string& url, const std::string* post, const std::atomic<bool>* cancel,
                          bool insecure, const std::string& token) {
        Page pg;
        std::string fetch_url = strip_fragment(url, &pg.fragment);  // FIX: never send #fragment to the server
        FetchResult r = NetworkFetcher::fetch(fetch_url, cancel, insecure, post);
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
            // FIX: non-HTTP redirects (blank page on DDG result links).
            int delay = 0;
            std::string target;
            if (parse_meta_refresh(text, delay, target) && delay <= 5) {
                pg.redirect_to = resolve_url(r.effective_url, clean_href(target));
            } else if (visible_text_len(text) < 200) {  // only near-empty "redirect pages"
                std::string js = find_js_redirect(text);
                if (!js.empty()) pg.redirect_to = resolve_url(r.effective_url, clean_href(js));
            }
            if (!pg.redirect_to.empty() && !is_http_scheme(pg.redirect_to)) pg.redirect_to.clear();
            pg.html = prepare_html(text, r.effective_url, token, pg.links, pg.forms, pg.refs);
        } else {
            // FIX: plain text was parsed as HTML (lost line breaks, '<' swallowed text).
            pg.html = "<html><body><pre>" + html_escape(text) + "</pre></body></html>";
        }
        return pg;
    }
};

// ---------------------------------------------------------------------------
// FEATURE: persisted settings (dark mode). %APPDATA%\LiteBird\config.ini on
// Windows, $XDG_CONFIG_HOME/litebird/config (default ~/.config) elsewhere.
// ---------------------------------------------------------------------------

#ifdef _WIN32
std::wstring config_dir() {
    const wchar_t* app = _wgetenv(L"APPDATA");  // wide API: non-ASCII user names work
    return app && *app ? std::wstring(app) + L"\\LiteBird" : std::wstring();
}
FILE* open_config(bool write) {
    std::wstring d = config_dir();
    if (d.empty()) return nullptr;
    if (write) _wmkdir(d.c_str());
    return _wfopen((d + L"\\config.ini").c_str(), write ? L"wb" : L"rb");
}
#else
std::string config_dir() {
    const char* x = std::getenv("XDG_CONFIG_HOME");
    if (x && *x == '/') return std::string(x) + "/litebird";
    const char* home = std::getenv("HOME");
    return home && *home ? std::string(home) + "/.config/litebird" : std::string();
}
FILE* open_config(bool write) {
    std::string d = config_dir();
    if (d.empty()) return nullptr;
    if (write) {
        // mkdir -p for the two levels we may need (~/.config and ~/.config/litebird)
        size_t slash = d.rfind('/');
        if (slash != std::string::npos && slash > 0) mkdir(d.substr(0, slash).c_str(), 0700);
        mkdir(d.c_str(), 0700);
    }
    return std::fopen((d + "/config").c_str(), write ? "wb" : "rb");
}
#endif

bool load_dark_setting() {
    FILE* f = open_config(false);
    if (!f) return false;
    char line[256];
    bool dark = false;
    while (std::fgets(line, sizeof(line), f)) {
        std::string l = trim(line);
        if (l == "dark=1" || l == "dark=true") dark = true;
        else if (l == "dark=0" || l == "dark=false") dark = false;
    }
    std::fclose(f);
    return dark;
}

void save_dark_setting(bool dark) {
    FILE* f = open_config(true);
    if (!f) return;
    std::fprintf(f, "# LiteBird settings\ndark=%d\n", dark ? 1 : 0);
    std::fclose(f);
}

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
    std::shared_ptr<Page> page;
};

// CHANGED: history entries keep the rendered page so Back/Fwd is instant and
// a POST result (e.g. a search results page) is not re-submitted. Only the
// entries within kCachedPages of the current position keep their copy.
struct HistEntry {
    std::string url;
    std::shared_ptr<Page> page;
};
const int kCachedPages = 10;

// A control in the form bar.
struct PanelItem {
    Fl_Widget* w = nullptr;
    int lw = 0, ww = 0;    // label width (left of widget) and widget width
    bool newline = false;  // first control of a form starts a new row
    int rx = 0, ry = 0;    // position inside the panel after layout
};

BrowserUI* g_ui = nullptr;  // FEATURE: for the global shortcut hook

class BrowserUI : public Fl_Window {
public:
    BrowserUI(int w, int h, bool insecure, bool dark)
        : Fl_Window(w, h, "LiteBird"), hist_idx_(-1), insecure_(insecure), dark_(dark) {
        // Random per-session token for rewritten links (see prepare_html()).
        std::random_device rd;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "lb%08x%08x", (unsigned)rd(), (unsigned)rd());
        token_ = buf;

        begin();
        // CHANGED: toolbar in its own group with the URL field as the resizable
        // widget; previously the buttons stretched/moved when the window resized.
        bar_ = new Fl_Group(0, 0, w, 44);
        new Fl_Box(5, 8, 45, 28, "URL:");
        url_ = new Fl_Input(50, 8, w - 305, 28);
        url_->value("https://example.com");
        url_->when(FL_WHEN_ENTER_KEY_ALWAYS);
        url_->callback(cb_go, this);
        auto* go = new Fl_Button(w - 250, 8, 55, 28, "Go");
        go->callback(cb_go, this);
        back_ = new Fl_Button(w - 190, 8, 55, 28, "Back");
        back_->callback(cb_back, this);
        back_->tooltip("Back (Alt+Left)");
        fwd_ = new Fl_Button(w - 130, 8, 55, 28, "Fwd");
        fwd_->callback(cb_fwd, this);
        fwd_->tooltip("Forward (Alt+Right)");
        // FEATURE: dark-mode toggle (also Ctrl+D).
        theme_btn_ = new Fl_Button(w - 70, 8, 60, 28, "Dark");
        theme_btn_->callback(cb_theme, this);
        theme_btn_->tooltip("Toggle dark mode (Ctrl+D)");
        bar_->resizable(url_);
        bar_->end();

        // FEATURE: form bar (scrolls when a page has many controls).
        panel_ = new Fl_Scroll(0, 44, w, 10);
        panel_->type(Fl_Scroll::VERTICAL);
        panel_->box(FL_THIN_DOWN_BOX);
        panel_->end();
        panel_->hide();

        view_ = new Fl_Help_View(5, 45, w - 10, h - 75);
        view_->textsize(14);
        view_->link(link_cb);
        view_->user_data(this);

        status_ = new Fl_Box(5, h - 25, w - 10, 20, "Ready");
        status_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        status_->labelsize(11);

        end();
        resizable(view_);
        size_range(480, 260);  // CHANGED: stop the window collapsing to an unusable size
        // FIX: FLTK's default window callback closes the window on Escape, so
        // pressing Esc in the address bar quit the browser. Only the window
        // manager's close button (or Alt+F4) closes it now.
        callback([](Fl_Widget* w, void*) {
            if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape) return;
            w->hide();
        });
        apply_theme_colors();
        update_nav_buttons();
        g_ui = this;
        // FEATURE: keyboard shortcuts are seen before any widget (Fl_Input
        // would otherwise consume Ctrl+D / Ctrl+L itself).
        Fl::event_dispatch(&BrowserUI::dispatch);
        show();
    }

    // FIX: worker threads are cancelled and joined before the window (and
    // later libcurl) goes away; the original had no shutdown path at all.
    ~BrowserUI() override {
        shutdown();
        if (g_ui == this) { g_ui = nullptr; Fl::event_dispatch(nullptr); }
    }

    void shutdown() {
        std::lock_guard<std::mutex> lk(jobs_mu_);
        for (auto& j : jobs_) j.cancel->store(true);
        for (auto& j : jobs_) if (j.thread.joinable()) j.thread.join();
        jobs_.clear();
    }

    // FEATURE: own layout pass so the form bar can grow and shrink.
    void resize(int X, int Y, int W, int H) override {
        Fl_Window::resize(X, Y, W, H);
        layout_all();
    }

    // CHANGED: navigation is asynchronous. The original fetched on the UI
    // thread, freezing the window for up to 25 s (no redraw, no close).
    // FEATURE: `post` carries an urlencoded form body (POST submission).
    void navigate(const std::string& raw, NavMode mode = NavMode::Push, const std::string* post = nullptr) {
        std::string err;
        std::string url = normalize_user_url(raw, err);
        if (url.empty()) {  // FIX: invalid input is reported instead of fetched
            auto pg = std::make_shared<Page>();
            pg->html = error_page(err, trim(raw));
            pg->is_error = true;
            show_document(pg);
            set_status(err);
            copy_label("Error - LiteBird");
            return;
        }
        std::string unwrapped = unwrap_redirector(url);  // FIX: DDG /l/?uddg= result links
        if (!unwrapped.empty()) url = unwrapped;
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
        bool has_post = post != nullptr;
        std::string body = post ? *post : std::string();
        BrowserUI* self = this;
        job.thread = std::thread([self, url, gen, mode, cancel, done, token, insecure, has_post, body]() {
            // Worker thread: no FLTK calls here except Fl::awake().
            auto* res = new NavResult{self, gen, mode, std::make_shared<Page>()};
            try {
                *res->page = BrowserEngine::load(url, has_post ? &body : nullptr, cancel.get(), insecure, token);
            } catch (const std::exception& e) {  // FIX: an exception in a std::thread calls std::terminate
                res->page->is_error = true;
                res->page->url = url;
                res->page->html = error_page(std::string("Internal error: ") + e.what(), url);
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
    Fl_Group* bar_;
    Fl_Input* url_;
    Fl_Scroll* panel_;
    Fl_Help_View* view_;
    Fl_Box* status_;
    Fl_Button* back_;
    Fl_Button* fwd_;
    Fl_Button* theme_btn_;
    std::vector<HistEntry> hist_;
    int hist_idx_;
    bool insecure_;
    bool dark_;
    std::string token_;
    std::shared_ptr<Page> cur_;          // page on screen (links, forms)
    std::vector<PanelItem> items_;       // form bar controls
    std::vector<Fl_Widget*> ref_widget_; // flat control index -> widget (nullptr for hidden)
    std::string pending_link_;
    int pending_form_ = -1, pending_submitter_ = -1;
    unsigned long gen_ = 0;              // only results for the latest navigation are shown
    bool in_set_value_ = false;          // true while we hand a document to view_
    std::mutex jobs_mu_;
    std::vector<Job> jobs_;

    static const int kRowH = 24, kGap = 6, kPad = 4, kMaxPanelRows = 3;

    // Layout (format()) calls the link callback for every <img>; the guard
    // lets link_cb tell those calls apart from clicks.
    void set_document(const std::string& html) {
        in_set_value_ = true;
        view_->value(html.c_str());
        in_set_value_ = false;
    }

    // Puts a page on screen (theme applied) and rebuilds the form bar.
    void show_document(const std::shared_ptr<Page>& pg) {
        cur_ = pg;
        set_document(dark_ ? apply_dark_theme(pg->html) : pg->html);
        if (!pg->fragment.empty()) view_->topline(pg->fragment.c_str());  // FIX: #fragment was ignored
        build_panel();
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

    void show_page(const std::shared_ptr<Page>& pg, NavMode mode) {
        show_document(pg);
        cursor(FL_CURSOR_DEFAULT);

        // FIX: history logic. The original Back handler re-pushed the page and
        // then popped it, and Fwd popped the entry it was about to visit, so
        // forward history was destroyed and Back/Fwd jumped to the wrong page.
        if (mode == NavMode::Push) {
            // A POST result (DDG Lite posts back to /lite/) is a new entry even
            // when the URL is unchanged, so Back returns to the search form.
            if (hist_idx_ >= 0 && hist_[hist_idx_].url == pg->url && !pg->from_post) {
                hist_[hist_idx_].page = pg;  // reload of the same page: no duplicate entry
            } else {
                hist_.resize(hist_idx_ + 1);  // drop forward entries
                hist_.push_back({pg->url, pg});
                hist_idx_ = (int)hist_.size() - 1;
            }
        } else if (hist_idx_ >= 0 && hist_idx_ < (int)hist_.size()) {
            hist_[hist_idx_] = {pg->url, pg};  // keep the post-redirect URL
        }
        for (int k = 0; k < (int)hist_.size(); k++)  // bounded page cache
            if (std::abs(k - hist_idx_) > kCachedPages) hist_[k].page.reset();
        // FIX: the URL bar now shows the final URL after redirects.
        url_->value(pg->url.c_str());
        set_status(pg->is_error ? "Error: " + pg->url : pg->url);
        const char* t = view_->title();  // CHANGED: page <title> in the window title
        // Fl_Help_View reports "Untitled" when the page has no <title>; show the URL then.
        std::string name = (t && *t && std::strcmp(t, "Untitled") != 0) ? std::string(t) : pg->url;
        std::string title = name + " - LiteBird";
        copy_label(title.c_str());
        update_nav_buttons();
    }

    // CHANGED: Back/Fwd show the cached page when there is one.
    void traverse(int idx) {
        hist_idx_ = idx;
        update_nav_buttons();
        if (hist_[idx].page) {
            ++gen_;
            reap_jobs(true);
            show_page(hist_[idx].page, NavMode::Traverse);
        } else {
            navigate(hist_[idx].url, NavMode::Traverse);
        }
    }

    void reload() {
        navigate(hist_idx_ >= 0 ? hist_[hist_idx_].url : std::string(url_->value()), NavMode::Push);
    }

    // ---------------- dark mode ----------------

    void apply_theme_colors() {
        if (dark_) {
            Fl::background(45, 45, 48);
            Fl::background2(30, 30, 30);
            Fl::foreground(230, 230, 230);
            Fl::set_color(FL_SELECTION_COLOR, 70, 110, 170);
            view_->color(fl_rgb_color(30, 30, 30));
            view_->textcolor(fl_rgb_color(224, 224, 224));
            theme_btn_->label("Light");
        } else {
            Fl::background(192, 192, 192);  // FLTK defaults
            Fl::background2(255, 255, 255);
            Fl::foreground(0, 0, 0);
            Fl::set_color(FL_SELECTION_COLOR, 0, 0, 128);
            view_->color(FL_WHITE);
            view_->textcolor(FL_BLACK);
            theme_btn_->label("Dark");
        }
    }

    void toggle_theme() {
        dark_ = !dark_;
        apply_theme_colors();
        if (cur_) {
            int top = view_->topline();
            set_document(dark_ ? apply_dark_theme(cur_->html) : cur_->html);
            view_->topline(top);
        }
        save_dark_setting(dark_);
        redraw();
    }

    // ---------------- form bar ----------------

    static std::string menu_escape(const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '/' || c == '\\' || c == '&' || c == '_') o += '\\';  // Fl_Menu_::add() specials
            if (c == '@') o += '@';
            o += c;
        }
        return o.empty() ? std::string(" ") : o;
    }

    void build_panel() {
        panel_->clear();  // Fl_Scroll::clear() keeps its scrollbars
        items_.clear();
        ref_widget_.assign(cur_ ? cur_->refs.size() : 0, nullptr);
        if (!cur_ || cur_->refs.empty()) { layout_all(); return; }

        panel_->begin();
        fl_font(FL_HELVETICA, 12);
        int last_form = -1;
        for (size_t idx = 0; idx < cur_->refs.size(); idx++) {
            const FieldRef& rf = cur_->refs[idx];
            const FormField& f = cur_->forms[rf.form].fields[rf.field];
            if (f.type == "hidden") continue;
            PanelItem it;
            it.newline = rf.form != last_form;
            last_form = rf.form;
            std::string cap = escape_label(field_caption(f));
            if (f.type == "text" || f.type == "password") {
                Fl_Input* in = f.type == "password" ? new Fl_Secret_Input(0, 0, 10, kRowH)
                                                    : new Fl_Input(0, 0, 10, kRowH);
                in->value(f.value.c_str());
                in->when(FL_WHEN_ENTER_KEY_ALWAYS);  // FEATURE: Enter submits the form
                in->callback(cb_field_enter, this);
                in->textsize(12);
                it.w = in;
                it.ww = std::max(120, std::min(320, (f.size > 0 ? f.size : 30) * 8));
            } else if (f.type == "textarea") {
                auto* in = new Fl_Multiline_Input(0, 0, 10, kRowH);
                in->value(f.value.c_str());
                in->textsize(12);
                it.w = in;
                it.ww = 320;
            } else if (f.type == "select") {
                auto* ch = new Fl_Choice(0, 0, 10, kRowH);
                int sel = 0, widest = 60;
                for (size_t k = 0; k < f.options.size(); k++) {
                    ch->add(menu_escape(f.options[k].label).c_str());
                    if (f.options[k].selected) sel = (int)k;
                    widest = std::max(widest, (int)fl_width(f.options[k].label.c_str()) + 30);
                }
                if (!f.options.empty()) ch->value(sel);
                ch->textsize(12);
                it.w = ch;
                it.ww = std::min(260, widest);
            } else if (f.type == "checkbox" || f.type == "radio") {
                Fl_Button* b = f.type == "radio" ? static_cast<Fl_Button*>(new Fl_Round_Button(0, 0, 10, kRowH))
                                                 : static_cast<Fl_Button*>(new Fl_Check_Button(0, 0, 10, kRowH));
                b->value(f.checked ? 1 : 0);
                b->copy_label(cap.c_str());
                if (f.type == "radio") b->callback(cb_radio, this);
                it.w = b;
                it.ww = (int)fl_width(cap.c_str()) + 30;
                cap.clear();  // label drawn inside the widget
            } else if (f.type == "submit") {
                std::string l = escape_label(f.label.empty() ? f.value : f.label);
                auto* b = new Fl_Button(0, 0, 10, kRowH);
                b->copy_label(l.c_str());
                b->callback(cb_submit_btn, this);
                it.w = b;
                it.ww = (int)fl_width(l.c_str()) + 24;
                cap.clear();
            }
            if (!it.w) continue;
            it.w->labelsize(12);
            if (!cap.empty()) {
                it.w->copy_label(cap.c_str());
                it.w->align(FL_ALIGN_LEFT);
                it.lw = (int)fl_width(cap.c_str()) + 8;
            }
            if (f.disabled) it.w->deactivate();
            ref_widget_[idx] = it.w;
            items_.push_back(it);
        }
        panel_->end();
        Fl_Group::current(nullptr);
        layout_all();
        // Focus the first text field so the user can type right away (DDG Lite).
        for (auto& it : items_)
            if (dynamic_cast<Fl_Input*>(it.w) && it.w->active()) { it.w->take_focus(); break; }
    }

    // Positions toolbar, form bar, page view and status bar.
    void layout_all() {
        int W = w(), H = h();
        bar_->resize(0, 0, W, 44);
        int panel_h = 0;
        if (!items_.empty()) {
            int x = kPad, y = kPad, maxx = W - 20;  // flow layout, panel-relative
            for (size_t k = 0; k < items_.size(); k++) {
                PanelItem& it = items_[k];
                int need = it.lw + it.ww;
                if (k > 0 && (it.newline || x + need > maxx)) { x = kPad; y += kRowH + kGap; }
                it.rx = x + it.lw;
                it.ry = y;
                x += need + kGap;
            }
            int content_h = y + kRowH + kPad;
            panel_h = std::min(content_h, kMaxPanelRows * (kRowH + kGap) + kPad);
            panel_->resize(0, 44, W, panel_h);
            panel_->scroll_to(0, 0);
            for (auto& it : items_)
                it.w->resize(panel_->x() + it.rx, panel_->y() + it.ry,
                             std::max(40, std::min(it.ww, maxx - it.rx)), kRowH);
            panel_->show();
        } else {
            panel_->hide();
        }
        int top = 45 + panel_h;
        view_->resize(5, top, W - 10, H - 30 - top);
        status_->resize(5, H - 25, W - 10, 20);
        redraw();
    }

    int ref_of(Fl_Widget* w) const {
        for (size_t k = 0; k < ref_widget_.size(); k++) if (ref_widget_[k] == w) return (int)k;
        return -1;
    }

    // FEATURE: collect the form's current values and submit (GET or POST).
    void submit_form(int form_idx, int submitter) {
        if (!cur_ || form_idx < 0 || form_idx >= (int)cur_->forms.size()) return;
        std::shared_ptr<Page> keep = cur_;  // navigate() may replace cur_
        const Form& form = keep->forms[form_idx];
        // Implicit submission (Enter) uses the form's first submit button.
        if (submitter < 0)
            for (size_t k = 0; k < keep->refs.size(); k++)
                if (keep->refs[k].form == form_idx && form.fields[keep->refs[k].field].type == "submit") {
                    submitter = (int)k;
                    break;
                }
        std::vector<std::pair<std::string, std::string>> kv;
        for (size_t k = 0; k < keep->refs.size(); k++) {
            if (keep->refs[k].form != form_idx) continue;
            const FormField& f = form.fields[keep->refs[k].field];
            if (f.name.empty() || f.disabled) continue;
            Fl_Widget* w = k < ref_widget_.size() ? ref_widget_[k] : nullptr;
            if (f.type == "hidden") kv.push_back({f.name, f.value});
            else if (f.type == "text" || f.type == "password" || f.type == "textarea") {
                auto* in = dynamic_cast<Fl_Input_*>(w);
                kv.push_back({f.name, in ? std::string(in->value()) : f.value});
            } else if (f.type == "select") {
                auto* ch = dynamic_cast<Fl_Choice*>(w);
                int v = ch ? ch->value() : -1;
                if (v >= 0 && v < (int)f.options.size()) kv.push_back({f.name, f.options[v].value});
            } else if (f.type == "checkbox" || f.type == "radio") {
                auto* b = dynamic_cast<Fl_Button*>(w);
                if (b ? b->value() != 0 : f.checked) kv.push_back({f.name, f.value});
            } else if (f.type == "submit") {
                if ((int)k == submitter) kv.push_back({f.name, f.value});
            }
        }
        if (!is_http_scheme(form.action)) { set_status("Unsupported form action: " + form.action); return; }
        std::string body = form_urlencode(kv);
        if (form.method == "post") navigate(form.action, NavMode::Push, &body);
        else navigate(form_get_url(form.action, body), NavMode::Push);
    }

    static void cb_field_enter(Fl_Widget* w, void* v) {
        auto* self = static_cast<BrowserUI*>(v);
        int r = self->ref_of(w);
        if (r >= 0) self->submit_form(self->cur_->refs[r].form, -1);
    }
    static void cb_submit_btn(Fl_Widget* w, void* v) {
        auto* self = static_cast<BrowserUI*>(v);
        int r = self->ref_of(w);
        if (r >= 0) self->submit_form(self->cur_->refs[r].form, r);
    }
    // Radio buttons: exclusive within (form, name), not within the parent group.
    static void cb_radio(Fl_Widget* w, void* v) {
        auto* self = static_cast<BrowserUI*>(v);
        int r = self->ref_of(w);
        if (r < 0) return;
        static_cast<Fl_Button*>(w)->value(1);  // a radio cannot be un-checked by clicking it
        const FieldRef me = self->cur_->refs[r];
        const std::string nm = self->cur_->forms[me.form].fields[me.field].name;
        for (size_t k = 0; k < self->cur_->refs.size(); k++) {
            const FieldRef& o = self->cur_->refs[k];
            if ((int)k == r || o.form != me.form) continue;
            const FormField& f = self->cur_->forms[o.form].fields[o.field];
            if (f.type == "radio" && f.name == nm && self->ref_widget_[k])
                static_cast<Fl_Button*>(self->ref_widget_[k])->value(0);
        }
    }

    // ---------------- callbacks ----------------

    static void cb_go(Fl_Widget*, void* v) {
        auto* self = static_cast<BrowserUI*>(v);
        self->navigate(self->url_->value(), NavMode::Push);
    }
    static void cb_back(Fl_Widget*, void* v) {
        auto* self = static_cast<BrowserUI*>(v);
        if (self->hist_idx_ > 0) self->traverse(self->hist_idx_ - 1);
    }
    static void cb_fwd(Fl_Widget*, void* v) {
        auto* self = static_cast<BrowserUI*>(v);
        if (self->hist_idx_ + 1 < (int)self->hist_.size()) self->traverse(self->hist_idx_ + 1);
    }
    static void cb_theme(Fl_Widget*, void* v) { static_cast<BrowserUI*>(v)->toggle_theme(); }

    // FEATURE: global shortcuts, checked before normal event delivery.
    static int dispatch(int e, Fl_Window* w) {
        if (e == FL_KEYBOARD && g_ui && g_ui->handle_shortcut()) return 1;
        return Fl::handle_(e, w);
    }
    bool handle_shortcut() {
        int k = Fl::event_key();
        int st = Fl::event_state();
        bool ctrl = (st & FL_COMMAND) != 0, alt = (st & FL_ALT) != 0;
        if (ctrl && k == 'd') { toggle_theme(); return true; }
        if (ctrl && k == 'l') { url_->take_focus(); url_->position(url_->size(), 0); return true; }
        if (k == FL_F + 5 || (ctrl && k == 'r')) { reload(); return true; }
        if (alt && k == FL_Left) { cb_back(nullptr, this); return true; }
        if (alt && k == FL_Right) { cb_fwd(nullptr, this); return true; }
        return false;
    }

    static void cb_deferred_nav(void* v) {
        auto* self = static_cast<BrowserUI*>(v);
        if (self->pending_form_ >= 0) {
            int f = self->pending_form_, s = self->pending_submitter_;
            self->pending_form_ = self->pending_submitter_ = -1;
            self->submit_form(f, s);
            return;
        }
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
        if (!self->cur_) return nullptr;
        const char* p = u.c_str() + prefix.size();
        char kind = (*p == 'f' || *p == 's') ? *p++ : 'l';
        char* end = nullptr;
        unsigned long idx = std::strtoul(p, &end, 10);
        if (!end || end == p || *end != '\0') return nullptr;
        if (kind != 'l') {
            // FEATURE: inline form placeholders. "f" focuses the control in the
            // form bar, "s" submits the form with that button as submitter.
            if (idx >= self->cur_->refs.size()) return nullptr;
            if (kind == 's') {
                self->pending_form_ = self->cur_->refs[idx].form;
                self->pending_submitter_ = (int)idx;
                Fl::add_timeout(0.0, cb_deferred_nav, self);
            } else if (idx < self->ref_widget_.size() && self->ref_widget_[idx]) {
                Fl_Widget* fw = self->ref_widget_[idx];
                for (auto& it : self->items_)
                    if (it.w == fw) self->panel_->scroll_to(0, std::max(0, it.ry - kPad));
                fw->take_focus();
            }
            return nullptr;
        }
        if (idx >= self->cur_->links.size()) return nullptr;
        const std::string& target = self->cur_->links[idx];
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
    std::cout << "Usage: " << argv0 << " [--insecure] [--dark|--light] [URL]\n"
              << "  --insecure   disable TLS certificate verification (unsafe)\n"
              << "  --dark       start in dark mode (--light: light mode); not saved\n"
              << "Shortcuts: Ctrl+D dark mode, Ctrl+L address bar, F5/Ctrl+R reload,\n"
              << "           Alt+Left / Alt+Right back / forward\n";
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
    bool dark = load_dark_setting();  // FEATURE: persisted dark-mode choice
    std::string start = "https://example.com";
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--insecure") insecure = true;
        else if (a == "--dark") dark = true;
        else if (a == "--light") dark = false;
        else if (a == "-h" || a == "--help") { print_usage(argv[0]); curl_global_cleanup(); return 0; }
        else start = a;
    }
    Fl::lock();  // FIX: enables Fl::awake() so worker threads can post results safely
    Fl::scheme("gtk+");  // CHANGED: flatter widget look that also suits dark mode
    int rc;
    {
        // FIX: the window (and its worker threads) is destroyed before
        // curl_global_cleanup(); before, cleanup ran while `win` was alive.
        BrowserUI win(920, 680, insecure, dark);
        win.navigate(start);
        rc = Fl::run();
        win.shutdown();
    }
    curl_global_cleanup();
    return rc;
}
