/* burnctl.c —— F32 空闲烧流量守护（C 版，替代 bash 版）
 *
 * 设备 busybox 裁掉了 curl / iw / hostapd_cli，hostapd 控制口也不响应，
 * 因此本程序：
 *   1. 事件源：NETLINK_ROUTE 的 RTMGRP_NEIGH 组播，内核在局域网邻居
 *      （终端）新增/删除时主动推送，不轮询；过滤 192.168.0.0/24、
 *      排除网关 192.168.0.1、忽略 INCOMPLETE/FAILED 条目；
 *   2. 收到终端接入（NEWNEIGH）立刻杀掉烧流子进程；邻居清空（DELNEIGH）
 *      且「网页手动 ICCID == 设备当前 ICCID」时，才拉起烧流子进程；
 *   3. 烧流子进程裸 TCP 直连公网大文件循环拉取，不依赖 curl/wget；
 *   4. 授权卡：不再使用内置白名单。网页把手动 ICCID 写到 /tmp/burn_manual_iccid，
 *      本程序实时读取当前 ICCID 并逐字比对；为空/不一致/读不到都不烧，
 *      每轮下载前复核，发现换卡或授权被清空立即自行退出。
 *
 * 用法：
 *   burnctl check   自检：当前局域网终端、手动/当前 ICCID、是否允许
 *   burnctl probe   只打印当前 ICCID（网页接口用）
 *   burnctl reload  重新读取 /tmp/burn_manual_iccid 并判定
 *   burnctl start   后台守护（默认）
 *   burnctl stop    停止
 *   burnctl status  状态
 *   burnctl run     前台运行（调试）
 *
 * 编译：
 *   zig cc -target arm-linux-musleabi -mcpu=arm1136j_s -mfloat-abi=soft -O2 -static -s -o burnctl burnctl.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if_arp.h>

#define SMS_HOST    "127.0.0.1"
#define SMS_PORT    8080
#define WEB_PORT    80
#define AT_PORT     9090    /* at_server：可执行 AT 命令，goform 的 ziccid/msisdn 在本固件恒为空 */
#define MANUALF     "/tmp/burn_manual_iccid" /* 网页手动填写 ICCID，重启后清空，需重新授权 */
#define BURN_SEC    20
#define BURN_SLEEP  2
#define BASE        "/mnt/userdata/etc_rw"
#define LOGF        BASE "/burn.log"
#define PIDF        BASE "/burn.pid"
#define WPIDF       BASE "/burn.worker.pid"
#define STATEF      BASE "/burn.state"
#define TOTALF      BASE "/burn_total"      /* 累计烧流字节（持久化） */
#define INFOF       BASE "/burn_info.txt"   /* 首页可读：累计|状态|最近一轮|授权ICCID */
#define MAX_STA     16

/* 累计字节持久化与首页信息文件 */
static unsigned long long load_total(void) {
    FILE *f = fopen(TOTALF, "r");
    unsigned long long v = 0;
    if (f) { fscanf(f, "%llu", &v); fclose(f); }
    return v;
}
static void save_total(unsigned long long v) {
    FILE *f = fopen(TOTALF, "w");
    if (f) { fprintf(f, "%llu", v); fclose(f); }
}
static void write_info(unsigned long long total, int state, long last, const char *auth) {
    FILE *f = fopen(INFOF, "w");
    if (f) { fprintf(f, "%llu|%d|%ld|%s\n", total, state, last, auth ? auth : ""); fclose(f); }
}

/* 192.168.0.0/24，网关 192.168.0.1 */
#define LAN_NET     0xC0A80000u
#define LAN_MASK    0x000000FFu
#define GATEWAY_IP  0xC0A80001u

static unsigned long neigh_ips[MAX_STA]; /* 网络序 IPv4 */
static int  neigh_n = 0;
static volatile sig_atomic_t g_stop = 0;

static void logline(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    FILE *f = fopen(LOGF, "a");
    if (f) {
        time_t t = time(NULL); struct tm *tm = localtime(&t);
        char ts[24]; strftime(ts, sizeof(ts), "%m-%d %H:%M:%S", tm);
        fprintf(f, "%s ", ts); vfprintf(f, fmt, ap); fputc('\n', f); fclose(f);
    }
    va_end(ap);
}

/* ---------- 裸 HTTP GET（不依赖 curl/wget） ---------- */
static int http_get(const char *host, int port, const char *path, char *out, int outsz) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) { close(s); return -1; }
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) { close(s); return -1; }
    char req[300];
    int rl = snprintf(req, sizeof req,
        "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    if (write(s, req, rl) != rl) { close(s); return -1; }
    int got = 0;
    while (got < outsz - 1) {
        int n = read(s, out + got, outsz - 1 - got);
        if (n <= 0) break;
        got += n;
    }
    out[got] = 0;
    close(s);
    return got;
}

/* ---------- 局域网邻居集合（192.168.0.0/24，排除网关） ---------- */
static int in_lan_net(unsigned long nip) { /* nip 网络序 */
    unsigned long h = ntohl(nip);
    return ((h >> 8) == (LAN_NET >> 8)) && h != GATEWAY_IP;
}

static void neigh_add(unsigned long ip) {
    for (int i = 0; i < neigh_n; i++) if (neigh_ips[i] == ip) return;
    if (neigh_n < MAX_STA) neigh_ips[neigh_n++] = ip;
}
static void neigh_del(unsigned long ip) {
    for (int i = 0; i < neigh_n; i++) {
        if (neigh_ips[i] == ip) {
            for (int j = i; j < neigh_n - 1; j++) neigh_ips[j] = neigh_ips[j+1];
            neigh_n--;
            return;
        }
    }
}

/* 启动时一次性读 /proc/net/arp 初始化在线表（非轮询） */
static void neigh_init_from_arp(void) {
    FILE *f = fopen("/proc/net/arp", "r");
    if (!f) return;
    char line[256];
    fgets(line, sizeof line, f); /* 表头 */
    while (fgets(line, sizeof line, f)) {
        char ipstr[20], mac[18]; int flags;
        /* 列：IP  HWtype  Flags  HWaddress  Mask  Device */
        if (sscanf(line, "%19s %*i %x %17s", ipstr, &flags, mac) != 3) continue;
        if (flags != 0x2) continue; /* 只认已解析（complete）条目 */
        unsigned long ip = inet_addr(ipstr); /* 网络序 */
        if (in_lan_net(ip)) neigh_add(ip);
    }
    fclose(f);
}

/* ---------- 授权卡判定：网页手动填写 ICCID，与设备实时读取值精确比对 ----------
 * 1) 网页写入 /tmp/burn_manual_iccid；空文件/文件不存在 → 一律不烧；
 * 2) 设备能读到 ICCID，且与手动填写的完全一致 → 放行；
 * 3) ICCID 读不到，或任何一位不同 → 拒绝，并停止正在运行的 worker；
 * 4) 不再使用内置 TARGET_ICCID / 号码 / IMSI 兜底，避免旧白名单绕过手动授权。
 * 授权文件放在 /tmp，设备重启后自动清空，必须重新到页面填写。
 */

/* 从 goform 返回里提取当前 IMSI（纯数字），失败返回 0 */
static int live_imsi(char *out, int outsz) {
    char buf[4096];
    if (http_get(SMS_HOST, WEB_PORT,
                 "/goform/goform_get_cmd_process?cmd=sim_imsi",
                 buf, sizeof buf) < 0) return 0;
    const char *p = strstr(buf, "\"sim_imsi\":\"");
    if (!p) return 0;
    p += strlen("\"sim_imsi\":\"");
    const char *q = strchr(p, '"');
    if (!q || q - p < 5) return 0;
    int n = (int)(q - p);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, p, n); out[n] = 0;
    for (int i = 0; out[i]; i++)
        if (!isdigit((unsigned char)out[i])) return 0;
    return 1;
}

/* 只保留数字 */
static void digits_only(const char *in, char *out, int outsz) {
    int n = 0;
    for (; *in && n < outsz - 1; in++)
        if (isdigit((unsigned char)*in)) out[n++] = *in;
    out[n] = 0;
}

/* 规整成 11 位手机号（去掉 +86/86）；不像手机号返回 0 */
static int norm_mobile(const char *raw, char *out, int outsz) {
    char d[32];
    digits_only(raw, d, sizeof d);
    if (strlen(d) == 13 && d[0] == '8' && d[1] == '6') {
        snprintf(out, outsz, "%s", d + 2);
        return 1;
    }
    if (strlen(d) == 11 && d[0] == '1') {
        snprintf(out, outsz, "%s", d);
        return 1;
    }
    return 0;
}

/* ICCID 可能是 BCD 十六进制串（含 A-F），只保留 0-9A-F 并转大写 */
static void hex_only(const char *in, char *out, int outsz) {
    int n = 0;
    for (; *in && n < outsz - 1; in++)
        if (isxdigit((unsigned char)*in)) out[n++] = (char)toupper((unsigned char)*in);
    out[n] = 0;
}

/* 取 buf 里第一段长度在 [min,max] 的连续十六进制串（ICCID 用） */
static int first_hex_run(const char *buf, int minlen, int maxlen, char *out, int outsz) {
    const char *p = buf;
    while (*p) {
        if (isxdigit((unsigned char)*p)) {
            const char *s = p;
            while (isxdigit((unsigned char)*p)) p++;
            int n = (int)(p - s);
            if (n >= minlen && n <= maxlen) {
                if (n >= outsz) n = outsz - 1;
                for (int i = 0; i < n; i++) out[i] = (char)toupper((unsigned char)s[i]);
                out[n] = 0;
                return 1;
            }
        } else p++;
    }
    return 0;
}

/* at_server 一次只处理一条 AT，连续请求之间留间隔 */
static void at_gap(void) { usleep(250000); }

/* ICCID：优先 at_server 执行 AT+ZICCID?（返回可能是 BCD 十六进制串） */
static int live_iccid(char *out, int outsz) {
    char buf[4096];
    for (int tries = 0; tries < 2; tries++) {
        at_gap();
        if (http_get(SMS_HOST, AT_PORT,
                     "/goform/goform_set_cmd_process?goformId=EXECUTE_AT_COMMAND&isTest=1&at_cmd=AT%2BZICCID%3F",
                     buf, sizeof buf) >= 0 &&
            first_hex_run(buf, 18, 20, out, outsz)) return 1;
    }

    if (http_get(SMS_HOST, WEB_PORT,
                 "/goform/goform_get_cmd_process?cmd=ziccid",
                 buf, sizeof buf) < 0) return 0;
    const char *p = strstr(buf, "\"ziccid\":\"");
    if (!p) return 0;
    p += strlen("\"ziccid\":\"");
    const char *q = strchr(p, '"');
    if (!q) return 0;
    int n = (int)(q - p);
    if (n < 18 || n >= outsz) return 0;
    memcpy(out, p, n); out[n] = 0;
    for (int i = 0; i < n; i++)
        if (!isdigit((unsigned char)out[i])) return 0;
    return 1;
}

/* 从文本里找第一段 11~13 位数字并规整成手机号 */
static int parse_mobile_runs(const char *buf, char *out, int outsz) {
    const char *p = buf;
    while (*p) {
        if (isdigit((unsigned char)*p)) {
            const char *s = p;
            while (isdigit((unsigned char)*p)) p++;
            int n = (int)(p - s);
            if (n >= 11 && n <= 13) {
                char tmp[32];
                if (n >= (int)sizeof tmp) n = sizeof tmp - 1;
                memcpy(tmp, s, n); tmp[n] = 0;
                if (norm_mobile(tmp, out, outsz)) return 1;
            }
        } else p++;
    }
    return 0;
}

/* 本机号码：优先 at_server 执行 AT+CNUM，其次 goform msisdn */
static int live_msisdn(char *out, int outsz) {
    char buf[4096];
    for (int tries = 0; tries < 2; tries++) {
        at_gap();
        if (http_get(SMS_HOST, AT_PORT,
                     "/goform/goform_set_cmd_process?goformId=EXECUTE_AT_COMMAND&isTest=1&at_cmd=AT%2BCNUM",
                     buf, sizeof buf) >= 0 &&
            parse_mobile_runs(buf, out, outsz)) return 1;
    }

    if (http_get(SMS_HOST, WEB_PORT,
                 "/goform/goform_get_cmd_process?cmd=msisdn",
                 buf, sizeof buf) < 0) return 0;
    const char *p = strstr(buf, "\"msisdn\":\"");
    if (!p) return 0;
    p += strlen("\"msisdn\":\"");
    const char *q = strchr(p, '"');
    if (!q) return 0;
    int n = (int)(q - p);
    if (n <= 0) return 0;
    if (n >= outsz) n = outsz - 1;
    memcpy(out, p, n); out[n] = 0;
    return 1;
}

/* 网页手动填写的 ICCID；空 = 未授权 */
static char g_manual[32] = "";

/* 读取网页写入的手动 ICCID；只接受 18~20 位十六进制（BCD 卡号含 A-F） */
static void load_manual_cfg(void) {
    g_manual[0] = 0;
    FILE *f = fopen(MANUALF, "r");
    if (!f) return;
    char line[128];
    if (fgets(line, sizeof line, f)) {
        char d[32];
        hex_only(line, d, sizeof d);
        if (strlen(d) >= 18 && strlen(d) <= 20)
            snprintf(g_manual, sizeof g_manual, "%s", d);
    }
    fclose(f);
}

/* 当前卡是否获授权：必须手动填写过，且与实时 ICCID 完全一致；否则一律拒绝 */
static int target_card(void) {
    char iccid[32] = "";
    if (!g_manual[0]) return 0;
    if (!live_iccid(iccid, sizeof iccid)) return 0;
    return strcmp(iccid, g_manual) == 0;
}

/* ---------- 烧流子进程 ---------- */
static pid_t worker_pid = 0;

/* 烧流下载源：直连公网大文件循环拉取，任一成功即用，全部失败则换下一轮 */
struct burn_url { const char *host; int port; const char *path; };
static const struct burn_url BURN_URLS[] = {
    { "mirrors.ustc.edu.cn", 80, "/ubuntu-releases/24.04/ubuntu-24.04.4-desktop-amd64.iso" },
    { "speed.cloudflare.com", 80, "/__down?bytes=10000000" },
    { "speed.hetzner.de", 80, "/100MB.bin" },
    { "mirrors.aliyun.com", 80, "/centos/7/isos/x86_64/CentOS-7-x86_64-Minimal-2009.iso" },
    { NULL, 0, NULL }
};
#define BURN_CAP   (8 * 1024 * 1024)   /* 每轮最多读 8MB 即断开重连 */
#define BURN_MINOK (100 * 1024)        /* 少于 100KB 视为失败（404 页面等） */

/* 单次下载：DNS+建连+GET，读到 cap 或对端关闭，返回字节数，失败 -1 */
static long burn_fetch(const struct burn_url *u) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port[8]; snprintf(port, sizeof port, "%d", u->port);
    if (getaddrinfo(u->host, port, &hints, &res) != 0 || !res) return -1;

    int s = socket(res->ai_family, res->ai_socktype, 0);
    if (s < 0) { freeaddrinfo(res); return -1; }
    struct timeval tv; tv.tv_sec = 15; tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(s, res->ai_addr, res->ai_addrlen) != 0) {
        close(s); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);

    char req[512];
    int rn = snprintf(req, sizeof req,
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Mozilla/5.0\r\nConnection: close\r\n\r\n",
        u->path, u->host);
    if (write(s, req, rn) < 0) { close(s); return -1; }

    long total = 0; char buf[8192];
    while (total < BURN_CAP) {
        int r = read(s, buf, sizeof buf);
        if (r <= 0) break;
        total += r;
    }
    close(s);
    return total > 0 ? total : -1;
}

/* ---------------------------------------------------- 多路并发拉满带宽
 * 单条 TCP 在 30~50ms RTT 下只能跑到 5~6 Mbps（窗口受限），
 * 要让空口（实测 60~80 Mbps）吃满必须 N 条并发。
 * 全部 fd 在同一个 worker 进程内由 poll 驱动，kill worker 即整轮立停，
 * 不会留下泄漏的孙进程继续偷跑流量。 */
#define BURN_STREAMS      8
#define BURN_CAP_PER      (20 * 1024 * 1024)   /* 每路 20MB */
#define BURN_ROUND_SEC    25                   /* 每轮最长 25 秒 */
#define BURN_CONN_SEC     6                    /* 建连总超时 */
#define BURN_MINOK_MULTI  (300 * 1024)         /* 整轮少于 300KB 视为失败 */

static long burn_fetch_multi(const struct burn_url *u) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port[8]; snprintf(port, sizeof port, "%d", u->port);
    if (getaddrinfo(u->host, port, &hints, &res) != 0 || !res) return -1;

    int fds[BURN_STREAMS];
    long got[BURN_STREAMS];
    char ready[BURN_STREAMS];
    for (int i = 0; i < BURN_STREAMS; i++) { fds[i] = -1; got[i] = 0; ready[i] = 0; }

    char req[512];
    int rn = snprintf(req, sizeof req,
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: curl/8.0\r\nConnection: close\r\n\r\n",
        u->path, u->host);

    /* 1) 同时发起 8 条非阻塞 connect */
    for (int i = 0; i < BURN_STREAMS; i++) {
        int s = socket(res->ai_family, res->ai_socktype, 0);
        if (s < 0) continue;
        int fl = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, fl | O_NONBLOCK);
        int rc = connect(s, res->ai_addr, res->ai_addrlen);
        if (rc == 0) { ready[i] = 1; fds[i] = s; }
        else if (errno == EINPROGRESS) fds[i] = s;
        else close(s);
    }
    freeaddrinfo(res);

    /* 2) 等建连完成，最多 BURN_CONN_SEC 秒 */
    time_t tc = time(NULL);
    while (1) {
        struct pollfd pf[BURN_STREAMS];
        int map[BURN_STREAMS], n = 0;
        for (int i = 0; i < BURN_STREAMS; i++) {
            if (fds[i] < 0 || ready[i]) continue;
            pf[n].fd = fds[i]; pf[n].events = POLLOUT; pf[n].revents = 0;
            map[n] = i; n++;
        }
        if (n == 0) break;
        if (time(NULL) - tc >= BURN_CONN_SEC) break;
        int pr = poll(pf, n, 100);
        if (pr <= 0) continue;
        for (int k = 0; k < n; k++) {
            if (!pf[k].revents) continue;
            int i = map[k];
            int err = 0; unsigned int len = sizeof err;
            getsockopt(fds[i], SOL_SOCKET, SO_ERROR, &err, &len);
            if (err == 0) ready[i] = 1;
            else { close(fds[i]); fds[i] = -1; }
        }
    }
    for (int i = 0; i < BURN_STREAMS; i++)
        if (fds[i] >= 0 && !ready[i]) { close(fds[i]); fds[i] = -1; }

    /* 3) 恢复阻塞态发请求，再切回非阻塞配合 poll 读 */
    int active = 0;
    for (int i = 0; i < BURN_STREAMS; i++) {
        if (fds[i] < 0) continue;
        int fl = fcntl(fds[i], F_GETFL, 0);
        fcntl(fds[i], F_SETFL, fl & ~O_NONBLOCK);
        struct timeval tv; tv.tv_sec = 5; tv.tv_usec = 0;
        setsockopt(fds[i], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fds[i], SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (write(fds[i], req, rn) < 0) { close(fds[i]); fds[i] = -1; continue; }
        fcntl(fds[i], F_SETFL, fl | O_NONBLOCK);
        active++;
    }

    /* 4) poll 驱动收数据，跑满 BURN_ROUND_SEC 或达到总量上限 */
    long total = 0;
    const long total_cap = (long)BURN_STREAMS * BURN_CAP_PER;
    time_t t0 = time(NULL);
    char buf[16384];
    while (active > 0 && total < total_cap) {
        if (time(NULL) - t0 >= BURN_ROUND_SEC) break;
        struct pollfd pf[BURN_STREAMS];
        int map[BURN_STREAMS], n = 0;
        for (int i = 0; i < BURN_STREAMS; i++) {
            if (fds[i] < 0) continue;
            pf[n].fd = fds[i]; pf[n].events = POLLIN; pf[n].revents = 0;
            map[n] = i; n++;
        }
        if (n == 0) break;
        int pr = poll(pf, n, 200);
        if (pr <= 0) continue;
        for (int k = 0; k < n; k++) {
            if (!(pf[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            int i = map[k];
            int r = (int)read(fds[i], buf, sizeof buf);
            if (r <= 0) { close(fds[i]); fds[i] = -1; active--; continue; }
            got[i] += r; total += r;
            if (got[i] >= BURN_CAP_PER) { close(fds[i]); fds[i] = -1; active--; }
        }
    }
    for (int i = 0; i < BURN_STREAMS; i++)
        if (fds[i] >= 0) { close(fds[i]); fds[i] = -1; }

    return total > 0 ? total : -1;
}

static void burn_loop(void) {
    int idx = 0;
    unsigned long long total = load_total();
    const char *auth = g_manual;
    for (;;) {
        /* 每轮下载前复核授权：手动 ICCID 被清空或与当前卡不一致，立即退出 */
        if (!target_card()) {
            logline("授权校验不通过（手动 ICCID 为空或与当前卡不一致），烧流子进程退出");
            _exit(0);
        }
        const struct burn_url *u = &BURN_URLS[idx];
        long n = burn_fetch_multi(u);
        if (n >= BURN_MINOK_MULTI) {
            total += (unsigned long long)n;
            save_total(total);
            write_info(total, 1, n, auth);
            logline("烧流一轮 %s 收到 %ld KB（累计 %llu MB）", u->host, n / 1024, total / 1048576);
            idx = 0; /* 成功源优先复用 */
            sleep(BURN_SLEEP);
        } else {
            logline("烧流源 %s 失败，换下一个", u->host);
            idx++;
            if (!BURN_URLS[idx].host) { idx = 0; sleep(5); }
        }
    }
}

static void start_burner(void) {
    if (worker_pid > 0) return;
    pid_t p = fork();
    if (p == 0) { burn_loop(); _exit(0); }
    if (p > 0) {
        worker_pid = p;
        FILE *f = fopen(WPIDF, "w"); if (f) { fprintf(f, "%d\n", p); fclose(f); }
        f = fopen(STATEF, "w"); if (f) { fputs("1", f); fclose(f); }
        {
            write_info(load_total(), 1, 0, g_manual);
            logline("开始烧流量 worker pid=%d（手动授权 ICCID=%s）", p, g_manual);
        }
    }
}

static void stop_burner(const char *reason) {
    if (worker_pid <= 0) return;
    kill(worker_pid, SIGTERM);
    int rc; waitpid(worker_pid, &rc, WNOHANG);
    sleep(1);
    if (kill(worker_pid, 0) == 0) kill(worker_pid, SIGKILL);
    waitpid(worker_pid, &rc, 0);
    worker_pid = 0;
    unlink(WPIDF);
    FILE *f = fopen(STATEF, "w"); if (f) { fputs("0", f); fclose(f); }
    write_info(load_total(), 0, 0, g_manual);
    logline("停止烧流量 (%s)", reason);
}

static void on_sig(int sig) { (void)sig; g_stop = 1; }
static volatile sig_atomic_t g_reload = 0;
static void on_usr1(int sig) { (void)sig; g_reload = 1; }

/* 根据当前邻居集合决定烧流状态；授权被清空/改错时立即停掉旧 worker */
static void evaluate(const char *why) {
    if (neigh_n > 0) {
        stop_burner(why);
    } else if (target_card()) {
        start_burner();
    } else {
        stop_burner("授权为空或不匹配");
        logline("无终端但手动授权为空或与当前 ICCID 不一致（授权=%s），不启动烧流量",
                g_manual[0] ? g_manual : "(空)");
    }
    write_info(load_total(), worker_pid > 0 ? 1 : 0, 0, g_manual);
}

/* ---------- netlink 邻居事件 ---------- */
static int netlink_open(void) {
    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0) return -1;
    struct sockaddr_nl sa; memset(&sa, 0, sizeof sa);
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = RTMGRP_NEIGH;
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return -1; }
    return fd;
}

static void netlink_drain(int fd) {
    char buf[8192];
    ssize_t n = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
    if (n <= 0) return;
    struct nlmsghdr *h = (struct nlmsghdr *)buf;
    for (; NLMSG_OK(h, n); h = NLMSG_NEXT(h, n)) {
        if (h->nlmsg_type != RTM_NEWNEIGH && h->nlmsg_type != RTM_DELNEIGH) continue;
        struct ndmsg *nd = (struct ndmsg *)NLMSG_DATA(h);
        struct rtattr *rta = (struct rtattr *)((char *)nd + NLMSG_ALIGN(sizeof *nd));
        int rtl = (int)NLMSG_PAYLOAD(h, sizeof *nd);
        unsigned long ip = 0;
        for (; RTA_OK(rta, rtl); rta = RTA_NEXT(rta, rtl)) {
            if (rta->rta_type == NDA_DST && rta->rta_len >= RTA_LENGTH(4))
                memcpy(&ip, RTA_DATA(rta), 4);
        }
        if (!in_lan_net(ip)) continue;
        struct in_addr in; in.s_addr = ip;
        if (h->nlmsg_type == RTM_NEWNEIGH) {
            /* 状态变为 INCOMPLETE(0x01)/FAILED(0x40) = 终端失联，按离开处理 */
            if (nd->ndm_state & (NUD_INCOMPLETE | NUD_FAILED)) {
                neigh_del(ip);
                logline("事件：终端失联 %s，剩余 %d", inet_ntoa(in), neigh_n);
                evaluate("终端失联");
            } else {
                neigh_add(ip);
                logline("事件：终端接入 %s → 停止烧流量", inet_ntoa(in));
                evaluate("终端接入");
            }
        } else {
            neigh_del(ip);
            logline("事件：终端离开 %s，剩余 %d", inet_ntoa(in), neigh_n);
            evaluate("终端离开");
        }
    }
}

/* ---------- 主循环 ---------- */
static void run_loop(int daemonized) {
    (void)daemonized;
    signal(SIGTERM, on_sig); signal(SIGINT, on_sig); signal(SIGUSR1, on_usr1);
    load_manual_cfg();

    int fd = netlink_open();
    if (fd < 0) { logline("netlink 打开失败: %s", strerror(errno)); return; }

    neigh_init_from_arp();
    {
        char iccid[32] = "";
        live_iccid(iccid, sizeof iccid);
        logline("守护启动，终端 %d；手动授权ICCID=%s 当前ICCID=%s；允许=%s",
                neigh_n, g_manual[0] ? g_manual : "(未填写)",
                iccid[0] ? iccid : "读取失败", target_card() ? "是" : "否");
        write_info(load_total(), 0, 0, g_manual);
    }
    evaluate("启动");

    while (!g_stop) {
        struct pollfd p = { fd, POLLIN, 0 };
        int pr = poll(&p, 1, 2000);
        if (pr > 0) netlink_drain(fd);
        if (g_reload) {
            g_reload = 0;
            load_manual_cfg();
            logline("收到手动授权更新，重新判定（授权=%s）", g_manual[0] ? g_manual : "(空)");
            evaluate("手动授权更新");
        }
        /* 回收已退出的子进程；退出后重新判定（换卡则保持不烧，不自动重启） */
        while (worker_pid > 0 && waitpid(worker_pid, NULL, WNOHANG) > 0) {
            worker_pid = 0; unlink(WPIDF);
            FILE *f = fopen(STATEF, "w"); if (f) { fputs("0", f); fclose(f); }
            evaluate("worker退出");
        }
    }
    stop_burner("退出");
    close(fd);
    logline("守护退出");
}

/* ---------- 子命令 ---------- */
static int do_check(void) {
    neigh_n = 0;
    neigh_init_from_arp();
    printf("局域网在线终端 %d:\n", neigh_n);
    for (int i = 0; i < neigh_n; i++) {
        struct in_addr in; in.s_addr = neigh_ips[i];
        printf("  %s\n", inet_ntoa(in));
    }
    char iccid[32] = "";
    live_iccid(iccid, sizeof iccid);
    printf("手动授权 ICCID: %s\n", g_manual[0] ? g_manual : "(未填写，禁止烧流)");
    printf("当前 ICCID: %s\n", iccid[0] ? iccid : "(读取失败)");
    printf("允许烧流: %s\n", target_card() ? "是" : "否");
    int nl = netlink_open();
    printf("netlink 事件源: %s\n", nl >= 0 ? "可用" : "打开失败");
    if (nl >= 0) close(nl);
    return 0;
}

/* 只探测当前 ICCID，供网页接口做即时比对 */
static int do_probe(void) {
    char iccid[32] = "";
    if (live_iccid(iccid, sizeof iccid)) {
        printf("%s\n", iccid);
        return 0;
    }
    printf("ERROR\n");
    return 1;
}

/* 自检：逐个试烧流源，打印每轮字节数 */
static int do_fetch(void) {
    /* 诊断：只跑首选源的多流一轮（约 100~160MB），不把 4 个源全跑一遍 */
    if (!BURN_URLS[0].host) return 1;
    time_t t0 = time(NULL);
    long n = burn_fetch_multi(&BURN_URLS[0]);
    long dt = (long)(time(NULL) - t0);
    if (dt <= 0) dt = 1;
    printf("%s -> %s (%ld bytes / %ld s / %.2f Mbps)\n", BURN_URLS[0].host,
           n >= BURN_MINOK_MULTI ? "成功" : "失败", n > 0 ? n : 0, dt,
           n > 0 ? (double)n * 8.0 / (double)dt / 1e6 : 0.0);
    return 0;
}

/* 诊断：打印 AT 接口原始返回 */
static int do_raw(void) {
    char buf[4096];
    int r = http_get(SMS_HOST, AT_PORT,
        "/goform/goform_set_cmd_process?goformId=EXECUTE_AT_COMMAND&isTest=1&at_cmd=AT%2BZICCID%3F",
        buf, sizeof buf);
    printf("ZICCID r=%d [%s]\n", r, buf);
    r = http_get(SMS_HOST, AT_PORT,
        "/goform/goform_set_cmd_process?goformId=EXECUTE_AT_COMMAND&isTest=1&at_cmd=AT%2BCNUM",
        buf, sizeof buf);
    printf("CNUM   r=%d [%s]\n", r, buf);
    return 0;
}

static void do_status(void) {
    char b[64]; FILE *f;
    printf("守护: ");
    if ((f = fopen(PIDF, "r"))) { int p; if (fscanf(f, "%d", &p)==1 && kill(p,0)==0) printf("运行中 pid %d\n", p); else printf("未运行(pid文件残留)\n"); fclose(f); }
    else printf("未运行\n");
    printf("烧流: ");
    if ((f = fopen(WPIDF, "r"))) { int p; fscanf(f,"%d",&p); printf("进行中 pid %d\n", p); fclose(f); }
    else printf("空闲\n");
    if ((f = fopen(STATEF, "r"))) { fgets(b, sizeof b, f); printf("状态: %s\n", b); fclose(f); }
    unsigned long long tot = load_total();
    printf("累计烧流: %llu MB (%llu 字节)\n", tot / 1048576, tot);
    printf("手动授权 ICCID: %s\n", g_manual[0] ? g_manual : "(未填写，禁止烧流)");
}

int main(int argc, char **argv) {
    const char *cmd = argc > 1 ? argv[1] : "start";
    load_manual_cfg();
    if (!strcmp(cmd, "check")) return do_check();
    if (!strcmp(cmd, "probe")) return do_probe();
    if (!strcmp(cmd, "raw")) return do_raw();
    if (!strcmp(cmd, "fetch")) return do_fetch();
    if (!strcmp(cmd, "status")) { do_status(); return 0; }
    if (!strcmp(cmd, "reload")) {
        FILE *f = fopen(PIDF, "r"); int p = 0;
        if (f) { fscanf(f, "%d", &p); fclose(f); }
        if (p > 0 && kill(p, 0) == 0) { kill(p, SIGUSR1); printf("已通知守护重新读取授权 pid %d\n", p); }
        else printf("守护未运行，无需重载\n");
        return 0;
    }
    if (!strcmp(cmd, "stop")) {
        FILE *f = fopen(PIDF, "r"); int p = 0;
        if (f) { fscanf(f, "%d", &p); fclose(f); }
        if (p > 0) { kill(p, SIGTERM); printf("已发送停止 pid %d\n", p); }
        unlink(PIDF); return 0;
    }
    if (!strcmp(cmd, "run")) { run_loop(1); return 0; }

    /* start：后台化 */
    FILE *f = fopen(PIDF, "r"); int old = 0;
    if (f) { fscanf(f, "%d", &old); fclose(f); }
    if (old > 0 && kill(old, 0) == 0) { printf("已在运行 pid %d\n", old); return 0; }

    pid_t p = fork();
    if (p < 0) return 1;
    if (p > 0) {
        FILE *wf = fopen(PIDF, "w"); if (wf) { fprintf(wf, "%d\n", p); fclose(wf); }
        printf("已后台启动 pid %d，日志 %s\n", p, LOGF);
        return 0;
    }
    setsid();
    int dev = open("/dev/null", 0); dup2(dev, 0); dup2(dev, 1); dup2(dev, 2);
    f = fopen(PIDF, "w"); if (f) { fprintf(f, "%d\n", getpid()); fclose(f); }
    run_loop(0);
    unlink(PIDF);
    return 0;
}
