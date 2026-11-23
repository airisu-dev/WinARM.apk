# WinARM.apk porting notes

Why the Windows on Arm port is built the way it is, the facts it depends on,
and its known gaps. [`README.md`](README.md) covers building and running.

## 1. Scope

WinARM.apk runs Android apps on Windows 11 on Arm as ordinary Windows
processes, with no emulator and no virtual machine, by porting the
[Mac.apk](https://github.com/kksimp/Mac.apk) runtime. It is based on
[israel2176/win.apk](https://github.com/israel2176/win.apk), which also targets
x64 PCs. Licensed under AGPL-3.0, like upstream.

Out of scope: x64 PCs (use israel2176/win.apk there), apps whose native code is only
`x86_64` or only `armeabi-v7a` (Windows on Arm CPUs have no AArch32), and
libraries whose use of x18 cannot be rewritten (section 3, D6).

## 2. From Mac.apk to Windows on Arm

| Upstream part | Location | On Windows on Arm |
|---|---|---|
| Android framework, DEX-to-JVM converter, boot loader | `runtime/java/` | Used as is on a stock HotSpot, with the hooks listed in `README.md` |
| Native runtime (JVM host, framework natives, Cocoa window, ANGLE glue) | `runtime/src/` | Replaced by `windows/host/`; two files are compiled from upstream (the GL bridges and `native_sqlite.cpp`) |
| arm64 ELF loader and Bionic shim | `runtime/translator/` | Replaced by `windows/src/`; upstream's is the behavioural reference |
| Swift launcher | `launcher/` | Replaced by `windows/launcher/` |

On Apple Silicon an app's arm64 `.so` runs directly. The same holds here:
Windows ARM64 and AAPCS64 agree on fixed arguments, so host functions bind
directly to a library's imports and HotSpot calls guest natives through thin
thunks. What differs from the Mac:

- **x18** is the TEB pointer on Windows on Arm; Android code may use it as a
  scratch register. Libraries that write it are rewritten at load to use a
  per-thread virtual x18, or refused if they cannot be
  (`include/winapk/arm64_x18_rewrite.h`).
- **TPIDR_EL0** is free for user code on Windows. Bionic keeps its TLS slots
  there (the stack guard is slot 5), so each thread gets a block on its first
  entry into guest code (`src/arm64/arm64_native.cpp`).
- **`svc #0`** would be a Windows system call. The loader patches it to a
  `brk` the shim answers as the Linux call.
- **Variadic floating-point arguments**: Windows passes them in integer
  registers, AAPCS64 in v0-v7. The variadic entry stubs spill both under an
  AAPCS64 `va_list` (`src/abi_thunk_arm64.cpp`).
- **Struct layouts, open flags and syscall numbers** arm64 Linux has
  differently from the shim's: `src/arm64/arm64_abi.cpp` binds those imports
  to wrappers.
- The **Bionic shim** maps Android libc onto the UCRT and Win32, and Mac
  services (Cocoa, Metal, AVFoundation, WKWebView, ...) have Windows
  counterparts in `windows/host/`.

## 3. Decisions

D1. **Soft fork.** `main` shares Mac.apk's history and merges its releases.
Windows code lives in `windows/` and other paths upstream does not have; an
upstream file changes only for a hook Windows needs or a framework fix worth
offering upstream, and every such change is listed in `README.md`.

D2. **Clang only**, MSYS2 CLANGARM64, Temurin 21 aarch64. CMake refuses any
target but ARM64.

D3. **Copy ELF segments into one `VirtualAlloc` region.** `MapViewOfFile`
needs file offsets aligned to 64 KiB; ELF segments are aligned to 4 or 16 KiB.

D4. **The guest gets its own `JNIEnv` and `JavaVM`**, generated from `jni.h`
(`src/guest_jni.cpp`, `jni_slots.inc`), so every call can be checked and the
variadic forms bridged.

D5. **Guest code enters the system in three places only**: the loader
(`ElfImage`), the linker's image registry (`GuestLinker`) and native-method
binding (`jni_bind`). New code must not call guest functions around them.

D6. **Refuse what cannot run, and say why**: an x86_64 or armeabi-v7a library, or
one that writes x18 and cannot be rewritten, fails at load with a message the user
sees (the launcher's install and start errors, the run log). A library that writes
x18 gets a virtual x18: each instruction that names x18 jumps to a thunk in a pool
after the image that runs it on a borrowed register, taken from and put back in a
per-thread TEB slot (`arm64_x18_rewrite.h`). x18 is never changed. The rewrite is all
or nothing and patches only words that are code: inside `.eh_frame` functions, not
read as a literal, and reachable (a function with an indirect `br` takes every block
start as reachable). A data word right after an unconditional branch in such a
function is told from code only by its block being plausible code.
Words naming x18 outside every `.eh_frame` function that are not text make a library
unloadable unless the code outside the tables is read too (Unity's `libunity.so` carries
physics and math code built without unwind info). Then each run of valid words between the
ranges (no reserved word, no two printable-ASCII words in a row), cut where a call lands
and at a frame-making first word after `ret`, `b` or `br`, is a function of its own, read
as one with an indirect `br`, except that a word after its last `ret`, `b` or `br` is not code (it may
be a constant loaded through `adrp`) unless the run goes on into a function the tables describe or into
words that only look like a string; it is kept only if that settles every word. A function that
is a call target is not joined to the functions that branch to it (a tail call).
A library with no x18 instruction in its `.eh_frame` functions, whose ranges cover 80% or
more of its code, is loaded unchanged only if no code outside them that a call reaches writes x18
(the runs between the ranges are read as above; a word that follows the last return of such a function,
or that only reads x18, does not count). What names x18 outside the ranges is then a table in `.text`
(OpenSSL's `ecp_nistz256_precomputed`, SHA-256 constants), and OpenSSL's own assembly
does not use x18. Code without unwind info that no call reaches (a virtual call, a jump table) and that
clobbers x18 would not be caught.
`blr x18` becomes `bl thunk` (x16 may be clobbered by a call). After a `br x18` nothing
can restore a borrowed register, so its thunk clobbers one that is dead wherever the
branch may land: a register of x9..x17, then x8..x0, then the link register (the last
only for a table dispatch, an indexed load then an add of the base, never for a tail
call through a pointer). The planner reads each function with the functions joined to
it by direct non-call branches (a hot/cold split, a tail call) and takes the block
starts as the landing points; a register is dead at one if the straight-line code from
it writes the register before reading it, or reaches a call; a word it does not fully
decode, a return (x0, x1 and the link register), a tail call or a branch that is not a
dispatch (the arguments and the link register) count as reads. No register left: the
library is refused. A hand-written routine that passes a value in x9..x15 across a call
is the one case this cannot see. `adr` and `adrp` into x18 are rebuilt with `adrp`+`add`
from the thunk's address; the image and the pool are one page-aligned allocation, so
the page distance is the same as in the file.
`WINAPK_ARM64_X18_REWRITE=0` refuses such a library instead, and
`WINAPK_ARM64_ALLOW_X18=1` loads it unchanged, for investigation.

D7. **A closing window unmaps no guest library.** The app's teardown (onDestroy, then the JVM's class unloading) can dlclose
libunity.so while its render and worker threads still run in it, and a thread that returns into the unmapped image ended
the process with 0xC0000005 instead of 0 (no crash line: the fault was in memory no module holds). From the first
close request `GuestLinker::retain_libraries()` makes dlclose drop its reference and nothing more: no finalizers, no
unmapping. The process ends within the teardown cap anyway, and Android does not unmap at exit either.

## 4. Technical findings

### Loader and ABI

- Code built for Linux does not probe its stack page by page as Windows code
  must: a frame larger than a page writes past the guard page, and Windows
  cannot deliver that fault. Guest threads get 1 MB of their stack committed
  before guest code runs (`src/guest_stack.cpp`).
- lld emits Android APS2 relocations with `--pack-dyn-relocs=android+relr`;
  the loader decodes them as bionic's `packed_reloc_iterator` does, and RELR.
- IRELATIVE resolvers are guest code: they run after the text is executable,
  with bionic's `(AT_HWCAP | _IFUNC_ARG_HWCAP, &arg)`.
- Test libraries need no NDK: plain clang + lld with `-nostdlib` and
  hand-declared libc imports (`test/build_guest.sh`).

### JNI

- JDK 21's `JNINativeInterface_` has 235 slots: 4 reserved, 169 with fixed
  arguments, the rest the `...` and `va_list` families.
- Variadic JNI arguments follow C promotion: boolean, byte, char, short and
  int arrive in an integer slot, float as a double.
- JNI cannot give a method's signature from a `jmethodID`; the variadic bridge
  asks JVMTI.
- JVMTI `NativeMethodBind` fires for name-bound natives and for
  `RegisterNatives`; the bound address is replaced with a thunk built from
  the signature.
- Other aarch64 JDKs' `jni_md.h` make `jint` a `long`; Temurin's is `int`.

### Linker

- Bionic's dlopen flags on LP64: `RTLD_LAZY` 1, `RTLD_NOW` 2, `RTLD_LOCAL` 0,
  `RTLD_GLOBAL` 0x100.
- When a library is linked, bionic searches the global group, then the
  library and its `DT_NEEDED` tree in breadth-first order.
- Bionic's `dladdr` names a symbol only when `[value, value + size)` covers
  the address.
- Guest paths are UTF-8; build `std::filesystem::path` with `u8path`.

### JVM hosting

- HotSpot reads JVM option strings in the ANSI code page; the runtime's
  manifest makes it UTF-8.
- `-Xss` does not apply to the thread that creates the VM, so the runtime
  creates it on a thread with a large stack.
- A vectored exception handler must pass on what it does not own, so
  HotSpot's own handling keeps working.
- `System.mapLibraryName("x")` is `x.dll`, which matters to SoLoader.

### Android paths

An app assumes a POSIX filesystem. One table decides where a guest path lives
(`src/bionic/paths.cpp` for native code, the java.base overlay's
`jdk.internal.util.WinApkPaths` for java.io and java.nio): the app sees
`/data/user/0/<pkg>` and `/storage/emulated/0`, the host opens the folder
under `%LOCALAPPDATA%\Win.apk\android`. File names with characters NTFS
forbids are escaped.

The APK is the one file an app is told by its Android name:
`getPackageCodePath()` is `/data/app/~~<h>/<pkg>-<h>/base.apk`, while
`ApplicationInfo.sourceDir` stays the host path the ELF and dex loaders open.
Unity's player indexes the APK with `fopen`/`fseek`/`fread` and serves
`<code path>/assets/...` from that index only when the path starts with `/`;
with a drive-letter path it `stat`s the entry as a plain file, so
`unity_app_guid` and il2cpp's `global-metadata.dat` could not be read.

## 5. Upstream behaviour the port preserves

- `com.macapk.loader.AppClassLoader.findLibrary` is the single funnel for
  native libraries; it returns null when the APK lacks the library, so the
  app gets `UnsatisfiedLinkError` as on Android.
- HotSpot loads what `findLibrary` returns with `LoadLibraryW` and keys
  libraries by path: each guest library gets its own copy of a stub DLL
  (`host/guest_stub.cpp`, `host/guest_libs.cpp`) whose `JNI_OnLoad` loads the
  ELF image and calls the guest's with the guest `JavaVM`.
- `Java_*` natives are bound lazily through `ClassLoader.findNative`, which
  searches only libraries registered with the declaring class's loader.
- `FindClass` tries the app class loader without initializing the class, then
  the `JNI_OnLoad` context loader, then HotSpot's `FindClass`.
- `DT_NEEDED` dependencies run their constructors, not their `JNI_OnLoad`;
  guest `dlopen` and `NativeActivity` never run `JNI_OnLoad`.
- Native threads created by guest code are not attached to the JVM
  automatically.

## 6. Known gaps

- The thunk pool is mapped read-write-execute and has no unwind information.
- Apps that ship only `x86_64` or only `armeabi-v7a` code do not run; neither do
  libraries whose use of x18 cannot be rewritten. A rewritten x18 instruction costs
  four to six extra instructions and two TEB accesses.
- Some shim answers still describe an x86_64 device (system properties such
  as `ro.product.cpu.abi`, `uname`'s machine); arm64 apps get arm64 layouts
  through `arm64_abi.cpp`, but these values are to be checked.
- Signals are delivered for faults and self-sent signals
  (`src/bionic/signal.cpp`); POSIX timers are recorded only. A signal to a thread
  asleep in a futex, semaphore or mutex wait wakes it to run the handler (Boehm
  GC's suspend signal, which Unity's il2cpp needs to start). The wake is
  `WakeByAddressAll` on the address the thread waits on (`wait_on_address`):
  `NtAlertThreadByThreadId` leaves the waiter's wait block linked in the
  WaitOnAddress hash bucket, and the next wait at that stack address turned the
  bucket into a cycle every later wait spun on. A thread inside ntdll, kernel32,
  kernelbase, win32u or user32 (not at a `svc`) is not redirected, since it may
  hold a lock the handler would stall; other code, the host CRT included, is.
  The module table is read before the target is suspended (it may hold the
  loader lock). The retry is bounded to 10 ms; a signal still dropped (EAGAIN)
  is noted once on stderr. A handler that waits keeps the outer wait wakeable
  (`wait_on_address` restores the address it found).
- Raw signal calls an arm64 library makes (`syscall()` or `svc`: tkill, tgkill,
  rt_sigqueueinfo, rt_tgsigqueueinfo) deliver as the shim's `tkill`, `tgkill`
  and `kill` do, to this process only; the queued `siginfo` is dropped, so the
  handler sees `SI_TKILL`. A crash reporter's handler (Crashlytics) ends by
  queueing the fault's signal again with the old action restored, and calls
  `_exit(191)` when that call fails: before these were answered it showed up
  as "guest called _exit(191)" after a `fork()` that failed with ENOSYS (the
  reporter starts its crash-handler process first and carries on without it), and
  hid the app's own fault.
- `process_vm_readv` / `process_vm_writev` (libc functions, `syscall()` 270/271 on arm64, 310/311 for
  the shim's x86_64 numbers) work on this process only (`ESRCH` for another pid, `EINVAL` for flags or more
  than 1024 segments) and answer an unreadable address with `EFAULT`, or the bytes copied before it, as
  Linux does (`Read`/`WriteProcessMemory` on the process itself). Unity's crash reporter reads 256 bytes
  around each register with them and, when they fail with `ENOSYS`, copies by hand: with a register
  holding 0 that faulted inside the signal handler (`ucrtbase` memmove, NULL source) and ended the
  process before Unity's own report, and anything after it, could finish.
- A per-thread Choreographer is driven by a 60 Hz timer through
  `NativeLooper.scheduleVsync`/`takeVsync` (`host/native_looper.cpp`); Unity's
  "UnityChoreographer" thread paces `UnityMain` with it. `/proc/cpuinfo` is
  synthesized (`stdio.cpp`; Unity counts its cores from it), and a path near
  MAX_PATH reaches the guest's file calls in its `\?\` form (`paths.cpp`).
- On a GPU, GL frames, the view tree and SurfaceView frames reach the window as
  DirectComposition layers (`host/gl/gl_present.h`); without one (WARP,
  SwiftShader) or with "Enable GPU rendering" off, the window is painted from a
  readback of every frame, as it is for the rest of a run once the GPU device
  is lost (a driver reset). CI has no GPU, so only the readback runs there.
  Partial redraw (`MACAPK_FRAMESPEED_DAMAGE=1`) is off by default.
- No in-app purchases, Play Integrity or Vulkan; OpenSL ES is a stub.
