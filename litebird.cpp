// LiteBird - Lightweight browser inspired by Ladybird's modular design
// Pure C++ + FLTK + libcurl. Minimal footprint for Ubuntu i386 live / v86.
//
// Ladybird ideas used (simplified):
// - Separation of networking from UI/rendering (NetworkFetcher ≈ RequestServer)
// - Thin BrowserEngine abstraction (so a stronger renderer can be swapped later)
// - Event-driven callbacks
// - Controlled loading of untrusted content with clear error paths

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Help_View.H>
#include <FL/Fl_Box.H>
#include <curl/curl.h>
#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

class NetworkFetcher {
public:
    static std::string fetch(const std::string& url) {
        CURL* curl = curl_easy_init();
        if (!curl) return errorPage("curl init failed");

        std::string response;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void* ptr, size_t size, size_t nmemb, void* user) -> size_t {
            static_cast<std::string*>(user)->append(static_cast<char*>(ptr), size * nmemb);
            return size * nmemb;
        });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "LiteBird/1.0 (Ladybird-inspired)");
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        CURLcode res = curl_easy_perform(curl);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
            return errorPage(std::string("Network: ") + curl_easy_strerror(res));
        if (code >= 400)
            return errorPage("HTTP " + std::to_string(code));
        if (response.empty())
            return errorPage("Empty body");
        return response;
    }

private:
    static std::string errorPage(const std::string& msg) {
        return "<html><head><title>Error</title></head><body style='font-family:sans-serif'>"
               "<h1>LiteBird</h1><p>" + msg + "</p>"
               "<p><a href='https://example.com'>example.com</a></p></body></html>";
    }
};

// Thin engine abstraction (inspired by the boundary between Ladybird's
// networking helpers and the WebContent / LibWeb renderer).
class BrowserEngine {
public:
    static std::string load(const std::string& url) {
        return NetworkFetcher::fetch(url);
    }
};

class BrowserUI : public Fl_Window {
public:
    BrowserUI(int w, int h)
        : Fl_Window(w, h, "LiteBird"), hist_idx_(-1) {
        begin();

        url_ = new Fl_Input(80, 8, w - 280, 28);
        url_->value("https://example.com");
        url_->when(FL_WHEN_ENTER_KEY_ALWAYS);
        url_->callback(cb_go, this);

        new Fl_Box(10, 8, 65, 28, "URL:");
        auto* go = new Fl_Button(w - 190, 8, 55, 28, "Go");
        go->callback(cb_go, this);
        auto* back = new Fl_Button(w - 130, 8, 55, 28, "Back");
        back->callback(cb_back, this);
        auto* fwd = new Fl_Button(w - 70, 8, 55, 28, "Fwd");
        fwd->callback(cb_fwd, this);

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
        show();
    }

    void navigate(const std::string& raw) {
        std::string url = raw;
        if (url.find("://") == std::string::npos)
            url = "https://" + url;

        status_->copy_label("Loading...");
        Fl::flush();

        // Engine boundary — currently just fetch; a stronger renderer
        // (or future LibWeb subset) would plug in here.
        std::string html = BrowserEngine::load(url);

#ifdef _WIN32
        char tmpdir[MAX_PATH];
        char tmpfile[MAX_PATH];
        if (GetTempPathA(MAX_PATH, tmpdir) && GetTempFileNameA(tmpdir, "lb", 0, tmpfile)) {
            std::string path = std::string(tmpfile) + ".html";
            if (MoveFileA(tmpfile, path.c_str())) {
                std::ofstream ofs(path);
                ofs << html;
                ofs.close();
                view_->load(path.c_str());
                DeleteFileA(path.c_str());
            } else {
                DeleteFileA(tmpfile);
                view_->value(html.c_str());
            }
        } else {
            view_->value(html.c_str());
        }
#else
        char tmpl[] = "/tmp/lbXXXXXX";
        int fd = mkstemp(tmpl);
        if (fd >= 0) {
            std::string path = std::string(tmpl) + ".html";
            close(fd);
            rename(tmpl, path.c_str());
            std::ofstream ofs(path);
            ofs << html;
            ofs.close();
            view_->load(path.c_str());
            unlink(path.c_str());
        } else {
            view_->value(html.c_str());
        }
#endif

        // history
        if (hist_idx_ >= 0 && hist_idx_ + 1 < (int)hist_.size())
            hist_.resize(hist_idx_ + 1);
        hist_.push_back(url);
        hist_idx_ = (int)hist_.size() - 1;
        url_->value(url.c_str());
        status_->copy_label(url.c_str());
    }

private:
    Fl_Input* url_;
    Fl_Help_View* view_;
    Fl_Box* status_;
    std::vector<std::string> hist_;
    int hist_idx_;

    static void cb_go(Fl_Widget*, void* v) {
        auto* self = static_cast<BrowserUI*>(v);
        self->navigate(self->url_->value());
    }
    static void cb_back(Fl_Widget*, void* v) {
        auto* self = static_cast<BrowserUI*>(v);
        if (self->hist_idx_ > 0) {
            self->hist_idx_--;
            self->navigate(self->hist_[self->hist_idx_]);
            self->hist_.pop_back();
            self->hist_idx_--;
        }
    }
    static void cb_fwd(Fl_Widget*, void* v) {
        auto* self = static_cast<BrowserUI*>(v);
        if (self->hist_idx_ + 1 < (int)self->hist_.size()) {
            self->hist_idx_++;
            std::string u = self->hist_[self->hist_idx_];
            self->hist_.pop_back();
            self->navigate(u);
        }
    }

    static const char* link_cb(Fl_Widget* w, const char* uri) {
        auto* self = static_cast<BrowserUI*>(w->user_data());
        if (!self || !uri) return uri;
        if (strncmp(uri, "http://", 7) == 0 || strncmp(uri, "https://", 8) == 0) {
            self->navigate(uri);
            return nullptr;
        }
        return uri;
    }
};

int main(int argc, char** argv) {
    curl_global_init(CURL_GLOBAL_ALL);
    BrowserUI win(920, 680);
    if (argc > 1)
        win.navigate(argv[1]);
    else
        win.navigate("https://example.com");
    int rc = Fl::run();
    curl_global_cleanup();
    return rc;
}
