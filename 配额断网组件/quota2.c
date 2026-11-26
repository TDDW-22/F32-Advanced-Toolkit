/* quota.c —— F32 随身WiFi「套餐总额用完自动断网」(实时计数版)
 *
 * 配置: /mnt/userdata/etc_rw/quota.conf
 *   total_gb=10      套餐总额(GB)；0=不限量
 *
 * 已用量: 直接取固件实时计数 monthly_tx_bytes + monthly_rx_bytes,
 *   已用 = 当前计数 / GB, 不叠加任何手填基准或快照。
 *
 * 断网动作: wan1 是承载数据的 ECM 网口, 置 IFF_UP=0 即真实断流量
 *   (固件 fastnat_level=2, iptables/route 都绕过 netfilter, 只有网口 down 有效)
 *   恢复: 置 IFF_UP=1 并补回 "default dev wan1" 默认路由(网口 down 时会被内核删掉)。
 *
 * 用法: quota once | probe | daemon
 * 调试: QUOTA_DEBUG=1, QUOTA_TOTAL_GB 覆盖总额, QUOTA_FORCE_CUT=1 强制断
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/route.h>

#define CFG     "/mnt/userdata/etc_rw/nv/main/cfg"
#define CONF    "/mnt/userdata/etc_rw/quota.conf"
#define BASE    "/mnt/userdata/etc_rw/quota.base"
#define LOG     "/mnt/userdata/etc_rw/quota.log"
#define STAT    "/mnt/userdata/etc_rw/quota.status"
#define FLAG    "/mnt/userdata/etc_rw/quota.cut"
#define WEBTXT  "/mnt/userdata/etc_rw/quota_info.txt"
#define WANIF   "wan1"
#define GB      (1024.0 * 1024.0 * 1024.0)
#define MAXCFG  (512 * 1024)

static unsigned char *g_buf = NULL;
static long g_len = 0;

/* ---------- cfg 读取 (cfg 含 NUL, 必须按长度逐字节找, 不能用 strstr) ---------- */
static int slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (!g_buf) g_buf = malloc(MAXCFG);
    if (!g_buf) { fclose(f); return 0; }
    g_len = (long)fread(g_buf, 1, MAXCFG, f);
    fclose(f);
    return 1;
}

/* 找到 key 后把值(到 NUL 或非数字/字母/下划线逗号为止)拷出 */
static long find_val(const char *key, char *out, int outlen) {
    long kl = (long)strlen(key);
    for (long i = 0; i + kl < g_len; i++) {
        if (memcmp(g_buf + i, key, kl) != 0) continue;
        /* key 前面必须是块边界 */
        if (i > 0 && (g_buf[i - 1] == '_' || (g_buf[i - 1] >= '0' && g_buf[i - 1] <= '9') ||
                      (g_buf[i - 1] >= 'a' && g_buf[i - 1] <= 'z') ||
                      (g_buf[i - 1] >= 'A' && g_buf[i - 1] <= 'Z')))
            continue;
        long p = i + kl;
        while (p < g_len && g_buf[p] == '=') p++;   /* key 后面可能带 '=' */
        int n = 0;
        while (p < g_len && n < outlen - 1) {
            unsigned char c = g_buf[p];
            if ((c >= '0' && c <= '9') || c == '_' || c == '.' ||
                (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == ',') {
                out[n++] = (char)c;
                p++;
            } else break;
        }
        out[n] = 0;
        return n;
    }
    out[0] = 0;
    return 0;
}

static long long find_num(const char *key) {
    char v[64];
    if (find_val(key, v, sizeof(v)) <= 0) return -1;
    return atoll(v);
}

/* ---------- 手填配置 ---------- */
static double conf_total = 0, conf_used = 0;
static long conf_mtime = 0;

static void load_conf(void) {
    FILE *f = fopen(CONF, "r");
    conf_total = 0; conf_used = 0; conf_mtime = 0;
    if (!f) return;
    struct stat st;
    if (stat(CONF, &st) == 0) conf_mtime = (long)st.st_mtime;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == 0) continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = p, *v = eq + 1;
        while (k[0] && (k[strlen(k) - 1] == ' ' || k[strlen(k) - 1] == '\t')) k[strlen(k) - 1] = 0;
        while (*v == ' ' || *v == '\t') v++;
        if (strcmp(k, "total_gb") == 0) conf_total = atof(v);
        else if (strcmp(k, "used_gb") == 0) conf_used = atof(v);
    }
    fclose(f);
}

/* ---------- 计数快照 ---------- */
static long long base_counter = 0;
static double base_used = 0;
static long base_mtime = 0;

static void load_base(void) {
    FILE *f = fopen(BASE, "r");
    base_counter = 0; base_used = 0; base_mtime = 0;
    if (!f) return;
    struct stat st;
    if (stat(BASE, &st) == 0) base_mtime = (long)st.st_mtime;
    char k[64];
    long long n;
    while (fscanf(f, "%63s %lld", k, &n) == 2) {
        if (strcmp(k, "counter") == 0) base_counter = n;
        else if (strcmp(k, "usedgb100") == 0) base_used = (double)n / 100.0;
    }
    fclose(f);
}

static void save_base(long long counter, double used) {
    FILE *f = fopen(BASE, "w");
    if (!f) return;
    fprintf(f, "counter %lld\nusedgb100 %lld\n", counter, (long long)(used * 100 + 0.5));
    fclose(f);
}

/* ---------- 网口与路由 ---------- */
static int iface_is_up(const char *name) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) { close(s); return -1; }
    close(s);
    return (ifr.ifr_flags & IFF_UP) ? 1 : 0;
}

static int iface_set_up(const char *name, int up) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) { close(s); return -1; }
    if (up) ifr.ifr_flags |= IFF_UP; else ifr.ifr_flags &= ~IFF_UP;
    int r = ioctl(s, SIOCSIFFLAGS, &ifr);
    close(s);
    return (r < 0) ? -1 : iface_is_up(name);
}

/* /proc/net/route 里是否已有 default */
static int has_default_route(void) {
    FILE *f = fopen("/proc/net/route", "r");
    if (!f) return 1;
    char line[256];
    int found = 0;
    if (fgets(line, sizeof(line), f)) { }        /* 跳过表头 */
    while (fgets(line, sizeof(line), f)) {
        char iface[64];
        unsigned long dest = 0, gw = 0, flags = 0;
        if (sscanf(line, "%63s %lx %lx %lx", iface, &dest, &gw, &flags) != 4) continue;
        if (dest == 0 && gw == 0) { found = 1; break; }
    }
    fclose(f);
    return found;
}

/* 补回 default dev wan1 (网口 down 时内核会丢掉这条) */
static int ensure_default_route(void) {
    if (has_default_route()) return 1;
    system("/sbin/route add default dev " WANIF " 2>/dev/null");
    if (!has_default_route())
        system("route add default dev " WANIF " 2>/dev/null");
    return has_default_route();
}

/* ---------- 给首页用的数字 (total|used|left|state) ---------- */
static void write_web(double used, double total, int cut) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%.2fGB|%.2fGB|%.2fGB|%s",
             total, used, total > used ? total - used : 0.0, cut ? "cut" : "ok");
    FILE *f = fopen(WEBTXT, "r");
    char old[128] = "";
    if (f) { if (fgets(old, sizeof(old), f)) { } fclose(f); }
    old[strcspn(old, "\n")] = 0;
    if (strcmp(old, buf) == 0) return;
    f = fopen(WEBTXT, "w");
    if (!f) return;
    fprintf(f, "%s\n", buf);
    fclose(f);
}

/* ---------- 状态/日志 ---------- */
static void logline(const char *msg) {
    FILE *f = fopen(LOG, "a");
    if (!f) return;
    fprintf(f, "%s\n", msg);
    fclose(f);
}

static void write_status(int cut, double used, double total, long long raw) {
    FILE *f = fopen(STAT, "w");
    if (!f) return;
    fprintf(f, "state=%s used=%.2fGB total=%.2fGB left=%.2fGB raw=%lld\n",
            cut ? "cut" : "ok", used, total, total - used, raw);
    fclose(f);
}

static void trim_log(void) {
    FILE *in = fopen(LOG, "r");
    if (!in) return;
    long n = 0;
    char line[512];
    while (fgets(line, sizeof(line), in)) n++;
    fclose(in);
    if (n <= 200) return;
    long skip = n - 150, i = 0, kl = 0;
    char *keep = malloc(150 * 512);
    if (!keep) return;
    in = fopen(LOG, "r");
    if (!in) { free(keep); return; }
    while (fgets(line, sizeof(line), in)) {
        if (i++ < skip) continue;
        long l = (long)strlen(line);
        if (kl + l < 150 * 512) { memcpy(keep + kl, line, l); kl += l; }
    }
    fclose(in);
    FILE *out = fopen(LOG, "w");
    if (out) { fwrite(keep, 1, kl, out); fclose(out); }
    free(keep);
}

/* ---------- 主判断 ---------- */
static int run_once(int quiet) {
    load_conf();
    long long tx = 0, rx = 0;
    if (slurp(CFG)) {
        tx = find_num("monthly_tx_bytes");
        rx = find_num("monthly_rx_bytes");
    }
    if (tx < 0) tx = 0;
    if (rx < 0) rx = 0;
    long long raw = tx + rx;

    /* 已用完全按设备实时收发计数, 不叠加手填基准 */
    double total = conf_total;
    double used = (double)raw / GB;
    const char *e1 = getenv("QUOTA_TOTAL_GB");
    if (e1 && *e1) total = atof(e1);

    if (getenv("QUOTA_DEBUG"))
        fprintf(stderr, "DEBUG total=%.2f used=%.2f raw=%lld\n",
                total, used, raw);

    int force = getenv("QUOTA_FORCE_CUT") ? atoi(getenv("QUOTA_FORCE_CUT")) : -1;
    int cut = (force >= 0 ? force : (total > 0 && used >= total));
    int was_cut = (access(FLAG, F_OK) == 0);

    char msg[256];
    if (cut) {
        int up = iface_is_up(WANIF);
        if (up != 0) {
            int now = iface_set_up(WANIF, 0);
            if (now == 0) {
                snprintf(msg, sizeof(msg),
                         "QUOTA cut used=%.2fGB total=%.2fGB wan1=down",
                         used, total);
            } else {
                snprintf(msg, sizeof(msg),
                         "QUOTA cut FAILED used=%.2fGB total=%.2fGB wan1_up=%d",
                         used, total, up);
            }
            logline(msg);
            if (!quiet) fprintf(stderr, "%s\n", msg);
        }
        FILE *f = fopen(FLAG, "w");
        if (f) { fprintf(f, "%.2f\n", used); fclose(f); }
        write_status(1, used, total, raw);
        write_web(used, total, 1);
        return 1;
    }

    if (was_cut) {
        iface_set_up(WANIF, 1);
        sleep(2);
        int rok = ensure_default_route();
        unlink(FLAG);
        snprintf(msg, sizeof(msg),
                 "QUOTA restore used=%.2fGB total=%.2fGB wan1=up route=%s",
                 used, total, rok ? "ok" : "MISSING");
        logline(msg);
        fprintf(stderr, "%s\n", msg);
    }
    write_status(0, used, total, raw);
    write_web(used, total, 0);
    return 0;
}

int main(int argc, char **argv) {
    int daemon_mode = (argc > 1 && strcmp(argv[1], "daemon") == 0);
    if (daemon_mode) {
        /* 脱离 adb/终端会话, 否则 shell 退出会被回收 */
        if (fork() > 0) _exit(0);
        setsid();
    }
    for (;;) {
        int cut = run_once(0);
        if (!daemon_mode) break;
        trim_log();
        /* 断网期间每 10 秒复核一次, 防止模组自行把 wan1 拉起 */
        sleep(cut ? 10 : 60);
    }
    return 0;
}
