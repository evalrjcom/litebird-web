// Unit tests for LiteBird's pure helpers (URL resolution, input normalisation,
// HTML rewriting, charset repair). Build: see Makefile target `test`.
#define main litebird_main
#include "../litebird.cpp"
#undef main
#include <cassert>

static int fails = 0;
static void expect(const std::string& got, const std::string& want, const char* what) {
    if (got != want) { std::cerr << "FAIL " << what << ": got [" << got << "] want [" << want << "]\n"; fails++; }
}

int main() {
    // RFC 3986 section 5.4 reference resolution examples
    const std::string b = "http://a/b/c/d;p?q";
    const char* cases[][2] = {
        {"g", "http://a/b/c/g"}, {"./g", "http://a/b/c/g"}, {"g/", "http://a/b/c/g/"},
        {"/g", "http://a/g"}, {"//g", "http://g/"}, {"?y", "http://a/b/c/d;p?y"},
        {"g?y", "http://a/b/c/g?y"}, {"#s", "http://a/b/c/d;p?q#s"}, {"g#s", "http://a/b/c/g#s"},
        {";x", "http://a/b/c/;x"}, {"", "http://a/b/c/d;p?q"}, {".", "http://a/b/c/"},
        {"./", "http://a/b/c/"}, {"..", "http://a/b/"}, {"../", "http://a/b/"},
        {"../g", "http://a/b/g"}, {"../..", "http://a/"}, {"../../g", "http://a/g"},
        {"../../../g", "http://a/g"}, {"/./g", "http://a/g"}, {"/../g", "http://a/g"},
        {"g.", "http://a/b/c/g."}, {"g..", "http://a/b/c/g.."}, {"./../g", "http://a/b/g"},
        {"g/./h", "http://a/b/c/g/h"}, {"g/../h", "http://a/b/c/h"},
        {"https://x.org/p", "https://x.org/p"},
    };
    for (auto& c : cases) expect(resolve_url(b, c[0]), c[1], c[0]);
    expect(resolve_url("https://example.com", "about"), "https://example.com/about", "empty base path");

    std::string err;
    expect(normalize_user_url("  Example.COM ", err), "https://example.com/", "bare host");
    expect(normalize_user_url("localhost:8080/x", err), "https://localhost:8080/x", "host:port");
    expect(normalize_user_url("example.com/?u=http://x", err), "https://example.com/?u=http://x", "embedded ://");
    expect(normalize_user_url("http://a.com/a b", err), "http://a.com/a%20b", "space");
    expect(normalize_user_url("file:///etc/passwd", err), "", "file scheme blocked");
    expect(normalize_user_url("javascript:alert(1)", err), "", "js scheme blocked");
    expect(normalize_user_url("   ", err), "", "empty");
    expect(normalize_user_url("https://", err), "", "no host");

    std::vector<std::string> links;
    std::string html = "<html><head><base href=\"/docs/\"></head><body>"
                       "<!-- <a href='evil'> --><a class=x HREF='p?a=1&amp;b=2'>x</a>"
                       "<a href=\"#top\">t</a><a href=https://o.org/>o</a>"
                       "<script>var s='<a href=z>';</script><p>ok</p></body></html>";
    std::string out = prepare_html(html, "https://h.com/a/b", "lbTOK", links);
    assert(links.size() == 2);
    expect(links[0], "https://h.com/docs/p?a=1&b=2", "base+entity");
    expect(links[1], "https://o.org/", "absolute");
    if (out.find("lbTOK:0") == std::string::npos || out.find("lbTOK:1") == std::string::npos) { std::cerr << "FAIL token\n"; fails++; }
    if (out.find("#top") == std::string::npos) { std::cerr << "FAIL anchor kept\n"; fails++; }
    if (out.find("<script") != std::string::npos || out.find("evil") != std::string::npos) { std::cerr << "FAIL strip\n"; fails++; }

    expect(to_utf8(std::string("caf\xE9 \x93q\x94", 8), true), "caf\xC3\xA9 \xE2\x80\x9Cq\xE2\x80\x9D", "cp1252");
    expect(to_utf8("caf\xC3\xA9", false), "caf\xC3\xA9", "utf8 kept");
    expect(to_utf8(std::string("a\0b\xFF", 4), false), "ab\xC3\xBF", "nul + invalid byte");
    expect(escape_label("medium.com/@me"), "medium.com/@@me", "label @");
    expect(find_charset("text/html; charset=utf-8"), "utf-8", "charset plain");
    expect(find_charset("text/html; charset=\"iso-8859-1\""), "iso-8859-1", "charset quoted");

    std::cout << (fails ? "FAILED: " : "All tests passed: ") << fails << " failures\n";
    return fails ? 1 : 0;
}
