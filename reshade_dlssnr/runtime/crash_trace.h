// First-chance fault reporting for crashes that die inside a module we do not
// own.
//
// Everything the addon logs is written by the code that ran *before* the
// crash, so a fault inside nvngx_dlssnr.dll or the NGX core leaves a log that
// says what we were about to do and nothing about where it died. This closes
// that gap with one line naming the faulting instruction's module and offset,
// plus a short stack.
//
// Deliberately built on raw Win32 rather than the CRT or reshade::log: this
// runs while the process is already broken, so it must not take a lock that
// the faulting thread might be holding, and it must not allocate.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>

namespace reshade_dlssnr::crash_trace {

namespace detail {

// Bounded so a game that probes with exceptions cannot fill the disk.
constexpr int kMaxReports = 8;

inline std::atomic<int> g_reported{0};

// The terminate handler that was installed before us, so ours can chain.
inline std::terminate_handler g_previous_terminate = nullptr;

inline char* Append(char* out, char* end, const char* text) {
  while (*text != '\0' && out + 1 < end) *out++ = *text++;
  return out;
}

inline char* AppendHex(char* out, char* end, uint64_t value) {
  char digits[16];
  int count = 0;
  do {
    digits[count++] = "0123456789ABCDEF"[value & 0xF];
    value >>= 4;
  } while (value != 0 && count < 16);

  if (out + 2 + count >= end) return out;
  *out++ = '0';
  *out++ = 'x';
  while (count > 0) *out++ = digits[--count];
  return out;
}

inline char* AppendDec(char* out, char* end, uint32_t value) {
  char digits[10];
  int count = 0;
  do {
    digits[count++] = static_cast<char>('0' + (value % 10));
    value /= 10;
  } while (value != 0 && count < 10);

  if (out + count >= end) return out;
  while (count > 0) *out++ = digits[--count];
  return out;
}

// "module.dll+0x1234", or the bare address when nothing owns it. The offset is
// the whole point: it is what turns "somewhere in NGX" into a fact.
inline char* AppendModule(char* out, char* end, const void* address) {
  HMODULE module = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(address), &module) != 0 &&
      module != nullptr) {
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(module, path, MAX_PATH) != 0) {
      const wchar_t* name = path;
      for (const wchar_t* p = path; *p != L'\0'; ++p) {
        if (*p == L'\\') name = p + 1;
      }
      for (const wchar_t* p = name; *p != L'\0' && out + 1 < end; ++p) {
        *out++ = (*p < 128) ? static_cast<char>(*p) : '?';
      }
    }
    out = Append(out, end, "+");
    return AppendHex(out, end,
                     static_cast<uint64_t>(reinterpret_cast<uintptr_t>(address) -
                                           reinterpret_cast<uintptr_t>(module)));
  }

  out = Append(out, end, "unknown ");
  return AppendHex(out, end,
                   static_cast<uint64_t>(reinterpret_cast<uintptr_t>(address)));
}

inline void Write(const char* text) {
  wchar_t directory[MAX_PATH] = {};
  const DWORD length = GetTempPathW(MAX_PATH, directory);
  if (length == 0 || length >= MAX_PATH) return;

  wchar_t path[MAX_PATH] = {};
  for (DWORD i = 0; i < length && i + 1 < MAX_PATH; ++i) path[i] = directory[i];

  const wchar_t* name = L"dlssnr_crash.txt";
  DWORD at = 0;
  while (path[at] != L'\0') ++at;
  for (DWORD i = 0; name[i] != L'\0' && at + 1 < MAX_PATH; ++i) path[at++] = name[i];
  path[at] = L'\0';

  HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;

  DWORD written = 0;
  WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
  CloseHandle(file);
}

}  // namespace detail

inline LONG CALLBACK Handler(EXCEPTION_POINTERS* info) {
  if (info == nullptr || info->ExceptionRecord == nullptr) return EXCEPTION_CONTINUE_SEARCH;

  const DWORD code = info->ExceptionRecord->ExceptionCode;
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case 0xC0000409:  // STATUS_STACK_BUFFER_OVERRUN
      break;
    default:
      // 0xE06D7363 is a C++ throw and 0x406D1388 is a thread name; games also
      // use other codes for control flow. None of them are a crash.
      return EXCEPTION_CONTINUE_SEARCH;
  }

  if (detail::g_reported.fetch_add(1) >= detail::kMaxReports) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  char buffer[1024];
  char* out = buffer;
  char* const end = buffer + sizeof(buffer) - 2;

  out = detail::Append(out, end, "fault code=");
  out = detail::AppendHex(out, end, code);
  out = detail::Append(out, end, " at ");
  out = detail::AppendModule(out, end, info->ExceptionRecord->ExceptionAddress);

  if (code == EXCEPTION_ACCESS_VIOLATION &&
      info->ExceptionRecord->NumberParameters >= 2) {
    out = detail::Append(out, end, " (");
    out = detail::AppendDec(
        out, end,
        static_cast<uint32_t>(info->ExceptionRecord->ExceptionInformation[0]));
    out = detail::Append(out, end, " @ ");
    out = detail::AppendHex(
        out, end,
        static_cast<uint64_t>(info->ExceptionRecord->ExceptionInformation[1]));
    out = detail::Append(out, end, ")");
  }

  out = detail::Append(out, end, "\r\n  stack:");
  void* frames[8] = {};
  const USHORT count = RtlCaptureStackBackTrace(0, 8, frames, nullptr);
  for (USHORT i = 0; i < count; ++i) {
    out = detail::Append(out, end, "\r\n    ");
    out = detail::AppendModule(out, end, frames[i]);
  }

  // The registers, because the fault address alone does not say what the call
  // was. x64 passes the first four arguments in rcx/rdx/r8/r9, and for a COM
  // method rcx is `this` -- so a fault at `this + small offset` is only
  // readable if you can see whether `this` was null, and which object it was.
  out = detail::Append(out, end, "\r\n  ctx rip=");
  out = detail::AppendHex(out, end, info->ContextRecord->Rip);
  out = detail::Append(out, end, " rsp=");
  out = detail::AppendHex(out, end, info->ContextRecord->Rsp);
  out = detail::Append(out, end, " rcx=");
  out = detail::AppendHex(out, end, info->ContextRecord->Rcx);
  out = detail::Append(out, end, " rdx=");
  out = detail::AppendHex(out, end, info->ContextRecord->Rdx);
  out = detail::Append(out, end, " r8=");
  out = detail::AppendHex(out, end, info->ContextRecord->R8);
  out = detail::Append(out, end, " r9=");
  out = detail::AppendHex(out, end, info->ContextRecord->R9);
  out = detail::Append(out, end, " tid=");
  out = detail::AppendDec(out, end, GetCurrentThreadId());
  out = detail::Append(out, end, "\r\n");
  *out = '\0';

  detail::Write(buffer);

  // Never swallow it: the game's own handler still gets its turn, so this
  // changes nothing about how the crash is reported to the user.
  return EXCEPTION_CONTINUE_SEARCH;
}

// abort() and std::terminate reach the kernel through __fastfail, which
// bypasses vectored exception handling entirely -- so a crash of that kind
// leaves nothing in this file at all, which is exactly why the exit crash had
// to be caught with a debugger attached. A terminate handler does see it.
inline void OnTerminate() {
  char buffer[160];
  char* out = buffer;
  char* const end = buffer + sizeof(buffer) - 2;

  out = detail::Append(out, end, "terminate called (abort, not a fault) tid=");
  out = detail::AppendDec(out, end, GetCurrentThreadId());
  out = detail::Append(out, end, "\r\n");
  *out = '\0';
  detail::Write(buffer);

  // Chained, so the game's own handling and the usual abort still happen.
  if (detail::g_previous_terminate != nullptr) detail::g_previous_terminate();
}

// Idempotent. Called once from Attach().
inline void Install() {
  static bool installed = false;
  if (installed) return;
  installed = true;
  AddVectoredExceptionHandler(1, Handler);
  detail::g_previous_terminate = std::set_terminate(OnTerminate);
}

}  // namespace reshade_dlssnr::crash_trace
