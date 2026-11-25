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
    std::vector<Form> forms;
    std::vector<FieldRef> refs;
    std::string html = "<html><head><base href=\"/docs/\"></head><body>"
                       "<!-- <a href='evil'> --><a class=x HREF='p?a=1&amp;b=2'>x</a>"
                       "<a href=\"#top\">t</a><a href=https://o.org/>o</a>"
                       "<script>var s='<a href=z>';</script><p>ok</p></body></html>";
    std::string out = prepare_html(html, "https://h.com/a/b", "lbTOK", links, forms, refs);
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


    // ---- DDG redirector unwrapping ----
    expect(unwrap_redirector("https://duckduckgo.com/l/?uddg=https%3A%2F%2Fwww.online-python.com%2F&rut=abc123"),
           "https://www.online-python.com/", "uddg basic");
    expect(unwrap_redirector("https://html.duckduckgo.com/l/?kh=-1&uddg=https%3A%2F%2Fen.wikipedia.org%2Fwiki%2FPython_(programming_language)%3Fa%3D1%26b%3D2"),
           "https://en.wikipedia.org/wiki/Python_(programming_language)?a=1&b=2", "uddg nested query");
    expect(unwrap_redirector("https://duckduckgo.com/l/?uddg=javascript%3Aalert(1)"), "", "uddg js blocked");
    expect(unwrap_redirector("https://evil-duckduckgo.com/l/?uddg=https%3A%2F%2Fx.org"), "", "uddg host check");
    expect(unwrap_redirector("https://duckduckgo.com/?q=x&uddg=https%3A%2F%2Fx.org"), "", "uddg path check");
    expect(unwrap_redirector("https://example.com/l/?uddg=https%3A%2F%2Fx.org"), "", "uddg other host");

    // ---- meta refresh ----
    int delay = -1; std::string tgt;
    bool ok = parse_meta_refresh("<html><head><META HTTP-EQUIV=\"Refresh\" CONTENT=\"0; URL='https://www.online-python.com/'\"></head></html>", delay, tgt);
    expect(ok ? tgt : "none", "https://www.online-python.com/", "meta refresh quoted");
    if (delay != 0) { std::cerr << "FAIL meta delay\n"; fails++; }
    ok = parse_meta_refresh("<meta http-equiv=refresh content=\"5;url=/next?a=1&amp;b=2\">", delay, tgt);
    expect(ok ? tgt : "none", "/next?a=1&b=2", "meta refresh entity");
    if (delay != 5) { std::cerr << "FAIL meta delay 5\n"; fails++; }
    ok = parse_meta_refresh("<meta http-equiv=\"refresh\" content=\"30\">", delay, tgt);
    if (ok) { std::cerr << "FAIL meta self refresh should be ignored\n"; fails++; }
    ok = parse_meta_refresh("<meta name=\"refresh\" content=\"0; url=x\">", delay, tgt);
    if (ok) { std::cerr << "FAIL meta name!=http-equiv\n"; fails++; }

    // ---- JS redirect ----
    expect(find_js_redirect("<script language=\"JavaScript\">function ffredirect(){window.location.replace('https://www.online-python.com/');}setTimeout('ffredirect()',100);</script>"),
           "https://www.online-python.com/", "js replace");
    expect(find_js_redirect("<script>window.location.href = \"https:\\/\\/a.org\\/x?b=1\\u0026c=2\";</script>"),
           "https://a.org/x?b=1&c=2", "js href escapes");
    expect(find_js_redirect("<p>location.href='no'</p>"), "", "js only in script");
    expect(find_js_redirect("<script>if (location.href == 'x') {}</script>"), "", "js comparison");

    // ---- form url-encoding ----
    expect(form_urlencode({{"q", "python tutorial"}, {"kl", ""}, {"x&y", "a=b/c+d"}}),
           "q=python+tutorial&kl=&x%26y=a%3Db%2Fc%2Bd", "urlencode");
    expect(form_urlencode({{"q", "caf\xC3\xA9"}}), "q=caf%C3%A9", "urlencode utf8");
    expect(form_get_url("https://h.com/s?old=1#f", "q=a"), "https://h.com/s?q=a", "get url");

    // ---- form extraction (DuckDuckGo Lite-like markup) ----
    {
        std::vector<std::string> l; std::vector<Form> f; std::vector<FieldRef> r;
        std::string ddg = "<form action=\"/lite/\" method=\"post\"><input class=\"query\" type=\"text\" size=\"40\" name=\"q\" value=\"\" autofocus>"
                          "<input class=\"submit\" type=\"submit\" value=\"Search\">"
                          "<select name=\"kl\"><option value=\"\">All Regions</option><option value=\"us-en\" selected>US</option></select>"
                          "<input type=\"hidden\" name=\"df\" value=\"\"><textarea name=t>a &amp; b</textarea>"
                          "<input type=checkbox name=c checked><input type=radio name=r value=1><button name=go value=yes>Go <b>now</b></button></form>"
                          "<input name=orphan>";
        std::string o = prepare_html(ddg, "https://lite.duckduckgo.com/lite/", "TK", l, f, r);
        if (f.size() != 2) { std::cerr << "FAIL form count " << f.size() << "\n"; fails++; }
        else {
            expect(f[0].action, "https://lite.duckduckgo.com/lite/", "form action");
            expect(f[0].method, "post", "form method");
            if (f[0].fields.size() != 8) { std::cerr << "FAIL field count " << f[0].fields.size() << "\n"; fails++; }
            else {
                expect(f[0].fields[0].type + ":" + f[0].fields[0].name, "text:q", "text field");
                expect(f[0].fields[1].type + ":" + f[0].fields[1].value, "submit:Search", "submit");
                expect(f[0].fields[2].options[1].value + std::to_string(f[0].fields[2].options[1].selected), "us-en1", "select");
                expect(f[0].fields[3].type, "hidden", "hidden");
                expect(f[0].fields[4].value, "a & b", "textarea");
                expect(std::string(f[0].fields[5].checked ? "on" : "off") + f[0].fields[5].value, "onon", "checkbox");
                expect(f[0].fields[7].label + "|" + f[0].fields[7].value, "Go now|yes", "button");
            }
            expect(f[1].action, "https://lite.duckduckgo.com/lite/", "orphan form");
        }
        if (o.find("TK:f0") == std::string::npos || o.find("TK:s1") == std::string::npos ||
            o.find("[ Search ]") == std::string::npos || o.find("<form") != std::string::npos ||
            o.find("TK:f3") != std::string::npos /* hidden has no placeholder */) {
            std::cerr << "FAIL placeholders: " << o << "\n"; fails++;
        }
    }

    // ---- dark theme ----
    {
        std::string d = apply_dark_theme("<html><body bgcolor=white text=black><table bgcolor=\"#fff\"><tr><td><font color=red size=2>x</font></td></tr></table></body></html>");
        if (d.find("#1e1e1e") == std::string::npos || d.find("bgcolor=white") != std::string::npos ||
            d.find("#fff") != std::string::npos || d.find("color=red") != std::string::npos || d.find("size=2") == std::string::npos) {
            std::cerr << "FAIL dark: " << d << "\n"; fails++;
        }
        if (apply_dark_theme("<p>x</p>").compare(0, 5, "<body") != 0) { std::cerr << "FAIL dark inject\n"; fails++; }
    }
    expect(percent_decode("a%2Fb+c%zz%4", true), "a/b c%zz%4", "percent_decode");
    expect(query_param("https://x/?a=1&b=two+words&c", "b"), "two words", "query_param");

    std::cout << (fails ? "FAILED: " : "All tests passed: ") << fails << " failures\n";
    return fails ? 1 : 0;
}
