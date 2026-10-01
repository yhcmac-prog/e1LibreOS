# e1repo —— e1LibreOS 依赖仓库

e1repo 是 e1LibreOS 的在线依赖仓库，承担两类职责：

1. **e1pkg 包仓库**：为自研 dnf 风格包管理器提供索引与包文件
2. **Alpine apk 镜像目录**：`setup-e1os` 安装系统时经此拉取 apk 包
   （grub、linux-lts、openssh、chrony 等），同时作为 Alpine 公共镜像的本地缓存

公开仓库地址：<https://github.com/yhcmac-prog/e1repo>

## 目录结构

```
e1repo/
├── README.md
├── repo.db                         # e1pkg 索引
├── packages/                       # e1pkg 包文件（.e1.tar.gz）
│   ├── hello-1.0.e1.tar.gz
│   ├── cowsay-1.0.e1.tar.gz
│   └── e1fetch-1.0.e1.tar.gz
└── apk/                            # Alpine apk 镜像（setup-e1os 使用）
    └── v3.21/
        ├── main/
        │   ├── x86_64/
        │   │   ├── APKINDEX.tar.gz
        │   │   └── *.apk
        │   └── aarch64/
        │       ├── APKINDEX.tar.gz
        │       └── *.apk
        └── community/
            ├── x86_64/
            └── aarch64/
```

## repo.db 索引格式

每行一个包，字段以 `|` 分隔：

```
name|version|summary|depends|filename
```

示例：

```
hello|1.0|Hello world demo||hello-1.0.e1.tar.gz
cowsay|1.0|ASCII art cow||cowsay-1.0.e1.tar.gz
e1fetch|1.0|Show system info (depends on cowsay)|cowsay|e1fetch-1.0.e1.tar.gz
```

包文件本身是 tar.gz，内含 `meta`（同名字段）与 `payload/`（释放到根目录的文件树）。

## 客户端如何寻址

- **e1pkg**（见 [/etc/e1pkg.conf](../rootfs/etc/e1pkg.conf)）：
  依次尝试 `$URL/repo.db`，包从 `$URL/packages/<filename>` 下载；
  网络源全部失败时回退到本地 `/srv/repo`。
- **setup-e1os**：探测 `$URL/apk/v3.21/main/<arch>/APKINDEX.tar.gz`，
  通过后写入目标系统 `/etc/apk/repositories`（main + community）。

预置主机列表（`E1REPO_HOSTS`）：

```
http://10.0.2.2:8000        # VirtualBox NAT 网关（VM → 宿主机）
http://192.168.1.102:8000   # 局域网地址
```

## 自建本地 e1repo

```sh
# 1. 生成 e1pkg 包索引（在 e1LibreOS 源码树执行 build.sh 时会自动重建到 rootfs/srv/repo）
mkdir -p e1repo/packages
cp rootfs/srv/repo/*.e1.tar.gz e1repo/packages/
cp rootfs/srv/repo/repo.db   e1repo/

# 2. 准备 apk 镜像目录（用 Alpine 官方仓库镜像同步，或仅放需要的包）
mkdir -p e1repo/apk/v3.21/main/x86_64
#   同步示例（宿主机）：
#   rsync -av --include='*.apk' --include='APKINDEX.tar.gz' \
#       rsync://dl-cdn.alpinelinux.org/alpine/v3.21/main/x86_64/ \
#       e1repo/apk/v3.21/main/x86_64/

# 3. 任意静态 HTTP 服务即可
cd e1repo && python3 -m http.server 8000
```

在 e1LibreOS 中验证：

```sh
e1pkg repolist
e1pkg install e1fetch && e1fetch
```

## 添加新的自研包

1. 在源码树 `packages/src/<name>/` 下创建 `meta` 与 `payload/`
2. `meta` 必填 `name=` `version=`，可选 `summary=` `depends=`
3. 重新运行 `./build.sh`，构建脚本自动打包并把索引行追加进 `repo.db`
4. 发布时把新的 `.e1.tar.gz` 与更新后的 `repo.db` 同步到 e1repo 仓库
