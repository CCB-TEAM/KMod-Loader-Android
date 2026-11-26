#include "net_conn.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace
{
// ---------------------------------------------------------------------------------------------
// base64 + SHA-1 (only for the WebSocket handshake)
// ---------------------------------------------------------------------------------------------
std::string B64(const uint8_t* d, size_t n)
{
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < n; i += 3)
    {
        uint32_t v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        o += t[v >> 18 & 63]; o += t[v >> 12 & 63];
        o += i + 1 < n ? t[v >> 6 & 63] : '='; o += i + 2 < n ? t[v & 63] : '=';
    }
    return o;
}

void Sha1(const std::string& in, uint8_t out[20])
{
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string m = in;
    uint64_t bits = (uint64_t)in.size() * 8;
    m += (char)0x80;
    while (m.size() % 64 != 56) m += (char)0;
    for (int i = 7; i >= 0; i--) m += (char)(bits >> (i * 8));
    auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
    for (size_t o = 0; o < m.size(); o += 64)
    {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = (uint8_t)m[o + i * 4] << 24 | (uint8_t)m[o + i * 4 + 1] << 16 | (uint8_t)m[o + i * 4 + 2] << 8 | (uint8_t)m[o + i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++)
        {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; i++) for (int j = 0; j < 4; j++) out[i * 4 + j] = (uint8_t)(h[i] >> (24 - j * 8));
}

// ---------------------------------------------------------------------------------------------
// Transport: plain socket or Java SSLSocket
// ---------------------------------------------------------------------------------------------
struct Transport
{
    virtual ~Transport() {}
    virtual bool Connect(const std::string& host, int port, int timeoutMs, std::string& err) = 0;
    virtual int Read(uint8_t* buf, int n, int timeoutMs) = 0;       // >0 bytes, 0 timeout, -1 closed/error
    virtual bool Write(const uint8_t* buf, size_t n) = 0;
    virtual void Close() = 0;
};

struct PlainTransport : Transport
{
    int fd = -1;
    bool udp = false;
    bool Connect(const std::string& host, int port, int timeoutMs, std::string& err) override
    {
        addrinfo hints = {}, *res = nullptr;
        hints.ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM;
        hints.ai_family = AF_UNSPEC;
        int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
        if (rc != 0 || !res) { err = std::string("cannot resolve ") + host + ": " + gai_strerror(rc); return false; }
        for (addrinfo* a = res; a; a = a->ai_next)
        {
            int s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s < 0) continue;
            fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
            int c = connect(s, a->ai_addr, a->ai_addrlen);
            if (c != 0 && errno == EINPROGRESS)
            {
                pollfd p = {s, POLLOUT, 0};
                if (poll(&p, 1, timeoutMs) == 1)
                {
                    int e = 0; socklen_t l = sizeof(e);
                    getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &l);
                    c = e == 0 ? 0 : -1;
                    if (e) errno = e;
                }
                else { errno = ETIMEDOUT; c = -1; }
            }
            if (c == 0) { fd = s; int one = 1; if (!udp) setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); freeaddrinfo(res); return true; }
            close(s);
        }
        freeaddrinfo(res);
        err = std::string("cannot connect to ") + host + ":" + std::to_string(port) + ": " + strerror(errno);
        return false;
    }
    int Read(uint8_t* buf, int n, int timeoutMs) override
    {
        pollfd p = {fd, POLLIN, 0};
        int r = poll(&p, 1, timeoutMs);
        if (r == 0) return 0;
        if (r < 0) return errno == EINTR ? 0 : -1;
        int got = (int)recv(fd, buf, n, 0);
        if (got > 0) return got;
        if (got == 0) return udp ? 0 : -1;
        return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
    }
    bool Write(const uint8_t* buf, size_t n) override
    {
        size_t off = 0;
        while (off < n)
        {
            ssize_t w = send(fd, buf + off, n - off, MSG_NOSIGNAL);
            if (w > 0) { off += w; continue; }
            if (w < 0 && (errno == EAGAIN || errno == EINTR)) { pollfd p = {fd, POLLOUT, 0}; poll(&p, 1, 1000); continue; }
            return false;
        }
        return true;
    }
    void Close() override { if (fd >= 0) { shutdown(fd, SHUT_RDWR); close(fd); fd = -1; } }
};

JavaVM* GetVM()
{
    static JavaVM* vm = nullptr;
    if (vm) return vm;
    using Fn = jint (*)(JavaVM**, jsize, jsize*);
    if (void* h = dlopen("libkmod.so", RTLD_NOW | RTLD_NOLOAD))
    {
        using Get = JavaVM* (*)();
        if (Get g = reinterpret_cast<Get>(dlsym(h, "KMod_GetJavaVM"))) vm = g();
    }
    for (const char* lib : {"libnativehelper.so", "libart.so", "libandroid_runtime.so"})
    {
        if (vm) break;
        if (void* h = dlopen(lib, RTLD_NOW | RTLD_NOLOAD))
            if (Fn f = reinterpret_cast<Fn>(dlsym(h, "JNI_GetCreatedJavaVMs"))) { jsize n = 0; f(&vm, 1, &n); }
    }
    return vm;
}

struct TlsTransport : Transport
{
    JNIEnv* env = nullptr;                       // the connection thread's env
    jobject sock = nullptr, in = nullptr, out = nullptr;
    jclass cIn = nullptr, cOut = nullptr, cTimeout = nullptr, cSock = nullptr;
    jbyteArray buf = nullptr;
    jmethodID mRead = nullptr, mWrite = nullptr, mFlush = nullptr;

    std::string Ex()
    {
        jthrowable ex = env->ExceptionOccurred();
        env->ExceptionClear();
        std::string s = "java exception";
        if (ex)
        {
            jclass cObj = env->FindClass("java/lang/Object");
            jstring js = static_cast<jstring>(env->CallObjectMethod(ex, env->GetMethodID(cObj, "toString", "()Ljava/lang/String;")));
            if (env->ExceptionCheck()) env->ExceptionClear();
            else if (js) { const char* c = env->GetStringUTFChars(js, nullptr); if (c) { s = c; env->ReleaseStringUTFChars(js, c); } env->DeleteLocalRef(js); }
            env->DeleteLocalRef(cObj); env->DeleteLocalRef(ex);
        }
        return s;
    }
#define TCHK() do { if (env->ExceptionCheck()) { err = Ex(); return false; } } while (0)
    bool Connect(const std::string& host, int port, int timeoutMs, std::string& err) override
    {
        cSock = env->FindClass("java/net/Socket");
        cIn = env->FindClass("java/io/InputStream");
        cOut = env->FindClass("java/io/OutputStream");
        cTimeout = env->FindClass("java/net/SocketTimeoutException");
        cSock = static_cast<jclass>(env->NewGlobalRef(cSock)); cIn = static_cast<jclass>(env->NewGlobalRef(cIn));
        cOut = static_cast<jclass>(env->NewGlobalRef(cOut)); cTimeout = static_cast<jclass>(env->NewGlobalRef(cTimeout));
        TCHK();
        jobject plain = env->NewObject(cSock, env->GetMethodID(cSock, "<init>", "()V"));
        TCHK();
        jclass cAddr = env->FindClass("java/net/InetSocketAddress");
        jstring jh = env->NewStringUTF(host.c_str());
        jobject addr = env->NewObject(cAddr, env->GetMethodID(cAddr, "<init>", "(Ljava/lang/String;I)V"), jh, (jint)port);
        TCHK();
        env->CallVoidMethod(plain, env->GetMethodID(cSock, "connect", "(Ljava/net/SocketAddress;I)V"), addr, (jint)timeoutMs);
        if (env->ExceptionCheck()) { err = "cannot connect to " + host + ":" + std::to_string(port) + ": " + Ex(); return false; }
        jclass cF = env->FindClass("javax/net/ssl/SSLSocketFactory");
        jobject factory = env->CallStaticObjectMethod(cF, env->GetStaticMethodID(cF, "getDefault", "()Ljavax/net/SocketFactory;"));
        TCHK();
        jobject ssl = env->CallObjectMethod(factory, env->GetMethodID(cF, "createSocket", "(Ljava/net/Socket;Ljava/lang/String;IZ)Ljava/net/Socket;"), plain, jh, (jint)port, JNI_TRUE);
        TCHK();
        sock = env->NewGlobalRef(ssl);
        jclass cSsl = env->FindClass("javax/net/ssl/SSLSocket");
        jmethodID mGetP = env->GetMethodID(cSsl, "getSSLParameters", "()Ljavax/net/ssl/SSLParameters;");
        jobject params = env->CallObjectMethod(sock, mGetP);
        TCHK();
        jclass cP = env->FindClass("javax/net/ssl/SSLParameters");
        jstring https = env->NewStringUTF("HTTPS");
        env->CallVoidMethod(params, env->GetMethodID(cP, "setEndpointIdentificationAlgorithm", "(Ljava/lang/String;)V"), https);   // check the host name
        env->CallVoidMethod(sock, env->GetMethodID(cSsl, "setSSLParameters", "(Ljavax/net/ssl/SSLParameters;)V"), params);
        TCHK();
        env->CallVoidMethod(sock, env->GetMethodID(cSock, "setSoTimeout", "(I)V"), (jint)timeoutMs);
        env->CallVoidMethod(sock, env->GetMethodID(cSsl, "startHandshake", "()V"));
        if (env->ExceptionCheck()) { err = "TLS handshake with " + host + " failed: " + Ex(); return false; }
        jobject i = env->CallObjectMethod(sock, env->GetMethodID(cSock, "getInputStream", "()Ljava/io/InputStream;"));
        jobject o = env->CallObjectMethod(sock, env->GetMethodID(cSock, "getOutputStream", "()Ljava/io/OutputStream;"));
        TCHK();
        in = env->NewGlobalRef(i); out = env->NewGlobalRef(o);
        mRead = env->GetMethodID(cIn, "read", "([BII)I");
        mWrite = env->GetMethodID(cOut, "write", "([BII)V");
        mFlush = env->GetMethodID(cOut, "flush", "()V");
        jbyteArray b = env->NewByteArray(16384);
        buf = static_cast<jbyteArray>(env->NewGlobalRef(b));
        return true;
    }
    int Read(uint8_t* dst, int n, int timeoutMs) override
    {
        env->CallVoidMethod(sock, env->GetMethodID(cSock, "setSoTimeout", "(I)V"), (jint)timeoutMs);
        jint got = env->CallIntMethod(in, mRead, buf, (jint)0, (jint)std::min(n, 16384));
        if (env->ExceptionCheck())
        {
            jthrowable ex = env->ExceptionOccurred();
            bool timeout = env->IsInstanceOf(ex, cTimeout);
            env->DeleteLocalRef(ex);
            env->ExceptionClear();
            return timeout ? 0 : -1;
        }
        if (got < 0) return -1;
        env->GetByteArrayRegion(buf, 0, got, reinterpret_cast<jbyte*>(dst));
        return got;
    }
    bool Write(const uint8_t* src, size_t n) override
    {
        size_t off = 0;
        while (off < n)
        {
            jsize k = (jsize)std::min<size_t>(16384, n - off);
            env->SetByteArrayRegion(buf, 0, k, reinterpret_cast<const jbyte*>(src + off));
            env->CallVoidMethod(out, mWrite, buf, (jint)0, (jint)k);
            if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
            off += k;
        }
        env->CallVoidMethod(out, mFlush);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        return true;
    }
    void Close() override
    {
        if (sock) { env->CallVoidMethod(sock, env->GetMethodID(cSock, "close", "()V")); if (env->ExceptionCheck()) env->ExceptionClear(); }
        for (jobject* o : {&sock, &in, &out}) if (*o) { env->DeleteGlobalRef(*o); *o = nullptr; }
        for (jclass* c : {&cSock, &cIn, &cOut, &cTimeout}) if (*c) { env->DeleteGlobalRef(*c); *c = nullptr; }
        if (buf) { env->DeleteGlobalRef(buf); buf = nullptr; }
    }
};

// ---------------------------------------------------------------------------------------------
// Connections
// ---------------------------------------------------------------------------------------------
struct Event { std::string type, data, extra; bool binary = false; };
struct Outgoing { int opcode; std::string data; };

struct Conn
{
    int id = 0;
    std::string owner, kind, host, path = "/", scheme;
    int port = 0;
    bool tls = false;
    std::vector<std::pair<std::string, std::string>> headers;
    int timeoutMs = 15000;
    std::atomic<bool> stop{false}, done{false};
    std::mutex mu;
    std::deque<Event> events;       // to the game thread
    std::deque<Outgoing> out;       // from the game thread
    std::thread th;
};

WorkerHooks g_hooks{};
std::vector<std::shared_ptr<Conn>> g_conns;        // game thread only
int g_nextId = 1;
const size_t kMaxMessage = 16u << 20;
const size_t kMaxQueue = 4096;

void Push(Conn* c, Event e)
{
    std::lock_guard<std::mutex> g(c->mu);
    if (c->events.size() < kMaxQueue) c->events.push_back(std::move(e));
}

std::string Frame(int opcode, const std::string& payload)      // client frames are masked
{
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::string f;
    f += (char)(0x80 | opcode);
    size_t n = payload.size();
    if (n < 126) f += (char)(0x80 | n);
    else if (n < 65536) { f += (char)(0x80 | 126); f += (char)(n >> 8); f += (char)n; }
    else { f += (char)(0x80 | 127); for (int i = 7; i >= 0; i--) f += (char)((uint64_t)n >> (i * 8)); }
    uint8_t key[4];
    for (auto& k : key) k = (uint8_t)rng();
    f.append(reinterpret_cast<char*>(key), 4);
    for (size_t i = 0; i < n; i++) f += (char)(payload[i] ^ key[i & 3]);
    return f;
}

bool ParseUrl(const std::string& url, Conn* c, std::string& err)
{
    size_t p = url.find("://");
    if (p == std::string::npos) { err = "bad url"; return false; }
    c->scheme = url.substr(0, p);
    if (c->scheme != "ws" && c->scheme != "wss") { err = "WebSocket urls start with ws:// or wss://"; return false; }
    c->tls = c->scheme == "wss";
    std::string rest = url.substr(p + 3);
    size_t slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    c->path = slash == std::string::npos ? "/" : rest.substr(slash);
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos && hostport.find(']') == std::string::npos) { c->host = hostport.substr(0, colon); c->port = atoi(hostport.c_str() + colon + 1); }
    else { c->host = hostport; c->port = c->tls ? 443 : 80; }
    if (c->host.empty()) { err = "bad url"; return false; }
    return true;
}

void RunConn(std::shared_ptr<Conn> c)
{
    JavaVM* vm = nullptr;
    JNIEnv* env = nullptr;
    std::unique_ptr<Transport> t;
    std::string err;
    bool attached = false;
    if (c->tls)
    {
        vm = GetVM();
        if (!vm || vm->AttachCurrentThread(&env, nullptr) != JNI_OK) { Push(c.get(), {"error", "no Java VM for TLS", ""}); Push(c.get(), {"close", "", ""}); c->done = true; return; }
        attached = true;
        auto* tt = new TlsTransport(); tt->env = env; t.reset(tt);
    }
    else { auto* pt = new PlainTransport(); pt->udp = c->kind == "udp"; t.reset(pt); }

    auto fail = [&](const std::string& m) { Push(c.get(), {"error", m, ""}); };
    std::string rbuf;                                  // received bytes not yet parsed (ws handshake / frames)
    bool ok = t->Connect(c->host, c->port, c->timeoutMs, err);
    if (!ok) fail(err);
    uint8_t chunk[16384];

    // WebSocket handshake
    if (ok && c->kind == "ws")
    {
        std::mt19937 rng{std::random_device{}()};
        uint8_t kb[16];
        for (auto& b : kb) b = (uint8_t)rng();
        std::string key = B64(kb, 16);
        std::string req = "GET " + c->path + " HTTP/1.1\r\nHost: " + c->host + ((c->tls && c->port == 443) || (!c->tls && c->port == 80) ? "" : ":" + std::to_string(c->port)) +
                          "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n";
        for (auto& h : c->headers) req += h.first + ": " + h.second + "\r\n";
        req += "\r\n";
        ok = t->Write(reinterpret_cast<const uint8_t*>(req.data()), req.size());
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(c->timeoutMs);
        size_t end = std::string::npos;
        while (ok && (end = rbuf.find("\r\n\r\n")) == std::string::npos)
        {
            if (std::chrono::steady_clock::now() > deadline || rbuf.size() > 65536 || c->stop) { ok = false; fail("WebSocket handshake timed out"); break; }
            int n = t->Read(chunk, sizeof(chunk), 200);
            if (n < 0) { ok = false; fail("connection closed during the WebSocket handshake"); break; }
            rbuf.append(reinterpret_cast<char*>(chunk), n);
        }
        if (ok)
        {
            std::string head = rbuf.substr(0, end);
            rbuf.erase(0, end + 4);
            uint8_t sha[20];
            Sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", sha);
            std::string accept = B64(sha, 20);
            std::string lower = head;
            for (auto& ch : lower) ch = (char)tolower((unsigned char)ch);
            if (head.find(" 101") == std::string::npos) { ok = false; fail("the server did not switch protocols: " + head.substr(0, head.find("\r\n"))); }
            else if (lower.find("sec-websocket-accept: " + [&] { std::string a = accept; for (auto& ch : a) ch = (char)tolower((unsigned char)ch); return a; }()) == std::string::npos)
            { ok = false; fail("bad Sec-WebSocket-Accept from the server"); }
        }
    }
    if (ok) Push(c.get(), {"open", "", ""});

    // main loop
    std::string message;           // WebSocket message being assembled
    int msgOpcode = 0;
    int closeCode = 1006;
    std::string closeReason;
    bool sentClose = false;
    while (ok && !c->stop)
    {
        // outgoing
        std::deque<Outgoing> out;
        { std::lock_guard<std::mutex> g(c->mu); out.swap(c->out); }
        for (auto& o : out)
        {
            std::string bytes = c->kind == "ws" ? Frame(o.opcode, o.data) : o.data;
            if (!t->Write(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size())) { fail("send failed"); ok = false; break; }
            if (c->kind == "ws" && o.opcode == 0x8) { sentClose = true; }
        }
        if (!ok) break;
        int n = t->Read(chunk, sizeof(chunk), 100);
        if (n < 0) { if (c->kind == "ws" && !sentClose) closeCode = 1006; ok = false; break; }
        if (n == 0) continue;
        if (c->kind != "ws") { Push(c.get(), {"data", std::string(reinterpret_cast<char*>(chunk), n), "", true}); continue; }
        rbuf.append(reinterpret_cast<char*>(chunk), n);
        // parse frames
        for (;;)
        {
            if (rbuf.size() < 2) break;
            uint8_t b0 = rbuf[0], b1 = rbuf[1];
            bool fin = b0 & 0x80;
            int op = b0 & 0x0f;
            bool masked = b1 & 0x80;
            uint64_t len = b1 & 0x7f;
            size_t hdr = 2;
            if (len == 126) { if (rbuf.size() < 4) break; len = (uint8_t)rbuf[2] << 8 | (uint8_t)rbuf[3]; hdr = 4; }
            else if (len == 127) { if (rbuf.size() < 10) break; len = 0; for (int i = 0; i < 8; i++) len = len << 8 | (uint8_t)rbuf[2 + i]; hdr = 10; }
            if (len > kMaxMessage) { fail("message too large"); ok = false; break; }
            size_t mk = masked ? 4 : 0;
            if (rbuf.size() < hdr + mk + len) break;
            std::string payload = rbuf.substr(hdr + mk, len);
            if (masked) for (size_t i = 0; i < len; i++) payload[i] ^= rbuf[hdr + (i & 3)];
            rbuf.erase(0, hdr + mk + len);
            if (op == 0x9) { std::lock_guard<std::mutex> g(c->mu); c->out.push_back({0xA, payload}); }                    // ping -> pong
            else if (op == 0x8)
            {
                closeCode = payload.size() >= 2 ? ((uint8_t)payload[0] << 8 | (uint8_t)payload[1]) : 1005;
                closeReason = payload.size() > 2 ? payload.substr(2) : "";
                if (!sentClose) { std::string echo = Frame(0x8, payload.substr(0, 2)); t->Write(reinterpret_cast<const uint8_t*>(echo.data()), echo.size()); }
                ok = false;
                break;
            }
            else if (op == 0x1 || op == 0x2 || op == 0x0)
            {
                if (op != 0x0) { msgOpcode = op; message.clear(); }
                message += payload;
                if (message.size() > kMaxMessage) { fail("message too large"); ok = false; break; }
                if (fin) { Push(c.get(), {"message", message, "", msgOpcode == 0x2}); message.clear(); }
            }
        }
    }
    if (c->stop && c->kind == "ws" && !sentClose)           // a clean close started by the mod
    {
        std::string f = Frame(0x8, std::string("\x03\xe8", 2));
        t->Write(reinterpret_cast<const uint8_t*>(f.data()), f.size());
        closeCode = 1000;
    }
    t->Close();
    if (attached) vm->DetachCurrentThread();
    Push(c.get(), {"close", closeReason, std::to_string(closeCode)});
    c->done = true;
}

void Shutdown(const std::shared_ptr<Conn>& c)
{
    c->stop = true;
    for (int i = 0; i < 200 && !c->done.load(); i++) usleep(5000);
    if (c->th.joinable()) { if (c->done.load()) c->th.join(); else c->th.detach(); }
}

std::shared_ptr<Conn> Find(int id) { for (auto& c : g_conns) if (c->id == id) return c; return nullptr; }
}

void NetConn_Init(const WorkerHooks& hooks) { g_hooks = hooks; }

// kmod.net_open(owner, kind, { url=, host=, port=, tls=, headers={}, timeout= }) -> id
int NetConn_Open(lua_State* L)
{
    auto c = std::make_shared<Conn>();
    c->owner = luaL_checkstring(L, 1);
    c->kind = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    if (c->kind != "tcp" && c->kind != "udp" && c->kind != "ws") return luaL_error(L, "net: unknown connection kind '%s'", c->kind.c_str());
    std::string err;
    lua_getfield(L, 3, "timeout"); if (lua_isnumber(L, -1)) { double t = lua_tonumber(L, -1); c->timeoutMs = (int)(t < 1000 ? 1000 : (t > 60000 ? 60000 : t)); } lua_pop(L, 1);
    if (c->kind == "ws")
    {
        lua_getfield(L, 3, "url");
        std::string url = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        if (!ParseUrl(url, c.get(), err)) return luaL_error(L, "WebSocket: %s", err.c_str());
        lua_getfield(L, 3, "headers");
        if (lua_istable(L, -1)) { lua_pushnil(L); while (lua_next(L, -2)) { if (lua_type(L, -2) == LUA_TSTRING && lua_isstring(L, -1)) c->headers.emplace_back(lua_tostring(L, -2), lua_tostring(L, -1)); lua_pop(L, 1); } }
        lua_pop(L, 1);
    }
    else
    {
        lua_getfield(L, 3, "host"); if (lua_isstring(L, -1)) c->host = lua_tostring(L, -1); lua_pop(L, 1);
        lua_getfield(L, 3, "port"); if (lua_isnumber(L, -1)) c->port = (int)lua_tointeger(L, -1); lua_pop(L, 1);
        lua_getfield(L, 3, "tls"); c->tls = lua_toboolean(L, -1) != 0 && c->kind == "tcp"; lua_pop(L, 1);
        if (c->host.empty() || c->port <= 0 || c->port > 65535) return luaL_error(L, "net: host and port are required");
    }
    if (g_conns.size() >= 16) return luaL_error(L, "net: too many connections (16 at most)");
    c->id = g_nextId++;
    g_conns.push_back(c);
    c->th = std::thread(RunConn, c);
    lua_pushinteger(L, c->id);
    return 1;
}

int NetConn_Send(lua_State* L)
{
    int id = (int)luaL_checkinteger(L, 1);
    size_t n; const char* d = luaL_checklstring(L, 2, &n);
    bool binary = lua_toboolean(L, 3) != 0;
    auto c = Find(id);
    if (!c || c->stop.load() || c->done.load()) { lua_pushboolean(L, 0); return 1; }
    {
        std::lock_guard<std::mutex> g(c->mu);
        if (c->out.size() >= kMaxQueue) { lua_pushboolean(L, 0); return 1; }
        c->out.push_back({c->kind == "ws" ? (binary ? 0x2 : 0x1) : 0, std::string(d, n)});
    }
    lua_pushboolean(L, 1);
    return 1;
}

int NetConn_Close(lua_State* L)
{
    int id = (int)luaL_checkinteger(L, 1);
    if (auto c = Find(id)) c->stop = true;          // the thread closes cleanly; Pump removes it after the close event
    return 0;
}

void NetConn_Pump(lua_State* L)
{
    if (g_conns.empty()) return;
    std::vector<std::shared_ptr<Conn>> list = g_conns;
    for (auto& c : list)
    {
        std::deque<Event> batch;
        {
            std::lock_guard<std::mutex> g(c->mu);
            size_t n = std::min<size_t>(c->events.size(), 256);
            for (size_t i = 0; i < n; i++) { batch.push_back(std::move(c->events.front())); c->events.pop_front(); }
        }
        for (auto& e : batch)
        {
            lua_getglobal(L, "kmod");
            lua_getfield(L, -1, "_net_event");
            lua_remove(L, -2);
            if (!lua_isfunction(L, -1)) { lua_pop(L, 1); continue; }
            lua_pushinteger(L, c->id);
            lua_pushstring(L, e.type.c_str());
            lua_pushlstring(L, e.data.data(), e.data.size());
            lua_pushstring(L, e.extra.c_str());
            lua_pushboolean(L, e.binary);
            g_hooks.pcall(L, 5, 0, "network event", c->owner);
        }
    }
    for (size_t i = 0; i < g_conns.size();)       // forget finished connections whose events were all delivered
    {
        auto& c = g_conns[i];
        bool empty;
        { std::lock_guard<std::mutex> g(c->mu); empty = c->events.empty(); }
        if (c->done.load() && empty) { Shutdown(c); g_conns.erase(g_conns.begin() + i); }
        else i++;
    }
}

void NetConn_CloseUser(lua_State*, const char* only)
{
    for (size_t i = 0; i < g_conns.size();)
    {
        if (!g_conns[i]->owner.empty() && (!only || g_conns[i]->owner == only)) { g_conns[i]->events.clear(); Shutdown(g_conns[i]); g_conns.erase(g_conns.begin() + i); }
        else i++;
    }
}
