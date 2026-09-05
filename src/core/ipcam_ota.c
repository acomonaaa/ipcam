#define _GNU_SOURCE
#include "ipcam_ota.h"
#include "ipcam_log.h"
#include "ipcam_param.h"
#include "ipcam_sys.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

/* ===== SHA256 (Brad Conte's public domain impl) ===== */
#define SHA256_BLOCK_SIZE 32

typedef struct {
    uint8_t  data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} sha256_ctx;

static const uint32_t SHA256_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define SHA_ROTR(a,b) (((a) >> (b)) | ((a) << (32-(b))))
#define SHA_CH(x,y,z)  (((x) & (y)) ^ (~(x) & (z)))
#define SHA_MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SHA_EP0(x) (SHA_ROTR(x,2) ^ SHA_ROTR(x,13) ^ SHA_ROTR(x,22))
#define SHA_EP1(x) (SHA_ROTR(x,6) ^ SHA_ROTR(x,11) ^ SHA_ROTR(x,25))
#define SHA_SIG0(x) (SHA_ROTR(x,7) ^ SHA_ROTR(x,18) ^ ((x) >> 3))
#define SHA_SIG1(x) (SHA_ROTR(x,17) ^ SHA_ROTR(x,19) ^ ((x) >> 10))

static void sha256_transform(sha256_ctx *ctx, const uint8_t data[])
{
    uint32_t a,b,c,d,e,f,g,h,i,j,t1,t2,m[64];
    for (i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = ((uint32_t)data[j]<<24) | ((uint32_t)data[j+1]<<16) |
                ((uint32_t)data[j+2]<<8) | ((uint32_t)data[j+3]);
    for (; i < 64; ++i)
        m[i] = SHA_SIG1(m[i-2]) + m[i-7] + SHA_SIG0(m[i-15]) + m[i-16];

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 64; ++i) {
        t1 = h + SHA_EP1(e) + SHA_CH(e,f,g) + SHA256_K[i] + m[i];
        t2 = SHA_EP0(a) + SHA_MAJ(a,b,c);
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
    ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}

static void sha256_init(sha256_ctx *ctx)
{
    ctx->datalen = 0; ctx->bitlen = 0;
    ctx->state[0]=0x6a09e667; ctx->state[1]=0xbb67ae85;
    ctx->state[2]=0x3c6ef372; ctx->state[3]=0xa54ff53a;
    ctx->state[4]=0x510e527f; ctx->state[5]=0x9b05688c;
    ctx->state[6]=0x1f83d9ab; ctx->state[7]=0x5be0cd19;
}

static void sha256_update(sha256_ctx *ctx, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen] = data[i];
        ctx->datalen++;
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(sha256_ctx *ctx, uint8_t hash[32])
{
    uint32_t i = ctx->datalen;
    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) ctx->data[i++] = 0;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) ctx->data[i++] = 0;
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }
    ctx->bitlen += ctx->datalen * 8;
    ctx->data[63] = (uint8_t)(ctx->bitlen);
    ctx->data[62] = (uint8_t)(ctx->bitlen >> 8);
    ctx->data[61] = (uint8_t)(ctx->bitlen >> 16);
    ctx->data[60] = (uint8_t)(ctx->bitlen >> 24);
    ctx->data[59] = (uint8_t)(ctx->bitlen >> 32);
    ctx->data[58] = (uint8_t)(ctx->bitlen >> 40);
    ctx->data[57] = (uint8_t)(ctx->bitlen >> 48);
    ctx->data[56] = (uint8_t)(ctx->bitlen >> 56);
    sha256_transform(ctx, ctx->data);

    /* 按 FIPS 180-4 顺序 H0..H7 输出 */
    for (int j = 0; j < 8; j++) {
        hash[j*4 + 0] = (uint8_t)(ctx->state[j] >> 24);
        hash[j*4 + 1] = (uint8_t)(ctx->state[j] >> 16);
        hash[j*4 + 2] = (uint8_t)(ctx->state[j] >> 8);
        hash[j*4 + 3] = (uint8_t)(ctx->state[j]);
    }
}

int ipcam_sha256_hex_file(const char *path, char *out_hex65)
{
    if (!path || !out_hex65) return -1;
    FILE *fp = fopen(path, "rb");
    if (!fp) { MLOGE("sha256: open %s: %s\n", path, strerror(errno)); return -1; }

    sha256_ctx ctx;
    sha256_init(&ctx);
    uint8_t buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        sha256_update(&ctx, buf, n);
    }
    fclose(fp);

    uint8_t hash[32];
    sha256_final(&ctx, hash);

    static const char *hex = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex65[i*2 + 0] = hex[hash[i] >> 4];
        out_hex65[i*2 + 1] = hex[hash[i] & 0xf];
    }
    out_hex65[64] = '\0';
    return 0;
}

/* ELF magic 校验（避免下载到非 ELF 把系统 brick） */
static int is_elf_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    uint8_t m[4];
    ssize_t n = read(fd, m, 4);
    close(fd);
    return (n == 4 && m[0] == 0x7f && m[1] == 'E' && m[2] == 'L' && m[3] == 'F');
}

/* ===== 状态机 ===== */
static ipcam_ota_state_t   s_state     = IPCAM_OTA_STATE_IDLE;
static char                s_msg[128]  = "";
static _Atomic int          s_busy      = 0;
static pthread_mutex_t     s_mtx       = PTHREAD_MUTEX_INITIALIZER;
static char                s_current_version[32] = "";

void ipcam_ota_get_status(ipcam_ota_result_t *out)
{
    if (!out) return;
    pthread_mutex_lock(&s_mtx);
    memset(out, 0, sizeof(*out));
    out->state = s_state;
    strncpy(out->message, s_msg, sizeof(out->message) - 1);
    pthread_mutex_unlock(&s_mtx);
    strncpy(out->current_version, s_current_version, sizeof(out->current_version) - 1);
    /* pending_version 从 param 读（持久化的待激活版本） */
    /* 简化：留空 */
}

const char *ipcam_ota_get_current_version(void)
{
    return s_current_version[0] ? s_current_version : "unknown";
}

static void set_state(ipcam_ota_state_t st, const char *msg)
{
    pthread_mutex_lock(&s_mtx);
    s_state = st;
    if (msg) {
        strncpy(s_msg, msg, sizeof(s_msg) - 1);
        s_msg[sizeof(s_msg) - 1] = '\0';
    }
    pthread_mutex_unlock(&s_mtx);
    MLOGI("ota: state=%d msg='%s'\n", (int)st, msg ? msg : "");
}

int ipcam_ota_init(const char *bin_path)
{
    if (bin_path && *bin_path) {
        /* 把 path 保存到 static 备用 */
    }
    /* 从 /proc/self/exe 读 symlink 拿当前路径，验证 ELF */
    char self[256];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        self[n] = '\0';
        if (is_elf_file(self)) {
            MLOGI("ota: running ELF verified at %s\n", self);
        }
    }
    /* 当前版本：从 param 读（启动期 IPCAM_VERSION 是 compile-time 默认） */
    strncpy(s_current_version, ipcam_param_get_swver(), sizeof(s_current_version) - 1);
    s_current_version[sizeof(s_current_version) - 1] = '\0';
    return 0;
}

/*
 * 从本地文件升级：
 *   1) 校验 SHA256（若提供）
 *   2) 校验 ELF magic
 *   3) 写 .new（保证原子切换）
 *   4) rename .new → 正式（之前 rename 旧 → .prev）
 */
/*
 * 内部 staging helper：写 .new、做完整校验。
 * 不操作 s_busy —— 由 caller（from_file / from_url）负责原子锁。
 */
static int stage_from_path(const char *src_path, const char *expected_sha256_hex,
                          ipcam_ota_result_t *out)
{
    /* 1) SHA256（若提供 expected） */
    if (expected_sha256_hex && *expected_sha256_hex) {
        char actual[65];
        if (ipcam_sha256_hex_file(src_path, actual) < 0) {
            set_state(IPCAM_OTA_STATE_FAILED, "sha256 compute failed");
            if (out) { out->state = IPCAM_OTA_STATE_FAILED; snprintf(out->message, sizeof(out->message), "sha256"); }
            return -1;
        }
        if (strcasecmp(actual, expected_sha256_hex) != 0) {
            char msg[128];
            snprintf(msg, sizeof(msg), "sha256 mismatch (got %s)", actual);
            set_state(IPCAM_OTA_STATE_FAILED, msg);
            if (out) { out->state = IPCAM_OTA_STATE_FAILED; snprintf(out->message, sizeof(out->message), "%s", msg); }
            return -1;
        }
    }

    /* 2) ELF magic 校验 */
    if (!is_elf_file(src_path)) {
        set_state(IPCAM_OTA_STATE_FAILED, "not an ELF file");
        if (out) { out->state = IPCAM_OTA_STATE_FAILED; snprintf(out->message, sizeof(out->message), "not ELF"); }
        return -1;
    }

    /* 3) 拷贝到 .new（保持 src 模式） */
    struct stat st;
    if (stat(src_path, &st) < 0) {
        set_state(IPCAM_OTA_STATE_FAILED, "stat src failed");
        return -1;
    }

    int src_fd = open(src_path, O_RDONLY | O_NOFOLLOW);
    if (src_fd < 0) {
        char msg[128]; snprintf(msg, sizeof(msg), "open src: %s", strerror(errno));
        set_state(IPCAM_OTA_STATE_FAILED, msg);
        return -1;
    }
    int dst_fd = open(IPCAM_OTA_PATH_NEW, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (dst_fd < 0) {
        char msg[128]; snprintf(msg, sizeof(msg), "open .new: %s", strerror(errno));
        set_state(IPCAM_OTA_STATE_FAILED, msg);
        close(src_fd);
        return -1;
    }

    char buf[8192];
    ssize_t nr;
    int write_failed = 0;
    while ((nr = read(src_fd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < nr) {
            ssize_t w = write(dst_fd, buf + off, (size_t)(nr - off));
            if (w < 0) {
                if (errno == EINTR) continue;
                char msg[128]; snprintf(msg, sizeof(msg), "write .new: %s", strerror(errno));
                set_state(IPCAM_OTA_STATE_FAILED, msg);
                write_failed = 1;
                break;
            }
            off += w;
        }
        if (write_failed) break;
    }
    close(src_fd);
    fsync(dst_fd);
    close(dst_fd);

    if (write_failed) {
        unlink(IPCAM_OTA_PATH_NEW);
        return -1;
    }

    /* 4) 拷贝后再次校验：大小 + SHA256（防 copy 中途失败留下的截断文件） */
    struct stat st2;
    if (stat(IPCAM_OTA_PATH_NEW, &st2) < 0 || st2.st_size != st.st_size) {
        char msg[128]; snprintf(msg, sizeof(msg), ".new size mismatch (src=%ld dst=%ld)",
                               (long)st.st_size, st2.st_size);
        set_state(IPCAM_OTA_STATE_FAILED, msg);
        unlink(IPCAM_OTA_PATH_NEW);
        return -1;
    }
    if (expected_sha256_hex && *expected_sha256_hex) {
        char actual2[65];
        if (ipcam_sha256_hex_file(IPCAM_OTA_PATH_NEW, actual2) < 0 ||
            strcasecmp(actual2, expected_sha256_hex) != 0) {
            set_state(IPCAM_OTA_STATE_FAILED, ".new sha256 mismatch after copy");
            unlink(IPCAM_OTA_PATH_NEW);
            return -1;
        }
    }
    if (!is_elf_file(IPCAM_OTA_PATH_NEW)) {
        set_state(IPCAM_OTA_STATE_FAILED, ".new ELF check failed after copy");
        unlink(IPCAM_OTA_PATH_NEW);
        return -1;
    }

    set_state(IPCAM_OTA_STATE_STAGED, "ready to install; call ota_commit & reboot");
    if (out) { out->state = IPCAM_OTA_STATE_STAGED; out->progress = 100;
               snprintf(out->message, sizeof(out->message), "staged"); }
    return 0;
}

int ipcam_ota_from_file(const char *src_path,
                        const char *expected_sha256_hex,
                        ipcam_ota_result_t *out)
{
    if (!src_path) return -1;
    if (atomic_exchange(&s_busy, 1) != 0) {
        set_state(IPCAM_OTA_STATE_FAILED, "another OTA in progress");
        if (out) { out->state = IPCAM_OTA_STATE_FAILED; snprintf(out->message, sizeof(out->message), "busy"); }
        return -1;
    }

    set_state(IPCAM_OTA_STATE_VERIFYING, "checking file");
    int rc = stage_from_path(src_path, expected_sha256_hex, out);
    atomic_store(&s_busy, 0);
    return rc;
}

/*
 * 原子切换：rename 旧文件 → .prev，rename .new → 正式路径
 * init.d 脚本若检测到 .prev 存在，会在 watchdog 超时后回滚
 */
int ipcam_ota_commit(const char *bin_path)
{
    const char *target = bin_path ? bin_path : IPCAM_OTA_PATH_DEF;
    char prev[256], newp[256];
    snprintf(prev, sizeof(prev), "%s.prev", target);
    snprintf(newp, sizeof(newp), "%s.new", target);

    if (access(newp, F_OK) != 0) {
        set_state(IPCAM_OTA_STATE_FAILED, "no .new to commit");
        return -1;
    }

    /* 删旧 .prev（保留一份） */
    unlink(prev);

    /* 旧 → .prev */
    if (rename(target, prev) != 0 && errno != ENOENT) {
        char msg[128]; snprintf(msg, sizeof(msg), "rename old->prev: %s", strerror(errno));
        set_state(IPCAM_OTA_STATE_FAILED, msg);
        return -1;
    }

    /* .new → 正式 */
    if (rename(newp, target) != 0) {
        set_state(IPCAM_OTA_STATE_FAILED, "rename .new->target failed");
        /* 尝试回滚 */
        rename(prev, target);
        return -1;
    }

    set_state(IPCAM_OTA_STATE_INSTALLED, "installed; reboot to activate");
    return 0;
}

int ipcam_ota_rollback(const char *bin_path)
{
    const char *target = bin_path ? bin_path : IPCAM_OTA_PATH_DEF;
    char prev[256];
    snprintf(prev, sizeof(prev), "%s.prev", target);

    if (access(prev, F_OK) != 0) {
        set_state(IPCAM_OTA_STATE_FAILED, "no .prev to rollback");
        return -1;
    }
    /* 当前 → .broken */
    char broken[256];
    snprintf(broken, sizeof(broken), "%s.broken", target);
    unlink(broken);
    rename(target, broken);
    if (rename(prev, target) != 0) {
        char msg[128]; snprintf(msg, sizeof(msg), "rollback rename: %s", strerror(errno));
        set_state(IPCAM_OTA_STATE_FAILED, msg);
        return -1;
    }
    set_state(IPCAM_OTA_STATE_INSTALLED, "rolled back to .prev");
    return 0;
}

/*
 * 简易 HTTP GET（仅支持 HTTP/1.0，不支持 https / redirect / chunked）。
 * 写文件到 out_path，边下边写盘，避免大文件占内存。
 */
#define IPCAM_OTA_MAX_DOWNLOAD  (64 * 1024 * 1024)  /* 64 MiB 上限 */

static int http_get_to_file(const char *url, const char *out_path, int *progress_pct)
{
    /* 解析 url：http://host[:port]/path */
    if (strncmp(url, "http://", 7) != 0) {
        set_state(IPCAM_OTA_STATE_FAILED, "only http:// supported");
        return -1;
    }
    const char *p = url + 7;
    const char *slash = strchr(p, '/');
    if (!slash) { set_state(IPCAM_OTA_STATE_FAILED, "url missing path"); return -1; }
    char host[128];
    size_t hlen = (size_t)(slash - p);
    if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
    memcpy(host, p, hlen); host[hlen] = '\0';
    const char *path = slash;
    int port = 80;
    char *colon = memchr(host, ':', hlen);
    if (colon) { *colon = '\0'; port = atoi(colon + 1); }

    /* DNS resolve */
    struct hostent *he = gethostbyname(host);
    if (!he) { set_state(IPCAM_OTA_STATE_FAILED, "dns failed"); return -1; }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { set_state(IPCAM_OTA_STATE_FAILED, "socket"); return -1; }
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], 4);

    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        set_state(IPCAM_OTA_STATE_FAILED, "connect failed");
        close(fd); return -1;
    }

    char req[1024];
    int rn = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: ipcam-ota/1.0\r\nConnection: close\r\n\r\n",
        path, host);
    if (rn <= 0 || rn >= (int)sizeof(req)) { close(fd); return -1; }
    if (write(fd, req, (size_t)rn) < 0) { close(fd); return -1; }

    /* 读 response head（找 \r\n\r\n） */
    char head[2048];
    int got = 0;
    while (got < (int)sizeof(head) - 1) {
        ssize_t n = read(fd, head + got, sizeof(head) - 1 - got);
        if (n <= 0) break;
        got += n;
        head[got] = '\0';
        char *eoh = strstr(head, "\r\n\r\n");
        if (eoh) {
            int hdr_len = (int)(eoh - head) + 4;
            /* 检查 status code */
            if (strncmp(head, "HTTP/", 5) != 0) {
                set_state(IPCAM_OTA_STATE_FAILED, "bad http response");
                close(fd); return -1;
            }
            char *sp = strchr(head, ' ');
            if (!sp || atoi(sp + 1) != 200) {
                char msg[128];
                snprintf(msg, sizeof(msg), "http status %s", sp ? sp+1 : "?");
                set_state(IPCAM_OTA_STATE_FAILED, msg);
                close(fd); return -1;
            }

            /* 把已读到的 body 部分追加到输出文件 */
            int body_already = got - hdr_len;

            /* 检查 Content-Length 上限 */
            long long clen = -1;
            char *cl = strcasestr(head, "Content-Length:");
            if (cl && cl < eoh) {
                cl += 14;
                while (*cl == ' ') cl++;
                clen = atoll(cl);
                if (clen < 0 || clen > IPCAM_OTA_MAX_DOWNLOAD) {
                    set_state(IPCAM_OTA_STATE_FAILED, "Content-Length too big");
                    close(fd); return -1;
                }
            }

            int out = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (out < 0) { close(fd); return -1; }
            long long total_written = 0;
            if (body_already > 0) {
                ssize_t wn = write(out, head + hdr_len, (size_t)body_already);
                if (wn < 0) { close(out); close(fd); return -1; }
                total_written += wn;
            }
            /* 继续读剩余 body；累计上限 */
            char buf[8192];
            int size_known = (clen >= 0);
            while ((n = read(fd, buf, sizeof(buf))) > 0) {
                if (size_known) {
                    if (total_written + n > clen) {
                        set_state(IPCAM_OTA_STATE_FAILED, "body exceeds Content-Length");
                        close(out); close(fd); return -1;
                    }
                } else {
                    if (total_written + n > IPCAM_OTA_MAX_DOWNLOAD) {
                        set_state(IPCAM_OTA_STATE_FAILED, "body exceeds max");
                        close(out); close(fd); return -1;
                    }
                }
                ssize_t off = 0;
                while (off < n) {
                    ssize_t w = write(out, buf + off, (size_t)(n - off));
                    if (w < 0) { if (errno == EINTR) continue; break; }
                    off += w;
                }
                total_written += n;
            }
            close(out);
            close(fd);
            if (progress_pct) *progress_pct = 100;
            return 0;
        }
    }
    set_state(IPCAM_OTA_STATE_FAILED, "no http header end");
    close(fd);
    return -1;
}

int ipcam_ota_from_url(const char *url, const char *expected_sha256_hex,
                       ipcam_ota_result_t *out)
{
    if (!url) return -1;

    if (atomic_exchange(&s_busy, 1) != 0) {
        set_state(IPCAM_OTA_STATE_FAILED, "another OTA in progress");
        return -1;
    }

    set_state(IPCAM_OTA_STATE_DOWNLOADING, "[downloading]");  /* 不在 s_msg 中暴露原始 URL */

    /* 验证 URL 路径不含 CRLF/NUL/space（防 request smuggling） */
    const char *path = strchr(url + 7, '/');
    if (!path) {
        atomic_store(&s_busy, 0);
        set_state(IPCAM_OTA_STATE_FAILED, "url missing path");
        return -1;
    }
    for (const char *q = path; *q; q++) {
        if (*q == '\r' || *q == '\n' || *q == '\0' || *q == ' ') {
            atomic_store(&s_busy, 0);
            set_state(IPCAM_OTA_STATE_FAILED, "url path has forbidden chars");
            return -1;
        }
    }

    char tmp_path[] = "/tmp/ipcam-ota-XXXXXX";
    int tmpfd = mkstemp(tmp_path);
    if (tmpfd < 0) {
        atomic_store(&s_busy, 0);
        set_state(IPCAM_OTA_STATE_FAILED, "mkstemp failed");
        return -1;
    }
    close(tmpfd);

    int progress = 0;
    int rc = http_get_to_file(url, tmp_path, &progress);
    if (rc < 0) {
        unlink(tmp_path);
        atomic_store(&s_busy, 0);
        return -1;
    }

    if (out) out->progress = 50;

    /* 调用内部 stage_from_path（不操作 s_busy）；最后释放 */
    int rc2 = stage_from_path(tmp_path, expected_sha256_hex, out);
    unlink(tmp_path);
    atomic_store(&s_busy, 0);
    return rc2;
}