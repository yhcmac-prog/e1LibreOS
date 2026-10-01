/*
 * e1gpt — e1LibreOS 安装器用的极简 GPT 分区工具
 *
 * busybox fdisk 的脚本化 GPT 支持在各版本间不稳定，安装器需要精确控制
 * 分区类型 GUID（ESP / Linux / swap），因此用这个零依赖静态小程序直接
 * 写 GPT（保护性 MBR + 主表头/条目 + 备份表头/条目）。
 *
 * 用法:
 *   e1gpt <磁盘> <类型:大小MiB>...
 *
 *   类型: esp       EFI System Partition (C12A7328-...) FAT32
 *         linux     Linux filesystem    (0FC63DAF-...)
 *         swap      Linux swap          (0657FD6D-...)
 *         biosboot  BIOS boot partition (21686148-...) 1MiB
 *
 *   大小: 整数 MiB；最后一个 linux 分区填 0 表示占用全部剩余空间。
 *
 * 例:
 *   e1gpt /dev/sda esp:512 linux:0
 *   e1gpt /dev/sda esp:512 biosboot:1 linux:8192 linux:4096 swap:2048
 *
 * 输出: 每个创建的分区一行 "/dev/sda1 esp 2048 1048576"（设备 类型 起始扇区 结束扇区）
 *
 * 警告：会清空目标盘上的现有分区表。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#ifdef __linux__
#include <linux/fs.h>
#else
#define BLKGETSIZE64 _IOR(0x12,114,size_t)
#define BLKRRPART    _IO(0x12,95)
#endif

/* 不依赖 htole64（旧 musl 头位置不一），小端目标 x86_64/aarch64 直接 memcpy */
static void put_le16(void *p, uint16_t v) { unsigned char *b = p; b[0]=v; b[1]=v>>8; }
static void put_le32(void *p, uint32_t v) { unsigned char *b = p; b[0]=v; b[1]=v>>8; b[2]=v>>16; b[3]=v>>24; }
static void put_le64(void *p, uint64_t v) {
    unsigned char *b = p; int i;
    for (i = 0; i < 8; i++) b[i] = (v >> (8 * i)) & 0xff;
}

/* 按 RFC 4122 字节序存放的 16 字节 GUID */
struct guid { unsigned char b[16]; };

static void guid_set(struct guid *g, const char *s /* "xxxxxxxx-xxxx-...." */) {
    uint32_t d1; uint16_t d2, d3;
    int i;
    unsigned char tail[8];
    sscanf(s, "%08x-%04hx-%04hx-%02hhx%02hhx-%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx",
           &d1, &d2, &d3,
           &tail[0], &tail[1], &tail[2], &tail[3],
           &tail[4], &tail[5], &tail[6], &tail[7]);
    /* GPT 前 3 组小端 */
    put_le32(&g->b[0], d1);
    put_le16(&g->b[4], d2);
    put_le16(&g->b[6], d3);
    for (i = 0; i < 8; i++) g->b[8 + i] = tail[i];
}

static uint32_t crc32(const void *data, size_t len) {
    static uint32_t t[256];
    static int init = 0;
    const unsigned char *p = data;
    uint32_t c = 0xffffffffu;
    size_t i; int j;
    if (!init) {
        for (i = 0; i < 256; i++) {
            uint32_t x = (uint32_t)i;
            for (j = 0; j < 8; j++)
                x = (x & 1) ? (0xedb88320u ^ (x >> 1)) : (x >> 1);
            t[i] = x;
        }
        init = 1;
    }
    for (i = 0; i < len; i++) c = t[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

#define LBA 512
struct part_spec { const char *type; uint64_t mib; };

static void make_header(unsigned char *h, uint64_t self_lba, uint64_t alt_lba,
                        uint64_t first_usable, uint64_t last_usable,
                        const unsigned char *ents) {
    static const unsigned char sig[8] = {'E','F','I',' ','P','A','R','T'};
    memset(h, 0, LBA);
    memcpy(h + 0, sig, 8);
    put_le32(h + 8, 0x00010000);               /* revision 1.0 */
    put_le32(h + 12, 92);                      /* header size */
    put_le32(h + 16, 0);                       /* CRC 占位 */
    put_le32(h + 20, 0);                       /* reserved */
    put_le64(h + 24, self_lba);
    put_le64(h + 32, alt_lba);
    put_le64(h + 40, first_usable);
    put_le64(h + 48, last_usable);
    guid_set((struct guid *)(h + 56), "e1000000-0000-4000-8000-000000000001");
    put_le64(h + 72, 2);                       /* partition entries LBA */
    put_le32(h + 80, 128);                     /* number of entries */
    put_le32(h + 84, 128);                     /* entry size */
    put_le32(h + 88, crc32(ents, 32 * LBA));  /* entries CRC */
    put_le32(h + 16, crc32(h, 92));           /* header CRC */
}

int main(int argc, char **argv) {
    int fd, i, n;
    uint64_t total_sectors;
    unsigned char mbr[LBA], hdr[LBA], ents[32 * LBA], hdr_backup[LBA];
    const char *disk;
    struct part_spec ps[128];
    char *endp;
    uint64_t cur, first_usable, last_usable;

    if (argc < 3) {
        fprintf(stderr,
            "usage: e1gpt <disk> <type:sizeMiB>...  (type: esp|linux|swap|biosboot, last linux size 0=rest)\n");
        return 2;
    }
    disk = argv[1];
    n = argc - 2;
    if (n > 128) { fprintf(stderr, "e1gpt: too many partitions\n"); return 2; }
    for (i = 0; i < n; i++) {
        char buf[64], *colon;
        snprintf(buf, sizeof buf, "%s", argv[2 + i]);
        colon = strchr(buf, ':');
        if (!colon) { fprintf(stderr, "e1gpt: bad spec '%s'\n", argv[2 + i]); return 2; }
        *colon = 0;
        ps[i].type = strdup(buf);
        ps[i].mib = strtoull(colon + 1, &endp, 10);
        if (!strcmp(ps[i].type, "esp") || !strcmp(ps[i].type, "linux") ||
            !strcmp(ps[i].type, "swap") || !strcmp(ps[i].type, "biosboot")) {
            /* ok */
        } else { fprintf(stderr, "e1gpt: unknown type '%s'\n", ps[i].type); return 2; }
    }

    fd = open(disk, O_RDWR | O_EXCL);
    if (fd < 0) { perror("open"); return 1; }
    {
        struct stat st;
        if (fstat(fd, &st) != 0) { perror("fstat"); return 1; }
#ifdef S_ISBLK
        if (S_ISBLK(st.st_mode)) {
            if (ioctl(fd, BLKGETSIZE64, &total_sectors) != 0) { perror("BLKGETSIZE64"); return 1; }
            total_sectors /= LBA;
        } else
#endif
        {
            if (st.st_size <= 0) { fprintf(stderr, "e1gpt: empty image file\n"); return 1; }
            total_sectors = (uint64_t)st.st_size / LBA;
        }
    }
    if (total_sectors < 4096) { fprintf(stderr, "e1gpt: disk too small\n"); return 1; }

    first_usable = 34;
    last_usable = total_sectors - 34;   /* 末尾 33 扇区留给备份条目+表头 */

    /* ---------- 计算布局（2048 扇区对齐） ---------- */
    uint64_t p_start[128], p_end[128];
    cur = 2048;
    /* 第一遍：固定大小分区 */
    for (i = 0; i < n; i++) {
        uint64_t sectors;
        if (!strcmp(ps[i].type, "biosboot")) {
            sectors = 2048;  /* 1 MiB */
        } else if (ps[i].mib == 0 && !strcmp(ps[i].type, "linux")) {
            continue;       /* 第二遍处理 */
        } else {
            sectors = ps[i].mib * 2048;
            if (sectors == 0) { fprintf(stderr, "e1gpt: zero size for %s\n", ps[i].type); return 2; }
        }
        p_start[i] = cur;
        p_end[i] = cur + sectors - 1;
        cur += sectors;
    }
    /* 第二遍：最后一个 size=0 的 linux 分区占满剩余空间 */
    for (i = 0; i < n; i++) {
        if (ps[i].mib == 0 && !strcmp(ps[i].type, "linux")) {
            if (cur >= last_usable) { fprintf(stderr, "e1gpt: no space left for partition %d\n", i+1); return 2; }
            p_start[i] = cur;
            p_end[i] = last_usable;
            cur = last_usable + 1;
        }
    }
    if (cur > last_usable + 1) { fprintf(stderr, "e1gpt: partitions exceed disk capacity\n"); return 2; }

    /* ---------- 保护性 MBR ---------- */
    memset(mbr, 0, sizeof mbr);
    mbr[446+0] = 0x00;                 /* 非活动 */
    mbr[446+1] = 0x00; mbr[446+2] = 0x02; mbr[446+3] = 0x00;
    mbr[446+4] = 0xEE;                 /* GPT protective */
    mbr[446+5] = 0xFF; mbr[446+6] = 0xFF; mbr[446+7] = 0xFF;
    put_le32(mbr + 446 + 8, 1);
    {
        uint32_t cover = (uint32_t)(total_sectors - 1);
        if (total_sectors > 0xffffffffULL) cover = 0xffffffff;
        put_le32(mbr + 446 + 12, cover);
    }
    mbr[510] = 0x55; mbr[511] = 0xAA;

    /* ---------- 分区条目（主/备份共用同一份内容） ---------- */
    memset(ents, 0, sizeof ents);
    for (i = 0; i < n; i++) {
        unsigned char *e = ents + (size_t)i * 128;
        struct guid type_guid, uniq;
        char type_str[40];
        if (!strcmp(ps[i].type, "esp"))
            snprintf(type_str, sizeof type_str, "C12A7328-F81F-11D2-BA4B-00A0C93EC93B");
        else if (!strcmp(ps[i].type, "swap"))
            snprintf(type_str, sizeof type_str, "0657FD6D-A4AB-43C4-84E5-0933C84B4F4F");
        else if (!strcmp(ps[i].type, "biosboot"))
            snprintf(type_str, sizeof type_str, "21686148-6449-6E6F-744E-656564454649");
        else
            snprintf(type_str, sizeof type_str, "0FC63DAF-8483-4772-8E79-3D69D8477DE4");
        guid_set(&type_guid, type_str);
        /* 唯一 GUID：用磁盘扇区位置造一个确定性值 */
        snprintf(type_str, sizeof type_str, "e1000001-0000-4000-8000-%012llx",
                 (unsigned long long)p_start[i]);
        guid_set(&uniq, type_str);
        memcpy(e + 0, type_guid.b, 16);
        memcpy(e + 16, uniq.b, 16);
        put_le64(e + 32, p_start[i]);
        put_le64(e + 40, p_end[i]);         /* EndingLBA：闭区间最后一扇区（UEFI 规范） */
        put_le64(e + 48, 0);                /* attributes */
        snprintf((char *)e + 56, 72, "e1os-%s%d", ps[i].type, i + 1);
    }

    /* ---------- GPT 表头 ---------- */
    make_header(hdr, 1, total_sectors - 1, first_usable, last_usable, ents);
    make_header(hdr_backup, total_sectors - 1, 1, first_usable, last_usable, ents);

    /* ---------- 落盘 ---------- */
    /* 清掉旧 GPT 残留痕迹（头 34 扇区 + 尾 34 扇区），再写新结构 */
    {
        unsigned char zero[LBA];
        memset(zero, 0, sizeof zero);
        for (i = 1; i < 34; i++) {
            if (pwrite(fd, zero, LBA, (off_t)i * LBA) != LBA) { perror("wipe head"); return 1; }
        }
        for (i = 0; i < 33; i++) {
            if (pwrite(fd, zero, LBA, (off_t)(total_sectors - 33 + i) * LBA) != LBA) {
                perror("wipe tail"); return 1;
            }
        }
    }
    #define W(buf, len, off) do { if (pwrite(fd, (buf), (len), (off_t)(off)) != (len)) { \
        perror("write"); return 1; } } while (0)
    W(mbr, LBA, 0);
    W(ents, 32 * LBA, 2ULL * LBA);
    W(hdr, LBA, LBA);
    W(ents, 32 * LBA, (total_sectors - 33) * LBA);   /* 备份条目 */
    W(hdr_backup, LBA, (total_sectors - 1) * LBA);
    fsync(fd);
    ioctl(fd, BLKRRPART, 0);   /* 普通文件镜像上会失败，忽略 */
    close(fd);

    for (i = 0; i < n; i++)
        printf("%s%d %s %llu %llu\n", disk, i + 1, ps[i].type,
               (unsigned long long)p_start[i],
               (unsigned long long)p_end[i]);
    return 0;
}
