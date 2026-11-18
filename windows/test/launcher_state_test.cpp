// launcher_state_test.cpp -- winapk-launcher-state-test: the launcher's state machines
// (launcher/app_runs.h, installs.h, install_names.h, shortcut_rules.h, clear_library.h, updates.h), which hold
// no lock and make no Windows call, driven step by step as the library's thread and a worker's thread drive them.
//
//   winapk-launcher-state-test
//
// An app's run: each path from start to end says the right thing (nothing, the splash goes, "didn't
// start", "stopped with an error"); Stop at each point of a run; steps out of order change nothing.
// An install: done, failed, left alone because the app runs; Cancel while it waits, while the file is
// copied, while the runtime converts it, once it is being kept and after its end; the queue's order.
// The update check and download: one check at a time; what each result leaves known and what the
// InfoBar says of it (once per release); a failed check keeps a newer version found before; the
// download's progress.
// The name a file installs under: free, the same package, another package, an unknown one; the
// generated name already taken.

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "../launcher/app_runs.h"
#include "../launcher/clear_library.h"
#include "../launcher/install_names.h"
#include "../launcher/installs.h"
#include "../launcher/shortcut_rules.h"
#include "../launcher/updates.h"

namespace {

int g_failures = 0;

void check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

using winapk::launcher::RunEnd;
using winapk::launcher::RunState;
using winapk::launcher::RunTable;

// Stands in for TerminateProcess: counts what it was asked to end, and says what it is told to.
struct Killer {
    std::vector<void*> killed;
    bool works = true;
    bool operator()(void* p) {
        if (works) killed.push_back(p);
        return works;
    }
};

int g_process_a, g_process_b;  // two handles' worth of addresses
void* const kA = &g_process_a;
void* const kB = &g_process_b;

RunEnd::Kind kind_of(RunTable& t, const std::wstring& name) {
    const auto end = t.finish(name);
    return end ? end->kind() : static_cast<RunEnd::Kind>(-1);
}

void test_runs() {
    std::printf("an app's run\n");
    const std::wstring app = L"app", other = L"other";
    {
        RunTable t;
        check("no run: not active, nothing to stop, nothing to finish",
              !t.active(app) && t.empty() && !t.state(app) && !t.finish(app) && !t.expect_end(app));
        check("a run begins starting", t.begin(app) && t.state(app) == RunState::kStarting && t.active(app));
        check("a second start of it is refused", !t.begin(app));
        check("it has no process yet", t.process(app) == nullptr && !t.stop_requested(app));
        check("the runtime started: running, not to be ended", !t.attach(app, kA) && t.state(app) == RunState::kRunning);
        check("its process is the one given", t.process(app) == kA);
        check("its window is up: ready", t.ready(app) && t.state(app) == RunState::kReady);
        check("it is not over until the process is", !t.finish(app) && t.active(app));
        check("the process exited: ended, the handle forgotten",
              t.ended(app, true) && t.state(app) == RunState::kEnded && t.process(app) == nullptr);
        check("still counted as running until the library hears", t.active(app) && !t.begin(app));
        check("closed by its user: nothing to say", kind_of(t, app) == RunEnd::kQuiet);
        check("and it is forgotten", !t.active(app) && t.empty() && t.begin(app));
    }
    {
        RunTable t;
        t.begin(app);
        t.attach(app, kA);
        t.ready(app);
        t.ended(app, false);
        check("an error after its window was up: failed", kind_of(t, app) == RunEnd::kFailed);
    }
    {
        RunTable t;
        t.begin(app);
        t.attach(app, kA);
        t.ended(app, false);
        const auto end = t.finish(app);
        check("an error before its window: did not start", end && end->kind() == RunEnd::kDidNotStart && !end->clean);
        t.begin(app);
        t.attach(app, kA);
        t.ended(app, true);
        const auto clean = t.finish(app);
        check("exit code 0 before its window: did not start, cleanly",
              clean && clean->kind() == RunEnd::kDidNotStart && clean->clean);
    }
    {
        RunTable t;
        t.begin(app);
        check("the runtime could not be started: ended from starting", t.ended(app, false));
        check("which is: did not start", kind_of(t, app) == RunEnd::kDidNotStart);
    }
    {  // Stop while the runtime is being started
        RunTable t;
        Killer kill;
        t.begin(app);
        check("stop while starting: taken, nothing to end yet", t.stop(app, kill) && kill.killed.empty());
        check("the run's thread sees it before it starts the runtime", t.stop_requested(app));
        t.ended(app, false);
        check("never started, by the user's word: stopped", kind_of(t, app) == RunEnd::kStopped);
    }
    {  // Stop lands between the thread's look and the runtime's start
        RunTable t;
        Killer kill;
        t.begin(app);
        const bool early = t.stop_requested(app);
        t.stop(app, kill);
        check("the thread saw no stop, then one came: the new runtime is to be ended at once",
              !early && t.attach(app, kA));
        check("it is recorded all the same", t.state(app) == RunState::kRunning && t.process(app) == kA);
        t.ended(app, false);
        check("its exit code 1 is the stop, not a failure", kind_of(t, app) == RunEnd::kStopped);
    }
    {  // Stop while it runs
        RunTable t;
        Killer kill;
        t.begin(app);
        t.attach(app, kA);
        check("stop before its window: the process is ended", t.stop(app, kill) && kill.killed == std::vector<void*>{kA});
        t.ended(app, false);
        check("stopped, not 'did not start'", kind_of(t, app) == RunEnd::kStopped);
        t.begin(app);
        t.attach(app, kB);
        t.ready(app);
        check("stop with its window up: the process is ended", t.stop(app, kill) && kill.killed.back() == kB);
        t.ended(app, false);
        check("stopped, not 'stopped with an error'", kind_of(t, app) == RunEnd::kStopped);
    }
    {  // A process that will not end
        RunTable t;
        Killer kill;
        kill.works = false;
        t.begin(app);
        t.attach(app, kA);
        t.ready(app);
        check("a process that would not end: not stopped", !t.stop(app, kill));
        t.ended(app, false);
        check("so its error is still told", kind_of(t, app) == RunEnd::kFailed);
    }
    {  // Stop after the end
        RunTable t;
        Killer kill;
        t.begin(app);
        t.attach(app, kA);
        t.ready(app);
        t.ended(app, false);
        check("stop after it ended: nothing to stop, nothing ended", !t.stop(app, kill) && kill.killed.empty());
        check("its error is still told", kind_of(t, app) == RunEnd::kFailed);
        t.begin(app);
        t.attach(app, kA);
        t.ended(app, false);
        check("an end the user asked for, heard of late, is expected", t.expect_end(app));
        check("and is no failure", kind_of(t, app) == RunEnd::kStopped);
    }
    {  // Ended by other means on the user's word: Remove, a permission taken away
        RunTable t;
        t.begin(app);
        t.attach(app, kA);
        t.ready(app);
        check("a running app whose runtime the launcher ends by other means: expected", t.expect_end(app));
        check("it is still running until its process is gone", t.state(app) == RunState::kReady);
        t.ended(app, false);
        check("its exit code 1 is no failure", kind_of(t, app) == RunEnd::kStopped);
    }
    {  // Steps out of order
        RunTable t;
        Killer kill;
        check("steps of a run that does not exist change nothing",
              !t.attach(app, kA) && !t.ready(app) && !t.ended(app, true) && !t.stop(app, kill) && t.empty());
        t.begin(app);
        check("ready before the runtime started: refused", !t.ready(app) && t.state(app) == RunState::kStarting);
        check("a runtime without a process: refused", !t.attach(app, nullptr) && t.state(app) == RunState::kStarting);
        t.attach(app, kA);
        check("a second runtime for the run: refused, the first kept", !t.attach(app, kB) && t.process(app) == kA);
        t.ready(app);
        check("ready twice: refused", !t.ready(app));
        t.ended(app, false);
        check("ended twice: refused, the first end kept", !t.ended(app, true));
        check("ready after the end: refused", !t.ready(app) && t.state(app) == RunState::kEnded);
        check("the first end is the one told", kind_of(t, app) == RunEnd::kFailed);
    }
    {  // Two apps
        RunTable t;
        Killer kill;
        t.begin(app);
        t.begin(other);
        t.attach(app, kA);
        t.attach(other, kB);
        t.ready(other);
        check("two runs are listed", t.names() == std::vector<std::wstring>{app, other});
        t.stop(app, kill);
        check("stopping one ends only its process", kill.killed == std::vector<void*>{kA});
        t.ended(app, false);
        t.ended(other, false);
        check("and excuses only its end", kind_of(t, app) == RunEnd::kStopped && kind_of(t, other) == RunEnd::kFailed);
    }
}

using winapk::launcher::InstallEnd;
using winapk::launcher::InstallState;
using winapk::launcher::InstallTable;

InstallEnd::Kind how(InstallTable& t, const std::wstring& name) {
    const auto end = t.finish(name);
    return end ? end->kind() : static_cast<InstallEnd::Kind>(-1);
}

void test_installs() {
    std::printf("an install\n");
    const std::wstring a = L"a", b = L"b", c = L"c";
    {
        InstallTable t;
        check("nothing queued: not pending, no current, nothing to finish",
              !t.pending(a) && t.empty() && t.current().empty() && !t.finish(a) && !t.state(a));
        check("an install is queued", t.begin(a) && t.state(a) == InstallState::kQueued && t.pending(a));
        check("the same file twice is refused", !t.begin(a));
        check("queued is not under way", t.current().empty());
        check("its turn: installing", t.start(a) && t.state(a) == InstallState::kInstalling && t.current() == a);
        check("the runtime started: not to be ended", !t.attach(a, kA));
        t.detach(a);
        check("the runtime succeeded: kept", t.commit(a) && t.state(a) == InstallState::kCommitted && t.current() == a);
        check("not over until its thread says so", !t.finish(a));
        check("ended", t.ended(a, true, false) && t.state(a) == InstallState::kEnded && t.current().empty());
        check("still pending until the library hears", t.pending(a) && !t.begin(a));
        check("it is done", how(t, a) == InstallEnd::kDone);
        check("and forgotten", !t.pending(a) && t.empty() && t.begin(a));
    }
    {
        InstallTable t;
        t.begin(a);
        t.start(a);
        t.attach(a, kA);
        t.detach(a);
        t.ended(a, false, false);
        check("the runtime failed: failed", how(t, a) == InstallEnd::kFailed);
        t.begin(a);
        t.start(a);
        t.ended(a, false, true);
        check("the app runs: left alone, and said so", how(t, a) == InstallEnd::kAppRunning);
        t.begin(a);
        t.start(a);
        t.attach(a, kA);
        t.detach(a);
        t.commit(a);
        t.ended(a, false, false);
        check("kept, but its name could not be written: failed", how(t, a) == InstallEnd::kFailed);
        t.begin(a);
        t.start(a);
        t.ended(a, true, false);
        check("ok without having been committed is not done", how(t, a) == InstallEnd::kFailed);
    }
    {  // Cancel while it waits for its turn
        InstallTable t;
        Killer kill;
        t.begin(a);
        check("cancel while queued: taken, nothing to end", t.cancel(a, kill) && kill.killed.empty());
        check("its turn comes: it does not start", !t.start(a) && t.state(a) == InstallState::kEnded);
        check("the thread's report changes nothing", !t.ended(a, false, false));
        check("cancelled", how(t, a) == InstallEnd::kCancelled);
    }
    {  // Cancel while the file is copied in, before the runtime
        InstallTable t;
        Killer kill;
        t.begin(a);
        t.start(a);
        check("cancel before the runtime: taken, nothing to end yet", t.cancel(a, kill) && kill.killed.empty());
        check("the runtime, started after it, is to be ended at once", t.attach(a, kA));
        t.detach(a);
        check("and the install is not kept", !t.commit(a) && t.state(a) == InstallState::kInstalling);
        t.ended(a, false, false);
        check("cancelled", how(t, a) == InstallEnd::kCancelled);
    }
    {  // Cancel while the runtime converts the app
        InstallTable t;
        Killer kill;
        t.begin(a);
        t.start(a);
        t.attach(a, kA);
        check("cancel under way: its runtime is ended", t.cancel(a, kill) && kill.killed == std::vector<void*>{kA});
        t.detach(a);
        t.ended(a, false, false);
        check("cancelled, not failed", how(t, a) == InstallEnd::kCancelled);
    }
    {  // The runtime succeeded, then Cancel: the cancel wins while the install is not kept yet
        InstallTable t;
        Killer kill;
        t.begin(a);
        t.start(a);
        t.attach(a, kA);
        t.detach(a);
        check("cancel after the runtime exited, before it is kept: taken", t.cancel(a, kill) && kill.killed.empty());
        check("so it is not kept", !t.commit(a));
        t.ended(a, true, false);
        check("cancelled, whatever the thread reports", how(t, a) == InstallEnd::kCancelled);
    }
    {  // Cancel once it is being kept
        InstallTable t;
        Killer kill;
        t.begin(a);
        t.start(a);
        t.attach(a, kA);
        t.detach(a);
        t.commit(a);
        check("cancel once it is being kept: refused", !t.cancel(a, kill));
        t.ended(a, true, false);
        check("it is done, never 'cancelled' with the app installed", how(t, a) == InstallEnd::kDone);
    }
    {  // Cancel after the end, before the library heard
        InstallTable t;
        Killer kill;
        t.begin(a);
        t.start(a);
        t.ended(a, false, false);
        check("cancel after the end: refused", !t.cancel(a, kill));
        check("its failure is still told", how(t, a) == InstallEnd::kFailed);
        check("cancel of one that is gone: refused", !t.cancel(a, kill));
        t.begin(a);
        check("the next install of that name starts: no cancel was left behind", t.start(a));
    }
    {  // The queue
        InstallTable t;
        Killer kill;
        t.begin(c);
        t.begin(a);
        t.begin(b);
        check("pending installs are listed in the order queued", t.names() == std::vector<std::wstring>{c, a, b});
        t.start(c);
        t.attach(c, kA);
        check("cancelling a waiting one ends nobody's runtime", t.cancel(a, kill) && kill.killed.empty());
        check("cancelling the one under way ends its own", t.cancel(c, kill) && kill.killed == std::vector<void*>{kA});
        t.detach(c);
        t.ended(c, false, false);
        t.start(a);
        t.start(b);
        t.attach(b, kB);
        check("the next one's runtime was not touched", kill.killed.size() == 1 && t.current() == b);
        check("what is left is listed", t.names() == std::vector<std::wstring>{c, a, b});
        check("each ends its own way", how(t, c) == InstallEnd::kCancelled && how(t, a) == InstallEnd::kCancelled);
        check("the queue shrinks", t.names() == std::vector<std::wstring>{b});
    }
    {  // Steps out of order, and an install in no table
        InstallTable t;
        Killer kill;
        check("an install in no table (the install helper's) cannot be cancelled: kept",
              t.commit(a) && !t.attach(a, kA) && !t.cancel(a, kill) && t.empty());
        check("steps of one that does not exist change nothing", !t.start(a) && !t.ended(a, true, false) && t.empty());
        t.begin(a);
        check("a runtime before its turn: refused", !t.attach(a, kA) && !t.commit(a));
        check("cancel does not reach a process that was refused", t.cancel(a, kill) && kill.killed.empty());
        t.start(a);
        t.finish(a);
        t.begin(b);
        t.start(b);
        check("its turn twice: refused", !t.start(b) && t.state(b) == InstallState::kInstalling);
        t.commit(b);
        check("kept twice: refused", !t.commit(b));
        t.ended(b, true, false);
        check("ended twice: refused, the first end kept", !t.ended(b, false, true) && how(t, b) == InstallEnd::kDone);
    }
}

struct TestRelease {
    std::wstring version;
};
using Tracker = winapk::launcher::UpdateTracker<TestRelease>;
using winapk::launcher::CheckResult;

// A check's result: GitHub asked or the kept answer read, by hand or not, and what came back.
CheckResult result(bool fetch, bool manual, bool reached, bool ok, bool found) {
    CheckResult r;
    r.fetch = fetch;
    r.manual = manual;
    r.reached = reached;
    r.ok = ok;
    r.found = found;
    return r;
}

void test_updates() {
    std::printf("the update check and download\n");
    using Say = Tracker::Say;
    using Known = Tracker::Known;
    const TestRelease v2{L"2.0"}, v3{L"3.0"}, same{L"1.0"};
    {
        Tracker t;
        check("at start: idle, nothing known, nothing to download",
              t.check() == Tracker::Check::kIdle && t.known() == Known::kNothing && !t.available() &&
                  !t.fetch_failed() && !t.downloading() && t.percent() == -1 && !t.begin_download());
        check("reading the kept answer is a check, but not asking",
              t.begin_check(false) && t.check() == Tracker::Check::kReadingKept && !t.asking());
        check("one check at a time", !t.begin_check(true) && t.check() == Tracker::Check::kReadingKept);
        check("no kept answer: nothing known, nothing said, no failure",
              t.checked(result(false, false, true, false, false), {}, false, true) == Say::kNothing &&
                  t.known() == Known::kNothing && !t.fetch_failed() && t.check() == Tracker::Check::kIdle);
        check("asking GitHub", t.begin_check(true) && t.asking());
        t.abort_check();
        check("a check whose thread did not start is over", t.check() == Tracker::Check::kIdle && t.begin_check(true));
        check("the daily check finds the same version: known, nothing said",
              t.checked(result(true, false, true, true, true), same, false, true) == Say::kNothing &&
                  t.known() == Known::kRelease && !t.available() && !t.fetch_failed());
        t.begin_check(true);
        check("'Check' finds the same version: up to date",
              t.checked(result(true, true, true, true, true), same, false, true) == Say::kUpToDate);
    }
    {
        Tracker t;
        t.begin_check(true);
        check("the daily check finds a newer version: said",
              t.checked(result(true, false, true, true, true), v2, true, true) == Say::kNewVersion && t.available() &&
                  t.latest().version == L"2.0");
        t.begin_check(true);
        check("the next check finds it again: said once only",
              t.checked(result(true, false, true, true, true), v2, true, true) == Say::kNothing && t.available());
        t.begin_check(true);
        check("'Check' finds it again: still nothing new to say, and not 'up to date'",
              t.checked(result(true, true, true, true, true), v2, true, true) == Say::kNothing);
        t.begin_check(true);
        check("a newer one still: said again",
              t.checked(result(true, false, true, true, true), v3, true, true) == Say::kNewVersion &&
                  t.latest().version == L"3.0");
    }
    {
        Tracker t;
        t.begin_check(false);
        check("the daily check off, a newer version in the kept answer: known, not said",
              t.checked(result(false, false, true, true, true), v2, true, false) == Say::kNothing && t.available());
        t.begin_check(true);
        check("'Check' with the daily check off: said",
              t.checked(result(true, true, true, true, true), v2, true, false) == Say::kNewVersion);
    }
    {
        Tracker t;
        t.begin_check(true);
        t.checked(result(true, false, true, true, true), v2, true, true);
        t.begin_check(true);
        check("GitHub not reached, by hand: could not check",
              t.checked(result(true, true, false, true, true), v2, true, true) == Say::kCouldNotCheck &&
                  t.fetch_failed());
        check("the newer version found before is still known", t.available() && t.latest().version == L"2.0");
        t.begin_check(true);
        check("GitHub not reached and nothing kept: failed, what was known stays",
              t.checked(result(true, false, false, false, false), {}, false, true) == Say::kNothing &&
                  t.fetch_failed() && t.known() == Known::kRelease && t.available());
        t.begin_check(false);
        check("reading the kept answer does not clear the failure",
              t.checked(result(false, false, true, true, true), v2, true, true) == Say::kNothing && t.fetch_failed());
        t.begin_check(true);
        check("GitHub answers again: no failure",
              t.checked(result(true, false, true, true, true), v2, true, true) == Say::kNothing && !t.fetch_failed());
        t.begin_check(true);
        check("an answer that cannot be read is a failure too",
              t.checked(result(true, true, true, false, false), {}, false, true) == Say::kCouldNotCheck);
        t.begin_check(true);
        check("a list without a release: none known, by hand 'up to date'",
              t.checked(result(true, true, true, true, false), {}, false, true) == Say::kUpToDate &&
                  t.known() == Known::kNoRelease && !t.available());
        t.begin_check(true);
        check("the release is back: news again",
              t.checked(result(true, false, true, true, true), v2, true, true) == Say::kNewVersion);
    }
    {
        Tracker t;
        t.progress(40);
        check("progress without a download changes nothing", !t.downloading() && t.percent() == -1);
        t.begin_check(true);
        t.checked(result(true, false, true, true, true), v2, true, true);
        check("the download starts at 0", t.begin_download() && t.downloading() && t.percent() == 0);
        check("one download at a time", !t.begin_download());
        t.progress(57);
        check("it moves", t.percent() == 57);
        check("a check may run meanwhile", t.begin_check(true) && t.downloading());
        t.checked(result(true, false, true, true, true), v2, true, true);
        check("and leaves the download alone", t.percent() == 57);
        t.downloaded();
        check("it ends", !t.downloading() && t.percent() == -1 && t.begin_download());
    }
    {
        using winapk::launcher::InstallerDownloaded;
        using winapk::launcher::installer_downloaded;
        check("a checked installer with the library shown runs",
              installer_downloaded(true, true) == InstallerDownloaded::kRun);
        check("a failed download with the library shown is reported",
              installer_downloaded(true, false) == InstallerDownloaded::kReport);
        check("a checked installer with the library hidden is kept, not run",
              installer_downloaded(false, true) == InstallerDownloaded::kKeep);
        check("a failed download with the library hidden is kept quiet too",
              installer_downloaded(false, false) == InstallerDownloaded::kKeep);
    }
}

// The library as a table of name -> package ("": held, package unknown); a name not in it is free.
struct Holders {
    std::map<std::wstring, std::string> held;
    winapk::launcher::NameHolder operator()(const std::wstring& n) const {
        const auto it = held.find(n);
        return it == held.end() ? winapk::launcher::NameHolder{} : winapk::launcher::NameHolder{true, it->second};
    }
};

void test_install_names() {
    std::printf("the name a file installs under\n");
    using winapk::launcher::choose_install_name;
    const std::wstring file = L"app-release";
    {
        Holders h;
        const auto c = choose_install_name(file, "com.a.one", h);
        check("a free name is used, as a new app", c.name == file && !c.update);
        check("so is one when the file's package cannot be read", choose_install_name(file, "", h).name == file);
    }
    {
        Holders h{{{file, "com.a.one"}}};
        const auto c = choose_install_name(file, "com.a.one", h);
        check("the same package is an update of that app", c.name == file && c.update);
    }
    {
        Holders h{{{file, "com.a.one"}}};
        const auto c = choose_install_name(file, "com.b.two", h);
        check("another package gets a name of its own, the package", c.name == L"com.b.two" && !c.update);
        h.held[L"com.b.two"] = "com.b.two";
        const auto again = choose_install_name(file, "com.b.two", h);
        check("that file again updates the app it became", again.name == L"com.b.two" && again.update);
        check("the first file still updates its own app", choose_install_name(file, "com.a.one", h).update);
    }
    {
        Holders h{{{file, "com.a.one"}, {L"com.b.two", "com.c.three"}}};
        const auto c = choose_install_name(file, "com.b.two", h);
        check("the package's name held by another app: numbered", c.name == L"com.b.two (2)" && !c.update);
        h.held[L"com.b.two (2)"] = "com.d.four";
        check("and the next free number", choose_install_name(file, "com.b.two", h).name == L"com.b.two (3)");
        h.held[L"com.b.two (3)"] = "com.b.two";
        const auto u = choose_install_name(file, "com.b.two", h);
        check("a numbered name of its own package is its update", u.name == L"com.b.two (3)" && u.update);
    }
    {
        Holders h{{{file, ""}}};
        const auto c = choose_install_name(file, "com.b.two", h);
        check("the holder's package unknown: never the same app", c.name == L"com.b.two" && !c.update);
    }
    {
        Holders h{{{file, "com.a.one"}}};
        const auto c = choose_install_name(file, "", h);
        check("the file's package unknown: numbered beside the name, never an update",
              c.name == file + L" (2)" && !c.update);
        h.held[file + L" (2)"] = "";
        check("and the next number", choose_install_name(file, "", h).name == file + L" (3)");
    }
    {
        Holders h{{{file, ""}}};
        const auto c = choose_install_name(file, "", h);
        check("both unknown: still not the same app", !c.update && c.name == file + L" (2)");
    }
    {
        Holders h{{{file, "com.a.one"}, {L"com.b.two", "x"}}};
        for (int n = 2; n <= 100; ++n) h.held[L"com.b.two (" + std::to_wstring(n) + L")"] = "x";
        check("no free name within reason: none, not an overwrite",
              choose_install_name(file, "com.b.two", h).name.empty());
    }
    {
        Holders h{{{file, "com.a.one"}}};
        for (const char* bad : {"..", "a/b:c", "x.", "a..b", "1abc"}) {
            const auto c = choose_install_name(file, bad, h);
            check("a package that is not a package name is treated as unreadable",
                  c.name == file + L" (2)" && !c.update);
        }
    }
}

void test_shortcut_rules() {
    using namespace winapk::launcher;
    check("the ProgID is ours", owns_association(L"WinApk.Package"));
    check("another program's type is not", !owns_association(L"Other.Handler"));
    check("an empty type is not", !owns_association(L""));

    const std::wstring exe = L"C:\\Users\\u\\AppData\\Local\\Programs\\Win.apk\\Win.apk.exe";
    const std::wstring run = L"--run \"C:\\lib\\a.apk\"";
    check("an app's shortcut to this launcher", is_own_app_shortcut(exe, run, exe));
    check("case and slashes do not matter",
          is_own_app_shortcut(L"c:/users/u/appdata/local/programs/win.apk/WIN.APK.EXE", run, exe));
    check("the launcher's own shortcut has no --run", !is_own_app_shortcut(exe, L"", exe));
    check("another program's shortcut", !is_own_app_shortcut(L"C:\\Windows\\notepad.exe", run, exe));
    check("another build of the launcher", !is_own_app_shortcut(L"D:\\dev\\Win.apk.exe", run, exe));
    check("a shortcut with no target", !is_own_app_shortcut(L"", run, exe));
    check("a different verb", !is_own_app_shortcut(exe, L"--runner x", exe));
    check("--run alone", is_own_app_shortcut(exe, L"--run", exe));

    check("version behind: refresh", shortcuts_need_refresh(0, 2, true));
    check("current and the folder there: no refresh", !shortcuts_need_refresh(2, 2, true));
    check("current but the folder gone (after an uninstall): refresh", shortcuts_need_refresh(2, 2, false));
    check("folder there: the user's desktop choice stands", refresh_wants_desktop(true, true, false) &&
                                                               !refresh_wants_desktop(true, false, true));
    check("folder gone: the setting decides", refresh_wants_desktop(false, false, true) &&
                                                  !refresh_wants_desktop(false, true, false));
}

void test_clear_outcome() {
    std::printf("clear library\n");
    using winapk::launcher::clear_outcome;
    const auto none = clear_outcome({});
    check("every app removed: success", none.ok && none.text == L"The library was cleared.");
    const auto one = clear_outcome({L"Chess"});
    check("one left: not a success, named, told to close it",
          !one.ok && one.text.find(L"Chess") != std::wstring::npos &&
              one.text.find(L"Close it") != std::wstring::npos && one.text.find(L"was cleared") == std::wstring::npos);
    const auto many = clear_outcome({L"A", L"B", L"C"});
    check("several left: counted, told to close them",
          !many.ok && many.text.find(L"3 apps") != std::wstring::npos &&
              many.text.find(L"Close them") != std::wstring::npos);
}

}  // namespace

int main() {
    test_runs();
    test_installs();
    test_updates();
    test_install_names();
    test_shortcut_rules();
    test_clear_outcome();
    std::printf("%d failed\n", g_failures);
    return g_failures ? 1 : 0;
}
