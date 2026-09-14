#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <string>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    std::wstring command;
    for (int i = 1; i < argc; ++i) { if (i > 1) command += L' '; command += L'"'; command += argv[i]; command += L'"'; }
    STARTUPINFOW si = {sizeof(si)}; PROCESS_INFORMATION pi{};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, DEBUG_ONLY_THIS_PROCESS | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return 2;
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(pi.hProcess, nullptr, FALSE);
    DEBUG_EVENT ev{};
    while (WaitForDebugEvent(&ev, INFINITE)) {
        DWORD action = DBG_CONTINUE;
        if (ev.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            wchar_t name[32768] = {};
            GetFinalPathNameByHandleW(ev.u.CreateProcessInfo.hFile, name, 32768, FILE_NAME_NORMALIZED);
            DWORD64 loaded = SymLoadModuleExW(pi.hProcess, ev.u.CreateProcessInfo.hFile, name, nullptr, (DWORD64)ev.u.CreateProcessInfo.lpBaseOfImage, 0, nullptr, 0);
            fprintf(stderr, "image base=%p symbols=%llx error=%lu\n", ev.u.CreateProcessInfo.lpBaseOfImage, loaded, GetLastError());
            if (ev.u.CreateProcessInfo.hFile) CloseHandle(ev.u.CreateProcessInfo.hFile);
        } else if (ev.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
            wchar_t name[32768] = {};
            GetFinalPathNameByHandleW(ev.u.LoadDll.hFile, name, 32768, FILE_NAME_NORMALIZED);
            SymLoadModuleExW(pi.hProcess, ev.u.LoadDll.hFile, name, nullptr, (DWORD64)ev.u.LoadDll.lpBaseOfDll, 0, nullptr, 0);
            if (ev.u.LoadDll.hFile) CloseHandle(ev.u.LoadDll.hFile);
        } else if (ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            auto& ex = ev.u.Exception;
            if (ex.ExceptionRecord.ExceptionCode != EXCEPTION_BREAKPOINT) {
                fprintf(stderr, "exception %08lx first=%lu address=%p info=%llu\n", ex.ExceptionRecord.ExceptionCode, ex.dwFirstChance, ex.ExceptionRecord.ExceptionAddress, ex.ExceptionRecord.ExceptionInformation[0]);
                if (ex.ExceptionRecord.ExceptionCode == 0xe06d7363 && ex.ExceptionRecord.NumberParameters >= 2) {
                    ULONG_PTR object[3] = {}; SIZE_T read;
                    ReadProcessMemory(pi.hProcess, (void*)ex.ExceptionRecord.ExceptionInformation[1], object, sizeof(object), &read);
                    char message[512] = {};
                    ReadProcessMemory(pi.hProcess, (void*)object[1], message, sizeof(message) - 1, &read);
                    fprintf(stderr, "C++ exception message: %.511s\n", message);
                }
                HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, ev.dwThreadId);
                CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL; GetThreadContext(thread, &ctx);
                fprintf(stderr,"registers rcx=%llx rdi=%llx rsi=%llx r12=%llx rax=%llx fault=%llx\n",ctx.Rcx,ctx.Rdi,ctx.Rsi,ctx.R12,ctx.Rax,ex.ExceptionRecord.ExceptionInformation[1]);
                STACKFRAME64 frame{};
                frame.AddrPC = {ctx.Rip, 0, AddrModeFlat}; frame.AddrStack = {ctx.Rsp, 0, AddrModeFlat}; frame.AddrFrame = {ctx.Rbp, 0, AddrModeFlat};
                for (int i = 0; i < 30 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, pi.hProcess, thread, &frame, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr); ++i) {
                    alignas(SYMBOL_INFO) char bytes[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
                    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(bytes); symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = MAX_SYM_NAME;
                    DWORD64 offset = 0; SymFromAddr(pi.hProcess, frame.AddrPC.Offset, &offset, symbol);
                    IMAGEHLP_LINE64 line = {sizeof(line)}; DWORD displacement = 0;
                    SymGetLineFromAddr64(pi.hProcess, frame.AddrPC.Offset, &displacement, &line);
                    fprintf(stderr, "  %llx %s+%llu %s:%lu\n", frame.AddrPC.Offset, symbol->Name, offset, line.FileName ? line.FileName : "", line.LineNumber);
                }
                CloseHandle(thread); fflush(stderr); action = DBG_EXCEPTION_NOT_HANDLED;
            }
        } else if (ev.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            const int code = static_cast<int>(ev.u.ExitProcess.dwExitCode);
            ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
            SymCleanup(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return code;
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, action);
    }
    return 1;
}
