/*
 * hello-pe.c — e1wine 兼容层的演示 PE32+ 程序
 *
 * 这是一个 Windows EXE 源码：直接调用 kernel32.dll，不链接 CRT。
 * 由 build.sh 用 zig cc -target x86_64-windows-gnu -nostdlib 交叉编译为
 * hello.exe，随 e1LibreOS 分发到 /usr/share/e1wine/hello.exe。
 *
 * 在 e1LibreOS 上用 e1wine 运行（与 wine 用法一致）：
 *   e1wine /usr/share/e1wine/hello.exe
 *
 * 演示 PE 加载、IAT 解析与 Win32 API 桩调用。
 */
typedef unsigned long DWORD;
typedef void *HANDLE;
typedef int BOOL;
typedef void *LPVOID;
typedef const void *LPCVOID;
typedef DWORD *LPDWORD;

#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define WINAPI __attribute__((ms_abi))

__attribute__((dllimport, ms_abi)) HANDLE WINAPI GetStdHandle(DWORD);
__attribute__((dllimport, ms_abi)) BOOL WINAPI WriteFile(HANDLE, LPCVOID, DWORD, LPDWORD, LPVOID);
__attribute__((dllimport, ms_abi, noreturn)) void WINAPI ExitProcess(DWORD);

/* _tls_index 占位（lld-link 链接器要求的符号） */
DWORD _tls_index = 0;

void entry(void) {
    const char *msg = "Hello! This is a Windows .exe running on e1LibreOS via e1wine.\r\n"
                      "e1wine (a Wine variant) PE32+ compatibility layer works!\r\n";
    DWORD n = 0;
    while (msg[n]) n++;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written;
    WriteFile(out, msg, n, &written, 0);
    ExitProcess(0);
}
