/*
 * mac-launcher — e1wine .app 包的 macOS 原生启动器
 *
 * 以 universal binary（x86_64 + arm64）编译，同时支持 Intel Mac 与
 * Apple Silicon。它本身不实现 PE 加载（Windows 程序在 macOS 上需要
 * Wine），职责是：
 *
 *   1. 定位 Contents/Resources/payload-name 指向的原始 EXE；
 *   2. 依次尝试系统中已安装的 Wine（Homebrew / Wine Stable / MacPorts）；
 *   3. 找到则启动 Wine 运行该 EXE；找不到则弹出原生对话框提示。
 *
 * Apple Silicon 机器上需先安装 Rosetta 2 与 x86_64 版 Wine。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <libgen.h>
#include <sys/stat.h>
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;

static int exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

/* Wine 可执行文件候选（绝对路径优先，PATH 查找由 spawnp_exists 兜底） */
static const char *wine_candidates[] = {
    "/opt/homebrew/bin/wine64",
    "/opt/homebrew/bin/wine",
    "/usr/local/bin/wine64",
    "/usr/local/bin/wine",
    "/opt/local/bin/wine64",
    "/opt/local/bin/wine",
    "/Applications/Wine Stable.app/Contents/Resources/wine/bin/wine64",
    "/Applications/Wine Stable.app/Contents/Resources/wine/bin/wine",
    "/Applications/Wine.app/Contents/Resources/wine/bin/wine64",
    "/Applications/Wine.app/Contents/Resources/wine/bin/wine",
    NULL
};

static int run_with_wine(const char *wine, const char *exe)
{
    pid_t pid;
    char *args[] = { (char *)wine, (char *)exe, NULL };
    posix_spawn_file_actions_t fa;
    int status;

    posix_spawn_file_actions_init(&fa);
    /* GUI 启动无终端：丢弃子进程标准流，避免挂在管道上 */
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null",
                                     O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null",
                                     O_WRONLY, 0);
    if (posix_spawn(&pid, wine, &fa, NULL, args, environ) != 0) {
        posix_spawn_file_actions_destroy(&fa);
        return -1;
    }
    posix_spawn_file_actions_destroy(&fa);
    while (waitpid(pid, &status, 0) < 0) {}
    return 0;
}

/* 在 PATH 中查找可执行程序 */
static int spawnp_exists(const char *prog)
{
    const char *path = getenv("PATH");
    char buf[4096], *save = NULL, *d;
    if (!path) path = "/usr/bin:/bin:/usr/local/bin:/opt/homebrew/bin";
    snprintf(buf, sizeof buf, "%s", path);
    for (d = strtok_r(buf, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
        char cand[1024];
        snprintf(cand, sizeof cand, "%s/%s", d, prog);
        if (exists(cand) && access(cand, X_OK) == 0) return 1;
    }
    return 0;
}

static void show_dialog(void)
{
    /* osascript 原生对话框（中英双语） */
    system(
        "osascript -e 'tell application \"System Events\" to display dialog "
        "\"此应用需要 Wine 才能运行其中的 Windows 程序。\\n\\n"
        "This app requires Wine to run the embedded Windows program.\\n\\n"
        "安装 / Install:\\n"
        "brew install --cask --no-quarantine wine-stable\\n"
        "Apple Silicon 还需 Rosetta 2。\" "
        "buttons {\"OK\"} default button \"OK\" with icon caution "
        "with title \"e1wine Launcher\"' >/dev/null 2>&1 &");
}

int main(int argc, char **argv)
{
    char selfbuf[4096], self[4096], exe[4096], resdir[4096];
    char payload[4096], namebuf[256];
    const char *exe_name = NULL;
    FILE *f;
    int i;
    (void)argc;

    /* self = .../Name.app/Contents/MacOS/e1wine-launcher */
    if (realpath(argv[0], selfbuf) == NULL) {
        fprintf(stderr, "e1wine-launcher: cannot resolve self path\n");
        return 1;
    }
    snprintf(self, sizeof self, "%s", selfbuf);
    snprintf(resdir, sizeof resdir, "%s/../Resources", dirname(self));

    snprintf(payload, sizeof payload, "%s/payload-name", resdir);
    f = fopen(payload, "r");
    if (f) {
        if (fgets(namebuf, sizeof namebuf, f)) {
            size_t l = strlen(namebuf);
            if (l && namebuf[l - 1] == '\n') namebuf[--l] = 0;
            if (l) exe_name = namebuf;
        }
        fclose(f);
    }

    if (!exe_name) exe_name = "program.exe";
    snprintf(exe, sizeof exe, "%s/%s", resdir, exe_name);
    if (!exists(exe)) {
        fprintf(stderr, "e1wine-launcher: payload not found: %s\n", exe);
        return 1;
    }

    /* 1) 绝对路径候选 */
    for (i = 0; wine_candidates[i]; i++) {
        if (exists(wine_candidates[i]) && run_with_wine(wine_candidates[i], exe) == 0)
            return 0;
    }
    /* 2) PATH 中的 wine64 / wine */
    if (spawnp_exists("wine64") && run_with_wine("wine64", exe) == 0)
        return 0;
    if (spawnp_exists("wine") && run_with_wine("wine", exe) == 0)
        return 0;

    /* 3) 没装 Wine：弹窗提示并退出 */
    show_dialog();
    return 1;
}
