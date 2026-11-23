// native_looper.cpp -- com.macapk.loader.NativeLooper on Windows.
//
// Java's android.os.Looper blocks in its thread's NDK looper (the shim's
// ALooper), so native descriptors a library registered with ALooper_addFd on
// that thread are serviced by the same loop that delivers its Java messages,
// as on a device where both are one Looper. Handler.enqueue wakes it.

#include "host_natives.h"

#include "winapk/mini_bionic.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <windows.h>

namespace winapk::host {

namespace {

void* looper(jlong nl) { return reinterpret_cast<void*>(nl); }

jlong JNICALL Looper_forCurrentThread(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(winapk::looper_for_current_thread());
}
void JNICALL Looper_pollOnce(JNIEnv*, jclass, jlong nl, jint timeout) {
    if (nl) winapk::looper_poll_once(looper(nl), timeout);
}
void JNICALL Looper_pollPlainFds(JNIEnv*, jclass, jlong nl) {
    if (nl) winapk::looper_poll_once(looper(nl), 0);
}
void JNICALL Looper_wake(JNIEnv*, jclass, jlong nl) {
    if (nl) winapk::looper_wake(looper(nl));
}
// A main thread whose looper has native descriptors (an engine's display
// looper) runs that looper here for good; any other returns at once.
jint JNICALL Looper_runMainPoll(JNIEnv*, jclass, jlong nl) {
    if (!nl || !winapk::looper_has_fds(looper(nl))) return 0;
    for (;;) winapk::looper_poll_once(looper(nl), -1);
}
// ---- display vsync for a per-thread Choreographer -------------------------------------------------
// AOSP's Choreographer asks its thread's Looper for ONE vsync (DisplayEventReceiver.scheduleVsync) and runs
// doFrame when the Looper hands it back (takeVsync). Unity's "UnityChoreographer" thread is such a Choreographer,
// and its doFrame is the only writer of the frame counter UnityMain waits on, so a vsync that never comes leaves
// the game on its first screen. One timer thread, running only while a request is pending, answers every request
// at the display's rate; the Display class reports 60 Hz, so that is the interval.
constexpr int64_t kVsyncIntervalNs = 1000000000LL / 60;

struct VsyncSlot {
    bool requested = false;     // scheduleVsync called, no tick since
    bool delivered = false;     // a tick answered it, takeVsync has not taken it
    int64_t frame_time_ns = 0;  // that tick's System.nanoTime()
};
// Leaked: the timer thread can still run while static destructors do.
std::mutex& vsync_lock() {
    static auto* m = new std::mutex;
    return *m;
}
std::condition_variable& vsync_wanted() {
    static auto* c = new std::condition_variable;
    return *c;
}
std::unordered_map<void*, VsyncSlot>& vsync_slots() {
    static auto* s = new std::unordered_map<void*, VsyncSlot>;
    return *s;
}

// System.nanoTime() as HotSpot computes it on Windows, so a stamp is in the timebase doFrame's contract names.
int64_t java_nano_time() {
    static const double frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<int64_t>(static_cast<double>(c.QuadPart) / frequency * 1e9);
}

void vsync_timer_thread() {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    int64_t next = java_nano_time();
    for (;;) {
        {
            std::unique_lock<std::mutex> hold(vsync_lock());
            vsync_wanted().wait(hold, [] {
                for (const auto& [looper, slot] : vsync_slots()) {
                    if (slot.requested) return true;
                }
                return false;
            });
        }
        // A tick on the grid of the last one, or a new grid after an idle stretch.
        const int64_t now = java_nano_time();
        next = next + kVsyncIntervalNs > now ? next + kVsyncIntervalNs : now + kVsyncIntervalNs;
        LARGE_INTEGER due;
        due.QuadPart = -(next - now) / 100;  // relative, in 100 ns
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep(static_cast<DWORD>((next - now) / 1000000));
        }
        std::vector<void*> woken;
        {
            std::lock_guard<std::mutex> hold(vsync_lock());
            const int64_t stamp = java_nano_time();
            for (auto& [looper, slot] : vsync_slots()) {
                if (!slot.requested) continue;
                slot.requested = false;
                slot.delivered = true;
                slot.frame_time_ns = stamp;
                woken.push_back(looper);
            }
        }
        for (void* l : woken) winapk::looper_wake(l);
    }
}

void JNICALL Looper_scheduleVsync(JNIEnv*, jclass, jlong nl) {
    if (!nl) return;
    static const bool started = [] {
        std::thread(vsync_timer_thread).detach();
        return true;
    }();
    (void)started;
    {
        std::lock_guard<std::mutex> hold(vsync_lock());
        vsync_slots()[looper(nl)].requested = true;
    }
    vsync_wanted().notify_one();
}
// -1 when none is pending; android.os.Looper calls it with a null looper to learn that the natives are bound.
jlong JNICALL Looper_takeVsync(JNIEnv*, jclass, jlong nl) {
    if (!nl) return -1;
    std::lock_guard<std::mutex> hold(vsync_lock());
    const auto it = vsync_slots().find(looper(nl));
    if (it == vsync_slots().end() || !it->second.delivered) return -1;
    it->second.delivered = false;
    return static_cast<jlong>(it->second.frame_time_ns);
}

constexpr const char* kCls = "com.macapk.loader.NativeLooper";

const HostMethod kMethods[] = {
    WINAPK_HOST_NATIVE(kCls, "forCurrentThread", "()J", Looper_forCurrentThread),
    WINAPK_HOST_NATIVE(kCls, "pollOnce", "(JI)V", Looper_pollOnce),
    WINAPK_HOST_NATIVE(kCls, "pollPlainFds", "(J)V", Looper_pollPlainFds),
    WINAPK_HOST_NATIVE(kCls, "wake", "(J)V", Looper_wake),
    WINAPK_HOST_NATIVE(kCls, "runMainPoll", "(J)I", Looper_runMainPoll),
    WINAPK_HOST_NATIVE(kCls, "scheduleVsync", "(J)V", Looper_scheduleVsync),
    WINAPK_HOST_NATIVE(kCls, "takeVsync", "(J)J", Looper_takeVsync),
};

}  // namespace

HostMethods native_looper_methods() { return {kMethods, sizeof(kMethods) / sizeof(kMethods[0])}; }

}  // namespace winapk::host
