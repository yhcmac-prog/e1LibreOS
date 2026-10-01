# 安装指南：setup-e1os

`setup-e1os` 是 e1LibreOS 的系统安装器，交互问答与 Alpine `setup-alpine` 逐项对齐
（键盘布局、主机名、网卡、root 密码、时区、代理、NTP、镜像源、用户、SSH、磁盘），
并在 “How would you like to use it?” 步骤提供五种安装模式。

```sh
setup-e1os                 # 交互式
setup-e1os --auto /dev/sda # 非交互（环境变量预置答案）
```

## 五种安装模式

### 1. sys —— 传统整盘安装

- **BIOS**：MBR 单分区 + ext4 + extlinux
- **UEFI**：GPT（ESP 512 MiB + root）+ GRUB（x86_64-efi / arm64-efi）
- 系统完全安装到硬盘，从硬盘引导

### 2. data —— 数据盘模式

- 系统仍以 diskless 方式从内存运行
- 数据盘持久化 `/var`、apk 缓存与配置（apkovl）
- 适合 U 盘/网络启动的固定工作站

### 3. custom —— 挂载点分配模式

GPT 分区，可精确分配各挂载点：

| 挂载点 | 状态 |
|---|---|
| `/`（root） | **必选** |
| `/boot/efi`（ESP） | **必选**（UEFI）；BIOS 下为 biosboot |
| `/home` | 推荐 |
| swap | 推荐（默认大小 = 内存） |

非交互变量：`ESP_MB`（默认 512）、`ROOT_MB`（0 = 剩余空间）、
`HOME_MB`、`SWAP_MB`。

### 4. preserve —— 保留现有操作系统

- 不动其它已有分区，仅在空闲空间创建 e1LibreOS 分区
- GRUB 自动生成链式引导（chainload）菜单项，与共存系统在开机菜单中选择

### 5. none —— 无盘模式

- 系统始终在内存运行，不写磁盘
- 配置通过 lbu 风格 `.apkovl` 包持久化
- `LBU_MEDIA` 可选 `usb`（U 盘）/ `data`（数据盘）/ `none`（纯内存）

## `--auto` 环境变量速查

| 变量 | 默认 | 说明 |
|---|---|---|
| `HOSTNAME` | e1libreos | 主机名 |
| `ROOTPASS` | e1os | root 密码 |
| `KEYMAP` / `KEYMAP_VARIANT` | none | 键盘布局 |
| `NIC` / `IPADDR` | 自动探测 / dhcp | 网卡与地址（dhcp/none/静态） |
| `TIMEZONE` | UTC | 时区（如 Asia/Shanghai） |
| `PROXY_URL` | none | HTTP 代理 |
| `NTPCLIENT` | chrony | busybox / openntpd / chrony / none |
| `MIRROR_PICK` | 1 | 1=e1repo，2=阿里云，3=官方，f=最快 |
| `USERNAME` / `USERPASS` | no / e1os | 普通用户 |
| `SSHD` / `SSH_ROOT` | openssh / prohibit-password | SSH 服务与 root 策略 |
| `DISKMODE` | sys | sys / data / custom / preserve / none |
| `FIRMWARE` | 自动检测 | efi / bios |
| `ESP_MB` | 512 | ESP 大小 |
| `ROOT_MB` | 0 | root 大小（0 = 全部剩余） |
| `HOME_MB` | 0 | /home 大小（0 = 不建） |
| `SWAP_MB` | = 内存 | swap 大小 |
| `LBU_MEDIA` | none | none 模式持久化介质：usb/data/none |

### 示例

```sh
# UEFI + 挂载点分配：ESP 512M、/home 2G、swap 1G、root 占剩余
DISKMODE=custom FIRMWARE=efi HOME_MB=2048 SWAP_MB=1024 \
    setup-e1os --auto /dev/sda

# 纯无盘，配置存 U 盘
DISKMODE=none LBU_MEDIA=usb setup-e1os --auto

# 静态 IP + 上海时区 + 普通用户
IPADDR=192.168.1.20 NETMASK=255.255.255.0 GATEWAY=192.168.1.1 \
TIMEZONE=Asia/Shanghai USERNAME=alice setup-e1os --auto /dev/vda
```

## 引导加载器说明

- 安装器在目标系统通过 apk 安装 `grub-efi efibootmgr dosfstools`（UEFI）或
  `grub-bios`（BIOS），随后 `grub-install`：
  - 先尝试写入 NVRAM 启动项；
  - 失败（如虚拟机/移动介质无 NVRAM 权限）自动回退到可移动介质路径
    `/EFI/BOOT/BOOTX64.EFI`（aarch64 为 `BOOTAA64.EFI`）。
- ESP 上的 grub stub 用 UUID `search` 定位根分区后读取 `/boot/grub/grub.cfg`，
  因此更换磁盘顺序不影响引导。

## 实测状态

- x86_64 BIOS：sys / data / custom / diskless 已在 VirtualBox 实测通过
- x86_64 UEFI（OVMF）：sys / custom 已实测通过（GPT + GRUB + 硬盘引导 + e1wm）
- aarch64：镜像可构建，建议在真机/ARM 虚拟机上验证
