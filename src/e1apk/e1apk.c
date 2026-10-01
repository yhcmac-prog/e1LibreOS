/*
 * e1apk — e1LibreOS 的 Android APK 工具（ZIP 解包 + 二进制 AXML 清单解析）
 *
 * APK 本质是 ZIP：本工具自研 ZIP 中央目录解析与 RAW DEFLATE 解码
 * （puff 风格，免 zlib 依赖），并解析 AndroidManifest.xml 的二进制
 * AXML 格式，提取包名/版本/SDK/权限/入口 Activity 等信息。
 *
 * 双平台：与 e1wine 同一套构建模式（tools 内联进 build.sh）：
 *   - Linux  版：x86_64-linux-musl 静态链接，内置于 e1LibreOS
 *   - macOS 版：cc 原生编译，用于开发调试
 *
 * 用法：
 *   e1apk info <file.apk>             显示清单信息（包名/版本/SDK/权限/入口）
 *   e1apk list <file.apk>             列出 APK 内容
 *   e1apk extract <file.apk> [目录]   解包全部条目（默认 ./<名>_ext）
 *   e1apk run <file.apk>              运行时检查（不执行 Dalvik 字节码，见说明）
 *   e1apk --version                   打印版本
 *
 * 关于 run：APK 的代码是 classes.dex 中的 Dalvik 字节码，原生执行需要
 * 完整 ART/Dalvik 运行时（远超最小兼容层范围）。本工具的 run 只做
 * 运行时可行性检查并如实报告；解包后的 assets/资源可供其他工具使用。
 *
 * 限制：
 *   - 仅标准 ZIP（不支持 ZIP64、加密、zipinfo 差异分卷）
 *   - AXML 仅解析清单常用属性；资源表（arsc）不解析，@ref 值按原样显示
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>

#ifndef E1APK_PLATFORM
#define E1APK_PLATFORM "unknown"
#endif
#define E1APK_VERSION "1.0"
#define E1APK_BANNER  "e1apk-" E1APK_VERSION

#define ZIP_EOCD_SIG 0x06054b50
#define ZIP_CD_SIG   0x02014b50
#define ZIP_LFH_SIG  0x04034b50

/* ============== 基础工具 ============== */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void die(const char *fmt, const char *arg) {
    fprintf(stderr, "e1apk: ");
    fprintf(stderr, fmt, arg);
    fprintf(stderr, "\n");
    exit(1);
}

static void *map_file(const char *path, size_t *size_out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("cannot open %s", path);
    struct stat st;
    if (fstat(fd, &st) != 0) die("fstat %s failed", path);
    void *map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) die("mmap %s failed", path);
    *size_out = st.st_size;
    return map;
}

/* ============== RAW DEFLATE（puff 风格）============== */
/* 输出缓冲区即 32K 滑动窗口（回溯距离不超过已输出长度且 ≤32768） */

struct infl {
    const uint8_t *in;  size_t inlen, inpos;
    uint32_t bitbuf;    int bitcnt;
    uint8_t *out;       size_t outlen, outpos;
    int err;
};

static int getb(struct infl *s, int need) {
    if (need == 0) return 0;
    while (s->bitcnt < need) {
        if (s->inpos >= s->inlen) { s->err = 1; return 0; }
        s->bitbuf |= (uint32_t)s->in[s->inpos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    int v = (int)(s->bitbuf & ((1u << need) - 1));
    s->bitbuf >>= need; s->bitcnt -= need;
    return v;
}

struct huff { int16_t count[16]; int16_t symbol[288]; };

/* 由码长数组构造规范霍夫曼表；返回 0=完整 >0=不完整(合法) -1=全零 -2=过订阅 */
static int hconstruct(struct huff *h, const uint16_t *lengths, int n) {
    int sym, len, left;
    for (len = 0; len <= 15; len++) h->count[len] = 0;
    for (sym = 0; sym < n; sym++) h->count[lengths[sym]]++;
    if (h->count[0] == n) return -1;
    left = 1;
    for (len = 1; len <= 15; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -2;
    }
    int16_t offs[16];
    offs[1] = 0;
    for (len = 1; len < 15; len++) offs[len + 1] = offs[len] + h->count[len];
    for (sym = 0; sym < n; sym++)
        if (lengths[sym]) h->symbol[offs[lengths[sym]]++] = (int16_t)sym;
    return left;
}

static int hdecode(struct infl *s, const struct huff *h) {
    int len = 1, code = 0, first = 0, index = 0;
    while (len <= 15) {
        code |= getb(s, 1);
        if (s->err) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count;
        first <<= 1; code <<= 1; len++;
    }
    s->err = 2;
    return -1;
}

static const uint16_t lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,
                                   51,59,67,83,99,115,131,163,195,227,258};
static const uint16_t lext[29]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const uint16_t dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,
                                   513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const uint16_t dext[30]  = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
static const uint16_t clorder[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

/* 解码 RAW DEFLATE 流（无 zlib/gzip 头，即 ZIP method 8）；0=成功 */
static int inflate(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen) {
    struct infl S = { in, inlen, 0, 0, 0, out, outlen, 0, 0 };
    struct infl *s = &S;
    struct huff lit, dist, cl;
    int last;

    do {
        last = getb(s, 1);
        int type = getb(s, 2);
        if (s->err) return s->err;

        if (type == 0) {
            /* 存储块：丢弃位缓冲剩余位对齐到字节边界 */
            s->bitbuf = 0; s->bitcnt = 0;
            if (s->inpos + 4 > s->inlen) { s->err = 1; return s->err; }
            uint16_t len = (uint16_t)(s->in[s->inpos] | (s->in[s->inpos + 1] << 8));
            s->inpos += 4;   /* LEN + NLEN */
            if (s->inpos + len > s->inlen || s->outpos + len > s->outlen) { s->err = 5; return s->err; }
            memcpy(s->out + s->outpos, s->in + s->inpos, len);
            s->inpos += len; s->outpos += len;
        } else if (type == 1 || type == 2) {
            if (type == 2) {
                /* 动态霍夫曼表 */
                int nlen = getb(s, 5) + 257;
                int ndist = getb(s, 5) + 1;
                int ncode = getb(s, 4) + 4;
                if (s->err) return s->err;
                uint16_t cllens[19];
                memset(cllens, 0, sizeof cllens);
                for (int i = 0; i < ncode; i++) cllens[clorder[i]] = (uint16_t)getb(s, 3);
                if (s->err) return s->err;
                if (hconstruct(&cl, cllens, 19) < 0) return 3;

                uint16_t lens[288 + 30];
                int n = 0;
                while (n < nlen + ndist) {
                    int sym = hdecode(s, &cl);
                    if (s->err) return s->err;
                    if (sym < 16) {
                        lens[n++] = (uint16_t)sym;
                    } else {
                        int prev = n ? lens[n - 1] : 0, rep;
                        if (sym == 16) {
                            if (!n) return 3;
                            rep = 3 + getb(s, 2);
                        } else if (sym == 17) {
                            prev = 0; rep = 3 + getb(s, 3);
                        } else {
                            prev = 0; rep = 11 + getb(s, 7);
                        }
                        if (s->err) return s->err;
                        if (n + rep > nlen + ndist) return 3;
                        while (rep--) lens[n++] = (uint16_t)prev;
                    }
                }
                if (lens[256] == 0) return 3;   /* 必须有块结束码 */
                if (hconstruct(&lit, lens, nlen) < 0) return 3;
                int dl = hconstruct(&dist, lens + nlen, ndist);
                if (dl < -1) return 3;          /* -1 = 全零距离表（无匹配），合法 */
            } else {
                /* 固定霍夫曼表 */
                uint16_t fl[288], fd[30];
                for (int i = 0; i < 144; i++) fl[i] = 8;
                for (int i = 144; i < 256; i++) fl[i] = 9;
                for (int i = 256; i < 280; i++) fl[i] = 7;
                for (int i = 280; i < 288; i++) fl[i] = 8;
                for (int i = 0; i < 30; i++) fd[i] = 5;
                hconstruct(&lit, fl, 288);
                hconstruct(&dist, fd, 30);
            }

            /* 块主体解码 */
            while (1) {
                int sym = hdecode(s, &lit);
                if (s->err) return s->err;
                if (sym < 256) {
                    if (s->outpos >= s->outlen) { s->err = 6; return s->err; }
                    s->out[s->outpos++] = (uint8_t)sym;
                } else if (sym == 256) {
                    break;
                } else {
                    sym -= 257;
                    if (sym >= 29) return 3;
                    int len = lbase[sym] + getb(s, lext[sym]);
                    int dsym = hdecode(s, &dist);
                    if (s->err) return s->err;
                    if (dsym >= 30) return 3;
                    int d = dbase[dsym] + getb(s, dext[dsym]);
                    if (s->err) return s->err;
                    if (d > (int)s->outpos || d > 32768) return 4;
                    if (s->outpos + (size_t)len > s->outlen) { s->err = 6; return s->err; }
                    while (len--) {
                        s->out[s->outpos] = s->out[s->outpos - d];
                        s->outpos++;
                    }
                }
            }
        } else {
            return 3;
        }
    } while (!last);
    return 0;
}

/* ============== ZIP 读取 ============== */

struct zip_entry {
    char *name;
    uint16_t method;
    uint32_t csize, usize;
    uint32_t lho;       /* local header offset */
};

struct zip {
    uint8_t *map; size_t size;
    struct zip_entry *ents; int n_ents;
};

static void zip_open(struct zip *z, const char *path) {
    memset(z, 0, sizeof *z);
    z->map = map_file(path, &z->size);
    if (z->size < 22) die("%s: too small for a ZIP", path);

    /* 从尾部扫描 EOCD（最多回退 64KB 注释区） */
    size_t eocd = (size_t)-1;
    size_t lo = z->size > 22 + 65536 ? z->size - 22 - 65536 : 0;
    for (size_t i = z->size - 22 + 1; i-- > lo; ) {
        if (rd32(z->map + i) == ZIP_EOCD_SIG) { eocd = i; break; }
    }
    if (eocd == (size_t)-1) die("%s: end-of-central-directory not found", path);

    int total = rd16(z->map + eocd + 10);
    uint32_t cdoff = rd32(z->map + eocd + 16);
    uint32_t cdsize = rd32(z->map + eocd + 12);
    if ((size_t)cdoff + cdsize > z->size) die("%s: central directory out of range", path);

    z->ents = calloc(total ? total : 1, sizeof(struct zip_entry));
    const uint8_t *p = z->map + cdoff;
    const uint8_t *end = p + cdsize;
    for (int i = 0; i < total && p + 46 <= end; i++) {
        if (rd32(p) != ZIP_CD_SIG) die("%s: bad central directory entry", path);
        uint16_t namelen = rd16(p + 28);
        uint16_t extralen = rd16(p + 30);
        uint16_t commentlen = rd16(p + 32);
        struct zip_entry *e = &z->ents[z->n_ents++];
        e->method = rd16(p + 10);
        e->csize = rd32(p + 20);
        e->usize = rd32(p + 24);
        e->lho = rd32(p + 42);
        e->name = malloc(namelen + 1);
        memcpy(e->name, p + 46, namelen);
        e->name[namelen] = 0;
        p += 46 + namelen + extralen + commentlen;
    }
}

static struct zip_entry *zip_find(struct zip *z, const char *name) {
    for (int i = 0; i < z->n_ents; i++)
        if (strcmp(z->ents[i].name, name) == 0) return &z->ents[i];
    return NULL;
}

/* 读取并解压一个条目；返回 malloc 缓冲（+1 尾零）或 NULL */
static uint8_t *zip_read(struct zip *z, struct zip_entry *e) {
    if (e->lho + 30 > z->size) return NULL;
    const uint8_t *p = z->map + e->lho;
    if (rd32(p) != ZIP_LFH_SIG) return NULL;
    uint16_t nlen = rd16(p + 26), elen = rd16(p + 28);
    const uint8_t *src = p + 30 + nlen + elen;
    if ((size_t)(src - z->map) + e->csize > z->size) return NULL;

    uint8_t *out = malloc((size_t)e->usize + 1);
    if (!out) return NULL;
    if (e->method == 0) {
        memcpy(out, src, e->usize);
    } else if (e->method == 8) {
        if (inflate(src, e->csize, out, e->usize) != 0) { free(out); return NULL; }
    } else {
        free(out);
        return NULL;
    }
    out[e->usize] = 0;
    return out;
}

static void zip_close(struct zip *z) {
    for (int i = 0; i < z->n_ents; i++) free(z->ents[i].name);
    free(z->ents);
    munmap(z->map, z->size);
}

/* ============== 二进制 AXML 解析 ============== */

#define AXML_MAX_STRS      4096
#define AXML_MAX_PERMS     16
#define AXML_MAX_ACTIVITIES 16

struct axml {
    char *strs[AXML_MAX_STRS];
    int n_strs;
};

struct manifest {
    char package[128], version_name[64], label[128];
    long version_code, min_sdk, target_sdk;
    char permissions[AXML_MAX_PERMS][128];
    int n_perm;
    char activities[AXML_MAX_ACTIVITIES][128];
    uint8_t launcher[AXML_MAX_ACTIVITIES];
    int n_act;
    char main_activity[128];
};

static const char *axml_str(const struct axml *a, int idx) {
    if (idx < 0 || idx >= a->n_strs) return "?";
    return a->strs[idx] ? a->strs[idx] : "?";
}

/* UTF-16LE → UTF-8（BMP 内） */
static char *utf16_to_utf8(const uint8_t *p, int units) {
    char *out = malloc((size_t)units * 4 + 1);
    int o = 0;
    for (int i = 0; i < units; i++) {
        uint32_t c = p[2 * i] | ((uint32_t)p[2 * i + 1] << 8);
        if (c < 0x80) {
            out[o++] = (char)c;
        } else if (c < 0x800) {
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (char)(0xE0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = 0;
    return out;
}

/* 解析 string pool 块（紧跟 8 字节文件头） */
static int axml_strings(const uint8_t *p, size_t size, struct axml *a) {
    memset(a, 0, sizeof *a);
    if (size < 8 || rd16(p) != 0x0003) return -1;
    size_t off = 8;
    if (off + 28 > size || rd16(p + off) != 0x0001) return -1;
    uint32_t hsize = rd16(p + off + 2);
    uint32_t csize = rd32(p + off + 4);
    if (off + csize > size) return -1;
    uint32_t count = rd32(p + off + 8);
    uint32_t flags = rd32(p + off + 16);
    uint32_t sstart = rd32(p + off + 20);
    if (count > AXML_MAX_STRS) count = AXML_MAX_STRS;
    int utf8 = flags & 0x100;

    const uint8_t *base = p + off + sstart;
    const uint8_t *offs_p = p + off + hsize;
    a->n_strs = (int)count;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t so = rd32(offs_p + 4 * i);
        const uint8_t *sp = base + so;
        if ((size_t)(sp - p) >= size) continue;
        if (utf8) {
            /* 双长度前缀：解码字符数、编码字节数（1 或 2 字节变体） */
            int el;
            if (*sp & 0x80) { sp += 2; } else { sp += 1; }          /* decLen（忽略） */
            if (*sp & 0x80) { el = ((sp[0] & 0x7F) << 8) | sp[1]; sp += 2; }
            else            { el = *sp; sp += 1; }
            if ((size_t)(sp - p) + (size_t)el > size) el = 0;
            a->strs[i] = malloc((size_t)el + 1);
            memcpy(a->strs[i], sp, (size_t)el);
            a->strs[i][el] = 0;
        } else {
            uint32_t ulen = rd16(sp);
            if (ulen & 0x8000) { ulen = ((ulen & 0x7FFF) << 16) | rd16(sp + 2); sp += 4; }
            else sp += 2;
            if ((size_t)(sp - p) + (size_t)ulen * 2 > size) ulen = 0;
            a->strs[i] = utf16_to_utf8(sp, (int)ulen);
        }
    }
    return 0;
}

/* 遍历元素块并填充 manifest（状态机） */
static void axml_parse_manifest(const uint8_t *p, size_t size,
                                const struct axml *a, struct manifest *m) {
    memset(m, 0, sizeof *m);
    int in_act = 0, saw_main = 0, saw_launcher = 0, cur_act = -1;
    size_t off = 8;

    while (off + 8 <= size) {
        uint16_t type = rd16(p + off);
        uint32_t csize = rd32(p + off + 4);
        if (csize < 8 || off + csize > size) break;

        if (type == 0x0102 && csize >= 36) {
            /* START_ELEMENT：node(16) + attrExt(20) + attrs[] */
            const uint8_t *ax = p + off + 16;
            const char *ename = axml_str(a, (int)rd32(ax + 4));
            uint16_t astart = rd16(ax + 8);
            uint16_t asize = rd16(ax + 10);
            uint16_t acount = rd16(ax + 12);
            if (asize < 20) asize = 20;

            if (strcmp(ename, "activity") == 0) {
                in_act = 1; saw_main = 0; saw_launcher = 0;
            }

            const uint8_t *attrs = ax + astart;
            for (uint16_t i = 0; i < acount; i++) {
                const uint8_t *at = attrs + (size_t)i * asize;
                if ((size_t)(at + 20 - p) > size) break;
                const char *an = axml_str(a, (int)rd32(at + 4));
                int32_t raw = (int32_t)rd32(at + 8);
                uint8_t dtype = at[15];
                uint32_t ddata = rd32(at + 16);
                char val[160];
                if (raw >= 0)
                    snprintf(val, sizeof val, "%s", axml_str(a, raw));
                else if (dtype == 0x03)   /* STRING */
                    snprintf(val, sizeof val, "%s", axml_str(a, (int)ddata));
                else if (dtype == 0x10)   /* INT_DEC */
                    snprintf(val, sizeof val, "%ld", (long)(int32_t)ddata);
                else if (dtype == 0x12)   /* INT_BOOLEAN */
                    snprintf(val, sizeof val, "%s", ddata ? "true" : "false");
                else
                    snprintf(val, sizeof val, "0x%x", ddata);

                if (strcmp(ename, "manifest") == 0) {
                    if (strcmp(an, "package") == 0)
                        snprintf(m->package, sizeof m->package, "%s", val);
                    else if (strcmp(an, "versionCode") == 0)
                        m->version_code = atol(val);
                    else if (strcmp(an, "versionName") == 0)
                        snprintf(m->version_name, sizeof m->version_name, "%s", val);
                } else if (strcmp(ename, "uses-sdk") == 0) {
                    if (strcmp(an, "minSdkVersion") == 0)
                        m->min_sdk = atol(val);
                    else if (strcmp(an, "targetSdkVersion") == 0)
                        m->target_sdk = atol(val);
                } else if (strcmp(ename, "uses-permission") == 0) {
                    if (strcmp(an, "name") == 0 && m->n_perm < AXML_MAX_PERMS)
                        snprintf(m->permissions[m->n_perm++],
                                 sizeof m->permissions[0], "%s", val);
                } else if (strcmp(ename, "application") == 0) {
                    if (strcmp(an, "label") == 0)
                        snprintf(m->label, sizeof m->label, "%s", val);
                } else if (strcmp(ename, "activity") == 0) {
                    if (strcmp(an, "name") == 0 && m->n_act < AXML_MAX_ACTIVITIES) {
                        cur_act = m->n_act;
                        snprintf(m->activities[m->n_act++],
                                 sizeof m->activities[0], "%s", val);
                    }
                } else if (in_act && strcmp(ename, "action") == 0) {
                    if (strcmp(an, "name") == 0 &&
                        strcmp(val, "android.intent.action.MAIN") == 0)
                        saw_main = 1;
                } else if (in_act && strcmp(ename, "category") == 0) {
                    if (strcmp(an, "name") == 0 &&
                        strcmp(val, "android.intent.category.LAUNCHER") == 0)
                        saw_launcher = 1;
                }
            }
        } else if (type == 0x0103 && csize >= 24) {
            /* END_ELEMENT：node(16) + ns(4) + name(4) */
            const char *ename = axml_str(a, (int)rd32(p + off + 16 + 4));
            if (strcmp(ename, "activity") == 0) {
                if (in_act && saw_main && saw_launcher && cur_act >= 0) {
                    m->launcher[cur_act] = 1;
                    if (!m->main_activity[0])
                        snprintf(m->main_activity, sizeof m->main_activity,
                                 "%s", m->activities[cur_act]);
                }
                in_act = 0; cur_act = -1;
            }
        }
        off += csize;
    }
}

/* ============== 命令实现 ============== */

static void cmd_list(struct zip *z) {
    for (int i = 0; i < z->n_ents; i++)
        printf("%8u  %s\n", z->ents[i].usize, z->ents[i].name);
}

static void cmd_info(struct zip *z) {
    printf("entries:     %d\n", z->n_ents);

    struct zip_entry *mf = zip_find(z, "AndroidManifest.xml");
    if (!mf) {
        printf("manifest:    (AndroidManifest.xml not found)\n");
        return;
    }
    uint8_t *data = zip_read(z, mf);
    if (!data) {
        printf("manifest:    (decompress failed)\n");
        return;
    }
    struct axml a;
    struct manifest m;
    if (axml_strings(data, mf->usize, &a) == 0) {
        axml_parse_manifest(data, mf->usize, &a, &m);
        printf("package:     %s\n", m.package);
        printf("version:     %s (%ld)\n",
               m.version_name[0] ? m.version_name : "-", m.version_code);
        if (m.min_sdk || m.target_sdk)
            printf("sdk:         min=%ld target=%ld\n", m.min_sdk, m.target_sdk);
        if (m.label[0])
            printf("label:       %s\n", m.label);
        if (m.n_perm) {
            printf("permissions: %d\n", m.n_perm);
            for (int i = 0; i < m.n_perm; i++)
                printf("  %s\n", m.permissions[i]);
        }
        printf("activities:  %d\n", m.n_act);
        for (int i = 0; i < m.n_act; i++)
            printf("  %s%s\n", m.activities[i], m.launcher[i] ? "  [launcher]" : "");
    } else {
        printf("manifest:    (not binary AXML)\n");
    }
    free(data);

    struct zip_entry *dex = zip_find(z, "classes.dex");
    if (dex) {
        uint8_t *d = zip_read(z, dex);
        if (d && dex->usize >= 8 && memcmp(d, "dex\n", 4) == 0)
            printf("dex:         version %.3s, %u bytes\n", d + 4, dex->usize);
        else
            printf("dex:         %u bytes (unrecognized)\n", dex->usize);
        free(d);
    }
}

/* 递归 mkdir（路径内逐级创建） */
static void mkdirs_for(const char *path) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *q = buf + 1; *q; q++) {
        if (*q == '/') {
            *q = 0;
            mkdir(buf, 0755);
            *q = '/';
        }
    }
}

static int path_dangerous(const char *name) {
    if (name[0] == '/') return 1;
    const char *p = name;
    while (p) {
        if (p[0] == '.' && p[1] == '.' &&
            (p[2] == '/' || p[2] == 0)) return 1;
        p = strchr(p, '/');
        if (p) p++;
    }
    return 0;
}

static int cmd_extract(struct zip *z, const char *outdir) {
    int ok = 0, fail = 0;
    for (int i = 0; i < z->n_ents; i++) {
        struct zip_entry *e = &z->ents[i];
        if (path_dangerous(e->name)) {
            fprintf(stderr, "e1apk: skip unsafe entry '%s'\n", e->name);
            fail++;
            continue;
        }
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", outdir, e->name);
        int is_dir = e->name[0] && e->name[strlen(e->name) - 1] == '/';
        if (is_dir) {
            mkdirs_for(path);
            mkdir(path, 0755);
            printf("d %s\n", path);
            ok++;
            continue;
        }
        mkdirs_for(path);
        uint8_t *data = zip_read(z, e);
        if (!data) {
            fprintf(stderr, "e1apk: extract failed: %s\n", e->name);
            fail++;
            continue;
        }
        FILE *f = fopen(path, "wb");
        if (!f) {
            fprintf(stderr, "e1apk: cannot write %s: %s\n", path, strerror(errno));
            free(data);
            fail++;
            continue;
        }
        fwrite(data, 1, e->usize, f);
        fclose(f);
        chmod(path, 0644);
        printf("f %s (%u bytes)\n", path, e->usize);
        free(data);
        ok++;
    }
    printf("extracted: %d ok, %d failed -> %s\n", ok, fail, outdir);
    return fail ? 1 : 0;
}

static void cmd_run(struct zip *z, const char *path) {
    printf("%s (%s)\n", E1APK_BANNER, E1APK_PLATFORM);
    struct zip_entry *dex = zip_find(z, "classes.dex");
    if (!dex) {
        printf("run: no classes.dex found in %s\n", path);
        return;
    }
    printf("run: classes.dex found (%u bytes, Dalvik bytecode)\n", dex->usize);
    printf("run: native execution requires a full ART/Dalvik runtime, which is\n"
           "     beyond this minimal compatibility layer.\n"
           "     Use 'e1apk info/extract' for manifest inspection and assets.\n");
}

/* ============== 主入口 ============== */

static void usage(void) {
    fprintf(stderr,
        "%s — Android APK toolkit for e1LibreOS (%s build)\n\n"
        "Usage:\n"
        "  e1apk info <file.apk>             Show manifest info\n"
        "  e1apk list <file.apk>             List APK contents\n"
        "  e1apk extract <file.apk> [dir]    Extract all entries\n"
        "  e1apk run <file.apk>              Runtime check (no Dalvik execution)\n"
        "  e1apk --version                   Print version and exit\n\n"
        "The runtime executes no Dalvik/ART bytecode by design; APK handling is\n"
        "limited to ZIP unpacking and binary AXML manifest parsing.\n",
        E1APK_BANNER, E1APK_PLATFORM);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }

    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
        printf("%s (%s)\n", E1APK_BANNER, E1APK_PLATFORM);
        return 0;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }

    const char *cmd = argv[1];
    if (argc < 3) { usage(); return 1; }
    const char *file = argv[2];

    struct zip z;
    zip_open(&z, file);

    if (strcmp(cmd, "info") == 0) {
        cmd_info(&z);
    } else if (strcmp(cmd, "list") == 0) {
        cmd_list(&z);
    } else if (strcmp(cmd, "extract") == 0) {
        char defdir[512];
        const char *dir = argv[3];
        if (!dir) {
            /* 默认输出目录：<去 .apk 后缀>_ext */
            const char *slash = strrchr(file, '/');
            const char *base = slash ? slash + 1 : file;
            size_t blen = strlen(base);
            if (blen > 4 && strcasecmp(base + blen - 4, ".apk") == 0)
                blen -= 4;
            if (blen >= sizeof defdir - 8) blen = sizeof defdir - 8;
            snprintf(defdir, sizeof defdir, "%.*s_ext", (int)blen, base);
            dir = defdir;
        }
        return cmd_extract(&z, dir);
    } else if (strcmp(cmd, "run") == 0) {
        cmd_run(&z, file);
        return 2;   /* 按约定：运行时不可执行返回非零 */
    } else {
        usage();
        zip_close(&z);
        return 1;
    }
    zip_close(&z);
    return 0;
}
