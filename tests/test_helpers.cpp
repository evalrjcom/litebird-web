// Unit tests for LiteBird's pure helpers (URL resolution, input normalisation,
// HTML rewriting, charset repair). Build: see Makefile target `test`.
#define main litebird_main
#include "../litebird.cpp"
#undef main
#include <cassert>
#include <chrono>

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
    // CHANGED(2): the action's #fragment is kept (HTML "mutate action URL").
    expect(form_get_url("https://h.com/s?old=1#f", "q=a"), "https://h.com/s?q=a#f", "get url");
    expect(form_get_url("https://h.com/s?old=1", "q=a"), "https://h.com/s?q=a", "get url no frag");

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
        // FIX(3): the control outside any <form> is dropped (no implicit form).
        if (f.size() != 1) { std::cerr << "FAIL form count " << f.size() << "\n"; fails++; }
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
            expect(f[0].visible ? "shown" : "not shown", "shown", "ddg form shown");
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


    // ================= AUDIT-2 tests =================
    // entities
    expect(decode_entities("a&nbsp;b"), "a\xC2\xA0" "b", "nbsp");
    expect(decode_entities("&copy;&eacute;&mdash;&euro;"), "\xC2\xA9\xC3\xA9\xE2\x80\x94\xE2\x82\xAC", "named");
    expect(decode_entities("&#150;&#x96;"), "\xE2\x80\x93\xE2\x80\x93", "cp1252 numeric");
    expect(decode_entities("&#xD800;&#0;&#1114112;"), "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD", "invalid numeric");
    expect(decode_entities("&# 65;&#-1;&#x;&;"), "&# 65;&#-1;&#x;&;", "malformed numeric");
    expect(decode_entities("a=1&copy=2&b"), "a=1&copy=2&b", "no semicolon");
    expect(decode_entities("&COPY;&Eacute;"), "&COPY;\xC3\x89", "case sensitive");
    {   // no quadratic scan: 200k '&' without ';' must be fast
        std::string big(200000, '&');
        auto t0 = std::chrono::steady_clock::now();
        std::string r = decode_entities(big);
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (r != big || sec > 1.0) { std::cerr << "FAIL entity perf " << sec << "s\n"; fails++; }
    }
    expect(utf8_truncate("\xC3\xA9\xC3\xA9\xC3\xA9", 2), "\xC3\xA9\xC3\xA9...", "utf8 truncate");
    expect(utf8_truncate("abc", 3), "abc", "utf8 truncate exact");
    expect(utf8_to_cp1252("caf\xC3\xA9 \xE2\x82\xAC \xE2\x9C\x93"), "caf\xE9 \x80 &#10003;", "utf8->cp1252");
    {
        std::string o;
        std::string le = std::string("\xFF\xFE", 2) + std::string("h\0i\0", 4) + std::string("\x3D\xD8\x00\xDE", 4);
        if (!decode_bom(le, o) || o != "hi\xF0\x9F\x98\x80") { std::cerr << "FAIL utf16 bom [" << o << "]\n"; fails++; }
        if (!decode_bom("\xEF\xBB\xBFok", o) || o != "ok") { std::cerr << "FAIL utf8 bom\n"; fails++; }
        if (decode_bom("plain", o)) { std::cerr << "FAIL no bom\n"; fails++; }
    }
    // forms: nested form (HTML: inner <form> ignored, the first </form> closes the outer one)
    {
        std::vector<std::string> l; std::vector<Form> f; std::vector<FieldRef> r;
        prepare_html("<form action=/a><input name=q><form action=/inner><input name=in1></form><input name=after>",
                     "https://h.com/x", "TK", l, f, r);
        expect(std::to_string(f.size()), "1", "nested form ignored");
        if (f.size() == 1)  // "after" is outside the form after the first </form>: dropped (FIX(3))
            expect(f[0].action + ":" + std::to_string(f[0].fields.size()), "https://h.com/a:2", "inner controls in outer form");
    }
    // forms: unclosed select, labels, image, autofocus, enctype, charset
    {
        std::vector<std::string> l; std::vector<Form> f; std::vector<FieldRef> r;
        std::string h =
            "<form action=/a method=post enctype=multipart/form-data accept-charset=iso-8859-1>"
            "<label for=q>Query</label><input id=q name=q autofocus>"
            "<select name=s><option>One<option>Two"
            "<input type=image name=go alt=Go>"
            "<label>Wrapped <input name=w></label>"
            "<button>unclosed"
            "</form>"
            "<p>visible text after</p>";
        std::string o = prepare_html(h, "https://h.com/x", "TK", l, f, r);
        expect(std::to_string(f.size()), "1", "one form");
        if (f.size() == 1 && f[0].fields.size() == 5) {
            expect(f[0].action + "|" + f[0].method + "|" + f[0].enctype + (f[0].single_byte ? "|1252" : "|utf8"),
                   "https://h.com/a|post|multipart|1252", "form attrs");
            expect(f[0].fields[0].label, "Query", "label for");
            expect(f[0].fields[0].autofocus ? "y" : "n", "y", "autofocus");
            expect(std::to_string(f[0].fields[1].options.size()), "2", "unclosed select bounded");
            expect(f[0].fields[2].image ? f[0].fields[2].label : "no", "Go", "image submit");
            expect(f[0].fields[3].label, "Wrapped", "implicit label");
            expect(f[0].fields[4].type + ":" + f[0].fields[4].label, "submit:unclosed", "unclosed button");
        } else { std::cerr << "FAIL form field count " << (f.empty() ? 0 : f[0].fields.size()) << "\n"; fails++; }
        if (o.find("visible text after") == std::string::npos) { std::cerr << "FAIL page text swallowed: " << o << "\n"; fails++; }
    }
    {   // select options with duplicate labels keep one entry each
        std::vector<std::string> l; std::vector<Form> f; std::vector<FieldRef> r;
        prepare_html("<form><select name=k><option value=1>--<option value=2>--<option value=3 selected>A&amp;B</select></form>",
                     "https://h.com/", "TK", l, f, r);
        expect(std::to_string(f[0].fields[0].options.size()), "3", "dup options parsed");
        expect(f[0].fields[0].options[2].label, "A&B", "option entity");
    }
    {   // textarea CRLF handling, text newline stripping
        std::vector<std::string> l; std::vector<Form> f; std::vector<FieldRef> r;
        prepare_html("<form><textarea name=t>\r\nl1\r\nl2</textarea><input name=v value=\"a&#10;b\"></form>", "https://h.com/", "TK", l, f, r);
        expect(f[0].fields[0].value, "l1\nl2", "textarea LF");
        expect(to_crlf(f[0].fields[0].value), "l1\r\nl2", "textarea CRLF on submit");
        expect(f[0].fields[1].value, "ab", "text newline stripped");
    }
    expect(form_multipart({{"a\"b", "1"}, {"c", "x\r\ny"}}, "BND"),
           "--BND\r\nContent-Disposition: form-data; name=\"a%22b\"\r\n\r\n1\r\n"
           "--BND\r\nContent-Disposition: form-data; name=\"c\"\r\n\r\nx\r\ny\r\n--BND--\r\n", "multipart");
    expect(form_text_plain({{"a", "1"}, {"b", "2"}}), "a=1\r\nb=2\r\n", "text/plain");
    {   // huge page with many unclosed selects: linear time, content kept
        std::string h = "<form>";
        for (int k = 0; k < 20000; k++) h += "<select name=s" + std::to_string(k) + "><option>x";
        h += "<p>END</p>";
        std::vector<std::string> l; std::vector<Form> f; std::vector<FieldRef> r;
        auto t0 = std::chrono::steady_clock::now();
        std::string o = prepare_html(h, "https://h.com/", "TK", l, f, r);
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (o.find("END") == std::string::npos || r.size() != 20000 || sec > 3.0) {
            std::cerr << "FAIL huge selects " << r.size() << " end=" << (o.find("END") != std::string::npos) << " " << sec << "s\n"; fails++;
        }
    }
    // JS redirect scanner
    expect(scan_js_location("var geolocation = 'Paris';"), "", "js word boundary");
    expect(scan_js_location("if (location.href == 'x') {}"), "", "js comparison");
    expect(scan_js_location("window.location.replace(\"https:\\/\\/a.com\\/?a=1\\u0026b=2\")"), "https://a.com/?a=1&b=2", "js replace escapes");
    expect(scan_js_location("document.location = 'it\\'s.html'"), "it's.html", "js escaped quote");
    expect(scan_js_location("location.assign( '/next' )"), "/next", "js assign");
    expect(scan_js_location("location.hash = '#x'"), "", "js hash ignored");
    {   // the std::regex version segfaulted here (deep recursion)
        std::string js = "<script>location.href='https://a.com/" + std::string(300000, 'x') + "'</script>";
        std::string u = find_js_redirect(js);
        expect(u.size() ? "len" + std::to_string(u.size() > 1000) : "empty", "empty", "huge literal bounded (no crash)");
        std::string js2 = "<script>" + std::string(2000000, 'a') + ";location='/ok'</script>";
        expect(find_js_redirect(js2), "/ok", "huge script");
    }
    // redirects
    expect(unwrap_redirector("https://duckduckgo.com/l/?uddg=https%3A%2F%2Fa.com%2Fx%00y%20z&rut=1"),
           "https://a.com/x%00y%20z", "uddg NUL/space normalised");
    expect(unwrap_redirector("https://duckduckgo.com/l/?uddg=https%3A%2F%2F%2Fpath"), "", "uddg no host");
    expect(unwrap_redirector("https://duckduckgo.com/l/?uddg=https%3A%2F%2FA.COM%2F%zz"), "https://a.com/%zz", "uddg malformed pct");
    expect(unwrap_all("https://duckduckgo.com/l/?uddg=" "https%3A%2F%2Fduckduckgo.com%2Fl%2F%3Fuddg%3Dhttps%253A%252F%252Fb.com%252F"),
           "https://b.com/", "nested uddg");
    expect(unwrap_redirector("https://evil.com/l/?uddg=https%3A%2F%2Fa.com"), "", "uddg only on ddg");
    expect(unwrap_redirector("https://duckduckgo.com.evil.com/l/?uddg=https%3A%2F%2Fa.com"), "", "uddg suffix spoof");
    {
        int d = -7; std::string t;
        bool ok = parse_meta_refresh("<meta http-equiv=refresh content=\"-1\">", d, t);
        expect(ok ? "found:" + t : "none", "none", "refresh without number");
        ok = parse_meta_refresh("<meta http-equiv=refresh content=\"99999999999999; url=/x\">", d, t);
        expect(ok ? std::to_string(d > 5) + t : "none", "1/x", "refresh huge delay");
        // commented-out refresh is gone after prepare_html (load_once now parses the prepared HTML)
        std::vector<std::string> l; std::vector<Form> f; std::vector<FieldRef> r; std::string base;
        std::string o = prepare_html("<base href=\"https://cdn.x/dir/\"><!-- <meta http-equiv=refresh content=\"0;url=/bad\"> -->"
                                     "<script>document.write('<meta http-equiv=refresh content=0;url=/bad2>')</script>",
                                     "https://h.com/", "TK", l, f, r, &base);
        expect(parse_meta_refresh(o, d, t) ? t : "none", "none", "refresh in comment/script ignored");
        expect(resolve_url(base, "next.html"), "https://cdn.x/dir/next.html", "refresh vs base");
    }
    // dark mode on malformed attributes
    {
        std::string d = apply_dark_theme("<font color=x\" bgcolor=\"y>TEXT</font>");
        if (d.find(">TEXT") == std::string::npos) { std::cerr << "FAIL dark malformed: " << d << "\n"; fails++; }
        expect(remove_attr("<td bgcolor=\"#fff>", "bgcolor"), "<td >", "remove_attr unclosed quote keeps >");
    }
    // config parsing / updating
    expect(parse_dark_setting("\xEF\xBB\xBF# c\r\ndark = YES\r\n") ? "1" : "0", "1", "cfg bom crlf");
    expect(parse_dark_setting("dark=1\ndark=0\n") ? "1" : "0", "0", "cfg last wins");
    expect(parse_dark_setting("garbage\x01\xFF\ndark\n=1") ? "1" : "0", "0", "cfg corrupt");
    expect(update_dark_setting("# hi\r\nzoom=2\ndark=0\ndark=1\n", true), "# hi\nzoom=2\ndark=1\n", "cfg preserve");
    expect(update_dark_setting("", false), "# LiteBird settings\ndark=0\n", "cfg new");
#ifndef _WIN32
    {   // mkdir -p + atomic write with a deep, non-ASCII XDG_CONFIG_HOME
        char tmpl[] = "/tmp/lbcfgXXXXXX";
        if (mkdtemp(tmpl)) {
            std::string deep = std::string(tmpl) + "/a/\xC3\xA9t\xC3\xA9/c";
            setenv("XDG_CONFIG_HOME", deep.c_str(), 1);
            save_dark_setting(true);
            expect(load_dark_setting() ? "1" : "0", "1", "cfg deep dir write+read");
            save_dark_setting(false);
            expect(load_dark_setting() ? "1" : "0", "0", "cfg rewrite");
            std::string cmd = std::string("rm -rf ") + tmpl;
            if (std::system(cmd.c_str()) != 0) {}
        }
    }
#endif

    // ================= FIX(3): JS-only controls, hidden content, images =================
    {   // w3schools-like page: no <form>, JS search/filter boxes, close buttons,
        // dark-mode checkbox, hidden dropdown <nav> menus, svg icons, images.
        std::string h =
            "<body><div class=\"tnb\"><input id=\"tnb-google-search-input\" type=\"text\" placeholder=\"Search...\" aria-label=\"Search field\">"
            "<button class=\"tnb-x\" aria-label=\"Close\">&times;</button>"
            "<a href=\"/\"><svg viewBox=\"0 0 10 10\"><title>Home icon</title><path d=\"M0\"/></svg>HOME</a>"
            "<i class=\"fa\">\xEE\xA0\x82</i><span>&#xE802;</span>"
            "<label><input type=\"checkbox\" id=\"darkToggle\"> Dark mode</label></div>"
            "<nav id=\"nav_tutorials\" class=\"dropdown-nav w3-hide-small navex\"><input placeholder=\"Filter...\">"
            "<button>&times;</button><a href=\"/html/\">HIDDEN MENU LINK</a></nav>"
            "<nav id=\"tnb-mobile-nav\" class=\"tnb-mobile-nav w3-hide\"><a href=\"/m\">MOBILE MENU</a></nav>"
            "<div class=\"w3-hide-small\">SHOWN ON DESKTOP</div>"
            "<div hidden>HIDDEN ATTR</div><div aria-hidden=\"true\">ARIA HIDDEN</div>"
            "<div style=\"color:red; display: none\">STYLE HIDDEN<div>nested</div></div>"
            "<template><p>TEMPLATE</p></template>"
            "<img src=\"/logo.png\" alt=\"W3Schools logo\"><img src=\"/deco.png\" alt=\"\">"
            "<h1>Python Tutorial</h1><button type=\"button\" onclick=\"go()\">Try it</button></body>";
        std::vector<std::string> l; std::vector<Form> f; std::vector<FieldRef> r;
        std::string o = prepare_html(h, "https://www.w3schools.com/python/", "TK", l, f, r);
        expect(std::to_string(f.size()) + "/" + std::to_string(r.size()), "0/0", "w3: no implicit form");
        for (const char* junk : {"TK:f", "TK:s", "[Search", "Filter", "HIDDEN", "MOBILE MENU", "ARIA",
                                 "STYLE HIDDEN", "nested", "TEMPLATE", "Home icon", "<img", "<svg", "\xEE\xA0\x82", "&#xE802;",
                                 "[ ]", "[x]", "Dark mode"})
            if (o.find(junk) != std::string::npos) { std::cerr << "FAIL w3 junk [" << junk << "] in: " << o << "\n"; fails++; }
        for (const char* keep : {"HOME", "SHOWN ON DESKTOP", "[W3Schools logo]", "Python Tutorial"})
            if (o.find(keep) == std::string::npos) { std::cerr << "FAIL w3 lost [" << keep << "] in: " << o << "\n"; fails++; }
    }
    {   // form="id" association, hidden controls submitted but not shown, form visibility
        std::string h =
            "<form id=s action=/search><input name=q placeholder=Query><input type=hidden name=t value=1>"
            "<input name=css value=x style=\"display:none\"><div hidden><input name=h2 value=y></div></form>"
            "<input name=extra form=s value=e><input name=lost form=nope>"
            "<form action=/token><input type=hidden name=csrf value=z></form>"
            "<form action=/next method=post><input type=hidden name=p value=2><input type=submit value=Next></form>";
        std::vector<std::string> l; std::vector<Form> f; std::vector<FieldRef> r;
        std::string o = prepare_html(h, "https://h.com/", "TK", l, f, r);
        expect(std::to_string(f.size()), "3", "three forms");
        if (f.size() == 3) {
            std::string names, vis;
            for (auto& fd : f[0].fields) { names += fd.name + ","; vis += fd.visible ? "1" : "0"; }
            expect(names + vis, "q,t,css,h2,extra,10001", "form= owner + hidden controls kept");
            expect(std::string(f[0].visible ? "1" : "0") + (f[1].visible ? "1" : "0") + (f[2].visible ? "1" : "0"),
                   "101", "form visibility (hidden-only form not shown, submit-only shown)");
        }
        if (o.find("[ Next ]") == std::string::npos || o.find("Query") == std::string::npos ||
            o.find(": x]") != std::string::npos || o.find(": y]") != std::string::npos) {
            std::cerr << "FAIL hidden placeholders: " << o << "\n"; fails++;
        }
    }
    {   // an unclosed hidden element must not hide the rest of the page
        std::vector<std::string> l; std::vector<Form> f; std::vector<FieldRef> r;
        std::string o = prepare_html("<div hidden>secret<p>after</p>", "https://h.com/", "TK", l, f, r);
        if (o.find("after") == std::string::npos) { std::cerr << "FAIL unclosed hidden: " << o << "\n"; fails++; }
        o = prepare_html("<div hidden><div>a</div><div>b</div></div>VISIBLE", "https://h.com/", "TK", l, f, r);
        if (o.find("VISIBLE") == std::string::npos || o.find(">a<") != std::string::npos) { std::cerr << "FAIL nested hidden: " << o << "\n"; fails++; }
    }
    {   // labels of rendered controls and plain labels stay; labels of dropped controls go
        std::vector<std::string> l; std::vector<Form> f; std::vector<FieldRef> r;
        std::string o = prepare_html("<form><label for=q>NameLbl</label><input id=q name=q></form>"
                                     "<label for=x>GoneLbl</label><input id=x><label>PlainLbl</label>",
                                     "https://h.com/", "TK", l, f, r);
        if (o.find("NameLbl") == std::string::npos || o.find("PlainLbl") == std::string::npos ||
            o.find("GoneLbl") != std::string::npos || o.find("TK:f0") == std::string::npos) {
            std::cerr << "FAIL labels: " << o << "\n"; fails++;
        }
    }
    {   // FIX(4): keyboard scrolling targets (doc 5000 px, view 500 px, scrollbar 16, line 40)
        auto T = [](int key, int st, FocusKind fk, int top) {
            return std::to_string(scroll_key_target(key, st, fk, top, 5000, 500, 16, 40));
        };
        const FocusKind P = FocusKind::Page, TI = FocusKind::TextInput, TA = FocusKind::TextArea, FC = FocusKind::FormControl;
        expect(T(FL_Page_Down, 0, P, 0), "444", "PageDown = visible height - one line");
        expect(T(FL_Page_Down, 0, TI, 0), "444", "PageDown from a single-line field scrolls");
        expect(T(FL_Page_Down, 0, FC, 0), "444", "PageDown from a form control scrolls");
        expect(T(FL_Page_Down, 0, TA, 0), "-1", "PageDown in a textarea is the textarea's");
        expect(T(FL_Page_Down, FL_CTRL, P, 0), "-1", "Ctrl+PageDown not ours");
        expect(T(FL_Page_Down, FL_NUM_LOCK, P, 0), "444", "NumLock ignored");
        expect(T(FL_Page_Up, 0, P, 1000), "556", "PageUp");
        expect(T(FL_Page_Up, 0, P, 100), "0", "PageUp clamps at 0");
        expect(T(FL_Page_Down, 0, P, 4400), "4516", "PageDown clamps at doc - visible");
        expect(T(' ', 0, P, 0), "444", "Space");
        expect(T(' ', FL_SHIFT, P, 1000), "556", "Shift+Space");
        expect(T(' ', 0, TI, 0), "-1", "Space types in a text field");
        expect(T(' ', 0, FC, 0), "-1", "Space presses a form button");
        expect(T(' ', FL_CTRL, P, 0), "-1", "Ctrl+Space not ours");
        expect(T(FL_Down, 0, P, 0), "40", "Down");
        expect(T(FL_Up, 0, P, 100), "60", "Up");
        expect(T(FL_Up, 0, P, 10), "0", "Up clamps");
        expect(T(FL_Down, 0, TI, 0), "-1", "Down in a text field is not ours");
        expect(T(FL_Down, 0, FC, 0), "-1", "Down on a form control is not ours");
        expect(T(FL_Down, FL_ALT, P, 0), "-1", "Alt+Down not ours");
        expect(T(FL_Home, 0, P, 900), "0", "Home");
        expect(T(FL_End, 0, P, 0), "4516", "End");
        expect(T(FL_Home, FL_CTRL, P, 900), "0", "Ctrl+Home");
        expect(T(FL_End, FL_CTRL, P, 0), "4516", "Ctrl+End");
        expect(T(FL_End, 0, TI, 0), "-1", "End in a text field moves the cursor");
        expect(T(FL_Left, 0, P, 0), "-1", "Left is not a scroll key");
        expect(T('a', 0, P, 0), "-1", "letters are not scroll keys");
        expect(std::to_string(scroll_key_target(FL_Page_Down, 0, P, 0, 300, 500, 16, 40)), "0", "short page stays at 0");
        expect(std::to_string(scroll_key_target(FL_End, 0, P, 0, 300, 500, 16, 40)), "0", "End on a short page");
        expect(std::to_string(scroll_key_target(FL_Page_Down, 0, P, 0, 5000, 30, 16, 40)), "40", "tiny view pages by a line");
    }
    std::cout << (fails ? "FAILED: " : "All tests passed: ") << fails << " failures\n";
    return fails ? 1 : 0;
}
