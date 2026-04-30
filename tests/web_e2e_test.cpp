// End-to-end test: real web_receiver.exe + real headless Chrome.
//
// Spawns web_receiver as a child process under a Windows Job Object so
// it gets cleaned up if this test crashes. Drives headless Chrome at
// the test page (which calls `new WebTransport(...)` against the
// receiver), captures the rendered DOM, asserts the page completed
// the datagram round-trip and the receiver logged a matching session.
//
// Exits 77 (skip) if Chrome isn't installed at the well-known path.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

constexpr uint16_t kSignalPort = 19080;
constexpr uint16_t kWtPort     = 19443;

std::string FindChrome() {
    static const char* kCandidates[] = {
        "C:/Program Files/Google/Chrome/Application/chrome.exe",
        "C:/Program Files (x86)/Google/Chrome/Application/chrome.exe",
        "C:/Program Files/Microsoft/Edge/Application/msedge.exe",
    };
    for (const char* p : kCandidates) {
        DWORD attrs = GetFileAttributesA(p);
        if (attrs != INVALID_FILE_ATTRIBUTES &&
            !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            return p;
        }
    }
    return {};
}

std::string ReceiverExePath() {
    char self[MAX_PATH];
    GetModuleFileNameA(nullptr, self, sizeof(self));
    std::string s = self;
    for (auto& c : s) if (c == '\\') c = '/';
    // .../build/tests/Debug/web_e2e_test.exe →
    // .../build/platform/windows/apps/web_receiver/Debug/web_receiver.exe
    auto cut = s.find("/build/");
    if (cut == std::string::npos) return {};
    std::string root = s.substr(0, cut + 6);  // include "/build"
    return root + "/platform/windows/apps/web_receiver/Debug/web_receiver.exe";
}

std::string TempPath(const char* tag) {
    char dir[MAX_PATH];
    GetTempPathA(sizeof(dir), dir);
    char p[MAX_PATH];
    GetTempFileNameA(dir, tag, 0, p);
    return p;
}

std::string SlurpFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

bool WaitForMarker(const std::string& path, const std::string& marker,
                    std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto s = SlurpFile(path);
        if (s.find(marker) != std::string::npos) return true;
        std::this_thread::sleep_for(100ms);
    }
    return false;
}

struct ChildJob {
    HANDLE job_handle = nullptr;
    HANDLE log_handle = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION pi = {};

    bool Spawn(const std::string& cmdline, const std::string& log_path,
                const std::string& workdir = {}) {
        // Job Object so the child dies when this process exits.
        job_handle = CreateJobObjectA(nullptr, nullptr);
        if (!job_handle) return false;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
        info.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_handle,
                                 JobObjectExtendedLimitInformation,
                                 &info, sizeof(info));

        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        log_handle = CreateFileA(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                  &sa, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log_handle == INVALID_HANDLE_VALUE) return false;

        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = log_handle;
        si.hStdError  = log_handle;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

        std::vector<char> mut(cmdline.begin(), cmdline.end());
        mut.push_back('\0');

        const char* wd = workdir.empty() ? nullptr : workdir.c_str();
        if (!CreateProcessA(nullptr, mut.data(), nullptr, nullptr, TRUE,
                              CREATE_SUSPENDED, nullptr, wd, &si, &pi)) {
            return false;
        }
        if (!AssignProcessToJobObject(job_handle, pi.hProcess)) {
            TerminateProcess(pi.hProcess, 1);
            return false;
        }
        ResumeThread(pi.hThread);
        return true;
    }

    void Kill() {
        if (job_handle) { CloseHandle(job_handle); job_handle = nullptr; }
        if (pi.hThread)  { CloseHandle(pi.hThread);  pi.hThread = nullptr;  }
        if (pi.hProcess) { CloseHandle(pi.hProcess); pi.hProcess = nullptr; }
        if (log_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(log_handle);
            log_handle = INVALID_HANDLE_VALUE;
        }
    }
};

int passed = 0;
int failed = 0;
#define CHECK(cond, msg) do { if (!(cond)) {                                   \
    std::fprintf(stderr, "FAIL [%s]: %s\n", __func__, msg); ++failed;          \
    return; }                                                                  \
} while (0)
#define CHECK_AND_DUMP(cond, msg) do { if (!(cond)) {                          \
    std::fprintf(stderr, "FAIL [%s]: %s\n", __func__, msg); ++failed;          \
    DumpFailureContext(recv_log, dom_path); return; }                          \
} while (0)

void DumpFailureContext(const std::string& recv_log,
                          const std::string& dom_path);

void test_browser_to_receiver_round_trip() {
    const std::string chrome = FindChrome();
    if (chrome.empty()) {
        std::fprintf(stderr,
                      "[skip] Chrome not found at well-known paths.\n");
        std::exit(77);
    }
    const std::string receiver = ReceiverExePath();
    CHECK(GetFileAttributesA(receiver.c_str()) != INVALID_FILE_ATTRIBUTES,
           ("receiver exe not found: " + receiver).c_str());
    std::printf("[e2e] receiver: %s\n", receiver.c_str());
    std::printf("[e2e] chrome:   %s\n", chrome.c_str());

    const std::string recv_log = TempPath("rcv");
    const std::string dom_path = TempPath("dom");

    // Working dir = receiver's exe dir (so test_page/ is resolvable).
    std::string recv_workdir = receiver.substr(0, receiver.find_last_of('/'));
    for (auto& c : recv_workdir) if (c == '/') c = '\\';

    std::string recv_cmd = "\"" + receiver + "\"" +
        " --signal-port=" + std::to_string(kSignalPort) +
        " --wt-port="     + std::to_string(kWtPort);

    ChildJob recv;
    CHECK(recv.Spawn(recv_cmd, recv_log, recv_workdir), "spawn web_receiver");
    std::printf("[e2e] spawned web_receiver, log=%s\n", recv_log.c_str());

    // Wait until web_receiver advertises ready.
    CHECK_AND_DUMP(WaitForMarker(recv_log, "Press Ctrl-C to stop", 10s),
                    "web_receiver did not start");

    // Drive headless Chrome at the test page. --virtual-time-budget
    // gives the page's setTimeout(4000) time to elapse so the DOM dump
    // captures post-roundtrip state. Spawn Chrome via CreateProcess so
    // we don't fight cmd.exe quoting around "Program Files".
    const std::string user_data = TempPath("cud");
    DeleteFileA(user_data.c_str());
    CreateDirectoryA(user_data.c_str(), nullptr);

    std::string chrome_bs = chrome;
    for (auto& c : chrome_bs) if (c == '/') c = '\\';

    // Drive Chrome on real wall-clock time (no --virtual-time-budget,
    // no --dump-dom) — the page encodes ~120 video frames in ~6s and
    // signals completion via a control-channel "done:..." datagram.
    // The receiver picks that up and prints FINAL_STATS, which we wait
    // for here.
    std::string chrome_cmd = "\"" + chrome_bs + "\""
        " --headless=new"
        " --user-data-dir=\"" + user_data + "\""
        " --use-fake-device-for-media-stream"
        " --use-fake-ui-for-media-stream"
        " --autoplay-policy=no-user-gesture-required"
        " \"http://localhost:" + std::to_string(kSignalPort) +
        "/sender/?frames=120&seconds=8\"";

    ChildJob chrome_job;
    CHECK_AND_DUMP(chrome_job.Spawn(chrome_cmd, dom_path), "spawn chrome");

    const bool finished = WaitForMarker(recv_log, "FINAL_STATS:", 25s);
    Sleep(500);
    chrome_job.Kill();

    const std::string log = SlurpFile(recv_log);

    CHECK_AND_DUMP(finished, "receiver never produced a FINAL_STATS line");
    CHECK_AND_DUMP(log.find("[wt] session established") != std::string::npos,
                    "receiver did not log a session");

    // Parse FINAL_STATS for the metrics. Format (one line):
    //   FINAL_STATS: dt_s=N video_received=N video_decoded=N
    //                video_decode_errors=N video_keyframes=N
    //                video_bytes=N audio_received=N audio_bytes=N
    //                video_recv_fps=N video_decode_fps=N audio_pkt_per_s=N
    auto extract = [&](const char* key) -> double {
        std::string m(key);
        m += "=";
        const auto p = log.find(m);
        if (p == std::string::npos) return -1;
        return std::strtod(log.c_str() + p + m.size(), nullptr);
    };
    const double video_received  = extract("video_received");
    const double video_decoded   = extract("video_decoded");
    const double video_keyframes = extract("video_keyframes");
    const double video_bytes     = extract("video_bytes");
    const double audio_received  = extract("audio_received");
    const double audio_bytes     = extract("audio_bytes");
    const double video_recv_fps  = extract("video_recv_fps");
    const double video_dec_fps   = extract("video_decode_fps");
    const double audio_pkt_per_s = extract("audio_pkt_per_s");

    std::printf("[e2e] FPS — video: received=%.0f decoded=%.0f keyframes=%.0f "
                 "bytes=%.0f recv_fps=%.2f decode_fps=%.2f\n",
                 video_received, video_decoded, video_keyframes, video_bytes,
                 video_recv_fps, video_dec_fps);
    std::printf("[e2e]       audio: received=%.0f bytes=%.0f pkt_per_s=%.2f\n",
                 audio_received, audio_bytes, audio_pkt_per_s);

    CHECK_AND_DUMP(video_received >= 60,
                    "fewer than 60 video chunks received");
    CHECK_AND_DUMP(video_keyframes >= 1, "no keyframes received");
    CHECK_AND_DUMP(audio_received >= 100,
                    "fewer than 100 audio packets received");
    CHECK_AND_DUMP(video_recv_fps >= 5.0, "video recv fps below threshold");
    // Decoded count may be 0 on a host without an MFT decoder. When
    // decoder is active, allow up to 5 frames lost (queue tail drained
    // late etc).
    if (video_dec_fps > 0) {
        CHECK_AND_DUMP(video_decoded >= video_received - 5,
                        "decoder dropped more than 5 frames");
    }

    std::printf("[e2e] OK — browser ↔ receiver round-trip\n");

    recv.Kill();
    DeleteFileA(recv_log.c_str());
    DeleteFileA(dom_path.c_str());
    RemoveDirectoryA(user_data.c_str());
    ++passed;
    return;

    // (unreachable) — on failure the CHECK macro returns without
    // reaching the cleanup above, leaving recv_log + dom_path on disk
    // for diagnosis. Uncomment to keep the temp dir too.
}

void DumpFailureContext(const std::string& recv_log,
                          const std::string& dom_path) {
    std::fprintf(stderr, "\n[e2e] receiver log =====\n%s\n",
                  SlurpFile(recv_log).c_str());
    std::fprintf(stderr, "\n[e2e] dom dump =====\n%s\n",
                  SlurpFile(dom_path).c_str());
}

}  // namespace

int main() {
    test_browser_to_receiver_round_trip();
    std::printf("web_e2e_test: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
