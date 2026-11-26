/*
 * smsapi.c —— 中兴 F32 短信转发（smsfwd）的 Web 控制 API
 *
 * 背景：设备上的 goahead 只允许白名单 CGI（upload.cgi 等），无法自建脚本，
 *      所以这里起一个极小的 HTTP 服务，供后台 web 的“短信转发”页面调用。
 *
 * 接口（全部 JSON，允许跨域，仅监听内网端口，默认 8080）：
 *   GET  /api/status   运行状态 + 配置 + 计数
 *   GET  /api/log?n=30 最近日志
 *   POST /api/save     webhook / secret / keyword / interval 保存并重启守护
 *   POST /api/action   do=start|stop|restart|once|test
 *   GET/POST /api/quota  配额总额读写；used_gb 取设备实时计数，POST 只存 total_gb
 *   GET  /api/netinfo  链路签约速率
 *   GET  /api/speedtest 设备端出站下行测速 + RTT
 *   POST /api/file?path=&token=&append=&exec=   原始 body 写入文件（手机端部署用）
 *   GET  /api/file?path=&token=                 读回文件内容（校验用）
 *   GET/POST /api/exec?token=&cmd=              执行 shell 命令并回显输出
 *   （以上三个运维接口必须带 token，路径限白名单目录，token 由 /api/save 的 api_key 设置）
 *
 * 编译（云主机）：
 *   python3 -m ziglang cc -target arm-linux-musleabi -O2 -static -o smsapi smsapi.c
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define PORT_DEF       8080
#define CONF_DEF       "/mnt/userdata/etc_rw/smsfwd.conf"
#define LOG_DEF        "/mnt/userdata/etc_rw/smsfwd.log"
#define PID_DEF        "/mnt/userdata/etc_rw/smsfwd.pid"
#define API_LOG        "/mnt/userdata/etc_rw/smsapi.log"
#define API_PID        "/mnt/userdata/etc_rw/smsapi.pid"
#define NET_CACHE      "/mnt/userdata/etc_rw/netinfo.cache"
#define NET_TTL        20
#define QUOTA_CONF     "/mnt/userdata/etc_rw/quota.conf"
#define QUOTA_STAT     "/mnt/userdata/etc_rw/quota.status"
#define QUOTA_BIN      "/mnt/userdata/etc_rw/quota"
#define BIN_USER       "/mnt/userdata/etc_rw/smsfwd"
#define BIN_SYS        "/sbin/smsfwd"
#define REQ_MAX        (32 * 1024)
#define BODY_MAX       (80 * 1024)

static const char *g_conf = CONF_DEF;
static const char *g_log  = LOG_DEF;
static const char *g_pid  = PID_DEF;
static char g_origin[128] = "";   /* 通过白名单校验的请求 Origin，用于收紧 CORS */

/* ------------------------------------------------------------------ 基础 */

static void apilog(const char *fmt, ...) {
    FILE *fp = fopen(API_LOG, "a");
    if (!fp) return;
    char ts[32];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    fprintf(fp, "%s ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

static char *read_all(const char *path, long *outlen) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    if (sz > 512 * 1024) sz = 512 * 1024;
    rewind(fp);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    long n = (long)fread(buf, 1, sz, fp);
    buf[n] = 0;
    fclose(fp);
    if (outlen) *outlen = n;
    return buf;
}

static int write_all(const char *path, const char *data, long len) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return -1;
    if (fwrite(data, 1, len, fp) != (size_t)len) { fclose(fp); unlink(tmp); return -1; }
    fflush(fp);
    fclose(fp);
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* JSON 字符串转义（输入/输出可为 UTF-8 中文，原样保留） */
static void json_esc(char *dst, int dsz, const char *src) {
    int d = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && d < dsz - 8; p++) {
        if (*p == '"' || *p == '\\') { dst[d++] = '\\'; dst[d++] = (char)*p; }
        else if (*p == '\n') { dst[d++] = '\\'; dst[d++] = 'n'; }
        else if (*p == '\r') { dst[d++] = '\\'; dst[d++] = 'r'; }
        else if (*p < 0x20) { d += snprintf(dst + d, dsz - d, "\\u%04x", *p); }
        else dst[d++] = (char)*p;
    }
    dst[d] = 0;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(const char *in, char *out, int osz) {
    int o = 0;
    for (const char *p = in; *p && o < osz - 1; p++) {
        if (*p == '+') { out[o++] = ' '; continue; }
        if (*p == '%' && p[1] && p[2]) {
            int h = hexval(p[1]), l = hexval(p[2]);
            if (h >= 0 && l >= 0) { out[o++] = (char)((h << 4) | l); p += 2; continue; }
        }
        out[o++] = *p;
    }
    out[o] = 0;
}

/* URL 编码（AT 命令里含 + 和 = 和 ?，必须转义） */
static void url_encode(const char *in, char *out, int osz) {
    int o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o < osz - 4; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[o++] = (char)*p;
        } else {
            o += snprintf(out + o, osz - o, "%%%02X", *p);
        }
    }
    out[o] = 0;
}

/* ---------------------------------------------------- 模组 AT 透传（127.0.0.1:9090） */

static int at_query(const char *at, char *out, int osz) {
    out[0] = 0;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(9090);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) { close(s); return -2; }

    char enc[256];
    url_encode(at, enc, sizeof(enc));
    char req[512];
    int n = snprintf(req, sizeof(req),
        "GET /goform/goform_set_cmd_process?goformId=EXECUTE_AT_COMMAND&at_cmd=%s HTTP/1.0\r\n"
        "Host: 127.0.0.1\r\nConnection: close\r\n\r\n", enc);
    if (write(s, req, n) < 0) { close(s); return -3; }

    char buf[2048];
    int got = 0;
    while (got < (int)sizeof(buf) - 1) {
        int r = (int)read(s, buf + got, sizeof(buf) - 1 - got);
        if (r <= 0) break;
        got += r;
        buf[got] = 0;
    }
    close(s);
    char *p = strstr(buf, "\r\n\r\n");
    if (!p) return -4;
    p += 4;
    while (*p == '\r' || *p == '\n') p++;
    snprintf(out, osz, "%s", p);
    int l = (int)strlen(out);
    while (l > 0 && (out[l - 1] == '\r' || out[l - 1] == '\n' || out[l - 1] == ' ')) out[--l] = 0;
    return out[0] ? 0 : -5;
}

/* 解析 AT+CGEQOSRDP 应答：1,6,"","","2147000,156000" → dl/ul（bps） */
static int parse_qos_rdp(const char *s, long *dl, long *ul, int *qci) {
    *dl = 0; *ul = 0; *qci = -1;
    const char *q = strrchr(s, '\"');
    if (!q) return -1;
    const char *p = q;
    while (p > s && p[-1] != '\"') p--;
    if (p == s) return -1;
    char pair[128];
    int n = (int)(q - p);
    if (n <= 0 || n > (int)sizeof(pair) - 1) return -1;
    memcpy(pair, p, n);
    pair[n] = 0;
    char *comma = strchr(pair, ',');
    if (!comma) return -1;
    *dl = strtol(pair, NULL, 10);
    *ul = strtol(comma + 1, NULL, 10);
    const char *c1 = strchr(s, ',');
    if (c1) *qci = (int)strtol(c1 + 1, NULL, 10);
    return (*dl > 0 || *ul > 0) ? 0 : -1;
}

/* 从 a=b&c=d 里取参数 */
static int get_param(const char *body, const char *key, char *out, int osz) {
    out[0] = 0;
    int klen = (int)strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        int seglen = amp ? (int)(amp - p) : (int)strlen(p);
        if (seglen > klen && !strncmp(p, key, klen) && p[klen] == '=') {
            char raw[2048];
            int vlen = seglen - klen - 1;
            if (vlen > (int)sizeof(raw) - 1) vlen = (int)sizeof(raw) - 1;
            memcpy(raw, p + klen + 1, vlen);
            raw[vlen] = 0;
            url_decode(raw, out, osz);
            return 1;
        }
        p = amp ? amp + 1 : NULL;
    }
    return 0;
}

/* ------------------------------------------------------------ 配置读写 */

static int conf_get(const char *key, char *out, int osz) {
    out[0] = 0;
    FILE *fp = fopen(g_conf, "r");
    if (!fp) return 0;
    char line[2048];
    int klen = (int)strlen(key);
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n' || *s == '\r' || *s == 0) continue;
        if (!strncmp(s, key, klen) && s[klen] == '=') {
            char *v = s + klen + 1;
            char *e = v + strlen(v);
            while (e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) e--;
            *e = 0;
            snprintf(out, osz, "%s", v);
            found = 1;
            break;
        }
    }
    fclose(fp);
    return found;
}

/* 更新若干 key=value；不存在的键追加到末尾；其它行原样保留 */
static int conf_update(char **keys, char **vals, int n) {
    char *data = read_all(g_conf, NULL);
    if (!data) data = strdup("# smsfwd 配置（由 Web 后台维护）\n");
    if (!data) return -1;

    char out[8192];
    int o = 0;
    int seen[8] = {0};
    const char *p = data;
    while (*p) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        char line[2048];
        if (len > (int)sizeof(line) - 1) len = (int)sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = 0;

        int replaced = 0;
        for (int i = 0; i < n; i++) {
            int klen = (int)strlen(keys[i]);
            const char *s = line;
            while (*s == ' ' || *s == '\t') s++;
            if (!strncmp(s, keys[i], klen) && s[klen] == '=') {
                int w = snprintf(out + o, sizeof(out) - o, "%s=%s\n", keys[i], vals[i]);
                if (w > 0) o += w;
                seen[i] = 1;
                replaced = 1;
                break;
            }
        }
        if (!replaced) {
            int w = snprintf(out + o, sizeof(out) - o, "%.*s\n", len, line);
            if (w > 0) o += w;
        }
        p = nl ? nl + 1 : p + strlen(p);
        if (o > (int)sizeof(out) - 4096) break;
    }
    for (int i = 0; i < n; i++) {
        if (!seen[i]) {
            int w = snprintf(out + o, sizeof(out) - o, "%s=%s\n", keys[i], vals[i]);
            if (w > 0) o += w;
        }
    }
    free(data);
    return write_all(g_conf, out, o);
}


/* ------------------------------------------------------------ 配额(套餐)接口 */

static int quota_conf_get(const char *key, char *out, int osz) {
    out[0] = 0;
    FILE *fp = fopen(QUOTA_CONF, "r");
    if (!fp) return 0;
    char line[256];
    int klen = (int)strlen(key);
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n' || *s == '\r' || *s == 0) continue;
        if (!strncmp(s, key, klen) && s[klen] == '=') {
            char *v = s + klen + 1;
            char *e = v + strlen(v);
            while (e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) e--;
            *e = 0;
            snprintf(out, osz, "%s", v);
            found = 1;
            break;
        }
    }
    fclose(fp);
    return found;
}

static int quota_num(const char *s, double *out) {
    if (!s || !*s) return 0;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s) return 0;
    while (*end == ' ' || *end == '\r' || *end == '\n') end++;
    if (*end != 0) return 0;
    if (v < 0 || v > 9999.0) return 0;
    *out = v;
    return 1;
}

static int quota_write(double total) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "total_gb=%.2f\n", total);
    return write_all(QUOTA_CONF, buf, n);
}

/* 从 quota.status 解析实时已用(GB), 形如 state=ok used=4.76GB total=... */
static void quota_stat_used(char *out, int osz) {
    out[0] = 0;
    FILE *f = fopen(QUOTA_STAT, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "used=");
        if (!p) continue;
        p += 5;
        char *e = p;
        while (*e && *e != 'G' && *e != ' ' && *e != '\n' && *e != '\r') e++;
        int n = (int)(e - p);
        if (n >= osz) n = osz - 1;
        strncpy(out, p, n); out[n] = 0;
        break;
    }
    fclose(f);
}

/* 写完立刻让守护进程按新配置复核一轮(非阻塞) */
static void quota_apply_once(void) {
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
        setsid();
        execl(QUOTA_BIN, QUOTA_BIN, "once", (char *)NULL);
        _exit(127);
    }
}

/* ------------------------------------------------------------ 进程控制 */

static const char *smsfwd_path(void) {
    if (access(BIN_USER, X_OK) == 0) return BIN_USER;
    if (access(BIN_SYS, X_OK) == 0) return BIN_SYS;
    return NULL;
}

static int daemon_pid(void) {
    FILE *fp = fopen(g_pid, "r");
    if (!fp) return 0;
    int pid = 0;
    if (fscanf(fp, "%d", &pid) != 1) pid = 0;
    fclose(fp);
    if (pid > 0 && kill(pid, 0) == 0) return pid;
    return 0;
}

static int self_pid(void) {
    FILE *fp = fopen(API_PID, "r");
    if (!fp) return 0;
    int pid = 0;
    if (fscanf(fp, "%d", &pid) != 1) pid = 0;
    fclose(fp);
    if (pid == getpid()) return pid;
    if (pid > 0 && kill(pid, 0) == 0) return pid;
    return 0;
}

static void self_write_pid(void) {
    FILE *fp = fopen(API_PID, "w");
    if (!fp) return;
    fprintf(fp, "%d\n", (int)getpid());
    fclose(fp);
}

static int run_smsfwd(const char *mode, int wait_it) {
    const char *bin = smsfwd_path();
    if (!bin) return -2;
    pid_t pid = fork();
    if (pid < 0) return -3;
    if (pid == 0) {
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, 0);
            dup2(fd, 1);
            dup2(fd, 2);
            if (fd > 2) close(fd);
        }
        setsid();
        if (mode) execl(bin, bin, g_conf, mode, (char *)NULL);
        else execl(bin, bin, g_conf, (char *)NULL);
        _exit(127);
    }
    if (!wait_it) return 0;
    int st = 0;
    for (int i = 0; i < 40; i++) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) break;
        if (r < 0) break;
        usleep(500 * 1000);
    }
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    return -1;
}

static int do_stop(void) {
    int pid = daemon_pid();
    if (pid <= 0) return 0;
    kill(pid, SIGTERM);
    for (int i = 0; i < 20; i++) {
        if (kill(pid, 0) != 0) break;
        usleep(200 * 1000);
    }
    if (kill(pid, 0) == 0) kill(pid, SIGKILL);
    unlink(g_pid);
    return 1;
}

/* ------------------------------------------------------------ 日志尾部 */

static char *tail_lines(const char *path, int want, int *count, long *bytes) {
    char *data = read_all(path, bytes);
    if (!data) { if (count) *count = 0; return NULL; }
    int total = 0;
    for (char *p = data; *p; p++) if (*p == '\n') total++;
    if (want > total) want = total;
    char *p = data + strlen(data);
    int found = 0;
    while (p > data && found < want) {
        p--;
        if (*p == '\n') found++;
    }
    if (p > data) p++;
    char *res = strdup(p);
    free(data);
    if (count) *count = want;
    return res;
}

/* 统计日志里已转发条数与最后一条时间 */
static void scan_log(int *forwarded, int *failed, char *last, int lsz) {
    *forwarded = 0;
    *failed = 0;
    last[0] = 0;
    long sz = 0;
    char *data = read_all(g_log, &sz);
    if (!data) return;
    char *p = data;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        if (len > 0) {
            char line[1024];
            int n = len > (int)sizeof(line) - 1 ? (int)sizeof(line) - 1 : len;
            memcpy(line, p, n);
            line[n] = 0;
            if (strstr(line, "已转发:")) {
                (*forwarded)++;
                snprintf(last, lsz, "%s", line);
            } else if (strstr(line, "未转发:")) {
                (*failed)++;
            }
        }
        p = nl ? nl + 1 : NULL;
    }
    free(data);
}

/* ------------------------------------------------------------ HTTP */

/* 解析 Origin 头并校验是否为本机/内网网关；通过则回填 g_origin，否则置空（不发 CORS 头） */
static void check_origin(const char *req) {
    g_origin[0] = 0;
    const char *p = strcasestr(req, "Origin:");
    if (!p) return;
    p += 7;
    while (*p == ' ' || *p == '\t') p++;
    char o[128]; int n = 0;
    while (*p && *p != '\r' && *p != '\n' && n < (int)sizeof(o) - 1) o[n++] = *p++;
    o[n] = 0;
    /* 取 host：去掉 scheme://，再去掉 :port */
    const char *host = strstr(o, "://");
    host = host ? host + 3 : o;
    char h[64]; int i = 0;
    while (host[i] && host[i] != ':' && host[i] != '/' && i < (int)sizeof(h) - 1) { h[i] = host[i]; i++; }
    h[i] = 0;
    if (!strcmp(h, "127.0.0.1") || !strcmp(h, "localhost") || !strcmp(h, "192.168.0.1")) {
        snprintf(g_origin, sizeof(g_origin), "%s", o);
    }
}

/* ---------------------------------------------------- 网速测速（设备端出站 HTTP 下行） */

static double speed_clock_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* 单次：DNS+建连+GET 拉取，返回收到字节；rtt_out=建连毫秒，elapsed_out=下载毫秒。失败返回负值 */
static int speedtest_once(const char *host, const char *path, double max_sec,
                          long cap_bytes, double *rtt_out, double *elapsed_out) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, "80", &hints, &res) != 0 || !res) return -1;

    int s = socket(res->ai_family, res->ai_socktype, 0);
    if (s < 0) { freeaddrinfo(res); return -1; }
    struct timeval tv;
    tv.tv_sec = (int)max_sec; tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    double t0 = speed_clock_ms();
    if (connect(s, res->ai_addr, res->ai_addrlen) != 0) { close(s); freeaddrinfo(res); return -2; }
    double t1 = speed_clock_ms();
    *rtt_out = t1 - t0;
    freeaddrinfo(res);

    char req[512];
    int rn = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: curl/8.0\r\nConnection: close\r\n\r\n",
        path, host);
    if (write(s, req, rn) < 0) { close(s); return -3; }

    long total = 0;
    char buf[4096];
    double deadline = t1 + max_sec * 1000.0;
    while (total < cap_bytes) {
        if (speed_clock_ms() > deadline) break;
        int r = (int)read(s, buf, sizeof(buf));
        if (r <= 0) break;
        total += r;
    }
    double t2 = speed_clock_ms();
    close(s);
    *elapsed_out = t2 - t1;
    return (int)total;
}

/* 多流并发下行测速：N 个子进程各自拉同一大文件，汇总总字节 / 墙钟时间。
 * 单流在 30~50ms RTT 下受 TCP 窗口限制，只能跑出 5~6 Mbps；
 * 并发多流才能把空口能力（实测 60~80 Mbps）压满。 */
#define SPEED_HOST     "mirrors.ustc.edu.cn"
#define SPEED_PATH     "/ubuntu-releases/24.04/ubuntu-24.04.4-desktop-amd64.iso"
#define SPEED_STREAMS  8

struct speed_war { long bytes; double rtt; };

static double speed_parallel_ms(double sec, long *total_out, double *rtt_out) {
    *total_out = 0;
    *rtt_out = -1.0;

    /* 父进程先做一次解析，预热 dnsmasq 缓存，避免 8 个子进程抢 DNS */
    {
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        getaddrinfo(SPEED_HOST, "80", &hints, &res);
        if (res) freeaddrinfo(res);
    }

    int pfd[2];
    if (pipe(pfd) != 0) return -1.0;

    double t0 = speed_clock_ms();
    pid_t kids[SPEED_STREAMS];
    for (int i = 0; i < SPEED_STREAMS; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            close(pfd[0]);
            alarm((int)sec + 8);            /* 兜底：连不上也别挂死 */
            struct speed_war w;
            w.bytes = 0;
            w.rtt = -1.0;
            double r = -1.0, e = 0.0;
            int n = speedtest_once(SPEED_HOST, SPEED_PATH, sec, 32L * 1024 * 1024, &r, &e);
            if (n > 0) w.bytes = n;
            w.rtt = r;
            write(pfd[1], &w, sizeof(w));
            close(pfd[1]);
            _exit(0);
        }
        kids[i] = pid;
    }
    close(pfd[1]);

    struct speed_war w;
    while (read(pfd[0], &w, sizeof(w)) == (ssize_t)sizeof(w)) {
        *total_out += w.bytes;
        if (w.rtt >= 0 && (*rtt_out < 0 || w.rtt < *rtt_out)) *rtt_out = w.rtt;
    }
    close(pfd[0]);
    for (int i = 0; i < SPEED_STREAMS; i++) waitpid(kids[i], NULL, 0);
    return speed_clock_ms() - t0;
}

static void send_resp(int fd, int code, const char *body) {
    char hdr[768];
    const char *st = code == 200 ? "OK"
                   : code == 204 ? "No Content"
                   : code == 400 ? "Bad Request"
                   : code == 403 ? "Forbidden"
                   : code == 500 ? "Internal Server Error"
                   : "Not Found";
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Cache-Control: no-store\r\n",
        code, st, (int)strlen(body));
    /* CORS 仅回显通过内网白名单的 Origin，不再用通配 * */
    if (g_origin[0]) {
        n += snprintf(hdr + n, sizeof(hdr) - n,
            "Access-Control-Allow-Origin: %s\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type, X-Requested-With\r\n",
            g_origin);
    }
    n += snprintf(hdr + n, sizeof(hdr) - n, "Connection: close\r\n\r\n");
    if (write(fd, hdr, n) < 0) return;
    write(fd, body, strlen(body));
}

/* ---------------------------------------------------- 远程运维（手机端部署用） */

/* 允许写入/读取的目录白名单，防止被当成任意文件读写器 */
static const char *ALLOW_PREFIX[] = {
    "/mnt/userdata/etc_rw/", "/tmp/", "/etc_ro/web/", "/sbin/",
    "/etc/", "/vendor/", "/data/", NULL
};

static int path_allowed(const char *p) {
    if (!p || p[0] != '/') return 0;
    if (strstr(p, "..")) return 0;
    if (strlen(p) > 200) return 0;
    for (int i = 0; ALLOW_PREFIX[i]; i++) {
        int k = (int)strlen(ALLOW_PREFIX[i]);
        if (!strncmp(p, ALLOW_PREFIX[i], k)) return 1;
    }
    return 0;
}

/* 1=通过 0=token错 -1=服务端未配置 token */
static int api_auth(const char *tk) {
    char want[256];
    if (!conf_get("api_key", want, sizeof(want)) || !want[0]) {
        if (!conf_get("secret", want, sizeof(want)) || !want[0]) return -1;
    }
    return (tk && *tk && !strcmp(tk, want)) ? 1 : 0;
}

static void handle_client(int fd) {
    char *req = malloc(REQ_MAX);
    if (!req) { close(fd); return; }
    memset(req, 0, REQ_MAX);
    int got = 0;
    int header_end = -1;
    while (got < REQ_MAX - 1) {
        int n = (int)read(fd, req + got, REQ_MAX - 1 - got);
        if (n <= 0) break;
        got += n;
        req[got] = 0;
        header_end = -1;
        for (int i = 0; i + 3 < got; i++) {
            if (!memcmp(req + i, "\r\n\r\n", 4)) { header_end = i + 4; break; }
        }
        if (header_end > 0) {
            char *cl = strcasestr(req, "Content-Length:");
            if (!cl) break;
            long need = strtol(cl + 15, NULL, 10) + header_end;
            if (need > REQ_MAX - 1) need = REQ_MAX - 1;
            if (got >= need) break;
        }
    }

    char method[16] = {0}, path[256] = {0}, query[512] = {0};
    if (sscanf(req, "%15s %255s", method, path) != 2) {
        send_resp(fd, 400, "{\"ok\":false,\"msg\":\"bad request\"}");
        free(req);
        close(fd);
        return;
    }
    char *q = strchr(path, '?');
    if (q) { snprintf(query, sizeof(query), "%s", q + 1); *q = 0; }

    long body_len = header_end > 0 ? (long)got - header_end : 0;
    if (body_len < 0) body_len = 0;
    char *body = header_end > 0 ? req + header_end : (char *)"";
    check_origin(req);
    char *payload = malloc(BODY_MAX);
    if (!payload) { free(req); close(fd); return; }
    snprintf(payload, BODY_MAX, "%s", body);

    char *out = malloc(BODY_MAX);
    if (!out) { free(payload); free(req); close(fd); return; }
    out[0] = 0;

    if (!strcmp(method, "OPTIONS")) {
        send_resp(fd, 204, "");
        goto done;
    }

    if (!strcmp(path, "/api/status") && !strcmp(method, "GET")) {
        char wh[1024], kw[256], sec[256], iv[64];
        char kw_esc[512], last[1200], last_esc[2048];
        conf_get("webhook", wh, sizeof(wh));
        conf_get("secret", sec, sizeof(sec));
        conf_get("keyword", kw, sizeof(kw));
        conf_get("interval", iv, sizeof(iv));
        int fwd = 0, fail = 0;
        scan_log(&fwd, &fail, last, sizeof(last));
        json_esc(kw_esc, sizeof(kw_esc), kw);
        json_esc(last_esc, sizeof(last_esc), last);
        int pid = daemon_pid();
        snprintf(out, BODY_MAX,
            "{\"ok\":true,\"running\":%d,\"pid\":%d,\"interval\":%d,"
            "\"webhook_set\":%d,\"keyword\":\"%s\",\"secret_set\":%d,"
            "\"forwarded\":%d,\"failed\":%d,\"last\":\"%s\","
            "\"conf\":\"%s\",\"bin\":\"%s\",\"api\":\"smsapi 1.0\"}",
            pid > 0 ? 1 : 0, pid, atoi(iv[0] ? iv : "30"),
            wh[0] ? 1 : 0, kw_esc, sec[0] ? 1 : 0, fwd, fail, last_esc,
            g_conf, smsfwd_path() ? smsfwd_path() : "未安装");
        send_resp(fd, 200, out);
        goto done;
    }

    if (!strcmp(path, "/api/netinfo") && !strcmp(method, "GET")) {
        long dl = 0, ul = 0, ts = 0;
        int qci = -1;
        FILE *cf = fopen(NET_CACHE, "r");
        if (cf) {
            if (fscanf(cf, "%ld|%ld|%ld|%d", &ts, &dl, &ul, &qci) != 4) { ts = 0; dl = 0; ul = 0; qci = -1; }
            fclose(cf);
        }
        long now = (long)time(NULL);
        int stale = 0;
        if (!(ts > 0 && now - ts >= 0 && now - ts <= NET_TTL && dl > 0)) {
            char resp[512];
            long ndl = 0, nul = 0;
            int nqci = -1;
            if (at_query("AT+CGEQOSRDP=1", resp, sizeof(resp)) == 0 && parse_qos_rdp(resp, &ndl, &nul, &nqci) == 0) {
                dl = ndl; ul = nul; qci = nqci; ts = now;
                FILE *f = fopen(NET_CACHE, "w");
                if (f) { fprintf(f, "%ld|%ld|%ld|%d\n", ts, dl, ul, qci); fclose(f); }
                apilog("netinfo: 签约速率 %ld/%ld bps qci=%d", dl, ul, qci);
            } else {
                stale = dl > 0;
            }
        }
        if (dl <= 0) {
            send_resp(fd, 200, "{\"ok\":false,\"msg\":\"未取得签约速率（网络未连接？）\"}");
            goto done;
        }
        snprintf(out, BODY_MAX,
            "{\"ok\":true,\"dl\":%ld,\"ul\":%ld,\"qci\":%d,\"age\":%ld,\"stale\":%d}",
            dl, ul, qci, ts > 0 ? now - ts : 0, stale);
        send_resp(fd, 200, out);
        goto done;
    }

    if (!strcmp(path, "/api/speedtest") && !strcmp(method, "GET")) {
        long total = 0;
        double rtt = -1.0;
        double wall = speed_parallel_ms(6.0, &total, &rtt);
        if (wall > 0 && total > 0) {
            double mbps = ((double)total * 8.0) / (wall / 1000.0) / 1e6;
            snprintf(out, BODY_MAX,
                "{\"ok\":true,\"dl_mbps\":%.2f,\"rtt_ms\":%.0f,\"bytes\":%ld,"
                "\"elapsed_ms\":%.0f,\"streams\":%d}",
                mbps, rtt >= 0 ? rtt : 0.0, total, wall, SPEED_STREAMS);
        } else {
            snprintf(out, BODY_MAX,
                "{\"ok\":false,\"msg\":\"测速失败:无法连到测速源\",\"rtt_ms\":%.0f}",
                rtt >= 0 ? rtt : 0.0);
        }
        send_resp(fd, 200, out);
        goto done;
    }

    if (!strcmp(path, "/api/log") && !strcmp(method, "GET")) {
        int n = 30;
        char ns[32];
        if (get_param(query, "n", ns, sizeof(ns))) n = atoi(ns);
        if (n < 1) n = 1;
        if (n > 200) n = 200;
        int cnt = 0;
        long bytes = 0;
        char *t = tail_lines(g_log, n, &cnt, &bytes);
        int o = snprintf(out, BODY_MAX, "{\"ok\":true,\"n\":%d,\"lines\":[", cnt);
        if (t) {
            char *p = t;
            int first = 1;
            while (p && *p) {
                char *nl = strchr(p, '\n');
                int len = nl ? (int)(nl - p) : (int)strlen(p);
                if (len > 0) {
                    char line[1200];
                    int m = len > (int)sizeof(line) - 1 ? (int)sizeof(line) - 1 : len;
                    memcpy(line, p, m);
                    line[m] = 0;
                    char esc[2400];
                    json_esc(esc, sizeof(esc), line);
                    o += snprintf(out + o, BODY_MAX - o, "%s\"%s\"", first ? "" : ",", esc);
                    first = 0;
                }
                p = nl ? nl + 1 : NULL;
            }
            free(t);
        }
        o += snprintf(out + o, BODY_MAX - o, "]}");
        send_resp(fd, 200, out);
        goto done;
    }

    if (!strcmp(path, "/api/save") && !strcmp(method, "POST")) {
        char wh[1024], sec[256], kw[256], iv[32], ak[256];
        int has = 0;
        static char *keys[8];
        static char *vals[8];
        int n = 0;
        if (get_param(payload, "webhook", wh, sizeof(wh))) {
            if (strchr(wh, '\n') || strchr(wh, '\r')) {
                send_resp(fd, 400, "{\"ok\":false,\"msg\":\"webhook 含非法字符\"}");
                goto done;
            }
            keys[n] = "webhook"; vals[n] = wh; n++;
        }
        if (get_param(payload, "secret", sec, sizeof(sec))) {
            keys[n] = "secret"; vals[n] = sec; n++;
        }
        if (get_param(payload, "keyword", kw, sizeof(kw))) {
            keys[n] = "keyword"; vals[n] = kw; n++;
        }
        if (get_param(payload, "interval", iv, sizeof(iv))) {
            int v = atoi(iv);
            if (v < 5) v = 5;
            if (v > 600) v = 600;
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", v);
            keys[n] = "interval"; vals[n] = strdup(buf); n++;
        }
        if (get_param(payload, "api_key", ak, sizeof(ak))) {
            const char *s = ak;
            int bad = 0;
            for (; *s; s++) if (*s == '\n' || *s == '\r' || *s == ' ' || *s == '#') bad = 1;
            if (bad || strlen(ak) < 8) {
                send_resp(fd, 400, "{\"ok\":false,\"msg\":\"api_key 非法(需>=8位且无空格换行)\"}");
                goto done;
            }
            keys[n] = "api_key"; vals[n] = ak; n++;
        }
        if (n == 0) {
            send_resp(fd, 400, "{\"ok\":false,\"msg\":\"没有可保存的字段\"}");
            goto done;
        }
        if (conf_update(keys, vals, n) != 0) {
            send_resp(fd, 400, "{\"ok\":false,\"msg\":\"写入配置失败\"}");
            goto done;
        }
        int was = daemon_pid() > 0;
        do_stop();
        int rc = 0;
        if (was) rc = run_smsfwd("start", 0);
        apilog("save: %d 项，重启=%d rc=%d", n, was, rc);
        snprintf(out, BODY_MAX, "{\"ok\":true,\"restarted\":%d,\"msg\":\"已保存\"}", was);
        send_resp(fd, 200, out);
        goto done;
    }

    if (!strcmp(path, "/api/action") && !strcmp(method, "POST")) {
        char act[32];
        if (!get_param(payload, "do", act, sizeof(act))) {
            send_resp(fd, 400, "{\"ok\":false,\"msg\":\"缺少 do\"}");
            goto done;
        }
        int rc = 0;
        char msg[128] = "";
        if (!strcmp(act, "start")) {
            rc = run_smsfwd("start", 0);
            snprintf(msg, sizeof(msg), "启动指令已下发");
        } else if (!strcmp(act, "stop")) {
            rc = do_stop();
            snprintf(msg, sizeof(msg), "已停止");
        } else if (!strcmp(act, "restart")) {
            do_stop();
            rc = run_smsfwd("start", 0);
            snprintf(msg, sizeof(msg), "已重启");
        } else if (!strcmp(act, "once")) {
            rc = run_smsfwd("", 1);
            snprintf(msg, sizeof(msg), "本轮检查完成 rc=%d", rc);
        } else if (!strcmp(act, "test")) {
            rc = run_smsfwd("test", 1);
            snprintf(msg, sizeof(msg), rc == 0 ? "测试消息已发送" : "测试发送失败 rc=%d", rc);
        } else {
            send_resp(fd, 400, "{\"ok\":false,\"msg\":\"未知动作\"}");
            goto done;
        }
        if (!strcmp(act, "test") && rc != 0) {
            snprintf(out, BODY_MAX, "{\"ok\":false,\"msg\":\"%s\"}", msg);
        } else if (!strcmp(act, "once") && rc != 0) {
            snprintf(out, BODY_MAX, "{\"ok\":false,\"msg\":\"%s\"}", msg);
        } else {
            snprintf(out, BODY_MAX, "{\"ok\":true,\"msg\":\"%s\",\"running\":%d}", msg, daemon_pid() > 0);
        }
        apilog("action %s rc=%d", act, rc);
        send_resp(fd, 200, out);
        goto done;
    }


    if (!strcmp(path, "/api/quota") && !strcmp(method, "GET")) {
        char t[32], u[32];
        quota_conf_get("total_gb", t, sizeof(t));
        quota_stat_used(u, sizeof(u));
        if (!u[0]) quota_conf_get("used_gb", u, sizeof(u));
        snprintf(out, BODY_MAX, "{\"ok\":true,\"total_gb\":\"%s\",\"used_gb\":\"%s\"}", t, u);
        send_resp(fd, 200, out);
        goto done;
    }

    if (!strcmp(path, "/api/quota") && !strcmp(method, "POST")) {
        char ts[32], us[32];
        double total = 0;
        if (!get_param(payload, "total_gb", ts, sizeof(ts)) || !quota_num(ts, &total) || total < 0) {
            send_resp(fd, 400, "{\"ok\":false,\"msg\":\"总额无效(0=不限量,0~9999)\"}");
            goto done;
        }
        if (quota_write(total) != 0) {
            send_resp(fd, 400, "{\"ok\":false,\"msg\":\"写入配置失败\"}");
            goto done;
        }
        quota_apply_once();
        quota_stat_used(us, sizeof(us));
        apilog("quota save: total=%.2f used=%s", total, us);
        snprintf(out, BODY_MAX, "{\"ok\":true,\"total_gb\":%.2f,\"used_gb\":\"%s\"}", total, us);
        send_resp(fd, 200, out);
        goto done;
    }

    /* ---------- 运维：写文件（原始 body 直接落盘，手机端部署用） ---------- */
    if (!strcmp(path, "/api/file") && !strcmp(method, "POST")) {
        char fp[256] = "", tk[128] = "", ap[16] = "", ex[16] = "";
        get_param(query, "path", fp, sizeof(fp));
        get_param(query, "token", tk, sizeof(tk));
        get_param(query, "append", ap, sizeof(ap));
        get_param(query, "exec", ex, sizeof(ex));
        int au = api_auth(tk);
        if (au < 0) { send_resp(fd, 403, "{\"ok\":false,\"msg\":\"服务端未配置 token，请先设置 api_key\"}"); goto done; }
        if (!au)    { send_resp(fd, 403, "{\"ok\":false,\"msg\":\"token 无效\"}"); goto done; }
        if (!path_allowed(fp)) { send_resp(fd, 400, "{\"ok\":false,\"msg\":\"路径不在白名单或非法\"}"); goto done; }
        FILE *wf = fopen(fp, atoi(ap) ? "ab" : "wb");
        if (!wf) { send_resp(fd, 500, "{\"ok\":false,\"msg\":\"打开失败(目录不存在或只读)\"}"); goto done; }
        long nb = 0;
        if (body_len > 0) nb = (long)fwrite(body, 1, (size_t)body_len, wf);
        fflush(wf);
        fsync(fileno(wf));
        fclose(wf);
        if (atoi(ex)) chmod(fp, 0755);
        apilog("file write: %s %ld bytes append=%d exec=%d", fp, nb, atoi(ap), atoi(ex));
        snprintf(out, BODY_MAX,
                 "{\"ok\":true,\"path\":\"%s\",\"bytes\":%ld,\"append\":%d,\"exec\":%d}",
                 fp, nb, atoi(ap), atoi(ex));
        send_resp(fd, 200, out);
        goto done;
    }

    /* ---------- 运维：读文件（校验写入结果用） ---------- */
    if (!strcmp(path, "/api/file") && !strcmp(method, "GET")) {
        char fp[256] = "", tk[128] = "";
        get_param(query, "path", fp, sizeof(fp));
        get_param(query, "token", tk, sizeof(tk));
        int au = api_auth(tk);
        if (au < 0) { send_resp(fd, 403, "{\"ok\":false,\"msg\":\"服务端未配置 token\"}"); goto done; }
        if (!au)    { send_resp(fd, 403, "{\"ok\":false,\"msg\":\"token 无效\"}"); goto done; }
        if (!path_allowed(fp)) { send_resp(fd, 400, "{\"ok\":false,\"msg\":\"路径不在白名单或非法\"}"); goto done; }
        FILE *rf = fopen(fp, "rb");
        if (!rf) { send_resp(fd, 404, "{\"ok\":false,\"msg\":\"文件不存在\"}"); goto done; }
        char *rbuf = malloc(48 * 1024);
        char *esc = malloc(BODY_MAX);
        int rn = 0, code = 200;
        if (!rbuf || !esc) {
            code = 500;
        } else {
            rn = (int)fread(rbuf, 1, 48 * 1024 - 1, rf);
            if (rn < 0) rn = 0;
            rbuf[rn] = 0;
            json_esc(esc, BODY_MAX, rbuf);
            snprintf(out, BODY_MAX, "{\"ok\":true,\"path\":\"%s\",\"size\":%d,\"content\":\"%s\"}",
                     fp, rn, esc);
        }
        free(rbuf);
        free(esc);
        fclose(rf);
        if (code == 500) send_resp(fd, 500, "{\"ok\":false,\"msg\":\"内存不足\"}");
        else send_resp(fd, 200, out);
        goto done;
    }

    /* ---------- 运维：执行命令（GET 也能用，手机浏览器直接开） ---------- */
    if (!strcmp(path, "/api/exec")) {
        char cmd[1400] = "", tk[128] = "";
        char q_cmd[1400] = "", p_cmd[1400] = "", q_tk[128] = "", p_tk[128] = "";
        get_param(query, "cmd", q_cmd, sizeof(q_cmd));
        get_param(payload, "cmd", p_cmd, sizeof(p_cmd));
        get_param(query, "token", q_tk, sizeof(q_tk));
        get_param(payload, "token", p_tk, sizeof(p_tk));
        snprintf(cmd, sizeof(cmd), "%s", p_cmd[0] ? p_cmd : q_cmd);
        snprintf(tk, sizeof(tk), "%s", p_tk[0] ? p_tk : q_tk);
        int au = api_auth(tk);
        if (au < 0) { send_resp(fd, 403, "{\"ok\":false,\"msg\":\"服务端未配置 token\"}"); goto done; }
        if (!au)    { send_resp(fd, 403, "{\"ok\":false,\"msg\":\"token 无效\"}"); goto done; }
        if (!cmd[0]) { send_resp(fd, 400, "{\"ok\":false,\"msg\":\"缺少 cmd\"}"); goto done; }
        FILE *pp = popen(cmd, "r");
        if (!pp) { send_resp(fd, 500, "{\"ok\":false,\"msg\":\"popen 失败\"}"); goto done; }
        /* 主进程为防僵尸把 SIGCHLD 设成了 SIG_IGN，会让 pclose 拿不到退出码，这里临时恢复 */
        signal(SIGCHLD, SIG_DFL);
        char *ob = malloc(8000);
        char *es1 = malloc(BODY_MAX);
        char *es2 = malloc(4096);
        int on = 0, ex = -1, code = 200;
        if (!ob || !es1 || !es2) {
            pclose(pp);
            code = 500;
        } else {
            while (on < 7999) {
                int r = (int)fread(ob + on, 1, 7999 - on, pp);
                if (r <= 0) break;
                on += r;
            }
            ob[on] = 0;
            int rc = pclose(pp);
            ex = (rc >= 0 && WIFEXITED(rc)) ? WEXITSTATUS(rc) : -1;
            signal(SIGCHLD, SIG_IGN);
            json_esc(es1, BODY_MAX, ob);
            json_esc(es2, 4096, cmd);
            snprintf(out, BODY_MAX,
                     "{\"ok\":%d,\"rc\":%d,\"cmd\":\"%s\",\"out\":\"%s\"}",
                     ex == 0 ? 1 : 0, ex, es2, es1);
            apilog("exec rc=%d: %s", ex, cmd);
        }
        free(ob); free(es1); free(es2);
        if (code == 500) send_resp(fd, 500, "{\"ok\":false,\"msg\":\"内存不足\"}");
        else send_resp(fd, 200, out);
        goto done;
    }

    send_resp(fd, 404, "{\"ok\":false,\"msg\":\"未知接口\"}");

done:
    free(out);
    free(payload);
    free(req);
    close(fd);
}

/* ------------------------------------------------------------ main */

int main(int argc, char **argv) {
    int port = PORT_DEF;
    int fg = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) fg = 1;
        else if (atoi(argv[i]) > 0) port = atoi(argv[i]);
    }

    if (!fg) {
        int old = self_pid();
        if (old > 0 && old != getpid()) return 0; /* 已在运行 */
        if (daemon(0, 0) != 0) return 1;
        self_write_pid();
    }

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return 1; }
    int on = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((unsigned short)port);
    if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        perror("bind");
        return 1;
    }
    if (listen(lfd, 16) != 0) { perror("listen"); return 1; }

    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    apilog("smsapi 启动，端口 %d", port);

    for (;;) {
        struct sockaddr_in ca;
        unsigned int cl = sizeof(ca);
        int cfd = accept(lfd, (struct sockaddr *)&ca, &cl);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            usleep(100 * 1000);
            continue;
        }
        pid_t pid = fork();
        if (pid == 0) {
            close(lfd);
            handle_client(cfd);
            _exit(0);
        }
        close(cfd);
    }
    return 0;
}
