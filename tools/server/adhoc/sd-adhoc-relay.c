/*
 * sd-adhoc-relay - session registry, UDP relay and lap-record keeper for
 * Speed Dreams VR ad-hoc races.
 *
 * Any player can host a race on their own headset. Headsets sit behind home
 * routers, so nobody can connect to them directly; instead the host keeps one
 * UDP "tunnel" socket open to this relay, and the relay gives the session a
 * public UDP port. Players who join send their ordinary ENet traffic to that
 * port; the relay wraps each datagram with a client id and passes it down the
 * tunnel, and the host's game unwraps it and hands it to its local race server.
 * Replies travel the same way back. The relay never looks inside the game
 * traffic - it is plain UDP forwarding, so ENet works through it unchanged.
 *
 * It also keeps the leaderboard: when a hosted race ends, the host reports each
 * player's best lap, and the relay keeps the ten best drivers per track on disk
 * and serves them over HTTP in the format the game's leaderboard screen reads.
 * Two relays (primary and backup) pass every new lap to each other and swap
 * their whole tables now and then, so the leaderboard is the same on both.
 *
 * Control port (default 28500/udp), from the host's tunnel socket:
 *   "HOST <proto> <maxplayers> <track> <name...>"  -> "OK <id> <port>" | "ERR <why>"
 *   "KA <id> <players>"                            -> "KA"   (every few seconds)
 *   "BYE <id>"                                     -> session closed
 *   "LAP 1 <track> <category> <car> <secs> <name...>"  a best lap from a finished race
 *   0x01 <u32 client id> <payload>                 -> payload sent to that client
 * from anyone:
 *   "LIST"  -> "LIST\n<id> <port> <players> <max> <track> <name>\n..."
 *   "PING"  -> "PONG"
 * from the peer relay only:
 *   "PLAP ..." (same fields as LAP), "PSYNC" (please send me all you have)
 * Relay -> host, on the tunnel:
 *   0x01 <u32 client id> <payload>                 a datagram from a client
 *   0x02 <u32 client id>                           that client has gone quiet
 *
 * Session ports: base .. base+count-1 (default 28510..28573/udp).
 * Stats (default 28600/tcp): GET /records (lap records), GET /sessions.
 *
 * Build:  cc -O2 -Wall -o sd-adhoc-relay sd-adhoc-relay.c
 * Run:    ./sd-adhoc-relay [-p 28500] [-b 28510] [-n 64] [-w 28600] [-d dir] [-P peer[:port]]
 */

#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PROTO_VERSION      1
#define MAX_SESSIONS_CAP   256
#define MAX_CLIENTS        32
#define SESSION_TIMEOUT_S  15   /* host silent this long -> session gone */
#define CLIENT_TIMEOUT_S   30   /* client silent this long -> forgotten */
#define BUF_SIZE           2048
#define MSG_DATA           0x01
#define MSG_GONE           0x02

#define FIELD              33   /* track, category, car, name: up to 32 chars */
#define RECORDS_PER_TRACK  10
#define MAX_TRACKS         512
#define MIN_LAP_S          5.0
#define MAX_LAP_S          3600.0
#define PEER_SYNC_S        600
#define MAX_HTTP           16
#define HTTP_TIMEOUT_S     3
#define SYNC_PER_TICK      40

typedef struct {
    int used;
    uint32_t cid;
    struct sockaddr_in addr;
    time_t last_seen;
} Client;

typedef struct {
    int used;
    uint32_t id;
    int fd;
    int port;
    struct sockaddr_in host;
    time_t last_seen;
    int players, max_players;
    char track[FIELD];
    char name[FIELD];
    uint32_t next_cid;
    Client clients[MAX_CLIENTS];
} Session;

typedef struct {
    char name[FIELD];
    char car[FIELD];
    double time;
} LapRec;

typedef struct {
    char track[FIELD];
    char category[FIELD];
    int n;
    LapRec times[RECORDS_PER_TRACK];
} TrackRecs;

typedef struct {
    int fd;
    char buf[2048];
    size_t len;
    time_t deadline;
} HttpClient;

static volatile sig_atomic_t g_running = 1;
static int g_ctl = -1;
static Session g_sessions[MAX_SESSIONS_CAP];
static int g_nsessions = 64;
static int g_base_port = 28510;
static uint32_t g_next_id = 1;

static TrackRecs g_tracks[MAX_TRACKS];
static int g_ntracks = 0;
static char g_records_path[512] = "records.tsv";
static int g_records_dirty = 0;

static int g_have_peer = 0;
static struct sockaddr_in g_peer;

/* Records still to be sent to the peer after it asked for them. */
static int g_sync_track = -1, g_sync_rec = 0;

static int g_http = -1;
static HttpClient g_httpc[MAX_HTTP];

static void on_signal(int sig) { (void)sig; g_running = 0; }

static void logf_(const char *fmt, ...)
{
    char ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(stderr, "%s ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static const char *addr_str(const struct sockaddr_in *a)
{
    static char buf[2][32];
    static int i;
    i ^= 1;
    snprintf(buf[i], sizeof buf[i], "%s:%u", inet_ntoa(a->sin_addr), ntohs(a->sin_port));
    return buf[i];
}

static int same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static int open_udp(int port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void send_to(int fd, const struct sockaddr_in *to, const void *buf, size_t len)
{
    sendto(fd, buf, len, 0, (const struct sockaddr *)to, sizeof *to);
}

static void reply_text(const struct sockaddr_in *to, const char *fmt, ...)
{
    char buf[BUF_SIZE];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) send_to(g_ctl, to, buf, (size_t)(n < (int)sizeof buf ? n : (int)sizeof buf - 1));
}

/* Keep only characters that are safe inside a one-line, space-separated reply
 * and a tab-separated file. */
static void clean_field(char *dst, size_t cap, const char *src, int allow_space)
{
    size_t j = 0;
    for (; *src && j + 1 < cap; src++) {
        unsigned char c = (unsigned char)*src;
        if (c < 0x20 || c == 0x7f) continue;
        if (c == ' ' && !allow_space) c = '_';
        dst[j++] = (char)c;
    }
    while (j > 0 && dst[j - 1] == ' ') j--;
    dst[j] = 0;
    if (!j && cap > 1) { dst[0] = '?'; dst[1] = 0; }
}

/* ------------------------------------------------------------------ records */

static int cmp_lap(const void *a, const void *b)
{
    double d = ((const LapRec *)a)->time - ((const LapRec *)b)->time;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

/* Fold one best lap into the table: one entry per driver, their best ever,
 * fastest first, ten per track. Returns 1 if the table changed. */
static int merge_lap(const char *track, const char *category, const char *car,
                     double secs, const char *name)
{
    if (!(secs >= MIN_LAP_S && secs <= MAX_LAP_S)) return 0;

    TrackRecs *t = NULL;
    for (int i = 0; i < g_ntracks; i++)
        if (!strcmp(g_tracks[i].track, track)) { t = &g_tracks[i]; break; }
    if (!t) {
        if (g_ntracks >= MAX_TRACKS) return 0;
        t = &g_tracks[g_ntracks++];
        memset(t, 0, sizeof *t);
        snprintf(t->track, sizeof t->track, "%s", track);
    }
    if (category[0] && strcmp(category, "-") && strcmp(t->category, category))
        snprintf(t->category, sizeof t->category, "%s", category);

    for (int i = 0; i < t->n; i++) {
        if (strcmp(t->times[i].name, name)) continue;
        if (secs >= t->times[i].time - 0.0005) return 0;     /* not better */
        t->times[i].time = secs;
        snprintf(t->times[i].car, sizeof t->times[i].car, "%s", car);
        qsort(t->times, (size_t)t->n, sizeof t->times[0], cmp_lap);
        return 1;
    }

    if (t->n == RECORDS_PER_TRACK && secs >= t->times[t->n - 1].time) return 0;
    if (t->n < RECORDS_PER_TRACK) t->n++;
    LapRec *r = &t->times[t->n - 1];
    snprintf(r->name, sizeof r->name, "%s", name);
    snprintf(r->car, sizeof r->car, "%s", car);
    r->time = secs;
    qsort(t->times, (size_t)t->n, sizeof t->times[0], cmp_lap);
    return 1;
}

static void load_records(void)
{
    FILE *f = fopen(g_records_path, "r");
    if (!f) return;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char *field[5];
        int k = 0;
        char *p = line;
        line[strcspn(line, "\r\n")] = 0;
        while (k < 5) {
            field[k++] = p;
            char *tab = strchr(p, '\t');
            if (!tab) break;
            *tab = 0;
            p = tab + 1;
        }
        if (k != 5) continue;
        if (merge_lap(field[0], field[1], field[3], atof(field[2]), field[4])) n++;
    }
    fclose(f);
    logf_("loaded %d lap records on %d tracks from %s", n, g_ntracks, g_records_path);
}

static void save_records(void)
{
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_records_path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        logf_("cannot write %s: %s", tmp, strerror(errno));
        return;
    }
    for (int i = 0; i < g_ntracks; i++)
        for (int j = 0; j < g_tracks[i].n; j++)
            fprintf(f, "%s\t%s\t%.3f\t%s\t%s\n", g_tracks[i].track,
                    g_tracks[i].category[0] ? g_tracks[i].category : "-",
                    g_tracks[i].times[j].time, g_tracks[i].times[j].car,
                    g_tracks[i].times[j].name);
    if (fclose(f) == 0 && rename(tmp, g_records_path) == 0)
        g_records_dirty = 0;
    else
        logf_("cannot replace %s: %s", g_records_path, strerror(errno));
}

/* "<track> <category> <car> <secs> <name...>" -> merged. */
static int take_lap(const char *args, int from_peer)
{
    char track[64], category[64], car[64];
    double secs = 0;
    int used = 0;
    if (sscanf(args, "%63s %63s %63s %lf %n", track, category, car, &secs, &used) < 4 || used <= 0)
        return 0;
    char ct[FIELD], cc[FIELD], ccar[FIELD], cn[FIELD];
    clean_field(ct, sizeof ct, track, 0);
    clean_field(cc, sizeof cc, category, 0);
    clean_field(ccar, sizeof ccar, car, 0);
    clean_field(cn, sizeof cn, args + used, 1);
    if (!merge_lap(ct, cc, ccar, secs, cn)) return 0;
    g_records_dirty = 1;
    logf_("lap record%s: %s, %.3f at %s in %s", from_peer ? " (from peer)" : "", cn, secs, ct, ccar);
    return 1;
}

static void send_peer_lap(const TrackRecs *t, const LapRec *r)
{
    if (!g_have_peer) return;
    reply_text(&g_peer, "PLAP %s %s %s %.3f %s", t->track,
               t->category[0] ? t->category : "-", r->car, r->time, r->name);
}

/* Pace the full-table answer to PSYNC, so it does not arrive as one burst. */
static void pump_sync(void)
{
    for (int k = 0; k < SYNC_PER_TICK && g_sync_track >= 0; k++) {
        if (g_sync_track >= g_ntracks) { g_sync_track = -1; break; }
        TrackRecs *t = &g_tracks[g_sync_track];
        if (g_sync_rec >= t->n) { g_sync_track++; g_sync_rec = 0; continue; }
        send_peer_lap(t, &t->times[g_sync_rec++]);
    }
}

/* ------------------------------------------------------------------ sessions */

static Session *find_session_by_id(uint32_t id)
{
    for (int i = 0; i < g_nsessions; i++)
        if (g_sessions[i].used && g_sessions[i].id == id) return &g_sessions[i];
    return NULL;
}

static Session *find_session_by_host(const struct sockaddr_in *host)
{
    for (int i = 0; i < g_nsessions; i++)
        if (g_sessions[i].used && same_addr(&g_sessions[i].host, host)) return &g_sessions[i];
    return NULL;
}

static void close_session(Session *s, const char *why)
{
    logf_("session %u (%s, %s) closed: %s", s->id, s->name, s->track, why);
    close(s->fd);
    s->used = 0;
    s->fd = -1;
}

static void cmd_host(const struct sockaddr_in *from, char *args)
{
    int proto = 0, maxp = 0, consumed = 0;
    char track[64] = "";
    if (sscanf(args, "%d %d %63s %n", &proto, &maxp, track, &consumed) < 3) {
        reply_text(from, "ERR syntax");
        return;
    }
    if (proto != PROTO_VERSION) {
        reply_text(from, "ERR version %d", PROTO_VERSION);
        return;
    }

    Session *s = find_session_by_host(from);
    if (!s) {
        for (int i = 0; i < g_nsessions; i++) {
            if (g_sessions[i].used) continue;
            int fd = open_udp(g_base_port + i);
            if (fd < 0) {
                logf_("cannot open session port %d: %s", g_base_port + i, strerror(errno));
                continue;
            }
            s = &g_sessions[i];
            memset(s, 0, sizeof *s);
            s->used = 1;
            s->fd = fd;
            s->port = g_base_port + i;
            s->id = g_next_id++;
            s->host = *from;
            s->next_cid = 1;
            break;
        }
        if (!s) {
            reply_text(from, "ERR full");
            return;
        }
        logf_("session %u opened on port %d by %s", s->id, s->port, addr_str(from));
    }
    s->max_players = maxp < 1 ? 1 : (maxp > MAX_CLIENTS ? MAX_CLIENTS : maxp);
    clean_field(s->track, sizeof s->track, track, 0);
    clean_field(s->name, sizeof s->name, args + consumed, 1);
    if (s->players < 1) s->players = 1;
    s->last_seen = time(NULL);
    reply_text(from, "OK %u %d", s->id, s->port);
}

static void cmd_list(const struct sockaddr_in *from)
{
    char buf[1400];
    int n = snprintf(buf, sizeof buf, "LIST\n");
    for (int i = 0; i < g_nsessions; i++) {
        Session *s = &g_sessions[i];
        if (!s->used) continue;
        int m = snprintf(buf + n, sizeof buf - (size_t)n, "%u %d %d %d %s %s\n",
                         s->id, s->port, s->players, s->max_players, s->track, s->name);
        if (m < 0 || n + m >= (int)sizeof buf) break;
        n += m;
    }
    send_to(g_ctl, from, buf, (size_t)n);
}

static int from_peer(const struct sockaddr_in *from)
{
    return g_have_peer && from->sin_addr.s_addr == g_peer.sin_addr.s_addr;
}

static void handle_control(void)
{
    unsigned char buf[BUF_SIZE];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    ssize_t len = recvfrom(g_ctl, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
    if (len <= 0) return;

    if (buf[0] == MSG_DATA) {
        /* Host -> one of its clients. Only the session's own host may do this. */
        if (len < 5) return;
        Session *s = find_session_by_host(&from);
        if (!s) return;
        uint32_t cid;
        memcpy(&cid, buf + 1, 4);
        cid = ntohl(cid);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (s->clients[i].used && s->clients[i].cid == cid) {
                send_to(s->fd, &s->clients[i].addr, buf + 5, (size_t)(len - 5));
                break;
            }
        }
        s->last_seen = time(NULL);
        return;
    }

    buf[len] = 0;
    char *text = (char *)buf;
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) text[--len] = 0;

    if (!strncmp(text, "HOST ", 5)) {
        cmd_host(&from, text + 5);
    } else if (!strncmp(text, "KA ", 3)) {
        unsigned id = 0;
        int players = 0;
        int got = sscanf(text + 3, "%u %d", &id, &players);
        Session *s = got >= 1 ? find_session_by_id(id) : NULL;
        if (s && same_addr(&s->host, &from)) {
            s->last_seen = time(NULL);
            if (got == 2 && players >= 0 && players <= MAX_CLIENTS) s->players = players;
            reply_text(&from, "KA");
        } else {
            reply_text(&from, "ERR nosession");
        }
    } else if (!strncmp(text, "BYE ", 4)) {
        unsigned id = (unsigned)strtoul(text + 4, NULL, 10);
        Session *s = find_session_by_id(id);
        if (s && same_addr(&s->host, &from)) close_session(s, "host left");
    } else if (!strncmp(text, "LAP 1 ", 6)) {
        /* Only someone hosting a race here can report its laps. */
        if (find_session_by_host(&from) && take_lap(text + 6, 0) && g_have_peer)
            reply_text(&g_peer, "PLAP %s", text + 6);
    } else if (!strncmp(text, "PLAP ", 5)) {
        if (from_peer(&from)) take_lap(text + 5, 1);
    } else if (!strcmp(text, "PSYNC")) {
        if (from_peer(&from) && g_sync_track < 0) {
            g_sync_track = 0;
            g_sync_rec = 0;
        }
    } else if (!strcmp(text, "LIST")) {
        cmd_list(&from);
    } else if (!strcmp(text, "PING")) {
        reply_text(&from, "PONG");
    }
}

static void handle_session(Session *s)
{
    unsigned char buf[BUF_SIZE];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    ssize_t len = recvfrom(s->fd, buf + 5, sizeof buf - 5, 0, (struct sockaddr *)&from, &fl);
    if (len <= 0) return;

    Client *c = NULL, *freec = NULL;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s->clients[i].used && same_addr(&s->clients[i].addr, &from)) { c = &s->clients[i]; break; }
        if (!s->clients[i].used && !freec) freec = &s->clients[i];
    }
    if (!c) {
        if (!freec) return; /* session full - drop */
        c = freec;
        c->used = 1;
        c->addr = from;
        c->cid = s->next_cid++;
        logf_("session %u: client %u is %s", s->id, c->cid, addr_str(&from));
    }
    c->last_seen = time(NULL);

    uint32_t ncid = htonl(c->cid);
    buf[0] = MSG_DATA;
    memcpy(buf + 1, &ncid, 4);
    send_to(g_ctl, &s->host, buf, (size_t)len + 5);
}

static void expire(void)
{
    time_t now = time(NULL);
    for (int i = 0; i < g_nsessions; i++) {
        Session *s = &g_sessions[i];
        if (!s->used) continue;
        if (now - s->last_seen > SESSION_TIMEOUT_S) {
            close_session(s, "host timed out");
            continue;
        }
        for (int k = 0; k < MAX_CLIENTS; k++) {
            Client *c = &s->clients[k];
            if (!c->used || now - c->last_seen <= CLIENT_TIMEOUT_S) continue;
            unsigned char msg[5];
            uint32_t ncid = htonl(c->cid);
            msg[0] = MSG_GONE;
            memcpy(msg + 1, &ncid, 4);
            send_to(g_ctl, &s->host, msg, sizeof msg);
            logf_("session %u: client %u timed out", s->id, c->cid);
            c->used = 0;
        }
    }
}

/* ------------------------------------------------------------------ HTTP */

typedef struct { char *p; size_t len, cap; } Str;

static void sputs(Str *s, const char *t)
{
    size_t n = strlen(t);
    if (s->len + n + 1 > s->cap) {
        size_t cap = (s->cap ? s->cap : 4096);
        while (cap < s->len + n + 1) cap *= 2;
        char *p = realloc(s->p, cap);
        if (!p) return;
        s->p = p;
        s->cap = cap;
    }
    memcpy(s->p + s->len, t, n + 1);
    s->len += n;
}

static void sprintf_(Str *s, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    sputs(s, buf);
}

static void sjson(Str *s, const char *t)
{
    sputs(s, "\"");
    for (; *t; t++) {
        char c[8];
        if (*t == '"' || *t == '\\') { c[0] = '\\'; c[1] = *t; c[2] = 0; }
        else if ((unsigned char)*t < 0x20) snprintf(c, sizeof c, "\\u%04x", (unsigned char)*t);
        else { c[0] = *t; c[1] = 0; }
        sputs(s, c);
    }
    sputs(s, "\"");
}

static void doc_records(Str *s)
{
    sputs(s, "{\n  \"tracks\": [\n");
    for (int i = 0; i < g_ntracks; i++) {
        TrackRecs *t = &g_tracks[i];
        sputs(s, "    { \"track\": ");
        sjson(s, t->track);
        sputs(s, ", \"category\": ");
        sjson(s, t->category);
        sputs(s, ",\n      \"times\": [\n");
        for (int j = 0; j < t->n; j++) {
            sprintf_(s, "        { \"rank\": %d, \"name\": ", j + 1);
            sjson(s, t->times[j].name);
            sputs(s, ", \"car\": ");
            sjson(s, t->times[j].car);
            sprintf_(s, ", \"time\": %.3f }%s\n", t->times[j].time, j + 1 < t->n ? "," : "");
        }
        sprintf_(s, "      ]\n    }%s\n", i + 1 < g_ntracks ? "," : "");
    }
    sputs(s, "  ]\n}\n");
}

static void doc_sessions(Str *s)
{
    int first = 1;
    sputs(s, "{\n  \"sessions\": [\n");
    for (int i = 0; i < g_nsessions; i++) {
        Session *se = &g_sessions[i];
        if (!se->used) continue;
        sprintf_(s, "%s    { \"id\": %u, \"port\": %d, \"players\": %d, \"maxPlayers\": %d, \"track\": ",
                 first ? "" : ",\n", se->id, se->port, se->players, se->max_players);
        sjson(s, se->track);
        sputs(s, ", \"host\": ");
        sjson(s, se->name);
        sputs(s, " }");
        first = 0;
    }
    sputs(s, "\n  ]\n}\n");
}

static void http_answer(HttpClient *c)
{
    char path[128] = "/";
    const char *sp = strchr(c->buf, ' ');
    if (sp) {
        const char *e = strchr(sp + 1, ' ');
        size_t n = e ? (size_t)(e - sp - 1) : strcspn(sp + 1, "\r\n");
        if (n >= sizeof path) n = sizeof path - 1;
        memcpy(path, sp + 1, n);
        path[n] = 0;
    }
    char *q = strchr(path, '?');
    if (q) *q = 0;

    Str body = { 0 };
    const char *status = "200 OK";
    if (!strcmp(path, "/records")) doc_records(&body);
    else if (!strcmp(path, "/sessions") || !strcmp(path, "/")) doc_sessions(&body);
    else { status = "404 Not Found"; sputs(&body, "{\"error\":\"not found\"}\n"); }

    char head[256];
    int hn = snprintf(head, sizeof head,
                      "HTTP/1.1 %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                      "Access-Control-Allow-Origin: *\r\nCache-Control: no-store\r\n"
                      "Connection: close\r\n\r\n", status, body.len);

    /* The answer is small enough for the socket buffer; the short timeout is
     * only there so a client that stops reading cannot hold up the relay. */
    struct timeval tv = { 0, 200000 };
    int fl = fcntl(c->fd, F_GETFL);
    fcntl(c->fd, F_SETFL, fl & ~O_NONBLOCK);
    setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    send(c->fd, head, (size_t)hn, MSG_NOSIGNAL);
    if (body.p) send(c->fd, body.p, body.len, MSG_NOSIGNAL);
    free(body.p);
}

static void http_close(HttpClient *c)
{
    close(c->fd);
    c->fd = -1;
}

static void http_accept(void)
{
    for (int k = 0; k < 8; k++) {
        int fd = accept(g_http, NULL, NULL);
        if (fd < 0) return;
        HttpClient *c = NULL;
        for (int i = 0; i < MAX_HTTP; i++)
            if (g_httpc[i].fd < 0) { c = &g_httpc[i]; break; }
        if (!c) { close(fd); continue; }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        c->fd = fd;
        c->len = 0;
        c->deadline = time(NULL) + HTTP_TIMEOUT_S;
    }
}

static void http_read(HttpClient *c)
{
    ssize_t n = recv(c->fd, c->buf + c->len, sizeof c->buf - 1 - c->len, 0);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        http_close(c);
        return;
    }
    c->len += (size_t)n;
    c->buf[c->len] = 0;
    if (strstr(c->buf, "\r\n\r\n") || strstr(c->buf, "\n\n") || c->len >= sizeof c->buf - 1) {
        http_answer(c);
        http_close(c);
    }
}

static int open_http(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

/* ------------------------------------------------------------------ main */

static int parse_peer(const char *arg, int default_port)
{
    char host[64];
    int port = default_port;
    snprintf(host, sizeof host, "%s", arg);
    char *colon = strchr(host, ':');
    if (colon) { *colon = 0; port = atoi(colon + 1); }
    memset(&g_peer, 0, sizeof g_peer);
    g_peer.sin_family = AF_INET;
    g_peer.sin_port = htons((uint16_t)port);
    if (!inet_aton(host, &g_peer.sin_addr)) return 0;
    g_have_peer = 1;
    return 1;
}

int main(int argc, char **argv)
{
    int ctl_port = 28500, http_port = 28600;
    const char *dir = ".", *peer = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) ctl_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) g_base_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) g_nsessions = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) http_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "-P") && i + 1 < argc) peer = argv[++i];
        else {
            fprintf(stderr, "usage: %s [-p control_port] [-b session_base_port] [-n max_sessions]\n"
                            "          [-w stats_port (0 = off)] [-d data_dir] [-P peer_ip[:port]]\n", argv[0]);
            return 2;
        }
    }
    if (g_nsessions < 1 || g_nsessions > MAX_SESSIONS_CAP) g_nsessions = 64;
    if (peer && *peer && !parse_peer(peer, ctl_port)) {
        fprintf(stderr, "peer must be an IP address: %s\n", peer);
        return 2;
    }
    snprintf(g_records_path, sizeof g_records_path, "%s/records.tsv", dir);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    g_ctl = open_udp(ctl_port);
    if (g_ctl < 0) {
        fprintf(stderr, "cannot bind control port %d: %s\n", ctl_port, strerror(errno));
        return 1;
    }
    for (int i = 0; i < MAX_HTTP; i++) g_httpc[i].fd = -1;
    if (http_port > 0) {
        g_http = open_http(http_port);
        if (g_http < 0)
            logf_("cannot open stats port %d: %s - leaderboard not served", http_port, strerror(errno));
    }

    load_records();
    logf_("sd-adhoc-relay: control port %d, session ports %d-%d, stats port %d, peer %s",
          ctl_port, g_base_port, g_base_port + g_nsessions - 1, g_http >= 0 ? http_port : 0,
          g_have_peer ? addr_str(&g_peer) : "none");

    time_t last_tick = time(NULL), last_sync = time(NULL), last_save = time(NULL);
    /* Ask the peer for its whole table straight away: catches up after this
     * relay was down, and fills it the first time the two meet. */
    if (g_have_peer) reply_text(&g_peer, "PSYNC");
    struct pollfd pfd[MAX_SESSIONS_CAP + MAX_HTTP + 2];
    Session *owner[MAX_SESSIONS_CAP + MAX_HTTP + 2];
    HttpClient *hown[MAX_SESSIONS_CAP + MAX_HTTP + 2];
    while (g_running) {
        int n = 0;
        pfd[n].fd = g_ctl; pfd[n].events = POLLIN; owner[n] = NULL; hown[n++] = NULL;
        if (g_http >= 0) { pfd[n].fd = g_http; pfd[n].events = POLLIN; owner[n] = NULL; hown[n++] = NULL; }
        for (int i = 0; i < g_nsessions; i++) {
            if (!g_sessions[i].used) continue;
            pfd[n].fd = g_sessions[i].fd; pfd[n].events = POLLIN; owner[n] = &g_sessions[i]; hown[n++] = NULL;
        }
        for (int i = 0; i < MAX_HTTP; i++) {
            if (g_httpc[i].fd < 0) continue;
            pfd[n].fd = g_httpc[i].fd; pfd[n].events = POLLIN; owner[n] = NULL; hown[n++] = &g_httpc[i];
        }
        int r = poll(pfd, (nfds_t)n, g_sync_track >= 0 ? 10 : 1000);
        if (r < 0 && errno != EINTR) break;
        for (int i = 0; r > 0 && i < n; i++) {
            if (!(pfd[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            if (hown[i]) { if (hown[i]->fd == pfd[i].fd) http_read(hown[i]); }
            else if (owner[i]) { if (owner[i]->used && owner[i]->fd == pfd[i].fd) handle_session(owner[i]); }
            else if (pfd[i].fd == g_ctl) handle_control();
            else if (pfd[i].fd == g_http) http_accept();
        }

        pump_sync();

        time_t now = time(NULL);
        if (now != last_tick) {
            expire();
            for (int i = 0; i < MAX_HTTP; i++)
                if (g_httpc[i].fd >= 0 && now > g_httpc[i].deadline) http_close(&g_httpc[i]);
            last_tick = now;
        }
        if (g_have_peer && now - last_sync >= PEER_SYNC_S) {
            /* And now and then after that, in case a lap went missing. */
            reply_text(&g_peer, "PSYNC");
            last_sync = now;
        }
        if (g_records_dirty && now - last_save >= 2) {
            save_records();
            last_save = now;
        }
    }

    if (g_records_dirty) save_records();
    for (int i = 0; i < g_nsessions; i++)
        if (g_sessions[i].used) close_session(&g_sessions[i], "relay shutting down");
    close(g_ctl);
    logf_("sd-adhoc-relay stopped");
    return 0;
}
