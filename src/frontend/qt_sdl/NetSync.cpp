/*
    NetSync: input-delay netplay for melonDS (Mario Party DS style local-wireless games).
    See NetSync.h.
*/

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include <enet/enet.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#else
#include <arpa/inet.h>
#endif

#include <QCoreApplication>
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHostAddress>
#include <QIcon>
#include <QImage>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointF>
#include <QPolygonF>
#include <QPushButton>
#include <QRadialGradient>
#include <QRandomGenerator>
#include <QTableWidget>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>
#include <QVBoxLayout>
#include <QString>
#include <QDir>
#include <QFileInfo>
#include <QEventLoop>
#include <QRegularExpression>
#include <QStandardPaths>
#include <cmath>
#ifdef __APPLE__
#include <pthread.h>
#endif

#include "CLI.h"
#include "NetSync.h"
#include "Config.h"

using namespace melonDS;

namespace NetSync
{

const u32 kMagic = 0x434E5953; // SYNC
const u32 kVersion = 9;
const int kMaxPlayers = 4;
const int kRing = 4096;
const int kRedundantFrames = 16;

enum
{
    Msg_Hello = 1,
    Msg_Welcome,
    Msg_Reject,
    Msg_Go,
    Msg_InputBatch,
    Msg_Hash,
    Msg_Desync,
    Msg_Ready,
    Msg_ResendReq,
    Msg_LobbyStart,
    Msg_Ping,
    Msg_Pong,
    Msg_LobbyState,
};

struct InputRec
{
    u16 Key;
    u8 Touch;
    u8 Valid;
    u16 X, Y;
    u32 Frame;
};

static bool gConfigured = false;
static std::atomic<bool> gActive{false};
static bool gIsHost = false;
static bool gUseJIT = true;
static int gSlot = 0;
static int gNumPlayers = 2;
static int gDelay = 7;
static bool gAutoDelay = false;
static int gPort = 7090;
static std::string gAddr;
static std::string gLocalName;
static std::string gRoomName = "Mario Party Lobby";
static u64 gPassHash = 0;
static std::string gLobbyUrl = "https://melonds-lobby.crackobdi.workers.dev";
static bool gPublishLobby = false;
static std::string gPlayerNames[kMaxPlayers];
static std::vector<u8> gPlayerAvatars[kMaxPlayers];
static std::vector<u8> gLocalAvatar;
static bool gPlayerConnected[kMaxPlayers] = {};
static u8 gPlayerArch[kMaxPlayers] = {};
static bool gJitOverridden = false;
static int gPeerPingMs[kMaxPlayers] = {-1, -1, -1, -1};

static u8 LocalArch()
{
#if defined(__x86_64__) || defined(_M_X64)
    return 1;
#elif defined(__aarch64__) || defined(_M_ARM64)
    return 2;
#else
    return 0;
#endif
}

static const char* ArchName(u8 a)
{
    switch (a) { case 1: return "x86_64"; case 2: return "arm64"; default: return "other"; }
}
static std::atomic<int> gLagAlertSlot{-1};
static int gSimLossPct = 0;

static std::string gCliMode;
static std::string gCliAddr;
static int gCliPort = -1;
static std::string gCliPlayers;
static int gCliDelay = -1;
static std::string gCliName;
static std::string gCliRoom;
static std::string gCliRoomId;
static std::string gCliPass;
static std::string gCliLobby;
static bool gCliPublic = false;

struct ScriptStep { u32 f0, f1; u32 key; u8 touch; u16 x, y; };
static std::vector<ScriptStep> gTestScript;

static ENetHost* gHost = nullptr;
static ENetPeer* gHostPeer = nullptr;              // client: connection to host
static ENetPeer* gClientPeers[kMaxPlayers] = {};   // host: connection per slot
static std::mutex gNetMutex;
static std::thread gNetThread;
static std::atomic<bool> gNetRun{false};

static std::mutex gInMutex;
static std::condition_variable gInCV;
static InputRec gInputs[kMaxPlayers][kRing];
static InputRec gRecentSent[kMaxPlayers][kRing];   // host caches all players; clients cache local slot
static u32 gLastFrameSent[kMaxPlayers] = {};
static std::atomic<bool> gGo{false};
static std::atomic<int> gReadyCount{0};
static std::atomic<bool> gPeerLost{false};

static std::vector<u8> gHostSave;

static std::mutex gHashMutex;
static std::map<u64, u32> gHostHashes;                   // key = (inst<<32)|frame -> hash
static std::map<u64, u32> gClientHashes[kMaxPlayers];    // per client slot: key = (inst<<32)|frame -> hash
static std::atomic<u32> gDesyncCount{0};
static std::atomic<u32> gLastDesyncFrame{0};

static std::unique_ptr<QDialog> gSyncDlg;
static QLabel* gSyncStatusLbl = nullptr;
static std::atomic<bool> gDlgCanceled{false};

namespace
{

QIcon makePlayerAvatarIcon(const std::vector<u8>& pngData, const std::string&, int)
{
    if (!pngData.empty())
    {
        QImage img;
        if (img.loadFromData(pngData.data(), (int)pngData.size(), "PNG") && !img.isNull())
        {
            QPixmap pm(28, 28);
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing, true);
            QPainterPath path;
            path.addRoundedRect(0, 0, 28, 28, 5, 5);
            p.setClipPath(path);
            p.drawImage(QRect(0, 0, 28, 28), img.scaled(28, 28, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
            return QIcon(pm);
        }
    }
    return QIcon();
}

} // namespace

void FinishLoading()
{
    gSyncStatusLbl = nullptr;
    if (gSyncDlg)
    {
        gSyncDlg->close();
        gSyncDlg.reset();
    }
}

static FILE* gLog = nullptr;
static std::mutex gLogMutex;

static u64 gRomHash = 0, gBios7Hash = 0, gBios9Hash = 0, gFwHash = 0;


void Log(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lk(gLogMutex);
    if (!gLog)
    {
        QString fname = gIsHost
            ? QString("netsync.log")
            : QString("netsync_p%1.log").arg(QCoreApplication::applicationPid());
        QString baseDir = QCoreApplication::applicationDirPath();
#ifdef __APPLE__
        {
            QDir d(baseDir);
            if (d.cd("..") && d.cd("..") && d.dirName().endsWith(".app") && d.cd(".."))
                baseDir = d.absolutePath();
        }
#endif
        QString path = baseDir + QDir::separator() + fname;
        gLog = fopen(path.toStdString().c_str(), "w");
        if (!gLog)
        {
            QString fallbackDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
            QDir().mkpath(fallbackDir);
            path = fallbackDir + QDir::separator() + fname;
            gLog = fopen(path.toStdString().c_str(), "w");
        }
        if (!gLog) return;
    }
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    fprintf(gLog, "[%lld] ", (long long)now);
    va_list args;
    va_start(args, fmt);
    vfprintf(gLog, fmt, args);
    va_end(args);
    fflush(gLog);
}

static u64 HashFile(const std::string& path, std::vector<u8>* keep = nullptr)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0;
    std::vector<u8> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    u64 h = 1469598103934665603ULL;
    for (u8 b : data) { h ^= b; h *= 1099511628211ULL; }
    if (keep) *keep = std::move(data);
    return h ^ (u64)data.size();
}

static std::string ResolvePath(const std::string& p)
{
    if (p.empty()) return p;
    QFileInfo fi(QString::fromStdString(p));
    if (fi.isAbsolute()) return p;
    QString qp = QString::fromStdString(p);
    QString appPath = QCoreApplication::applicationDirPath() + QDir::separator() + qp;
    if (QFileInfo::exists(appPath)) return appPath.toStdString();
    QString subPath = QCoreApplication::applicationDirPath() + "/BIOS_and_ROMs/" + qp;
    if (QFileInfo::exists(subPath)) return subPath.toStdString();
    QString parentPath = QCoreApplication::applicationDirPath() + "/../" + qp;
    if (QFileInfo::exists(parentPath)) return QFileInfo(parentPath).canonicalFilePath().toStdString();
    QString parentSubPath = QCoreApplication::applicationDirPath() + "/../BIOS_and_ROMs/" + qp;
    if (QFileInfo::exists(parentSubPath)) return QFileInfo(parentSubPath).canonicalFilePath().toStdString();
#ifdef __APPLE__
    QString bundleAdjacent = QCoreApplication::applicationDirPath() + "/../../.." + QDir::separator() + qp;
    if (QFileInfo::exists(bundleAdjacent)) return QFileInfo(bundleAdjacent).canonicalFilePath().toStdString();
    QString bundleSub = QCoreApplication::applicationDirPath() + "/../../../BIOS_and_ROMs/" + qp;
    if (QFileInfo::exists(bundleSub)) return QFileInfo(bundleSub).canonicalFilePath().toStdString();
#endif
    QString cfgPath = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QDir::separator() + qp;
    if (QFileInfo::exists(cfgPath)) return cfgPath.toStdString();
    return appPath.toStdString();
}

bool Configured() { return gConfigured; }
bool Active() { return gActive.load(); }
bool Aborted() { return gPeerLost.load(); }
bool IsHost() { return gIsHost; }
bool UseJIT() { return gUseJIT; }
int LocalSlot() { return gSlot; }
int NumPlayers() { return gNumPlayers; }
int Delay() { return gDelay; }
std::string PlayerName(int slot) { return (slot >= 0 && slot < kMaxPlayers) ? gPlayerNames[slot] : std::string(); }
int ConsumeLagSlot() { return gLagAlertSlot.exchange(-1); }

static u64 HashPassword(const std::string& pass)
{
    if (pass.empty()) return 0;
    u64 h = 1469598103934665603ULL ^ 0x4D504453ULL;
    for (u8 c : pass) { h ^= c; h *= 1099511628211ULL; }
    return h ? h : 1;
}

void ApplyCLIOptions(const CLI::CommandLineOptions& opts)
{
    if (!opts.netsyncMode.has_value() || opts.netsyncMode->isEmpty()) return;
    gConfigured = true;
    gCliMode = opts.netsyncMode->toStdString();
    if (opts.netsyncAddr.has_value()) gCliAddr = opts.netsyncAddr->toStdString();
    if (opts.netsyncPort.has_value()) gCliPort = *opts.netsyncPort;
    if (opts.netsyncPlayers.has_value()) gCliPlayers = opts.netsyncPlayers->toStdString();
    if (opts.netsyncDelay.has_value()) gCliDelay = *opts.netsyncDelay;
    if (opts.netsyncName.has_value()) gCliName = opts.netsyncName->toUtf8().toStdString();
    if (opts.netsyncRoom.has_value()) gCliRoom = opts.netsyncRoom->toUtf8().toStdString();
    if (opts.netsyncRoomId.has_value()) gCliRoomId = opts.netsyncRoomId->toStdString();
    if (opts.netsyncPass.has_value()) gCliPass = opts.netsyncPass->toUtf8().toStdString();
    if (opts.netsyncLobbyUrl.has_value()) gCliLobby = opts.netsyncLobbyUrl->toStdString();
    gCliPublic = opts.netsyncPublic;
}

static const char kCrockford[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
static const u64 kRoomMask = 0x5A3C96E1B472D8ULL;

std::string EncodeRoomID(u32 ipHostOrder, u16 port)
{
    u8 bytes[7] = {
        (u8)((ipHostOrder >> 24) & 0xFF), (u8)((ipHostOrder >> 16) & 0xFF),
        (u8)((ipHostOrder >> 8) & 0xFF),  (u8)(ipHostOrder & 0xFF),
        (u8)((port >> 8) & 0xFF),         (u8)(port & 0xFF),
        0
    };
    u8 crc = 0x5D;
    for (int i = 0; i < 6; i++) crc = (u8)((crc * 31) ^ bytes[i]);
    bytes[6] = crc;

    u64 v = 0;
    for (int i = 0; i < 7; i++) v = (v << 8) | bytes[i];
    v ^= kRoomMask;

    char chars[12];
    for (int i = 11; i >= 0; i--)
    {
        chars[i] = kCrockford[v & 31];
        v >>= 5;
    }
    std::string out = "MP-";
    out.append(chars, 4); out.push_back('-');
    out.append(chars + 4, 4); out.push_back('-');
    out.append(chars + 8, 4);
    return out;
}

bool DecodeRoomID(const std::string& text, std::string& outIp, u16& outPort)
{
    std::string clean;
    for (char ch : text)
    {
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        if (ch == 'I' || ch == 'L') ch = '1';
        if (ch == 'O') ch = '0';
        if (ch == '-' || ch == ' ' || ch == '\t') continue;
        clean.push_back(ch);
    }
    if (clean.size() == 14 && clean[0] == 'M' && clean[1] == 'P')
        clean = clean.substr(2);
    if (clean.size() != 12) return false;

    u64 v = 0;
    for (char ch : clean)
    {
        const char* p = strchr(kCrockford, ch);
        if (!p) return false;
        v = (v << 5) | (u64)(p - kCrockford);
    }
    v ^= kRoomMask;

    u8 bytes[7];
    for (int i = 6; i >= 0; i--) { bytes[i] = (u8)(v & 0xFF); v >>= 8; }
    u8 crc = 0x5D;
    for (int i = 0; i < 6; i++) crc = (u8)((crc * 31) ^ bytes[i]);
    if (crc != bytes[6]) return false;

    outPort = (u16)((bytes[4] << 8) | bytes[5]);
    outIp = std::to_string(bytes[0]) + "." + std::to_string(bytes[1]) + "." +
            std::to_string(bytes[2]) + "." + std::to_string(bytes[3]);
    return true;
}

std::string BestLocalIP()
{
    std::string tailscale, lan, anyIp;
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces())
    {
        if (!(iface.flags() & QNetworkInterface::IsUp) || (iface.flags() & QNetworkInterface::IsLoopBack))
            continue;
        for (const QNetworkAddressEntry& entry : iface.addressEntries())
        {
            QHostAddress ha = entry.ip();
            if (ha.protocol() != QAbstractSocket::IPv4Protocol || ha.isLoopback()) continue;
            u32 ip = ha.toIPv4Address();
            u8 a = (u8)(ip >> 24), b = (u8)((ip >> 16) & 0xFF);
            std::string s = ha.toString().toStdString();
            if (a == 100 && b >= 64 && b <= 127 && tailscale.empty()) tailscale = s;
            else if ((a == 192 && b == 168) || a == 10 || (a == 172 && b >= 16 && b <= 31)) { if (lan.empty()) lan = s; }
            else if (anyIp.empty()) anyIp = s;
        }
    }
    if (!lan.empty()) return lan;
    if (!tailscale.empty()) return tailscale;
    if (!anyIp.empty()) return anyIp;
    return "127.0.0.1";
}

bool DiscoverPublicEndpoint(u16 localUdpPort, std::string& outIp, u16& outMappedPort, std::string& outNatNote, u16* outLocalPort)
{
    outIp = BestLocalIP();
    outMappedPort = localUdpPort;
    outNatNote = "Lokal / LAN";

    struct StunHost { const char* host; u16 port; };
    const StunHost servers[] = { {"stun.l.google.com", 19302}, {"stun.cloudflare.com", 3478}, {"stun1.l.google.com", 19302} };

    for (const auto& srv : servers)
    {
        QUdpSocket sock;
        if (!sock.bind(QHostAddress::AnyIPv4, localUdpPort, QAbstractSocket::ShareAddress | QAbstractSocket::ReuseAddressHint))
        {
            if (!sock.bind(QHostAddress::AnyIPv4, 0)) continue;
        }
        u16 boundPort = sock.localPort();
        u8 req[20] = {
            0x00, 0x01, 0x00, 0x00,
            0x21, 0x12, 0xA4, 0x42,
            0x4D, 0x50, 0x44, 0x53, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
        };
        ENetAddress saddr{};
        if (enet_address_set_host(&saddr, srv.host) != 0) continue;
        u32 sip = ntohl(saddr.host);
        sock.writeDatagram((const char*)req, sizeof(req), QHostAddress(sip), srv.port);
        if (!sock.waitForReadyRead(800)) continue;

        QByteArray resp;
        resp.resize((int)sock.pendingDatagramSize());
        sock.readDatagram(resp.data(), resp.size());
        if (resp.size() < 20) continue;
        const u8* p = (const u8*)resp.constData();
        if (p[0] != 0x01 || p[1] != 0x01) continue;
        int pos = 20;
        while (pos + 4 <= resp.size())
        {
            u16 atype = (u16)((p[pos] << 8) | p[pos + 1]);
            u16 alen = (u16)((p[pos + 2] << 8) | p[pos + 3]);
            pos += 4;
            if (pos + alen > resp.size()) break;
            if ((atype == 0x0020 || atype == 0x0001) && alen >= 8 && p[pos + 1] == 0x01)
            {
                u16 port = (u16)((p[pos + 2] << 8) | p[pos + 3]);
                u32 ip = ((u32)p[pos + 4] << 24) | ((u32)p[pos + 5] << 16) | ((u32)p[pos + 6] << 8) | p[pos + 7];
                if (atype == 0x0020) { port ^= 0x2112; ip ^= 0x2112A442U; }
                outMappedPort = port;
                outIp = QHostAddress(ip).toString().toStdString();
                if (outLocalPort) *outLocalPort = boundPort;
                outNatNote = "Online-Lobby aktiv";
                return true;
            }
            pos += (alen + 3) & ~3;
        }
    }
    return false;
}
const std::vector<u8>& HostSave() { return gHostSave; }


// ---------- packing helpers ----------

struct Writer
{
    std::vector<u8> b;
    void u8_(u8 v) { b.push_back(v); }
    void u16_(u16 v) { b.push_back(v & 0xFF); b.push_back(v >> 8); }
    void u32_(u32 v) { for (int i = 0; i < 4; i++) b.push_back((v >> (8*i)) & 0xFF); }
    void u64_(u64 v) { for (int i = 0; i < 8; i++) b.push_back((v >> (8*i)) & 0xFF); }
    void bytes(const u8* d, size_t n) { b.insert(b.end(), d, d + n); }
};

struct Reader
{
    const u8* p; size_t n; size_t pos = 0; bool ok = true;
    Reader(const u8* d, size_t len) : p(d), n(len) {}
    bool has(size_t k) const { return pos + k <= n; }
    bool need(size_t k) { if (pos + k > n) { ok = false; return false; } return true; }
    u8 u8_() { if (!need(1)) return 0; return p[pos++]; }
    u16 u16_() { if (!need(2)) return 0; u16 v = p[pos] | (p[pos+1] << 8); pos += 2; return v; }
    u32 u32_() { if (!need(4)) return 0; u32 v = 0; for (int i = 0; i < 4; i++) v |= (u32)p[pos+i] << (8*i); pos += 4; return v; }
    u64 u64_() { if (!need(8)) return 0; u64 v = 0; for (int i = 0; i < 8; i++) v |= (u64)p[pos+i] << (8*i); pos += 8; return v; }
};

// caller must hold gNetMutex
static void SendTo(ENetPeer* peer, const Writer& w, bool reliable = true)
{
    if (!peer) return;
    if (!reliable && gSimLossPct > 0 && ((rand() % 100) < gSimLossPct))
        return;
    u32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;
    u8 chan = reliable ? 0 : 1;
    ENetPacket* pkt = enet_packet_create(w.b.data(), w.b.size(), flags);
    enet_peer_send(peer, chan, pkt);
}

static void DisableThrottle(ENetPeer* peer)
{
    if (!peer) return;
    enet_peer_throttle_configure(peer, ENET_PEER_PACKET_THROTTLE_INTERVAL, 0, 0);
    peer->packetThrottle = ENET_PEER_PACKET_THROTTLE_SCALE;
    peer->packetThrottleLimit = ENET_PEER_PACKET_THROTTLE_SCALE;
    enet_peer_timeout(peer, 0, 8000, 12000);
}


// ---------- input store ----------

static bool StoreInput(int inst, u32 frame, u16 key, u8 touch, u16 x, u16 y)
{
    if (inst < 0 || inst >= kMaxPlayers) return false;
    bool added = false;
    {
        std::lock_guard<std::mutex> lk(gInMutex);
        InputRec& c = gRecentSent[inst][frame % kRing];
        if (!c.Valid || c.Frame < frame)
        {
            c.Key = key; c.Touch = touch; c.X = x; c.Y = y;
            c.Frame = frame; c.Valid = 1;
            if (frame > gLastFrameSent[inst]) gLastFrameSent[inst] = frame;
        }

        InputRec& r = gInputs[inst][frame % kRing];
        if (!r.Valid || r.Frame < frame)
        {
            r.Key = key; r.Touch = touch; r.X = x; r.Y = y;
            r.Frame = frame; r.Valid = 1;
            added = true;
        }
    }
    if (added) gInCV.notify_all();
    return added;
}

static Writer BuildBatchPacket(int inst, u32 upToFrame, int count)
{
    Writer w;
    std::vector<InputRec> recs;
    recs.reserve(count);
    {
        std::lock_guard<std::mutex> lk(gInMutex);
        u32 start = (upToFrame + 1 > (u32)count) ? (upToFrame + 1 - (u32)count) : 0;
        for (u32 f = start; f <= upToFrame; f++)
        {
            const InputRec& r = gRecentSent[inst][f % kRing];
            if (r.Valid && r.Frame == f)
                recs.push_back(r);
        }
    }
    w.u8_(Msg_InputBatch);
    w.u8_((u8)inst);
    w.u8_((u8)recs.size());
    for (const auto& r : recs)
    {
        w.u32_(r.Frame);
        w.u16_(r.Key);
        w.u8_(r.Touch);
        w.u16_(r.X);
        w.u16_(r.Y);
    }
    return w;
}

static void BroadcastInputBatch(int inst, u32 upToFrame, ENetPeer* except, bool reliable = false)
{
    Writer w = BuildBatchPacket(inst, upToFrame, kRedundantFrames);

    std::lock_guard<std::mutex> lk(gNetMutex);
    if (gIsHost)
    {
        for (int i = 1; i < kMaxPlayers; i++)
            if (gClientPeers[i] && gClientPeers[i] != except)
                SendTo(gClientPeers[i], w, reliable);
    }
    else
    {
        SendTo(gHostPeer, w, reliable);
    }
    enet_host_flush(gHost);
}


// ---------- desync hash comparison (host) ----------

static void CompareHashPairLocked(int slot, int inst, u32 frame, u32 hostHash, u32 clientHash)
{
    if (hostHash != clientHash)
    {
        gDesyncCount++;
        gLastDesyncFrame = frame;
        Log("DESYNC slot %d inst %d frame %u (host %08X / client %08X)\n", slot, inst, frame, hostHash, clientHash);
        Writer w; w.u8_(Msg_Desync); w.u8_((u8)inst); w.u32_(frame);
        std::lock_guard<std::mutex> nl(gNetMutex);
        for (int i = 1; i < kMaxPlayers; i++)
            if (gClientPeers[i]) SendTo(gClientPeers[i], w, true);
    }
    else if ((frame % 600) == 0)
    {
        Log("sync ok slot %d inst %d frame %u (%08X)\n", slot, inst, frame, hostHash);
    }
}

static void CheckClientHash(int slot, int inst, u32 frame, u32 hash)
{
    if (!gIsHost || slot < 1 || slot >= kMaxPlayers) return;
    u64 key = ((u64)inst << 32) | frame;
    std::lock_guard<std::mutex> lk(gHashMutex);
    auto it = gHostHashes.find(key);
    if (it != gHostHashes.end())
    {
        CompareHashPairLocked(slot, inst, frame, it->second, hash);
        return;
    }
    gClientHashes[slot][key] = hash;
    if (gClientHashes[slot].size() > 1024)
        gClientHashes[slot].erase(gClientHashes[slot].begin());
}


// ---------- network thread ----------

static int FindClientSlot(ENetPeer* peer)
{
    for (int i = 1; i < kMaxPlayers; i++)
        if (gClientPeers[i] == peer) return i;
    return -1;
}

static void HandlePacket(ENetPeer* peer, const u8* data, size_t len)
{
    Reader r(data, len);
    u8 type = r.u8_();
    switch (type)
    {
    case Msg_InputBatch:
        {
            int inst = r.u8_();
            int count = r.u8_();
            if (!r.ok || inst < 0 || inst >= gNumPlayers) break;
            u32 maxFrame = 0;
            bool anyNew = false;
            for (int i = 0; i < count; i++)
            {
                u32 frame = r.u32_(); u16 key = r.u16_(); u8 touch = r.u8_();
                u16 x = r.u16_(); u16 y = r.u16_();
                if (!r.ok) break;
                if (StoreInput(inst, frame, key, touch, x, y))
                {
                    anyNew = true;
                    if (frame > maxFrame) maxFrame = frame;
                }
            }
            if (gIsHost && anyNew)
                BroadcastInputBatch(inst, maxFrame, peer, false); // relay redundantly to other clients
        }
        break;

    case Msg_ResendReq:
        {
            int inst = r.u8_();
            u32 frame = r.u32_();
            if (!r.ok || inst < 0 || inst >= gNumPlayers) break;
            u32 latest = 0;
            {
                std::lock_guard<std::mutex> lk(gInMutex);
                latest = std::max(frame, gLastFrameSent[inst]);
            }
            Writer w = BuildBatchPacket(inst, std::min(latest, frame + (u32)kRedundantFrames - 1), kRedundantFrames);
            std::lock_guard<std::mutex> nl(gNetMutex);
            SendTo(peer, w, true);
            enet_host_flush(gHost);
        }
        break;

    case Msg_Hash:
        {
            int inst = r.u8_(); u32 frame = r.u32_(); u32 hash = r.u32_();
            if (!r.ok || !gIsHost) break;
            int slot = FindClientSlot(peer);
            if (slot > 0)
                CheckClientHash(slot, inst, frame, hash);
        }
        break;

    case Msg_Desync:
        {
            int inst = r.u8_(); u32 frame = r.u32_();
            gDesyncCount++;
            gLastDesyncFrame = frame;
            Log("DESYNC reported by host: inst %d frame %u\n", inst, frame);
        }
        break;

    case Msg_Go:
        Log("GO received\n");
        gGo = true;
        gInCV.notify_all();
        break;

    case Msg_Ready:
        gReadyCount++;
        Log("READY from a client (%d)\n", gReadyCount.load());
        break;
    }
}

static void NetThreadFunc()
{
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    while (gNetRun)
    {
        ENetSocket sock = ENET_SOCKET_NULL;
        bool got = false;
        {
            std::lock_guard<std::mutex> lk(gNetMutex);
            if (gHost) sock = gHost->socket;
            ENetEvent ev;
            while (gHost && enet_host_service(gHost, &ev, 0) > 0)
            {
                got = true;
                if (ev.type == ENET_EVENT_TYPE_RECEIVE)
                {
                    // HandlePacket may need gNetMutex (relay) -> copy and handle outside the lock
                    std::vector<u8> buf(ev.packet->data, ev.packet->data + ev.packet->dataLength);
                    ENetPeer* peer = ev.peer;
                    enet_packet_destroy(ev.packet);
                    gNetMutex.unlock();
                    HandlePacket(peer, buf.data(), buf.size());
                    gNetMutex.lock();
                }
                else if (ev.type == ENET_EVENT_TYPE_DISCONNECT)
                {
                    if (ev.peer == gHostPeer) gHostPeer = nullptr;
                    for (int i = 1; i < kMaxPlayers; i++)
                        if (gClientPeers[i] == ev.peer) gClientPeers[i] = nullptr;
                    Log("peer disconnected\n");
                    gPeerLost = true;
                    gInCV.notify_all();
                }
                else if (ev.type == ENET_EVENT_TYPE_CONNECT)
                {
                    // late connections are not supported once the session runs
                    enet_peer_disconnect(ev.peer, 0);
                }
            }
        }
        if (!got)
        {
            if (sock != ENET_SOCKET_NULL)
            {
                enet_uint32 cond = ENET_SOCKET_WAIT_RECEIVE;
                enet_socket_wait(sock, &cond, 1);
            }
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}


// ---------- handshake ----------

static bool PumpUntil(std::function<bool()> canceled, std::function<bool(ENetEvent&)> onEvent, std::function<bool()> done, std::string& error)
{
    if (!QCoreApplication::instance())
    {
        for (;;)
        {
            if (canceled()) { error = "Canceled."; return false; }
            if (done()) { Log("pump done\n"); return true; }
            ENetEvent ev;
            int r;
            {
                std::lock_guard<std::mutex> lk(gNetMutex);
                r = enet_host_service(gHost, &ev, 10);
            }
            if (r > 0)
            {
                Log("pump event type=%d\n", (int)ev.type);
                if (!onEvent(ev)) return false;
                if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
            }
        }
    }

    QEventLoop loop;
    bool result = false;
    bool finished = false;
    QTimer timer;
    timer.setInterval(8);
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        if (finished) return;
        for (int iter = 0; iter < 16; iter++)
        {
            if (canceled()) { error = "Canceled."; finished = true; result = false; timer.stop(); loop.quit(); return; }
            if (done()) { Log("pump done\n"); finished = true; result = true; timer.stop(); loop.quit(); return; }

            ENetEvent ev;
            int r;
            {
                std::lock_guard<std::mutex> lk(gNetMutex);
                r = enet_host_service(gHost, &ev, 0);
            }
            if (r <= 0) break;
            Log("pump event type=%d\n", (int)ev.type);
            if (!onEvent(ev))
            {
                if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
                finished = true; result = false; timer.stop(); loop.quit();
                return;
            }
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
        }
        if (canceled()) { error = "Canceled."; finished = true; result = false; timer.stop(); loop.quit(); }
        else if (done()) { Log("pump done\n"); finished = true; result = true; timer.stop(); loop.quit(); }
    });
    timer.start();
    loop.exec();
    return result;
}

bool Start(const std::string& romPath, std::string& error)
{
    const char* envMode = getenv("MELONDS_NETSYNC");
    std::string mode = !gCliMode.empty() ? gCliMode : (envMode ? envMode : "");
    if (mode.empty()) return true;
    gConfigured = true;
    bool headless = (getenv("MELONDS_NETSYNC_HEADLESS") && *getenv("MELONDS_NETSYNC_HEADLESS"));

    gIsHost = (mode == "host");
    if (const char* sl = getenv("MELONDS_NETSYNC_SIM_LOSS_PCT")) gSimLossPct = std::clamp(atoi(sl), 0, 50);
    if (gCliPort > 0) gPort = gCliPort;
    if (const char* p = getenv("MELONDS_NETSYNC_PORT")) gPort = atoi(p);
    if (!gCliAddr.empty()) gAddr = gCliAddr;
    if (const char* a = getenv("MELONDS_NETSYNC_ADDR")) gAddr = a;
    if (!gCliRoom.empty()) gRoomName = gCliRoom;
    if (!gCliPass.empty()) gPassHash = HashPassword(gCliPass);
    if (const char* pw = getenv("MELONDS_NETSYNC_PASS"); pw && *pw) gPassHash = HashPassword(pw);
    if (!gCliLobby.empty()) gLobbyUrl = gCliLobby;
    if (const char* lu = getenv("MELONDS_NETSYNC_LOBBY"); lu && *lu) gLobbyUrl = lu;
    gPublishLobby = gCliPublic || (getenv("MELONDS_NETSYNC_PUBLIC") && atoi(getenv("MELONDS_NETSYNC_PUBLIC")) != 0);

    bool autoLobby = false;
    int maxPlayers = kMaxPlayers;
    int autoStartAt = 0;
    if (gIsHost)
    {
        const char* envP = getenv("MELONDS_NETSYNC_PLAYERS");
        std::string pStr = !gCliPlayers.empty() ? gCliPlayers : (envP ? envP : "auto");
        autoLobby = (pStr.empty() || pStr == "auto" || atoi(pStr.c_str()) == 0);
        if (autoLobby)
        {
            if (pStr.rfind("auto:", 0) == 0)
                maxPlayers = std::clamp(atoi(pStr.c_str() + 5), 2, kMaxPlayers);
            gNumPlayers = maxPlayers;
            if (const char* as = getenv("MELONDS_NETSYNC_AUTOSTART_AT")) autoStartAt = atoi(as);
        }
        else
        {
            gNumPlayers = std::clamp(atoi(pStr.c_str()), 2, kMaxPlayers);
            maxPlayers = gNumPlayers;
        }
        int reqDelay = gCliDelay;
        if (const char* d = getenv("MELONDS_NETSYNC_DELAY")) reqDelay = atoi(d);
        if (reqDelay <= 0)
        {
            gAutoDelay = true;
            gDelay = (gNumPlayers >= 3) ? 4 : 3;
        }
        else
        {
            gAutoDelay = false;
            gDelay = std::clamp(reqDelay, 1, 30);
        }
        if (const char* j = getenv("MELONDS_NETSYNC_JIT"))
        {
            gUseJIT = (atoi(j) != 0);
            gJitOverridden = true;
        }
        gPlayerArch[0] = LocalArch();
    }
    else
    {
        // Decode Room-ID ("MP-XXXX-XXXX-XXXX") or host:port automatically
        std::string decIp;
        u16 decPort = 0;
        if (DecodeRoomID(gAddr, decIp, decPort))
        {
            gAddr = decIp;
            if (decPort != 0) gPort = decPort;
        }
        else
        {
            size_t colon = gAddr.rfind(':');
            if (colon != std::string::npos && gAddr.find(':') == colon)
            {
                int cp = atoi(gAddr.c_str() + colon + 1);
                if (cp > 0 && cp <= 65535) gPort = cp;
                gAddr = gAddr.substr(0, colon);
            }
        }
    }

    const char* envNm = getenv("MELONDS_NETSYNC_NAME");
    std::string rawName = !gCliName.empty() ? gCliName : (envNm ? envNm : "");
    if (!rawName.empty())
    {
        QString qn = QString::fromUtf8(rawName.c_str()).trimmed().left(10);
        gLocalName = qn.toUtf8().toStdString();
    }
    else
    {
        Config::Table localCfg = Config::GetLocalTable(0);
        QString defName = localCfg.GetQString("Firmware.Username").trimmed().left(10);
        bool okName = false;
        QString chosen = QInputDialog::getText(nullptr, "Multiplayer - Nickname",
            "Enter your nickname (max. 10 characters):", QLineEdit::Normal, defName, &okName);
        if (!okName) { error = "Canceled."; return false; }
        chosen = chosen.trimmed().left(10);
        if (!chosen.isEmpty())
        {
            localCfg.SetQString("Firmware.Username", chosen);
            Config::Save();
        }
        gLocalName = chosen.toUtf8().toStdString();
    }

    Log("NetSync start: mode=%s port=%d addr=%s rom=%s jit=%d autoDelay=%d simLoss=%d\n",
        mode.c_str(), gPort, gAddr.c_str(), romPath.c_str(), gUseJIT ? 1 : 0, gAutoDelay ? 1 : 0, gSimLossPct);

    // hash the files that must be identical on every PC
    Config::Table cfg = Config::GetGlobalTable();
    gRomHash = HashFile(romPath);
    gBios7Hash = HashFile(ResolvePath(cfg.GetString("DS.BIOS7Path")));
    gBios9Hash = HashFile(ResolvePath(cfg.GetString("DS.BIOS9Path")));
    gFwHash = HashFile(ResolvePath(cfg.GetString("DS.FirmwarePath")));
    Log("hashes rom=%016llX b7=%016llX b9=%016llX fw=%016llX\n",
        (unsigned long long)gRomHash, (unsigned long long)gBios7Hash, (unsigned long long)gBios9Hash, (unsigned long long)gFwHash);
    if (!gRomHash) { error = "ROM not found: " + romPath; return false; }
    if (!gBios7Hash || !gBios9Hash || !gFwHash)
    {
        error = "Missing bios7.bin, bios9.bin, or firmware.bin.\n"
                "Please select your BIOS folder in the Multiplayer menu.";
        return false;
    }

    if (enet_initialize() != 0) { error = "Failed to initialize network (ENet)."; return false; }

    memset(gInputs, 0, sizeof(gInputs));
    memset(gRecentSent, 0, sizeof(gRecentSent));
    memset(gLastFrameSent, 0, sizeof(gLastFrameSent));
    for (int i = 0; i < kMaxPlayers; i++)
    {
        gPeerPingMs[i] = -1;
        gPlayerConnected[i] = false;
        gPlayerAvatars[i].clear();
    }
    gLocalAvatar.clear();
    if (!headless)
    {
        QString b64 = Config::GetLocalTable(0).GetQString("Netplay.AvatarData");
        if (!b64.isEmpty())
        {
            QByteArray raw = QByteArray::fromBase64(b64.toLatin1());
            if (raw.size() > 0 && raw.size() <= 16384)
                gLocalAvatar.assign((const u8*)raw.constData(), (const u8*)raw.constData() + raw.size());
        }
    }

    FinishLoading();
    gDlgCanceled = false;
    QTableWidget* playerTable = nullptr;
    QPushButton* startBtn = nullptr;
    QPushButton* cancelBtn = nullptr;
    QLabel* hostInfoLbl = nullptr;
    QWidget* hostPanel = nullptr;

    auto ensureSyncDialog = [&](bool showHostPanel)
    {
        if (headless) return;
        if (!gSyncDlg)
        {
            gSyncDlg = std::make_unique<QDialog>();
            gSyncDlg->setWindowTitle("Multiplayer");
            gSyncDlg->setMinimumWidth(480);

            auto* layout = new QVBoxLayout(gSyncDlg.get());

            gSyncStatusLbl = new QLabel("Initializing...", gSyncDlg.get());
            gSyncStatusLbl->setAlignment(Qt::AlignCenter);
            gSyncStatusLbl->setWordWrap(true);
            layout->addWidget(gSyncStatusLbl);

            if (showHostPanel)
            {
                hostPanel = new QWidget(gSyncDlg.get());
                auto* hv = new QVBoxLayout(hostPanel);
                hv->setContentsMargins(0, 0, 0, 0);

                hostInfoLbl = new QLabel(hostPanel);
                hostInfoLbl->setAlignment(Qt::AlignCenter);
                hv->addWidget(hostInfoLbl);

                auto* roomRow = new QHBoxLayout();
                roomRow->addWidget(new QLabel("Room ID:", hostPanel));
                auto* roomEdit = new QLineEdit(hostPanel);
                roomEdit->setObjectName("hostRoomEdit");
                roomEdit->setReadOnly(true);
                roomEdit->setAlignment(Qt::AlignCenter);
                auto* copyBtn = new QPushButton("Copy", hostPanel);
                QObject::connect(copyBtn, &QPushButton::clicked, [roomEdit]() {
                    QApplication::clipboard()->setText(roomEdit->text());
                });
                roomRow->addWidget(roomEdit, 1);
                roomRow->addWidget(copyBtn);
                hv->addLayout(roomRow);

                layout->addWidget(hostPanel);
            }

            playerTable = new QTableWidget(0, 5, gSyncDlg.get());
            playerTable->setHorizontalHeaderLabels({"", "#", "Player", "Ping", "Status"});
            playerTable->setIconSize(QSize(28, 28));
            playerTable->setColumnWidth(0, 38);
            playerTable->setColumnWidth(1, 28);
            playerTable->setColumnWidth(3, 68);
            playerTable->setColumnWidth(4, 72);
            playerTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
            playerTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
            playerTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
            playerTable->verticalHeader()->setVisible(false);
            playerTable->verticalHeader()->setDefaultSectionSize(34);
            playerTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
            playerTable->setSelectionMode(QAbstractItemView::NoSelection);
            playerTable->setMinimumHeight(174);
            playerTable->setMaximumHeight(184);
            if (!showHostPanel)
                playerTable->hide();
            layout->addWidget(playerTable);

            auto* btnRow = new QHBoxLayout();
            if (showHostPanel)
            {
                startBtn = new QPushButton("Waiting for Players...", gSyncDlg.get());
                startBtn->setEnabled(false);
                startBtn->setDefault(true);
                btnRow->addWidget(startBtn, 1);
            }
            cancelBtn = new QPushButton("Cancel", gSyncDlg.get());
            btnRow->addWidget(cancelBtn);
            layout->addLayout(btnRow);

            QObject::connect(cancelBtn, &QPushButton::clicked, []() { gDlgCanceled = true; });
            QObject::connect(gSyncDlg.get(), &QDialog::rejected, []() { gDlgCanceled = true; });
            gSyncDlg->adjustSize();
            gSyncDlg->show();
            gSyncDlg->raise();
            gSyncDlg->activateWindow();
            QCoreApplication::processEvents();
        }
    };

    auto refreshRosterTable = [&]()
    {
        if (headless || !playerTable) return;
        playerTable->setRowCount(0);
        for (int i = 0; i < kMaxPlayers; i++)
        {
            if (!gPlayerConnected[i]) continue;
            int row = playerTable->rowCount();
            playerTable->insertRow(row);
            auto* avItem = new QTableWidgetItem();
            avItem->setIcon(makePlayerAvatarIcon(gPlayerAvatars[i], gPlayerNames[i], i));
            playerTable->setItem(row, 0, avItem);
            playerTable->setItem(row, 1, new QTableWidgetItem(QString::number(i + 1)));
            playerTable->setItem(row, 2, new QTableWidgetItem(QString::fromUtf8(gPlayerNames[i].c_str())));
            QString pingStr = (i == 0) ? "Host" : (gPeerPingMs[i] >= 0 ? QString("%1 ms").arg(gPeerPingMs[i]) : "...");
            playerTable->setItem(row, 3, new QTableWidgetItem(pingStr));
            playerTable->setItem(row, 4, new QTableWidgetItem("Ready"));
        }
        if (playerTable->rowCount() > 0 && playerTable->isHidden())
        {
            playerTable->show();
            if (gSyncDlg) gSyncDlg->adjustSize();
        }
    };

    auto setSyncStatus = [&](const QString& text)
    {
        if (headless || !gSyncStatusLbl) return;
        gSyncStatusLbl->setText(text);
        QCoreApplication::processEvents();
    };

    if (gIsHost)
    {
        gPlayerNames[0] = gLocalName.empty() ? "Player 1" : gLocalName;
        gPlayerAvatars[0] = gLocalAvatar;
        gPlayerConnected[0] = true;
        ensureSyncDialog(autoLobby);
        setSyncStatus("Opening room & checking network...");

        // Short 4-digit customizable Room ID
        std::string roomId;
        for (char ch : gCliRoomId)
        {
            if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))
            {
                if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
                roomId.push_back(ch);
                if (roomId.size() >= 4) break;
            }
        }
        if (roomId.empty())
            roomId = std::to_string(QRandomGenerator::global()->bounded(1000, 10000));

        std::string pubIp = BestLocalIP();
        u16 pubPort = (u16)gPort;
        std::string natNote = "LAN Lobby Active";

        // 1. Run UPnP port forwarding BEFORE STUN so STUN sees the mapped port
        if (!headless && autoLobby)
        {
            QUdpSocket ssdp;
            ssdp.bind(QHostAddress::AnyIPv4, 0);
            const char* msearch =
                "M-SEARCH * HTTP/1.1\r\n"
                "HOST: 239.255.255.250:1900\r\n"
                "MAN: \"ssdp:discover\"\r\n"
                "MX: 1\r\n"
                "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n";
            ssdp.writeDatagram(msearch, QHostAddress("239.255.255.250"), 1900);
            if (ssdp.waitForReadyRead(650))
            {
                QByteArray r;
                r.resize((int)ssdp.pendingDatagramSize());
                ssdp.readDatagram(r.data(), r.size());
                QRegularExpression locRx("LOCATION:\\s*(http://[^\\r\\n]+)", QRegularExpression::CaseInsensitiveOption);
                auto m = locRx.match(QString::fromLatin1(r));
                if (m.hasMatch())
                {
                    QUrl descUrl(m.captured(1).trimmed());
                    QNetworkAccessManager upnpMgr;
                    QEventLoop el;
                    QNetworkReply* rep = upnpMgr.get(QNetworkRequest(descUrl));
                    QTimer::singleShot(900, &el, &QEventLoop::quit);
                    QObject::connect(rep, &QNetworkReply::finished, &el, &QEventLoop::quit);
                    el.exec();
                    if (rep->isFinished() && rep->error() == QNetworkReply::NoError)
                    {
                        QString xml = QString::fromUtf8(rep->readAll());
                        QRegularExpression ctrlRx("<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>.*?<controlURL>([^<]+)</controlURL>", QRegularExpression::DotMatchesEverythingOption);
                        auto cm = ctrlRx.match(xml);
                        if (!cm.hasMatch())
                        {
                            QRegularExpression ctrlRx2("<serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1</serviceType>.*?<controlURL>([^<]+)</controlURL>", QRegularExpression::DotMatchesEverythingOption);
                            cm = ctrlRx2.match(xml);
                        }
                        if (cm.hasMatch())
                        {
                            QUrl ctrlUrl = descUrl.resolved(QUrl(cm.captured(1).trimmed()));
                            QString localIp = QString::fromStdString(BestLocalIP());
                            QByteArray soap = QString(
                                "<?xml version=\"1.0\"?>"
                                "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
                                "<s:Body><u:AddPortMapping xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\">"
                                "<NewRemoteHost></NewRemoteHost><NewExternalPort>%1</NewExternalPort><NewProtocol>UDP</NewProtocol>"
                                "<NewInternalPort>%1</NewInternalPort><NewInternalClient>%2</NewInternalClient>"
                                "<NewEnabled>1</NewEnabled><NewPortMappingDescription>melonDS Netplay</NewPortMappingDescription>"
                                "<NewLeaseDuration>3600</NewLeaseDuration>"
                                "</u:AddPortMapping></s:Body></s:Envelope>").arg(gPort).arg(localIp).toUtf8();
                            QNetworkRequest sreq(ctrlUrl);
                            sreq.setHeader(QNetworkRequest::ContentTypeHeader, "text/xml; charset=\"utf-8\"");
                            sreq.setRawHeader("SOAPAction", "\"urn:schemas-upnp-org:service:WANIPConnection:1#AddPortMapping\"");
                            QNetworkReply* srep = upnpMgr.post(sreq, soap);
                            QEventLoop el2;
                            QTimer::singleShot(900, &el2, &QEventLoop::quit);
                            QObject::connect(srep, &QNetworkReply::finished, &el2, &QEventLoop::quit);
                            el2.exec();
                            if (srep->isFinished() && srep->error() == QNetworkReply::NoError)
                                natNote = "Online Lobby Active";
                            srep->deleteLater();
                        }
                    }
                    rep->deleteLater();
                }
            }
        }

        // 2. Discover public endpoint via STUN
        if (!headless && autoLobby)
        {
            std::string stunNote;
            if (DiscoverPublicEndpoint((u16)gPort, pubIp, pubPort, stunNote))
                natNote = "Online Lobby Active";
        }
        Log("host endpoint pub=%s:%u localPort=%d nat=%s\n", pubIp.c_str(), pubPort, gPort, natNote.c_str());

        ENetAddress addr;
        addr.host = ENET_HOST_ANY;
        addr.port = gPort;
        gHost = enet_host_create(&addr, kMaxPlayers, 2, 0, 0);
        if (!gHost) { FinishLoading(); error = "Could not open host socket (another melonDS instance may still be running)."; return false; }

        // host save data, sent to every client so all PCs start identically
        std::string sav = romPath;
        size_t dot = sav.find_last_of('.');
        if (dot != std::string::npos) sav = sav.substr(0, dot);
        sav += ".sav";
        HashFile(sav, &gHostSave);
        Log("host save %s: %zu bytes, roomId=%s\n", sav.c_str(), gHostSave.size(), roomId.c_str());

        int connected = 1;
        bool startRequested = false;

        QNetworkAccessManager* nam = nullptr;
        QUdpSocket* beaconSock = nullptr;
        QTimer* lobbyTimer = nullptr;

        struct PunchTarget
        {
            ENetAddress dst;
            std::chrono::steady_clock::time_point expires;
        };
        std::vector<PunchTarget> punchTargets;

        auto recommendedDelay = [&]() -> int
        {
            int top1 = -1, top2 = -1;
            for (int i = 1; i < kMaxPlayers; i++)
            {
                if (!gClientPeers[i] || gPeerPingMs[i] < 0) continue;
                int p = gPeerPingMs[i];
                if (p > top1) { top2 = top1; top1 = p; }
                else if (p > top2) { top2 = p; }
            }
            int minFrames = (connected >= 3) ? 4 : 2;
            if (top1 < 0) return std::max(gDelay, minFrames);
            // In 3-4P star relay, worst-case path is client1 -> host -> client2
            double oneWayMs = (connected >= 3 && top2 >= 0) ? ((top1 + top2) * 0.5) : (top1 * 0.5);
            int d = (int)std::ceil((oneWayMs + 22.0) / 16.667);
            return std::clamp(d, minFrames, 20);
        };

        auto updateLobbyUI = [&]()
        {
            if (autoLobby && !headless && playerTable)
            {
                if (gAutoDelay)
                    gDelay = recommendedDelay();
                refreshRosterTable();
                startBtn->setEnabled(connected >= 2);
                startBtn->setText(connected >= 2
                    ? QString("Start Game (%1/%2)").arg(connected).arg(maxPlayers)
                    : QString("Waiting for Players (%1/%2)").arg(connected).arg(maxPlayers));
                setSyncStatus(connected >= maxPlayers
                    ? "Room full - click Start Game to begin!"
                    : (connected >= 2
                        ? "Players ready - click Start Game to begin!"
                        : QString("Share Room ID \"%1\" with your friends").arg(QString::fromStdString(roomId))));
            }
            else if (!headless)
            {
                QStringList names;
                for (int i = 0; i < kMaxPlayers; i++)
                    if (gPlayerConnected[i])
                        names << QString("%1. %2").arg(i + 1).arg(QString::fromUtf8(gPlayerNames[i].c_str()));
                setSyncStatus(QString("Waiting for players (%1/%2)...\n%3")
                    .arg(connected).arg(gNumPlayers).arg(names.join("  |  ")));
            }
        };

        auto broadcastLobbyStateLocked = [&]()
        {
            if (!gHost) return;
            u8 mask = 0;
            for (int i = 0; i < kMaxPlayers; i++)
                if (gPlayerConnected[i]) mask |= (1 << i);
            Writer w;
            w.u8_(Msg_LobbyState);
            w.u8_(mask);
            w.u8_((u8)maxPlayers);
            for (int i = 0; i < kMaxPlayers; i++)
            {
                if (!(mask & (1 << i))) continue;
                w.u32_((u32)gPlayerNames[i].size());
                w.bytes((const u8*)gPlayerNames[i].data(), gPlayerNames[i].size());
                w.u16_((u16)(s16)gPeerPingMs[i]);
                u32 avLen = (u32)std::min<size_t>(gPlayerAvatars[i].size(), 16384);
                w.u32_(avLen);
                if (avLen > 0) w.bytes(gPlayerAvatars[i].data(), avLen);
            }
            for (int i = 1; i < kMaxPlayers; i++)
                if (gClientPeers[i]) SendTo(gClientPeers[i], w, true);
        };

        auto sendLobbyPingsLocked = [&]()
        {
            if (!gHost) return;
            u64 nowUs = (u64)std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            Writer w; w.u8_(Msg_Ping); w.u64_(nowUs);
            for (int i = 1; i < kMaxPlayers; i++)
                if (gClientPeers[i]) SendTo(gClientPeers[i], w, false);
        };

        auto sendLobbyPings = [&]()
        {
            std::lock_guard<std::mutex> lk(gNetMutex);
            sendLobbyPingsLocked();
        };

        auto sendPunchBursts = [&]()
        {
            std::lock_guard<std::mutex> lk(gNetMutex);
            if (!gHost || punchTargets.empty()) return;
            auto nowTp = std::chrono::steady_clock::now();
            punchTargets.erase(
                std::remove_if(punchTargets.begin(), punchTargets.end(),
                    [&](const PunchTarget& t) { return t.expires < nowTp; }),
                punchTargets.end());
            u8 punchByte[2] = {'P', 'N'};
            ENetBuffer buf;
            buf.data = punchByte;
            buf.dataLength = sizeof(punchByte);
            for (auto& pt : punchTargets)
                for (int k = 0; k < 2; k++)
                    enet_socket_send(gHost->socket, &pt.dst, &buf, 1);
        };

        auto pollPunches = [&](bool fetchCloud)
        {
            sendPunchBursts();
            if (!fetchCloud || headless || gLobbyUrl.empty() || !nam) return;
            QString base = QString::fromStdString(gLobbyUrl).trimmed();
            while (base.endsWith('/')) base.chop(1);
            QUrl purl(base + "/punches?roomId=" + QString::fromStdString(roomId));
            QNetworkReply* prep = nam->get(QNetworkRequest(purl));
            QObject::connect(prep, &QNetworkReply::finished, prep, [prep, &punchTargets]() {
                prep->deleteLater();
                if (prep->error() != QNetworkReply::NoError) return;
                std::lock_guard<std::mutex> lk(gNetMutex);
                if (!gHost) return;
                QJsonDocument pd = QJsonDocument::fromJson(prep->readAll());
                QJsonArray arr = pd.object().value("peers").toArray();
                Log("punches polled: %d peer(s)\n", (int)arr.size());
                auto expTp = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                for (const QJsonValue& v : arr)
                {
                    QJsonObject po = v.toObject();
                    std::string cip = po.value("ip").toString().toStdString();
                    int cport = po.value("port").toInt(0);
                    if (cip.empty() || cport <= 0 || cport > 65535) continue;
                    ENetAddress dst{};
                    if (enet_address_set_host(&dst, cip.c_str()) == 0)
                    {
                        dst.port = (enet_uint16)cport;
                        punchTargets.push_back({dst, expTp});
                        ENetBuffer buf;
                        u8 punchByte[2] = {'P', 'N'};
                        buf.data = punchByte;
                        buf.dataLength = sizeof(punchByte);
                        for (int k = 0; k < 3; k++)
                            enet_socket_send(gHost->socket, &dst, &buf, 1);
                    }
                }
            });
        };

        auto sendNatKeepalive = [&]()
        {
            std::lock_guard<std::mutex> lk(gNetMutex);
            if (!gHost) return;
            ENetAddress dst{};
            if (enet_address_set_host(&dst, "stun.cloudflare.com") == 0)
            {
                dst.port = 3478;
                u8 req[20] = {
                    0x00, 0x01, 0x00, 0x00,
                    0x21, 0x12, 0xA4, 0x42,
                    0x4D, 0x50, 0x44, 0x53, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
                };
                ENetBuffer buf;
                buf.data = req;
                buf.dataLength = sizeof(req);
                enet_socket_send(gHost->socket, &dst, &buf, 1);
            }
        };

        auto sendLobbyBeacon = [&](bool closing, bool sendCloud = true)
        {
            if (headless) return;
            QFileInfo romFi(QString::fromStdString(romPath));
            QString wireRoomName = QString::fromUtf8(gRoomName.c_str());
            if (!gPublishLobby && !wireRoomName.startsWith("__priv__:"))
                wireRoomName = "__priv__:" + wireRoomName;

            QJsonObject obj;
            obj["proto"] = "melonDS-Netplay";
            obj["roomId"] = QString::fromStdString(roomId);
            obj["roomName"] = wireRoomName;
            obj["hostName"] = QString::fromUtf8(gPlayerNames[0].c_str());
            obj["game"] = romFi.completeBaseName();
            obj["romHash"] = QString::number((qulonglong)gRomHash, 16);
            obj["addr"] = QString::fromStdString(pubIp);
            obj["port"] = (int)pubPort;
            obj["localPort"] = gPort;
            obj["players"] = closing ? 0 : connected;
            obj["maxPlayers"] = maxPlayers;
            obj["hasPassword"] = (gPassHash != 0);
            QByteArray payload = QJsonDocument(obj).toJson(QJsonDocument::Compact);

            if (!closing && beaconSock)
            {
                beaconSock->writeDatagram(payload, QHostAddress::Broadcast, kLanDiscoveryPort);
                beaconSock->writeDatagram(payload, QHostAddress::LocalHost, kLanDiscoveryPort);
                for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces())
                {
                    if (!(iface.flags() & QNetworkInterface::IsUp) || (iface.flags() & QNetworkInterface::IsLoopBack))
                        continue;
                    for (const QNetworkAddressEntry& entry : iface.addressEntries())
                    {
                        if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.broadcast().isNull())
                            beaconSock->writeDatagram(payload, entry.broadcast(), kLanDiscoveryPort);
                    }
                }
            }

            if (sendCloud && !gLobbyUrl.empty() && nam)
            {
                QString base = QString::fromStdString(gLobbyUrl).trimmed();
                while (base.endsWith('/')) base.chop(1);
                QUrl url(base + (closing ? "/close" : "/announce"));
                QNetworkRequest req(url);
                req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
                QNetworkReply* reply = nam->post(req, payload);
                if (closing)
                {
                    QEventLoop closeEl;
                    QTimer::singleShot(300, &closeEl, &QEventLoop::quit);
                    QObject::connect(reply, &QNetworkReply::finished, &closeEl, &QEventLoop::quit);
                    closeEl.exec();
                    reply->deleteLater();
                }
                else
                {
                    QObject::connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
                }
            }
        };

        if (autoLobby && !headless && gSyncDlg)
        {
            if (auto* roomEdit = gSyncDlg->findChild<QLineEdit*>("hostRoomEdit"))
                roomEdit->setText(QString::fromStdString(roomId));
            if (hostInfoLbl)
            {
                hostInfoLbl->setText(QString("<b>%1</b> &nbsp;|&nbsp; %2 &nbsp;|&nbsp; %3")
                    .arg(QString::fromUtf8(gRoomName.c_str()).toHtmlEscaped())
                    .arg(gPassHash ? "Password" : "Open")
                    .arg(QString::fromStdString(natNote).toHtmlEscaped()));
            }
            if (startBtn)
                QObject::connect(startBtn, &QPushButton::clicked, [&]() { startRequested = true; });

            nam = new QNetworkAccessManager(gSyncDlg.get());
            beaconSock = new QUdpSocket(gSyncDlg.get());
            lobbyTimer = new QTimer(gSyncDlg.get());
            static int tick = 0;
            tick = 0;
            QObject::connect(lobbyTimer, &QTimer::timeout, [&]() {
                pollPunches(connected < maxPlayers && (tick % 2) == 0);
                sendLobbyPings();
                if ((tick % 2) == 0)
                {
                    updateLobbyUI();
                    sendLobbyBeacon(false, (tick % 100) == 0);
                }
                if ((tick % 4) == 0 && autoLobby)
                {
                    std::lock_guard<std::mutex> lk(gNetMutex);
                    broadcastLobbyStateLocked();
                }
                if ((tick % 32) == 16)
                    sendNatKeepalive();
                tick++;
            });
            lobbyTimer->start(250);
            sendLobbyBeacon(false, true);
            updateLobbyUI();
        }
        else if (!headless)
        {
            updateLobbyUI();
        }

        bool ok = PumpUntil([&]() { return !headless && gDlgCanceled.load(); }, [&](ENetEvent& ev) -> bool
        {
            if (ev.type == ENET_EVENT_TYPE_CONNECT)
            {
                DisableThrottle(ev.peer);
            }
            else if (ev.type == ENET_EVENT_TYPE_DISCONNECT)
            {
                {
                    std::lock_guard<std::mutex> lk(gNetMutex);
                    for (int i = 1; i < kMaxPlayers; i++)
                        if (gClientPeers[i] == ev.peer)
                        {
                            gClientPeers[i] = nullptr;
                            gPlayerConnected[i] = false;
                            gPlayerNames[i].clear();
                            gPlayerAvatars[i].clear();
                            gPeerPingMs[i] = -1;
                            connected--;
                        }
                    broadcastLobbyStateLocked();
                }
                updateLobbyUI();
                sendLobbyBeacon(false, true);
            }
            else if (ev.type == ENET_EVENT_TYPE_RECEIVE)
            {
                Reader r(ev.packet->data, ev.packet->dataLength);
                u8 pktType = r.u8_();
                if (pktType == Msg_Ready)
                {
                    gReadyCount++;
                    Log("READY from a client during lobby (%d)\n", gReadyCount.load());
                    return true;
                }
                if (pktType == Msg_Pong)
                {
                    u64 sentUs = r.u64_();
                    if (r.ok)
                    {
                        u64 nowUs = (u64)std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        int slot = FindClientSlot(ev.peer);
                        if (slot > 0 && nowUs >= sentUs)
                            gPeerPingMs[slot] = (int)((nowUs - sentUs) / 1000);
                    }
                    return true;
                }
                if (pktType != Msg_Hello) return true;
                u32 magic = r.u32_(), ver = r.u32_();
                u64 rh = r.u64_(), b7 = r.u64_(), b9 = r.u64_(), fw = r.u64_(), ph = r.u64_();
                u32 nameLen = r.u32_();
                std::string clientName;
                if (nameLen > 0 && nameLen <= 64 && r.need(nameLen))
                {
                    clientName.assign((const char*)r.p + r.pos, nameLen);
                    r.pos += nameLen;
                }
                u32 avLen = r.u32_();
                std::vector<u8> clientAvatar;
                if (avLen > 0 && avLen <= 16384 && r.need(avLen))
                {
                    clientAvatar.assign(r.p + r.pos, r.p + r.pos + avLen);
                    r.pos += avLen;
                }
                u8 clientArch = r.has(1) ? r.u8_() : 1;
                std::string reason;
                if (!r.ok || magic != kMagic) reason = "Netplay protocol mismatch. All players must use melonDS Multiplayer.";
                else if (ver != kVersion) reason = "Netplay version mismatch (host v" + std::to_string(kVersion) + ", client v" + std::to_string(ver) + ").";
                else if (gPassHash != 0 && ph != gPassHash) reason = "Wrong room password.";
                else if (rh != gRomHash) reason = "ROM mismatch with host. Every player must use the exact same ROM file.";
                else if (b7 != gBios7Hash || b9 != gBios9Hash) reason = "BIOS mismatch (bios7.bin / bios9.bin) with host.";
                else if (fw != gFwHash) reason = "Firmware mismatch (firmware.bin) with host.";
                else if (connected >= maxPlayers) reason = "Room is full.";

                std::lock_guard<std::mutex> lk(gNetMutex);
                if (!reason.empty())
                {
                    Log("rejecting client: %s\n", reason.c_str());
                    Writer w; w.u8_(Msg_Reject); w.u32_((u32)reason.size()); w.bytes((const u8*)reason.data(), reason.size());
                    SendTo(ev.peer, w, true);
                    enet_host_flush(gHost);
                    return true;
                }

                int slot = 1;
                while (slot < kMaxPlayers && gClientPeers[slot]) slot++;
                gClientPeers[slot] = ev.peer;
                gPlayerConnected[slot] = true;
                gPlayerArch[slot] = clientArch;
                gPlayerNames[slot] = clientName.empty() ? ("Player " + std::to_string(slot + 1)) : clientName;
                gPlayerAvatars[slot] = std::move(clientAvatar);
                gPeerPingMs[slot] = -1;
                connected++;

                Writer w;
                w.u8_(Msg_Welcome); w.u8_((u8)slot); w.u8_((u8)0); w.u8_((u8)gDelay); w.u8_(gUseJIT ? 1 : 0);
                w.u32_((u32)gHostSave.size()); w.bytes(gHostSave.data(), gHostSave.size());
                SendTo(ev.peer, w, true);
                broadcastLobbyStateLocked();
                sendLobbyPingsLocked();
                enet_host_flush(gHost);
                Log("client accepted as slot %d (%s)\n", slot, gPlayerNames[slot].c_str());
                updateLobbyUI();
                sendLobbyBeacon(false, true);
                if (autoLobby && autoStartAt >= 2 && connected >= autoStartAt)
                    startRequested = true;
            }
            return true;
        }, [&]() {
            if (autoLobby) return headless ? (connected >= maxPlayers) : (startRequested && connected >= 2);
            return connected >= gNumPlayers;
        }, error);

        if (lobbyTimer) { lobbyTimer->stop(); lobbyTimer->disconnect(); }
        sendLobbyBeacon(true);
        if (!ok) { FinishLoading(); return false; }
        if (autoLobby)
        {
            if (gAutoDelay) gDelay = recommendedDelay();
            gNumPlayers = connected;
        }
        if (hostPanel) hostPanel->hide();
        if (playerTable) playerTable->hide();
        if (startBtn) startBtn->hide();
        if (cancelBtn) cancelBtn->setEnabled(false);
        setSyncStatus("Loading game & synchronizing players...");
        {
            std::lock_guard<std::mutex> lk(gNetMutex);
            Writer w;
            w.u8_(Msg_LobbyStart);
            w.u8_((u8)gNumPlayers);
            w.u8_((u8)gDelay);
            for (int i = 0; i < gNumPlayers; i++)
            {
                w.u32_((u32)gPlayerNames[i].size());
                w.bytes((const u8*)gPlayerNames[i].data(), gPlayerNames[i].size());
            }
            w.u8_(gUseJIT ? 1 : 0);
            for (int i = 1; i < kMaxPlayers; i++)
                if (gClientPeers[i]) SendTo(gClientPeers[i], w, true);
            enet_host_flush(gHost);
            Log("lobby starting with %d players (delay=%d jit=%d)\n", gNumPlayers, gDelay, gUseJIT ? 1 : 0);
        }
        gSlot = 0;
    }
    else
    {
        if (gAddr.empty()) { FinishLoading(); error = "No Room ID or host address provided."; return false; }

        ensureSyncDialog(false);
        setSyncStatus(QString("Connecting to \"%1\"...").arg(QString::fromStdString(gAddr)));

        // Resolve short 4-digit Room ID (NOTE: QHostAddress parses pure numbers like "9341" as uint32 IPs,
        // so treat any <=4-char string without '.' or ':' as a Room ID first!)
        bool looksLikeRoomId = (gAddr.size() <= 4 && gAddr.find('.') == std::string::npos && gAddr.find(':') == std::string::npos);
        QHostAddress directHa(QString::fromStdString(gAddr));
        if (looksLikeRoomId || directHa.isNull())
        {
            QString targetId = QString::fromStdString(gAddr).trimmed().toUpper();
            setSyncStatus(QString("Searching for Room \"%1\"...").arg(targetId));

            bool found = false;
            QUdpSocket lanFind;
            lanFind.bind(QHostAddress::AnyIPv4, kLanDiscoveryPort, QAbstractSocket::ShareAddress | QAbstractSocket::ReuseAddressHint);
            auto lanDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
            while (!found && !gDlgCanceled.load() && std::chrono::steady_clock::now() < lanDeadline)
            {
                QCoreApplication::processEvents();
                while (lanFind.hasPendingDatagrams())
                {
                    QByteArray d;
                    QHostAddress sender;
                    d.resize((int)lanFind.pendingDatagramSize());
                    lanFind.readDatagram(d.data(), d.size(), &sender);
                    QJsonDocument doc = QJsonDocument::fromJson(d);
                    QJsonObject obj = doc.object();
                    if (obj.value("proto").toString() == "melonDS-Netplay" &&
                        obj.value("roomId").toString().toUpper() == targetId)
                    {
                        gAddr = sender.toString().toStdString();
                        gPort = obj.value("localPort").toInt(7090);
                        gCliRoomId = targetId.toStdString();
                        found = true;
                        Log("room %s resolved via LAN -> %s:%d\n", targetId.toStdString().c_str(), gAddr.c_str(), gPort);
                        break;
                    }
                }
                if (!found) std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }

            for (int attempt = 0; !found && !gDlgCanceled.load() && attempt < 2 && !gLobbyUrl.empty(); attempt++)
            {
                if (attempt > 0)
                {
                    auto waitEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
                    while (!gDlgCanceled.load() && std::chrono::steady_clock::now() < waitEnd)
                    {
                        QCoreApplication::processEvents();
                        std::this_thread::sleep_for(std::chrono::milliseconds(16));
                    }
                    if (gDlgCanceled.load()) break;
                }
                QString base = QString::fromStdString(gLobbyUrl).trimmed();
                while (base.endsWith('/')) base.chop(1);
                QNetworkAccessManager qnam;
                QEventLoop el;
                QNetworkReply* rep = qnam.get(QNetworkRequest(QUrl(base + "/rooms")));
                QTimer::singleShot(2500, &el, &QEventLoop::quit);
                QObject::connect(rep, &QNetworkReply::finished, &el, &QEventLoop::quit);
                el.exec();
                if (rep->isFinished() && rep->error() == QNetworkReply::NoError)
                {
                    QJsonDocument doc = QJsonDocument::fromJson(rep->readAll());
                    QJsonArray arr = doc.isArray() ? doc.array() : doc.object().value("rooms").toArray();
                    for (const QJsonValue& val : arr)
                    {
                        QJsonObject obj = val.toObject();
                        if (obj.value("roomId").toString().toUpper() == targetId)
                        {
                            gAddr = obj.value("addr").toString().toStdString();
                            gPort = obj.value("port").toInt(7090);
                            gCliRoomId = targetId.toStdString();
                            found = !gAddr.empty();
                            Log("room %s resolved via lobby -> %s:%d\n", targetId.toStdString().c_str(), gAddr.c_str(), gPort);
                            break;
                        }
                    }
                }
                else
                {
                    Log("lobby /rooms lookup failed: err=%d %s\n", (int)rep->error(), rep->errorString().toStdString().c_str());
                }
                rep->deleteLater();
            }

            if (gDlgCanceled.load()) { FinishLoading(); error = "Canceled."; return false; }
            if (!found)
            {
                FinishLoading();
                error = "Room ID \"" + gAddr + "\" not found.\nMake sure the host opened the room first.";
                return false;
            }
        }

        // Discover BOTH our local UDP port and mapped public endpoint for UDP hole punching
        if (!headless && !gLobbyUrl.empty())
        {
            setSyncStatus("Preparing online connection...");
            std::string cPubIp, cNat;
            u16 cPubPort = 0;
            u16 cLocalPort = 0;
            if (DiscoverPublicEndpoint(0, cPubIp, cPubPort, cNat, &cLocalPort) && cPubPort > 0 && cLocalPort > 0)
            {
                ENetAddress localBind{};
                localBind.host = ENET_HOST_ANY;
                localBind.port = cLocalPort;
                gHost = enet_host_create(&localBind, 1, 2, 0, 0);
                if (gHost)
                {
                    std::string targetRoomId = gCliRoomId;
                    if (targetRoomId.empty())
                    {
                        bool hasSep = (gAddr.find('.') != std::string::npos || gAddr.find(':') != std::string::npos);
                        QHostAddress ha(QString::fromStdString(gAddr));
                        targetRoomId = (!hasSep || ha.isNull()) ? gAddr : EncodeRoomID(ha.toIPv4Address(), (u16)gPort);
                    }
                    QString base = QString::fromStdString(gLobbyUrl).trimmed();
                    while (base.endsWith('/')) base.chop(1);
                    QNetworkAccessManager punchMgr;
                    QJsonObject pobj;
                    pobj["roomId"] = QString::fromStdString(targetRoomId);
                    pobj["ip"] = QString::fromStdString(cPubIp);
                    pobj["port"] = (int)cPubPort;
                    QNetworkRequest preq(QUrl(base + "/punch"));
                    preq.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
                    QNetworkReply* prep = punchMgr.post(preq, QJsonDocument(pobj).toJson(QJsonDocument::Compact));
                    QEventLoop pel;
                    QTimer::singleShot(700, &pel, &QEventLoop::quit);
                    QObject::connect(prep, &QNetworkReply::finished, &pel, &QEventLoop::quit);
                    pel.exec();
                    prep->deleteLater();

                    auto punchWait = std::chrono::steady_clock::now() + std::chrono::milliseconds(350);
                    ENetAddress punchHostAddr{};
                    if (enet_address_set_host(&punchHostAddr, gAddr.c_str()) == 0)
                    {
                        punchHostAddr.port = (enet_uint16)gPort;
                        u8 punchByte[2] = {'P', 'N'};
                        ENetBuffer pbuf;
                        pbuf.data = punchByte;
                        pbuf.dataLength = sizeof(punchByte);
                        for (int k = 0; k < 3; k++)
                            enet_socket_send(gHost->socket, &punchHostAddr, &pbuf, 1);
                    }
                    while (!gDlgCanceled.load() && std::chrono::steady_clock::now() < punchWait)
                    {
                        QCoreApplication::processEvents();
                        std::this_thread::sleep_for(std::chrono::milliseconds(16));
                    }
                }
            }
        }

        if (!gHost) gHost = enet_host_create(nullptr, 1, 2, 0, 0);
        if (!gHost) { FinishLoading(); error = "Could not create network client."; return false; }

        ENetAddress addr;
        enet_address_set_host(&addr, gAddr.c_str());
        addr.port = gPort;
        Log("client connecting to %s:%d (roomId=%s)\n", gAddr.c_str(), gPort, gCliRoomId.c_str());
        gHostPeer = enet_host_connect(gHost, &addr, 2, 0);
        if (!gHostPeer) { FinishLoading(); error = "Could not initiate connection to host."; return false; }

        setSyncStatus("Connecting to host...");

        bool welcomed = false;
        bool lobbyStarted = false;
        auto start = std::chrono::steady_clock::now();
        auto lastPunch = start;
        int connectRetries = 0;
        bool ok = PumpUntil([&]() { return !headless && gDlgCanceled.load(); }, [&](ENetEvent& ev) -> bool
        {
            if (ev.type == ENET_EVENT_TYPE_CONNECT)
            {
                DisableThrottle(gHostPeer);
                Writer w;
                w.u8_(Msg_Hello); w.u32_(kMagic); w.u32_(kVersion);
                w.u64_(gRomHash); w.u64_(gBios7Hash); w.u64_(gBios9Hash); w.u64_(gFwHash); w.u64_(gPassHash);
                w.u32_((u32)gLocalName.size()); w.bytes((const u8*)gLocalName.data(), gLocalName.size());
                u32 avLen = (u32)std::min<size_t>(gLocalAvatar.size(), 16384);
                w.u32_(avLen);
                if (avLen > 0) w.bytes(gLocalAvatar.data(), avLen);
                w.u8_(LocalArch());
                std::lock_guard<std::mutex> lk(gNetMutex);
                SendTo(gHostPeer, w, true);
                enet_host_flush(gHost);
                setSyncStatus("Connected, verifying files...");
            }
            else if (ev.type == ENET_EVENT_TYPE_DISCONNECT)
            {
                error = "Disconnected from host.";
                return false;
            }
            else if (ev.type == ENET_EVENT_TYPE_RECEIVE)
            {
                Reader r(ev.packet->data, ev.packet->dataLength);
                u8 t = r.u8_();
                if (t == Msg_Ping)
                {
                    u64 ts = r.u64_();
                    if (r.ok)
                    {
                        Writer pw; pw.u8_(Msg_Pong); pw.u64_(ts);
                        std::lock_guard<std::mutex> lk(gNetMutex);
                        SendTo(gHostPeer, pw, false);
                        enet_host_flush(gHost);
                    }
                    return true;
                }
                if (t == Msg_Reject)
                {
                    u32 n = r.u32_();
                    if (r.need(n)) error = std::string((const char*)r.p + r.pos, n);
                    else error = "Rejected by host.";
                    return false;
                }
                if (t == Msg_Welcome)
                {
                    gSlot = r.u8_(); u8 welcomePlayers = r.u8_(); gDelay = r.u8_(); gUseJIT = (r.u8_() != 0);
                    u32 n = r.u32_();
                    if (r.need(n)) gHostSave.assign(r.p + r.pos, r.p + r.pos + n);
                    Log("welcome: slot=%d players=%d delay=%d jit=%d save=%u\n", gSlot, welcomePlayers, gDelay, gUseJIT ? 1 : 0, n);
                    welcomed = true;
                    if (welcomePlayers > 0)
                    {
                        gNumPlayers = welcomePlayers;
                        lobbyStarted = true;
                    }
                    else
                    {
                        setSyncStatus(QString("Connected as Player %1 – Waiting for host to start...").arg(gSlot + 1));
                    }
                }
                else if (t == Msg_LobbyState)
                {
                    u8 mask = r.u8_();
                    r.u8_(); // maxPlayers
                    for (int i = 0; i < kMaxPlayers; i++)
                    {
                        if (!(mask & (1 << i)))
                        {
                            gPlayerConnected[i] = false;
                            gPlayerNames[i].clear();
                            gPlayerAvatars[i].clear();
                            gPeerPingMs[i] = -1;
                            continue;
                        }
                        u32 nl = r.u32_();
                        std::string nm;
                        if (nl > 0 && nl <= 64 && r.need(nl))
                        {
                            nm.assign((const char*)r.p + r.pos, nl);
                            r.pos += nl;
                        }
                        s16 ping = (s16)r.u16_();
                        u32 al = r.u32_();
                        std::vector<u8> av;
                        if (al > 0 && al <= 16384 && r.need(al))
                        {
                            av.assign(r.p + r.pos, r.p + r.pos + al);
                            r.pos += al;
                        }
                        if (r.ok)
                        {
                            gPlayerConnected[i] = true;
                            gPlayerNames[i] = nm.empty() ? ("Player " + std::to_string(i + 1)) : nm;
                            gPeerPingMs[i] = ping;
                            gPlayerAvatars[i] = std::move(av);
                        }
                    }
                    refreshRosterTable();
                }
                else if (t == Msg_LobbyStart)
                {
                    gNumPlayers = r.u8_();
                    u8 finalDelay = r.u8_();
                    if (finalDelay >= 1 && finalDelay <= 30) gDelay = finalDelay;
                    for (int i = 0; i < gNumPlayers && i < kMaxPlayers; i++)
                    {
                        u32 nl = r.u32_();
                        if (nl > 0 && nl <= 64 && r.need(nl))
                        {
                            gPlayerNames[i].assign((const char*)r.p + r.pos, nl);
                            r.pos += nl;
                        }
                        else
                            gPlayerNames[i] = "Player " + std::to_string(i + 1);
                    }
                    if (r.has(1)) gUseJIT = (r.u8_() != 0);
                    lobbyStarted = true;
                    if (playerTable) playerTable->hide();
                    if (cancelBtn) cancelBtn->setEnabled(false);
                    setSyncStatus("Loading game...");
                    Log("lobby start: players=%d delay=%d jit=%d\n", gNumPlayers, gDelay, gUseJIT ? 1 : 0);
                }
            }
            return true;
        }, [&]()
        {
            if (lobbyStarted) return true;
            if (!welcomed)
            {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = now - start;
                if (now - lastPunch >= std::chrono::seconds(2))
                {
                    lastPunch = now;
                    u8 punchByte[2] = {'P', 'N'};
                    ENetBuffer pbuf{};
                    pbuf.data = punchByte;
                    pbuf.dataLength = sizeof(punchByte);
                    enet_socket_send(gHost->socket, &addr, &pbuf, 1);
                }
                if ((connectRetries == 0 && elapsed >= std::chrono::seconds(6)) ||
                    (connectRetries == 1 && elapsed >= std::chrono::seconds(12)))
                {
                    connectRetries++;
                    if (connectRetries == 1 && !headless && !gCliRoomId.empty() && !gLobbyUrl.empty())
                    {
                        QHostAddress curHa(QString::fromStdString(gAddr));
                        u32 ip4 = curHa.toIPv4Address();
                        bool isPrivate = ((ip4 >> 24) == 10) ||
                                         ((ip4 >> 16) == 0xC0A8) ||
                                         ((ip4 >> 20) == 0xAC1);
                        if (isPrivate)
                        {
                            QString base = QString::fromStdString(gLobbyUrl).trimmed();
                            while (base.endsWith('/')) base.chop(1);
                            QNetworkAccessManager qnam;
                            QEventLoop el;
                            QNetworkReply* rep = qnam.get(QNetworkRequest(QUrl(base + "/rooms")));
                            QTimer::singleShot(1500, &el, &QEventLoop::quit);
                            QObject::connect(rep, &QNetworkReply::finished, &el, &QEventLoop::quit);
                            el.exec();
                            if (rep->isFinished() && rep->error() == QNetworkReply::NoError)
                            {
                                QJsonDocument doc = QJsonDocument::fromJson(rep->readAll());
                                QJsonArray arr = doc.isArray() ? doc.array() : doc.object().value("rooms").toArray();
                                QString targetId = QString::fromStdString(gCliRoomId).trimmed().toUpper();
                                for (const QJsonValue& val : arr)
                                {
                                    QJsonObject obj = val.toObject();
                                    if (obj.value("roomId").toString().toUpper() == targetId)
                                    {
                                        std::string wanAddr = obj.value("addr").toString().toStdString();
                                        int wanPort = obj.value("port").toInt(gPort);
                                        if (!wanAddr.empty() && wanAddr != gAddr)
                                        {
                                            gAddr = wanAddr;
                                            gPort = wanPort;
                                            enet_address_set_host(&addr, gAddr.c_str());
                                            addr.port = (enet_uint16)gPort;
                                            u8 punchByte[2] = {'P', 'N'};
                                            ENetBuffer pbuf{};
                                            pbuf.data = punchByte;
                                            pbuf.dataLength = sizeof(punchByte);
                                            for (int k = 0; k < 3; k++)
                                                enet_socket_send(gHost->socket, &addr, &pbuf, 1);
                                            Log("LAN connect timed out, falling back to public lobby addr %s:%d\n", gAddr.c_str(), gPort);
                                        }
                                        break;
                                    }
                                }
                            }
                            rep->deleteLater();
                        }
                    }
                    if (gHostPeer) enet_peer_reset(gHostPeer);
                    gHostPeer = enet_host_connect(gHost, &addr, 2, 0);
                    Log("retrying connection to %s:%d (attempt %d)\n", gAddr.c_str(), gPort, connectRetries + 1);
                }
                if (elapsed > std::chrono::seconds(20))
                {
                    error = "Could not reach host. Check the Room ID / address and make sure the host opened the lobby first.";
#ifdef __APPLE__
                    error += "\n\nOn macOS: make sure 'Local Network' access is enabled for melonDS in System Settings -> Privacy & Security, and run 'xattr -cr melonDS.app' if downloaded via browser.";
#endif
                    return true;
                }
            }
            return false;
        }, error);

        if (!ok || !lobbyStarted) { FinishLoading(); return false; }
    }

    // prefill the first <delay> frames with neutral input for every instance
    for (int i = 0; i < gNumPlayers; i++)
        for (int f = 0; f < gDelay; f++)
            StoreInput(i, f, 0xFFF, 0, 0, 0);

    gActive = true;
    gNetRun = true;
    gTestScript.clear();
    if (const char* sp = getenv("MELONDS_TEST_SCRIPT"); sp && *sp)
    {
        if (FILE* sf = fopen(sp, "r"))
        {
            ScriptStep st{};
            while (fscanf(sf, "%u %u %x %hhu %hu %hu", &st.f0, &st.f1, &st.key, &st.touch, &st.x, &st.y) == 6)
                gTestScript.push_back(st);
            fclose(sf);
            Log("loaded %zu test script steps from %s\n", gTestScript.size(), sp);
        }
    }
#ifdef _WIN32
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
#endif
    gNetThread = std::thread(NetThreadFunc);
    Log("handshake done: slot=%d players=%d delay=%d jit=%d\n", gSlot, gNumPlayers, gDelay, gUseJIT ? 1 : 0);
    return true;
}

bool WaitAndGo(std::string& error)
{
    if (!Active()) return true;

    if (!gIsHost)
    {
        Writer w; w.u8_(Msg_Ready);
        std::lock_guard<std::mutex> lk(gNetMutex);
        SendTo(gHostPeer, w, true);
        enet_host_flush(gHost);
        Log("READY sent\n");
        return true;
    }

    auto waitStart = std::chrono::steady_clock::now();
    while (gReadyCount < gNumPlayers - 1)
    {
        QCoreApplication::processEvents();
        if (gDlgCanceled.load()) { error = "Canceled."; return false; }
        if (gPeerLost) { error = "A player disconnected before launch."; return false; }
        if (std::chrono::steady_clock::now() - waitStart > std::chrono::seconds(15))
        {
            error = "Timed out waiting for players to start.";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    {
        Writer w; w.u8_(Msg_Go);
        std::lock_guard<std::mutex> lk(gNetMutex);
        for (int i = 1; i < kMaxPlayers; i++)
            if (gClientPeers[i]) SendTo(gClientPeers[i], w, true);
        enet_host_flush(gHost);
    }
    Log("GO sent\n");
    gGo = true;
    gInCV.notify_all();
    return true;
}

void Abort()
{
    gPeerLost = true;
    gGo = true;
    gInCV.notify_all();
}

void Stop()
{
    if (!gConfigured) return;
    Abort();
    FinishLoading();
    gActive = false;
    gNetRun = false;
    if (gNetThread.joinable()) gNetThread.join();
    std::lock_guard<std::mutex> lk(gNetMutex);
    gHostPeer = nullptr;
    for (int i = 0; i < kMaxPlayers; i++) gClientPeers[i] = nullptr;
    if (gHost)
    {
        enet_host_flush(gHost);
        enet_host_destroy(gHost);
        gHost = nullptr;
    }
#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
    timeEndPeriod(1);
#endif
    Log("stopped\n");
}


// ---------- per-frame ----------

void FrameInput(int inst, u32 frame, u32& keymask, bool& touching, u16& tx, u16& ty, bool& mic)
{
    if (!Active()) return;
    if (inst < 0 || inst >= gNumPlayers) return;

    if (frame == 0 && inst == 0)
    {
        std::unique_lock<std::mutex> lk(gInMutex);
        gInCV.wait(lk, []() { return gGo.load() || gPeerLost.load(); });
    }

    if (inst == gSlot)
    {
        for (const auto& st : gTestScript)
        {
            if (frame >= st.f0 && frame <= st.f1)
            {
                keymask = st.key;
                touching = (st.touch != 0);
                tx = st.x; ty = st.y;
                break;
            }
        }
        // our own input is scheduled <delay> frames in the future, on every PC
        u32 target = frame + gDelay;
        u16 key = (u16)(keymask & 0xFFF);
        u8 touch = (touching ? 1 : 0) | (mic ? 2 : 0);
        StoreInput(inst, target, key, touch, tx, ty);
        BroadcastInputBatch(inst, target, nullptr, false);
    }

    auto waitStart = std::chrono::steady_clock::now();
    int resendCount = 0;
    bool logged = false;
    std::unique_lock<std::mutex> lk(gInMutex);
    for (;;)
    {
        InputRec& r = gInputs[inst][frame % kRing];
        if (r.Valid && r.Frame == frame)
        {
            keymask = r.Key;
            touching = (r.Touch & 1) != 0;
            mic = (r.Touch & 2) != 0;
            tx = r.X; ty = r.Y;
            r.Valid = 0;
            {
                static double sWaitMs[4] = {0, 0, 0, 0};
                static int sWaitCnt[4] = {0, 0, 0, 0};
                static double sMaxMs[4] = {0, 0, 0, 0};
                double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
                if (inst < 4) { sWaitMs[inst] += ms; if (ms > 2.0) sWaitCnt[inst]++; if (ms > sMaxMs[inst]) sMaxMs[inst] = ms; }
                if (inst == 0 && (frame % 600) == 0)
                {
                    auto pnm = [](int s) -> const char* {
                        return gPlayerNames[s].empty() ? "?" : gPlayerNames[s].c_str();
                    };
                    Log("WAITSTATS f=%u (ms / #waits>2ms / max ms): %s %.0f/%d/%.0f | %s %.0f/%d/%.0f | %s %.0f/%d/%.0f | %s %.0f/%d/%.0f\n", frame,
                        pnm(0), sWaitMs[0], sWaitCnt[0], sMaxMs[0],
                        pnm(1), sWaitMs[1], sWaitCnt[1], sMaxMs[1],
                        pnm(2), sWaitMs[2], sWaitCnt[2], sMaxMs[2],
                        pnm(3), sWaitMs[3], sWaitCnt[3], sMaxMs[3]);
                    for (int i = 0; i < 4; i++) { sWaitMs[i] = 0; sWaitCnt[i] = 0; sMaxMs[i] = 0; }
                }
            }
            if (!gTestScript.empty() && (keymask != 0xFFF || touching))
                Log("APPLY inst=%d f=%u k=%03X t=%d (%u,%u)\n", inst, frame, keymask, touching?1:0, tx, ty);
            return;
        }
        if (gPeerLost)
        {
            // keep running with neutral input so the user can still close the window
            keymask = 0xFFF; touching = false; mic = false;
            return;
        }
        gInCV.wait_for(lk, std::chrono::milliseconds(10));
        auto waited = std::chrono::steady_clock::now() - waitStart;
        auto nextReqAt = (resendCount < 15)
            ? std::chrono::milliseconds(20 * (resendCount + 1))
            : std::chrono::milliseconds(300 + 100 * (resendCount - 14));
        if (waited > nextReqAt)
        {
            resendCount++;
            lk.unlock();
            Writer w; w.u8_(Msg_ResendReq); w.u8_((u8)inst); w.u32_(frame);
            {
                std::lock_guard<std::mutex> nl(gNetMutex);
                ENetPeer* dst = gIsHost ? gClientPeers[inst] : gHostPeer;
                SendTo(dst, w, false);
                if (gHost) enet_host_flush(gHost);
            }
            lk.lock();
        }
        if (!logged && waited > std::chrono::milliseconds(1500))
        {
            const char* pname = (inst >= 0 && inst < kMaxPlayers && !gPlayerNames[inst].empty()) ? gPlayerNames[inst].c_str() : "Player";
            Log("%s (inst %d) stalled waiting for input of frame %u\n", pname, inst, frame);
            gLagAlertSlot.store(inst);
            logged = true;
        }
    }
}

void ReportRAMHash(int inst, u32 frame, u32 hash)
{
    if (!Active()) return;
    inst &= 0x7F;

    if (!gIsHost)
    {
        Writer w; w.u8_(Msg_Hash); w.u8_((u8)inst); w.u32_(frame); w.u32_(hash);
        std::lock_guard<std::mutex> lk(gNetMutex);
        SendTo(gHostPeer, w, true);
        return;
    }

    u64 key = ((u64)inst << 32) | frame;
    std::lock_guard<std::mutex> lk(gHashMutex);
    gHostHashes[key] = hash;
    if (gHostHashes.size() > 1024)
        gHostHashes.erase(gHostHashes.begin());

    for (int slot = 1; slot < gNumPlayers; slot++)
    {
        auto it = gClientHashes[slot].find(key);
        if (it != gClientHashes[slot].end())
        {
            u32 clientHash = it->second;
            gClientHashes[slot].erase(it);
            CompareHashPairLocked(slot, inst, frame, hash, clientHash);
        }
    }
}

std::string StatusText()
{
    if (!Active()) return "";
    std::string s = gIsHost ? "Netplay Host" : ("Netplay Player " + std::to_string(gSlot + 1));
    if (gPeerLost) s += " - DISCONNECTED";
    else if (gDesyncCount) s += " - DESYNC (" + std::to_string(gDesyncCount.load()) + ")";
    return s;
}

}
