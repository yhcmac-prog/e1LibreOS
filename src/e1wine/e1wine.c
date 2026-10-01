/*
 * e1wine — e1LibreOS 的 Wine 变种（轻量 Win32 PE 兼容层）
 *
 * 设计理念源自 Wine（Wine Is Not an Emulator）：不模拟 CPU，而是把
 * PE32+ 可执行映像直接装载到进程地址空间，用 __attribute__((ms_abi))
 * 桥接 Windows x64 ABI 与宿主 System V ABI，并提供 Win32 API 桩。
 *
 * 双平台：同一套源码编译两个原生构建
 *   - Linux  版：x86_64-linux-musl 静态链接，内置于 e1LibreOS
 *   - macOS 版：clang 原生编译，用于开发/调试 PE 加载器
 *
 * Wine 风格用法：
 *   e1wine program.exe [args...]     直接运行（同 wine program.exe）
 *   e1wine --version                 打印版本
 *   e1wine --info program.exe        显示 PE 信息（机器/入口/节/导入）
 *   e1wine --list program.exe        仅列导入的 DLL
 *
 * 应用包装（ELF/.app/X11 封装）已移至独立工具 e1wxfly。
 *
 * Wine 风格环境变量：
 *   WINEPREFIX  配置目录（默认 ~/.e1wine），首次运行自动创建
 *   WINEDEBUG   调试通道（-all 静默；默认仅 err 级别）
 *
 * 限制（相对完整 Wine 的裁剪）：
 *   - 仅 x86_64 PE32+；32 位 PE 与 .NET/PE 托管程序不支持
 *   - 仅解析静态导入表，LoadLibrary 链返回 NULL
 *   - 不实现 SEH 展开、TLS 回调、窗口子系统；适合直接调 kernel32 的控制台程序
 */

#ifndef E1WINE_PLATFORM
#define E1WINE_PLATFORM "unknown"
#endif
#define E1WINE_VERSION "1.0"
#define E1WINE_BANNER  "e1wine-" E1WINE_VERSION


#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <wchar.h>

/* ============== PE 格式常量与结构（精简版）============== */

typedef uint8_t  BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef uint32_t UINT;
typedef uint64_t ULONGLONG;
typedef int32_t  LONG;
typedef uint32_t ULONG;
typedef uint64_t ULONG_PTR;
typedef uint64_t DWORD64;

#define IMAGE_DOS_SIGNATURE     0x5A4D      /* "MZ" */
#define IMAGE_NT_SIGNATURE      0x00004550  /* "PE\0\0" */
#define IMAGE_NUMBEROF_DIRECTORY_ENTRIES 16
#define IMAGE_DIRECTORY_ENTRY_IMPORT 1

#define IMAGE_SCN_CNT_CODE              0x20
#define IMAGE_SCN_CNT_INITIALIZED_DATA 0x40
#define IMAGE_SCN_MEM_EXECUTE           0x20000000
#define IMAGE_SCN_MEM_READ             0x40000000
#define IMAGE_SCN_MEM_WRITE            0x80000000

#define IMAGE_FILE_MACHINE_I386     0x014c
#define IMAGE_FILE_MACHINE_AMD64    0x8664

#pragma pack(push, 1)
struct DOS_HEADER {
    WORD e_magic;
    WORD e_cblp;
    WORD e_cp;
    WORD e_crlc;
    WORD e_cparhdr;
    WORD e_minalloc;
    WORD e_maxalloc;
    WORD e_ss;
    WORD e_sp;
    WORD e_csum;
    WORD e_ip;
    WORD e_cs;
    WORD e_lfarlc;
    WORD e_ovno;
    WORD e_res[4];
    WORD e_oemid;
    WORD e_oeminfo;
    WORD e_res2[10];
    DWORD e_lfanew;       /* PE 头偏移 */
};

struct FILE_HEADER {
    WORD Machine;
    WORD NumberOfSections;
    DWORD TimeDateStamp;
    DWORD PointerToSymbolTable;
    DWORD NumberOfSymbols;
    WORD SizeOfOptionalHeader;
    WORD Characteristics;
};

struct OPTIONAL_HEADER64 {
    WORD Magic;                  /* 0x20b = PE32+ */
    BYTE MajorLinkerVersion;
    BYTE MinorLinkerVersion;
    DWORD SizeOfCode;
    DWORD SizeOfInitializedData;
    DWORD SizeOfUninitializedData;
    DWORD AddressOfEntryPoint;
    DWORD BaseOfCode;
    ULONGLONG ImageBase;
    DWORD SectionAlignment;
    DWORD FileAlignment;
    WORD MajorOperatingSystemVersion;
    WORD MinorOperatingSystemVersion;
    WORD MajorImageVersion;
    WORD MinorImageVersion;
    WORD MajorSubsystemVersion;
    WORD MinorSubsystemVersion;
    DWORD Win32VersionValue;
    DWORD SizeOfImage;
    DWORD SizeOfHeaders;
    DWORD CheckSum;
    WORD Subsystem;
    WORD DllCharacteristics;
    ULONGLONG SizeOfStackReserve;
    ULONGLONG SizeOfStackCommit;
    ULONGLONG SizeOfHeapReserve;
    ULONGLONG SizeOfHeapCommit;
    DWORD LoaderFlags;
    DWORD NumberOfRvaAndSizes;
    /* DataDirectory[NumberOfRvaAndSizes] */
};

struct OPTIONAL_HEADER32 {
    WORD Magic;                  /* 0x10b = PE32 */
    BYTE MajorLinkerVersion;
    BYTE MinorLinkerVersion;
    DWORD SizeOfCode;
    DWORD SizeOfInitializedData;
    DWORD SizeOfUninitializedData;
    DWORD AddressOfEntryPoint;
    DWORD BaseOfCode;
    DWORD BaseOfData;
    DWORD ImageBase;
    DWORD SectionAlignment;
    DWORD FileAlignment;
    WORD MajorOperatingSystemVersion;
    WORD MinorOperatingSystemVersion;
    WORD MajorImageVersion;
    WORD MinorImageVersion;
    WORD MajorSubsystemVersion;
    WORD MinorSubsystemVersion;
    DWORD Win32VersionValue;
    DWORD SizeOfImage;
    DWORD SizeOfHeaders;
    DWORD CheckSum;
    WORD Subsystem;
    WORD DllCharacteristics;
    DWORD SizeOfStackReserve;
    DWORD SizeOfStackCommit;
    DWORD SizeOfHeapReserve;
    DWORD SizeOfHeapCommit;
    DWORD LoaderFlags;
    DWORD NumberOfRvaAndSizes;
};

struct SECTION_HEADER {
    char Name[8];
    DWORD VirtualSize;
    DWORD VirtualAddress;
    DWORD SizeOfRawData;
    DWORD PointerToRawData;
    DWORD PointerToRelocations;
    DWORD PointerToLinenumbers;
    WORD NumberOfRelocations;
    WORD NumberOfLinenumbers;
    DWORD Characteristics;
};

struct DATA_DIRECTORY {
    DWORD VirtualAddress;
    DWORD Size;
};

struct IMPORT_DESC {
    DWORD OriginalFirstThunk;   /* INT（Import Name Table）RVA */
    DWORD TimeDateStamp;
    DWORD ForwarderChain;
    DWORD Name;                 /* DLL 名 RVA */
    DWORD FirstThunk;           /* IAT（Import Address Table）RVA */
};

struct IMPORT_BY_NAME {
    WORD Hint;
    char Name[1];
};
#pragma pack(pop)

#define OPT_MAGIC_PE32   0x10b
#define OPT_MAGIC_PE32P  0x20b

/* ============== 全局状态 ============== */

static int g_argc = 0;
static char **g_argv = NULL;
static char g_cmdline[4096] = {0};

/* ============== Wine 风格配置与日志 ============== */
/* 仿 WINEDEBUG：0=静默(-all)，1=仅 err/fixme（默认），2=warn，3=trace */
static int wine_dbg_level = 1;
static char wine_prefix[512] = {0};

static void wine_init_config(void) {
    const char *p = getenv("WINEPREFIX");
    if (p && *p) {
        snprintf(wine_prefix, sizeof wine_prefix, "%s", p);
    } else {
        const char *home = getenv("HOME");
        snprintf(wine_prefix, sizeof wine_prefix,
                 "%s/.e1wine", (home && *home) ? home : "/tmp");
    }
    const char *dbg = getenv("WINEDEBUG");
    if (dbg) {
        if (strstr(dbg, "-all")) wine_dbg_level = 0;
        else if (strstr(dbg, "trace")) wine_dbg_level = 3;
        else if (strstr(dbg, "warn")) wine_dbg_level = 2;
    }
}

/* 首次运行时建立 WINEPREFIX 目录骨架（drive_c 等），仿 wine 行为 */
static void wine_ensure_prefix(void) {
    if (wine_prefix[0] == 0) return;
    mkdir(wine_prefix, 0755);
    char buf[600];
    snprintf(buf, sizeof buf, "%s/drive_c", wine_prefix); mkdir(buf, 0755);
    snprintf(buf, sizeof buf, "%s/drive_c/windows", wine_prefix); mkdir(buf, 0755);
    snprintf(buf, sizeof buf, "%s/drive_c/windows/system32", wine_prefix); mkdir(buf, 0755);
}

#define wine_err(...)   fprintf(stderr, "e1wine: " __VA_ARGS__)
#define wine_fixme(...) do { if (wine_dbg_level >= 1) fprintf(stderr, "fixme:e1wine: " __VA_ARGS__); } while (0)
#define wine_warn(...)  do { if (wine_dbg_level >= 2) fprintf(stderr, "warn:e1wine: " __VA_ARGS__); } while (0)
#define wine_trace(...) do { if (wine_dbg_level >= 3) fprintf(stderr, "trace:e1wine: " __VA_ARGS__); } while (0)

/* ============== 辅助 ============== */

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "e1wine: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

static void *map_pe_file(const char *path, size_t *size_out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("cannot open %s: %s", path, strerror(errno));
    struct stat st;
    if (fstat(fd, &st) != 0) die("fstat failed: %s", strerror(errno));
    void *map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) die("mmap failed: %s", strerror(errno));
    *size_out = st.st_size;
    return map;
}

/* RVA -> 文件偏移：在节表中查找包含该 RVA 的节 */
static DWORD rva_to_off(DWORD rva, struct SECTION_HEADER *sec, int n) {
    int i;
    for (i = 0; i < n; i++) {
        DWORD vs = sec[i].VirtualSize ? sec[i].VirtualSize : sec[i].SizeOfRawData;
        if (rva >= sec[i].VirtualAddress && rva < sec[i].VirtualAddress + vs) {
            return sec[i].PointerToRawData + (rva - sec[i].VirtualAddress);
        }
    }
    return 0;
}

/* ============== Win32 API 桩（ms_abi 调用约定）============== */
/* 这些函数将被 Windows 代码以 Windows x64 ABI 调用；
 * ms_abi 让 GCC/clang 在 Linux 平台也生成正确的入口/出口代码。 */

#define WINBASEAPI __attribute__((ms_abi))
#define WINAPI
#define STD_INPUT_HANDLE  ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE  ((DWORD)-12)
#define INVALID_HANDLE_VALUE ((void *)-1)
#define TRUE 1
#define FALSE 0
#define BOOL int
#define HANDLE void*
#define HMODULE void*
#define LPDWORD DWORD*
#define LPVOID void*
#define LPCSTR const char*
#define LPSTR char*
#define LPCVOID const void*
#define LPVOID void*
#define DWORD_PTR ULONG_PTR

WINBASEAPI void WINAPI ExitProcess(UINT uExitCode) {
    fflush(stdout);
    fflush(stderr);
    _exit((int)uExitCode);
}

WINBASEAPI void WINAPI Sleep(DWORD dwMilliseconds) {
    if (dwMilliseconds == 0) return;
    usleep(dwMilliseconds * 1000);
}

WINBASEAPI HANDLE WINAPI GetStdHandle(DWORD nStdHandle) {
    switch (nStdHandle) {
        case STD_INPUT_HANDLE:  return (HANDLE)(intptr_t)0;
        case STD_OUTPUT_HANDLE: return (HANDLE)(intptr_t)1;
        case STD_ERROR_HANDLE:  return (HANDLE)(intptr_t)2;
    }
    return INVALID_HANDLE_VALUE;
}

WINBASEAPI BOOL WINAPI WriteFile(HANDLE hFile, LPCVOID lpBuffer,
                                  DWORD nBytes, LPDWORD lpWritten, LPVOID lpOverlapped) {
    int fd = (int)(intptr_t)hFile;
    ssize_t n = write(fd, lpBuffer, nBytes);
    if (n < 0) return FALSE;
    if (lpWritten) *lpWritten = (DWORD)n;
    return TRUE;
}

WINBASEAPI BOOL WINAPI ReadFile(HANDLE hFile, LPVOID lpBuffer,
                                 DWORD nBytes, LPDWORD lpRead, LPVOID lpOverlapped) {
    int fd = (int)(intptr_t)hFile;
    ssize_t n = read(fd, lpBuffer, nBytes);
    if (n < 0) return FALSE;
    if (lpRead) *lpRead = (DWORD)n;
    return TRUE;
}

WINBASEAPI BOOL WINAPI CloseHandle(HANDLE h) { return TRUE; }

WINBASEAPI DWORD WINAPI GetTickCount(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (DWORD)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

WINBASEAPI void WINAPI GetSystemTimeAsFileTime(void *ft) {
    /* FILETIME = 100ns 自 1601；近似返回 0，由调用者很少用 */
    memset(ft, 0, 16);
}

WINBASEAPI DWORD WINAPI GetCurrentProcessId(void) { return (DWORD)getpid(); }
WINBASEAPI DWORD WINAPI GetCurrentThreadId(void)  { return 1; }

WINBASEAPI DWORD WINAPI GetLastError(void)         { return 0; }
WINBASEAPI void  WINAPI SetLastError(DWORD e)       { (void)e; }

WINBASEAPI HMODULE WINAPI GetModuleHandleA(LPCSTR lpModuleName) {
    /* 主模块返回非空，其它返回 NULL（未实现动态查询）*/
    if (lpModuleName == NULL) return (HMODULE)(intptr_t)1;
    return NULL;
}

WINBASEAPI HMODULE WINAPI LoadLibraryA(LPCSTR lpLibFileName) {
    wine_fixme("LoadLibraryA(\"%s\") not implemented, returning NULL\n",
               lpLibFileName ? lpLibFileName : "(null)");
    return NULL;
}

WINBASEAPI void* WINAPI GetProcAddress(HMODULE hModule, LPCSTR lpProcName) {
    /* IAT 静态导入已在装载阶段填好桩地址；运行时动态查询暂不支持 */
    wine_fixme("GetProcAddress(%p, \"%s\") not implemented, returning NULL\n",
               hModule, lpProcName ? lpProcName : "(null)");
    return NULL;
}

WINBASEAPI LPSTR WINAPI GetCommandLineA(void) {
    return g_cmdline;
}

WINBASEAPI void WINAPI GetStartupInfoA(void *si) {
    memset(si, 0, 0x68);  /* STARTUPINFOA 大小约 104 字节 */
}

/* user32 桩：MessageBoxA/W 在无窗口环境下输出到 stderr */
WINBASEAPI int WINAPI MessageBoxA(void *hWnd, LPCSTR lpText,
                                   LPCSTR lpCaption, UINT uType) {
    fprintf(stderr, "[MessageBox] %s: %s\n",
            lpCaption ? lpCaption : "", lpText ? lpText : "");
    return 1;  /* IDOK */
}

WINBASEAPI int WINAPI MessageBoxW(void *hWnd, const wchar_t *lpText,
                                   const wchar_t *lpCaption, UINT uType) {
    /* 简化：把 wchar_t 当字节流打印 */
    fprintf(stderr, "[MessageBoxW] ");
    if (lpCaption) {
        const char *p = (const char *)lpCaption;
        while (*p || *(p+1)) { fputc(*p, stderr); p += 2; }
        fprintf(stderr, ": ");
    }
    if (lpText) {
        const char *p = (const char *)lpText;
        while (*p || *(p+1)) { fputc(*p, stderr); p += 2; }
    }
    fputc('\n', stderr);
    return 1;
}

/* ---- 额外 kernel32 桩：常见字符串/内存/进程函数 ---- */
WINBASEAPI int WINAPI lstrlenA(LPCSTR s) {
    return s ? (int)strlen(s) : 0;
}
WINBASEAPI LPSTR WINAPI lstrcpyA(LPSTR dst, LPCSTR src) {
    if (dst && src) strcpy(dst, src);
    return dst;
}
WINBASEAPI LPSTR WINAPI lstrcatA(LPSTR dst, LPCSTR src) {
    if (dst && src) strcat(dst, src);
    return dst;
}
WINBASEAPI LPVOID WINAPI VirtualAlloc(LPVOID addr, size_t size,
                                       DWORD allocType, DWORD protect) {
    (void)allocType; (void)protect;
    void *p = mmap(addr ? addr : NULL, size,
                   PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | (addr ? MAP_FIXED : 0), -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
WINBASEAPI BOOL WINAPI VirtualFree(LPVOID addr, size_t size, DWORD freeType) {
    if (addr && size) munmap(addr, size);
    return TRUE;
}
WINBASEAPI BOOL WINAPI VirtualProtect(LPVOID addr, size_t size,
                                       DWORD newProt, DWORD *oldProt) {
    int prot = PROT_NONE;
    if (newProt & 0x02) prot = PROT_READ;       /* PAGE_READONLY */
    if (newProt & 0x04) prot = PROT_READ | PROT_WRITE;
    if (newProt & 0x10) prot = PROT_READ | PROT_WRITE | PROT_EXEC;
    if (newProt & 0x20) prot = PROT_READ | PROT_EXEC;
    if (oldProt) *oldProt = 0;
    return mprotect(addr, size, prot) == 0 ? TRUE : FALSE;
}
/* MEMORY_BASIC_INFORMATION 最小桩：返回固定可读区域 */
WINBASEAPI size_t WINAPI VirtualQuery(LPCVOID addr, void *info, size_t len) {
    (void)addr;
    if (info && len >= 32) memset(info, 0, len);
    if (info) {
        ((DWORD *)info)[0] = 0x1000;   /* BaseAddress 偏移占位 */
        ((DWORD *)info)[4] = 0x40;      /* State = MEM_COMMIT */
        ((DWORD *)info)[6] = 0x04;      /* Protect = PAGE_READWRITE */
        ((size_t *)info)[5] = 0x1000;   /* RegionSize */
    }
    return len >= 32 ? len : 0;
}
/* SEH 展开处理器：no-op（异常处理不实现，正常返回 */
WINBASEAPI void WINAPI C_specific_handler(void) {}
WINBASEAPI LPVOID WINAPI HeapAlloc(void *hHeap, DWORD flags, size_t size) {
    (void)hHeap; (void)flags;
    return malloc(size);
}
WINBASEAPI BOOL WINAPI HeapFree(void *hHeap, DWORD flags, LPVOID p) {
    (void)hHeap; (void)flags;
    free(p);
    return TRUE;
}
WINBASEAPI DWORD WINAPI TlsAlloc(void) { return 0; }
WINBASEAPI LPVOID WINAPI TlsGetValue(DWORD idx) { (void)idx; return NULL; }
WINBASEAPI BOOL WINAPI TlsSetValue(DWORD idx, LPVOID val) { (void)idx; (void)val; return TRUE; }
WINBASEAPI void WINAPI InitializeCriticalSection(void *cs) { (void)cs; }
WINBASEAPI void WINAPI EnterCriticalSection(void *cs) { (void)cs; }
WINBASEAPI void WINAPI LeaveCriticalSection(void *cs) { (void)cs; }
WINBASEAPI void WINAPI DeleteCriticalSection(void *cs) { (void)cs; }
WINBASEAPI void WINAPI SetUnhandledExceptionFilter(void *f) { (void)f; }
WINBASEAPI DWORD WINAPI GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD n) {
    if (buf && n) { snprintf(buf, n, "e1wine"); return 6; }
    return 0;
}
WINBASEAPI DWORD WINAPI GetVersion(void) { return 0x00060001; }  /* "6.1" Windows 7 */
WINBASEAPI BOOL WINAPI SetConsoleCtrlHandler(void *f, BOOL add) { (void)f; (void)add; return TRUE; }
WINBASEAPI BOOL WINAPI SetConsoleMode(HANDLE h, DWORD mode) { (void)h; (void)mode; return TRUE; }
WINBASEAPI BOOL WINAPI GetConsoleMode(HANDLE h, LPDWORD mode) { if (mode) *mode = 0; (void)h; return TRUE; }
WINBASEAPI DWORD WINAPI GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD n) {
    (void)name; if (buf && n) buf[0] = 0; return 0;
}
/* 未知 Win32 调用兜底：打印并退出，避免 NULL 调用导致段错误 */
static WINBASEAPI void WINAPI unresolved_crash(void) {
    wine_err("called an unresolved Win32 API; aborting\n");
    fflush(stderr);
    _exit(127);
}

/* msvcrt 常见桩：简化返回 */
WINBASEAPI int *_errno(void) {
    static int e = 0;
    return &e;
}
/* 数据型导入（如 _commode/_fmode）：可写存储 */
static char  d_commode;
static char  d_fmode;
static void *d_acmdln;
static void *d_initenv;

/* msvcrt 函数桩：直接转发到 musl，ms_abi 包装。
 * stdin/stdout/stderr 的 FILE* 是 __iob_func 返回的标记值，写操作据此路由到真实 fd。 */
#define WIN_STDIN   ((void *)0x100)
#define WIN_STDOUT  ((void *)0x200)
#define WIN_STDERR  ((void *)0x300)

static FILE *msvcrt_stream_to_fp(void *f) {
    if (f == WIN_STDOUT) return stdout;
    if (f == WIN_STDERR) return stderr;
    if (f == WIN_STDIN)  return stdin;
    return stdout;   /* 默认按 stdout 处理 */
}

WINBASEAPI void WINAPI msvcrt_exit(int code) { _exit(code); }
WINBASEAPI void WINAPI msvcrt_abort(void) { abort(); }
WINBASEAPI void *WINAPI msvcrt_malloc(size_t n) { return malloc(n); }
WINBASEAPI void *WINAPI msvcrt_calloc(size_t n, size_t s) { return calloc(n, s); }
WINBASEAPI void  WINAPI msvcrt_free(void *p) { free(p); }
WINBASEAPI size_t WINAPI msvcrt_strlen(const char *s) { return strlen(s); }
WINBASEAPI int WINAPI msvcrt_strncmp(const char *a, const char *b, size_t n) {
    return strncmp(a, b, n);
}
WINBASEAPI size_t WINAPI msvcrt_fwrite(const void *p, size_t s, size_t n, void *f) {
    return fwrite(p, s, n, msvcrt_stream_to_fp(f));
}
WINBASEAPI int WINAPI msvcrt_signal(int sig, void *h) { (void)sig; (void)h; return 0; }
/* CRT 初始化桩：no-op 或返回空 */
WINBASEAPI void WINAPI msvcrt_initterm(void *a, void *b) { (void)a; (void)b; }
WINBASEAPI void *WINAPI msvcrt_iob_func(void) {
    /* 返回标记值数组：[0]=stdin [1]=stdout [2]=stderr */
    static void *iob[3] = { WIN_STDIN, WIN_STDOUT, WIN_STDERR };
    return iob;
}
WINBASEAPI int WINAPI msvcrt_getmainargs(int *ac, char ***av, char ***env,
                                          int do_wild, void *startup) {
    (void)do_wild; (void)startup;
    *ac = g_argc; *av = g_argv; *env = NULL;
    return 0;
}
WINBASEAPI int WINAPI msvcrt_amsg_exit(int code) { (void)code; _exit(1); }
WINBASEAPI int WINAPI msvcrt_set_app_type(int t) { (void)t; return 0; }
WINBASEAPI void WINAPI msvcrt_setusermatherr(void *p) { (void)p; }
WINBASEAPI void WINAPI msvcrt_cexit(void) { _exit(0); }
WINBASEAPI int WINAPI msvcrt_onexit(void *f) { (void)f; return 0; }
/* fprintf/vfprintf：ms_abi 变参无法直接用 va_start（SysV 假设不同）。
 * 这些桩主要用于 CRT 内部错误路径（多为无格式参数的固定串），
 * 直接把格式串写入对应流即可；主体程序输出经 fwrite 已正确处理。 */
WINBASEAPI int WINAPI msvcrt_fprintf(void *f, const char *fmt) {
    FILE *fp = msvcrt_stream_to_fp(f);
    int r = fmt ? (int)fputs(fmt, fp) : 0;
    fflush(fp);
    return r;
}
WINBASEAPI int WINAPI msvcrt_vfprintf(void *f, const char *fmt, void *ap) {
    (void)ap;
    return msvcrt_fprintf(f, fmt);
}

/* ============== 导入表桩地址表 ============== */
/* 把"模块名!函数名"映射到桩函数地址。运行时查表填 IAT。 */

struct stub_entry { const char *dll; const char *fn; void *addr; };

#define STUB(d, f) { d, #f, (void *)(f) }
#define STUB_N(d, name, f) { d, name, (void *)(f) }
#define DATA_STUB(d, name, sym) { d, name, (void *)&(sym) }

static const struct stub_entry stub_table[] = {
    /* kernel32.dll */
    STUB("kernel32.dll", ExitProcess),
    STUB("kernel32.dll", Sleep),
    STUB("kernel32.dll", GetStdHandle),
    STUB("kernel32.dll", WriteFile),
    STUB("kernel32.dll", ReadFile),
    STUB("kernel32.dll", CloseHandle),
    STUB("kernel32.dll", GetTickCount),
    STUB("kernel32.dll", GetSystemTimeAsFileTime),
    STUB("kernel32.dll", GetCurrentProcessId),
    STUB("kernel32.dll", GetCurrentThreadId),
    STUB("kernel32.dll", GetLastError),
    STUB("kernel32.dll", SetLastError),
    STUB("kernel32.dll", GetModuleHandleA),
    STUB("kernel32.dll", LoadLibraryA),
    STUB("kernel32.dll", GetProcAddress),
    STUB("kernel32.dll", GetCommandLineA),
    STUB("kernel32.dll", GetStartupInfoA),
    STUB("kernel32.dll", lstrlenA),
    STUB("kernel32.dll", lstrcpyA),
    STUB("kernel32.dll", lstrcatA),
    STUB("kernel32.dll", VirtualAlloc),
    STUB("kernel32.dll", VirtualFree),
    STUB("kernel32.dll", VirtualProtect),
    STUB("kernel32.dll", VirtualQuery),
    STUB_N("kernel32.dll", "__C_specific_handler", C_specific_handler),
    STUB("kernel32.dll", HeapAlloc),
    STUB("kernel32.dll", HeapFree),
    STUB("kernel32.dll", TlsAlloc),
    STUB("kernel32.dll", TlsGetValue),
    STUB("kernel32.dll", TlsSetValue),
    STUB("kernel32.dll", InitializeCriticalSection),
    STUB("kernel32.dll", EnterCriticalSection),
    STUB("kernel32.dll", LeaveCriticalSection),
    STUB("kernel32.dll", DeleteCriticalSection),
    STUB("kernel32.dll", SetUnhandledExceptionFilter),
    STUB("kernel32.dll", GetModuleFileNameA),
    STUB("kernel32.dll", GetVersion),
    STUB("kernel32.dll", SetConsoleCtrlHandler),
    STUB("kernel32.dll", SetConsoleMode),
    STUB("kernel32.dll", GetConsoleMode),
    STUB("kernel32.dll", GetEnvironmentVariableA),
    /* user32.dll */
    STUB("user32.dll", MessageBoxA),
    STUB("user32.dll", MessageBoxW),
    /* msvcrt.dll 函数（Windows 导入名与 C 桩名不同，用 STUB_N 显式指定）*/
    STUB("msvcrt.dll", _errno),
    STUB_N("msvcrt.dll", "exit", msvcrt_exit),
    STUB_N("msvcrt.dll", "abort", msvcrt_abort),
    STUB_N("msvcrt.dll", "malloc", msvcrt_malloc),
    STUB_N("msvcrt.dll", "calloc", msvcrt_calloc),
    STUB_N("msvcrt.dll", "free", msvcrt_free),
    STUB_N("msvcrt.dll", "strlen", msvcrt_strlen),
    STUB_N("msvcrt.dll", "strncmp", msvcrt_strncmp),
    STUB_N("msvcrt.dll", "fwrite", msvcrt_fwrite),
    STUB_N("msvcrt.dll", "signal", msvcrt_signal),
    STUB_N("msvcrt.dll", "_initterm", msvcrt_initterm),
    STUB_N("msvcrt.dll", "__iob_func", msvcrt_iob_func),
    STUB_N("msvcrt.dll", "__getmainargs", msvcrt_getmainargs),
    STUB_N("msvcrt.dll", "_amsg_exit", msvcrt_amsg_exit),
    STUB_N("msvcrt.dll", "__set_app_type", msvcrt_set_app_type),
    STUB_N("msvcrt.dll", "__setusermatherr", msvcrt_setusermatherr),
    STUB_N("msvcrt.dll", "_cexit", msvcrt_cexit),
    STUB_N("msvcrt.dll", "_onexit", msvcrt_onexit),
    STUB_N("msvcrt.dll", "fprintf", msvcrt_fprintf),
    STUB_N("msvcrt.dll", "vfprintf", msvcrt_vfprintf),
    /* 数据型导入：IAT 项指向变量存储 */
    DATA_STUB("msvcrt.dll", "_commode", d_commode),
    DATA_STUB("msvcrt.dll", "_fmode", d_fmode),
    DATA_STUB("msvcrt.dll", "_acmdln", d_acmdln),
    DATA_STUB("msvcrt.dll", "__initenv", d_initenv),
};
#define STUB_COUNT (sizeof(stub_table) / sizeof(stub_table[0]))

static void *find_stub(const char *dll, const char *fn) {
    size_t i;
    for (i = 0; i < STUB_COUNT; i++) {
        if (strcasecmp(stub_table[i].dll, dll) == 0 &&
            strcasecmp(stub_table[i].fn, fn) == 0)
            return stub_table[i].addr;
    }
    return NULL;
}

/* ============== PE 加载器 ============== */

struct pe_image {
    void *file_map;          /* 整个 PE 文件 mmap */
    size_t file_size;
    void *image_base;       /* 加载基址（mmap 出来的可执行内存）*/
    size_t image_size;
    ULONGLONG prefer_base;  /* PE 中声明的 ImageBase */
    DWORD entry_rva;
    int is_pe32p;            /* 1=PE32+ (64位) */
    struct SECTION_HEADER *secs;
    int n_secs;
    struct DATA_DIRECTORY *datadir;
    int n_datadir;
};

/* 解析 PE 头；返回 0=成功 */
static int parse_pe(struct pe_image *im) {
    struct DOS_HEADER *dos = (struct DOS_HEADER *)im->file_map;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        die("not a PE file (no MZ signature)");
    if ((size_t)dos->e_lfanew + 4 > im->file_size)
        die("invalid PE header offset");

    DWORD *nt_sig = (DWORD *)((char *)im->file_map + dos->e_lfanew);
    if (*nt_sig != IMAGE_NT_SIGNATURE)
        die("not a PE file (no PE signature)");

    struct FILE_HEADER *fh = (struct FILE_HEADER *)((char *)nt_sig + 4);
    if (fh->Machine != IMAGE_FILE_MACHINE_AMD64)
        die("only x86_64 PE32+ is supported (machine=0x%04x)", fh->Machine);

    struct OPTIONAL_HEADER64 *oh = (struct OPTIONAL_HEADER64 *)((char *)fh + sizeof(*fh));
    if (oh->Magic == OPT_MAGIC_PE32P) {
        im->is_pe32p = 1;
    } else if (oh->Magic == OPT_MAGIC_PE32) {
        die("PE32 (32-bit) not supported; only PE32+");
    } else {
        die("unknown optional header magic: 0x%04x", oh->Magic);
    }

    im->prefer_base = oh->ImageBase;
    im->image_size  = oh->SizeOfImage;
    im->entry_rva   = oh->AddressOfEntryPoint;
    im->secs = (struct SECTION_HEADER *)((char *)oh + fh->SizeOfOptionalHeader);
    im->n_secs = fh->NumberOfSections;
    im->datadir = (struct DATA_DIRECTORY *)((char *)oh + sizeof(*oh));
    im->n_datadir = (int)oh->NumberOfRvaAndSizes;
    return 0;
}

/* 把 PE 的所有节按 SectionAlignment 加载到 image_base 的对应 RVA */
static void load_sections(struct pe_image *im) {
    /* 分配整个 SizeOfImage 内存，可读可写可执行 */
    void *base = mmap((void *)im->prefer_base, im->image_size,
                      PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        /* 优先基址被占（Linux 不喜欢 0x140000000 之类）→ 让内核选地址 */
        base = mmap(NULL, im->image_size,
                    PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base == MAP_FAILED) die("mmap image failed: %s", strerror(errno));
    }
    im->image_base = base;

    /* 先拷头部（SizeOfHeaders 字节）*/
    size_t hdrs = 0x400;  /* 安全上限 */
    if (im->file_size < hdrs) hdrs = im->file_size;
    memcpy(base, im->file_map, hdrs);

    /* 逐节加载 */
    int i;
    for (i = 0; i < im->n_secs; i++) {
        struct SECTION_HEADER *s = &im->secs[i];
        char *dst = (char *)base + s->VirtualAddress;
        size_t copy = s->SizeOfRawData;
        if (s->PointerToRawData + copy > im->file_size)
            copy = im->file_size > s->PointerToRawData ?
                   im->file_size - s->PointerToRawData : 0;
        if (copy > 0)
            memcpy(dst, (char *)im->file_map + s->PointerToRawData, copy);
        /* BSS 部分（VirtualSize > SizeOfRawData）mmap 已清零，无需额外处理 */
    }
}

/* 解析导入表，对每个 IAT 项填入对应桩函数地址 */
static void resolve_imports(struct pe_image *im) {
    if (im->n_datadir <= IMAGE_DIRECTORY_ENTRY_IMPORT) return;
    DWORD imp_rva = im->datadir[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (imp_rva == 0) return;
    DWORD imp_off = rva_to_off(imp_rva, im->secs, im->n_secs);
    if (imp_off == 0) die("cannot map import directory RVA");

    struct IMPORT_DESC *id = (struct IMPORT_DESC *)((char *)im->file_map + imp_off);
    while (id->Name) {
        DWORD name_off = rva_to_off(id->Name, im->secs, im->n_secs);
        const char *dll_name = name_off ? (char *)im->file_map + name_off : "?";
        /* 拿 IAT 与 INT */
        DWORD iat_rva = id->FirstThunk;
        DWORD int_rva = id->OriginalFirstThunk;
        if (int_rva == 0) int_rva = iat_rva;
        DWORD iat_off = rva_to_off(iat_rva, im->secs, im->n_secs);
        DWORD int_off = rva_to_off(int_rva, im->secs, im->n_secs);
        if (iat_off == 0) { id++; continue; }

        ULONG_PTR *iat_entry = (ULONG_PTR *)((char *)im->image_base + iat_rva);
        ULONG_PTR *int_entry = int_off ?
            (ULONG_PTR *)((char *)im->file_map + int_off) : NULL;
        int idx = 0;
        while (int_entry ? int_entry[idx] : iat_entry[idx]) {
            ULONG_PTR thunk = int_entry ? int_entry[idx] : iat_entry[idx];
            void *fn_addr = NULL;
            if (thunk & 0x8000000000000000ULL) {
                /* 序号导入：未实现，指向兜底桩 */
                wine_fixme("%s ordinal import #%llu unsupported\n",
                           dll_name, (unsigned long long)(thunk & 0xFFFF));
                fn_addr = (void *)unresolved_crash;
            } else {
                DWORD name_rva = (DWORD)thunk;
                DWORD name_off2 = rva_to_off(name_rva, im->secs, im->n_secs);
                if (name_off2) {
                    struct IMPORT_BY_NAME *ibn =
                        (struct IMPORT_BY_NAME *)((char *)im->file_map + name_off2);
                    fn_addr = find_stub(dll_name, ibn->Name);
                    if (!fn_addr) {
                        wine_warn("%s!%s unresolved (will abort if called)\n",
                                  dll_name, ibn->Name);
                        fn_addr = (void *)unresolved_crash;
                    }
                } else {
                    fn_addr = (void *)unresolved_crash;
                }
            }
            iat_entry[idx] = (ULONG_PTR)fn_addr;
            idx++;
        }
        id++;
    }
}

/* ============== 信息打印 ============== */

static void cmd_info(struct pe_image *im) {
    printf("PE format: %s\n", im->is_pe32p ? "PE32+ (x86_64)" : "PE32 (i386)");
    printf("Preferred ImageBase: 0x%llx\n", (unsigned long long)im->prefer_base);
    printf("SizeOfImage:          %zu (0x%zx)\n", im->image_size, im->image_size);
    printf("AddressOfEntryPoint:  0x%x (RVA)\n", im->entry_rva);
    printf("Sections (%d):\n", im->n_secs);
    int i;
    for (i = 0; i < im->n_secs; i++) {
        struct SECTION_HEADER *s = &im->secs[i];
        char name[9] = {0};
        memcpy(name, s->Name, 8);
        printf("  %-8s VA=0x%08x VSize=0x%-8x RawOff=0x%-8x RawSize=0x%-8x flags=0x%08x\n",
               name, s->VirtualAddress, s->VirtualSize,
               s->PointerToRawData, s->SizeOfRawData, s->Characteristics);
    }
    if (im->n_datadir > IMAGE_DIRECTORY_ENTRY_IMPORT &&
        im->datadir[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress) {
        printf("Imports:\n");
        DWORD imp_rva = im->datadir[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        DWORD imp_off = rva_to_off(imp_rva, im->secs, im->n_secs);
        struct IMPORT_DESC *id = (struct IMPORT_DESC *)((char *)im->file_map + imp_off);
        while (id->Name) {
            DWORD name_off = rva_to_off(id->Name, im->secs, im->n_secs);
            const char *dll = name_off ? (char *)im->file_map + name_off : "?";
            printf("  %s\n", dll);
            ULONG_PTR *int_entry = NULL;
            DWORD int_rva = id->OriginalFirstThunk;
            if (int_rva == 0) int_rva = id->FirstThunk;
            DWORD int_off = rva_to_off(int_rva, im->secs, im->n_secs);
            if (int_off) int_entry = (ULONG_PTR *)((char *)im->file_map + int_off);
            int idx = 0;
            while (int_entry && int_entry[idx]) {
                ULONG_PTR t = int_entry[idx];
                if (t & 0x8000000000000000ULL) {
                    printf("    #%llu\n", (unsigned long long)(t & 0xFFFF));
                } else {
                    DWORD no = rva_to_off((DWORD)t, im->secs, im->n_secs);
                    if (no) {
                        struct IMPORT_BY_NAME *ibn =
                            (struct IMPORT_BY_NAME *)((char *)im->file_map + no);
                        printf("    %s\n", ibn->Name);
                    }
                }
                idx++;
            }
            id++;
        }
    } else {
        printf("Imports: (none)\n");
    }
}

/* ============== 执行 ============== */

static void cmd_run(struct pe_image *im, int argc, char **argv) {
    /* 加载到内存、解析 IAT */
    load_sections(im);
    resolve_imports(im);

    /* 构造 Windows 风格命令行：prog arg1 arg2 ... */
    size_t off = 0;
    int i;
    for (i = 0; i < argc && off < sizeof(g_cmdline) - 1; i++) {
        if (i) g_cmdline[off++] = ' ';
        const char *a = argv[i];
        size_t l = strlen(a);
        if (off + l >= sizeof(g_cmdline) - 1) l = sizeof(g_cmdline) - 1 - off;
        memcpy(g_cmdline + off, a, l);
        off += l;
    }
    g_cmdline[off] = 0;

    /* 入口点：Windows EXE 入口签名约定为
     *   void entry(PVOID ImageBase, void *Reserved, void *Reserved)
     * 由链接器决定；CRT 启动代码进一步调用 mainCRTStartup -> main。
     * 我们调用 ms_abi 函数指针，参数简单传 0。 */
    typedef void (*entry_t)(void *, void *, void *)
        __attribute__((ms_abi));
    void *entry_addr = (char *)im->image_base + im->entry_rva;
    wine_trace("entry=%p image_base=%p size=0x%zx\n",
               entry_addr, im->image_base, im->image_size);

    entry_t entry = (entry_t)entry_addr;
    entry(im->image_base, NULL, NULL);

    /* 如果入口未调用 ExitProcess（直接 ret），到这里算正常退出 */
    wine_trace("entry returned, exiting\n");
    fflush(stdout);
    _exit(0);
}

/* ============== 主入口 ============== */

static void usage(void) {
    fprintf(stderr,
        "%s — a lightweight Wine variant for e1LibreOS (%s build)\n\n"
        "Usage:\n"
        "  e1wine [options] <program.exe> [args...]  Run a Windows PE32+ program\n"
        "  e1wine --info <file.exe>                  Show PE headers and imports\n"
        "  e1wine --list <file.exe>                  List imported DLLs only\n"
        "  e1wine --version                          Print version and exit\n"
        "  e1wine --help                             Show this help\n\n"
        "Application wrapping (ELF/.app/X11) has moved to e1wxfly.\n\n"
        "Environment:\n"
        "  WINEPREFIX   Configuration directory (default: ~/.e1wine)\n"
        "  WINEDEBUG    -all (silent) | err (default) | warn | trace\n",
        E1WINE_BANNER, E1WINE_PLATFORM);
}

/* 仅列导入的 DLL 名 */
static void cmd_list(struct pe_image *im) {
    if (im->n_datadir > IMAGE_DIRECTORY_ENTRY_IMPORT &&
        im->datadir[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress) {
        DWORD imp_rva = im->datadir[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        DWORD imp_off = rva_to_off(imp_rva, im->secs, im->n_secs);
        struct IMPORT_DESC *id = (struct IMPORT_DESC *)((char *)im->file_map + imp_off);
        while (id->Name) {
            DWORD no = rva_to_off(id->Name, im->secs, im->n_secs);
            printf("%s\n", no ? (char *)im->file_map + no : "?");
            id++;
        }
    }
}

int main(int argc, char **argv) {
    const char *file = NULL;
    int mode_run = 1;
    int passthrough_argc = 0;
    char **passthrough_argv = NULL;

    wine_init_config();

    if (argc < 2) {
        usage();
        return 1;
    }

    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
        printf("%s (%s)\n", E1WINE_BANNER, E1WINE_PLATFORM);
        return 0;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }

    if (strcmp(argv[1], "--info") == 0) {
        if (argc < 3) { usage(); return 1; }
        mode_run = 0; file = argv[2];
        passthrough_argc = 0; passthrough_argv = NULL;
    } else if (strcmp(argv[1], "--list") == 0) {
        if (argc < 3) { usage(); return 1; }
        mode_run = 0; file = argv[2];
        passthrough_argc = -1; passthrough_argv = NULL;   /* -1 表示 list */
    } else {
        file = argv[1];
        passthrough_argc = argc - 1;
        passthrough_argv = argv + 1;
    }

run_file:
    ;
    struct pe_image im = {0};
    im.file_map = map_pe_file(file, &im.file_size);
    parse_pe(&im);

    if (!mode_run) {
        if (passthrough_argc == -1) cmd_list(&im);
        else cmd_info(&im);
        return 0;
    }

    /* 仿 wine：首次运行创建配置目录并打印 banner */
    int is_new = (access(wine_prefix, F_OK) != 0);
    wine_ensure_prefix();
    if (is_new && wine_dbg_level >= 1)
        fprintf(stderr, "%s: created the configuration directory '%s'\n",
                E1WINE_BANNER, wine_prefix);

    g_argc = passthrough_argc;
    g_argv = passthrough_argv;
    cmd_run(&im, passthrough_argc, passthrough_argv);
    return 0;
}
