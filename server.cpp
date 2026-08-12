// CrazyRedis -- a small educational Redis-compatible server.
//
// Speaks a real RESP parser (not fixed-index string splitting), is
// thread-safe, supports lazy key expiry, a bounded LRU key store, and
// basic master/replica replication. Builds on POSIX (Linux/macOS/WSL)
// and Windows (MinGW-w64 / MSVC via Winsock2).

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using socket_t = SOCKET;
  static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
  static void closeSocket(socket_t s) { closesocket(s); }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  using socket_t = int;
  static constexpr socket_t kInvalidSocket = -1;
  static void closeSocket(socket_t s) { close(s); }
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <list>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Small cross-platform networking shims
// ---------------------------------------------------------------------------

static bool networkInit() {
#ifdef _WIN32
  WSADATA wsaData;
  return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
#else
  return true;
#endif
}

static void networkCleanup() {
#ifdef _WIN32
  WSACleanup();
#endif
}

static ssize_t socketSend(socket_t fd, const std::string &data) {
  return send(fd, data.data(), static_cast<int>(data.size()), 0);
}

static ssize_t socketRecv(socket_t fd, char *buf, size_t len) {
  return recv(fd, buf, static_cast<int>(len), 0);
}

// Connects to host:port using getaddrinfo, which works identically on
// POSIX and Windows and (unlike the previous implementation) actually
// resolves and dials the requested host instead of INADDR_ANY.
static socket_t connectTo(const std::string &host, int port) {
  struct addrinfo hints {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo *res = nullptr;
  std::string portStr = std::to_string(port);
  if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
    return kInvalidSocket;
  }

  socket_t fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd == kInvalidSocket) {
    freeaddrinfo(res);
    return kInvalidSocket;
  }

  if (connect(fd, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
    closeSocket(fd);
    freeaddrinfo(res);
    return kInvalidSocket;
  }

  freeaddrinfo(res);
  return fd;
}

// ---------------------------------------------------------------------------
// RESP protocol helpers
// ---------------------------------------------------------------------------

static std::string toLower(const std::string &s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                  [](unsigned char c) { return std::tolower(c); });
  return out;
}

static std::string respSimpleString(const std::string &s) { return "+" + s + "\r\n"; }
static std::string respError(const std::string &s) { return "-ERR " + s + "\r\n"; }
static std::string respNilBulkString() { return "$-1\r\n"; }
static std::string respBulkString(const std::string &s) {
  return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
}
static std::string respArray(const std::vector<std::string> &items) {
  std::string out = "*" + std::to_string(items.size()) + "\r\n";
  for (const auto &item : items) out += respBulkString(item);
  return out;
}
static std::string respCommandArray(const std::vector<std::string> &args) {
  return respArray(args);
}

// Parses a single RESP array-of-bulk-strings command from the front of
// `buffer`. Returns the parsed args and erases the consumed bytes from
// `buffer` on success. Returns std::nullopt if the buffer doesn't yet
// contain a full command (the caller should wait for more bytes).
static std::optional<std::vector<std::string>> parseRespCommand(std::string &buffer) {
  if (buffer.empty()) return std::nullopt;

  size_t pos = 0;
  if (buffer[pos] != '*') return std::nullopt;

  size_t lineEnd = buffer.find("\r\n", pos);
  if (lineEnd == std::string::npos) return std::nullopt;

  int argCount = 0;
  try {
    argCount = std::stoi(buffer.substr(pos + 1, lineEnd - pos - 1));
  } catch (...) {
    buffer.clear();
    return std::nullopt;
  }
  pos = lineEnd + 2;

  std::vector<std::string> args;
  args.reserve(std::max(argCount, 0));

  for (int i = 0; i < argCount; ++i) {
    if (pos >= buffer.size() || buffer[pos] != '$') return std::nullopt;

    size_t lenLineEnd = buffer.find("\r\n", pos);
    if (lenLineEnd == std::string::npos) return std::nullopt;

    int len = 0;
    try {
      len = std::stoi(buffer.substr(pos + 1, lenLineEnd - pos - 1));
    } catch (...) {
      buffer.clear();
      return std::nullopt;
    }
    size_t dataStart = lenLineEnd + 2;
    size_t dataEnd = dataStart + len;
    if (dataEnd + 2 > buffer.size()) return std::nullopt;  // need more bytes

    args.push_back(buffer.substr(dataStart, len));
    pos = dataEnd + 2;
  }

  buffer.erase(0, pos);
  return args;
}

// ---------------------------------------------------------------------------
// Bounded, thread-safe LRU key/value store with lazy expiry
// ---------------------------------------------------------------------------

class LruStore {
 public:
  explicit LruStore(size_t capacity) : capacity_(capacity) {}

  void set(const std::string &key, const std::string &value,
           std::optional<int64_t> ttlMillis) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<Clock::time_point> expiresAt;
    if (ttlMillis) expiresAt = Clock::now() + std::chrono::milliseconds(*ttlMillis);

    auto it = index_.find(key);
    if (it != index_.end()) {
      it->second->value = value;
      it->second->expiresAt = expiresAt;
      touch(it->second);
      return;
    }

    if (index_.size() >= capacity_) evictLru();

    order_.push_front(Entry{key, value, expiresAt});
    index_[key] = order_.begin();
  }

  std::optional<std::string> get(const std::string &key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(key);
    if (it == index_.end()) return std::nullopt;
    if (isExpired(it->second)) {
      order_.erase(it->second);
      index_.erase(it);
      return std::nullopt;
    }
    touch(it->second);
    return it->second->value;
  }

  bool remove(const std::string &key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(key);
    if (it == index_.end()) return false;
    order_.erase(it->second);
    index_.erase(it);
    return true;
  }

  std::vector<std::string> keys() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    result.reserve(index_.size());
    for (auto it = order_.begin(); it != order_.end(); ++it) {
      if (!isExpired(it)) result.push_back(it->key);
    }
    return result;
  }

  // Sweeps expired keys. Safe to call periodically from a background thread.
  void reapExpired() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = order_.begin(); it != order_.end();) {
      if (isExpired(it)) {
        auto toErase = it++;
        index_.erase(toErase->key);
        order_.erase(toErase);
      } else {
        ++it;
      }
    }
  }

 private:
  struct Entry {
    std::string key;
    std::string value;
    std::optional<Clock::time_point> expiresAt;
  };
  using Iter = std::list<Entry>::iterator;

  bool isExpired(const Iter &it) const {
    return it->expiresAt.has_value() && Clock::now() >= *it->expiresAt;
  }

  void touch(Iter &it) {
    order_.splice(order_.begin(), order_, it);
  }

  void evictLru() {
    if (order_.empty()) return;
    index_.erase(order_.back().key);
    order_.pop_back();
  }

  size_t capacity_;
  std::mutex mutex_;
  std::list<Entry> order_;  // front = most recently used
  std::unordered_map<std::string, Iter> index_;
};

// ---------------------------------------------------------------------------
// Server state
// ---------------------------------------------------------------------------

struct ServerState {
  uint16_t port = 6379;
  std::string role = "role:master";
  std::string masterHost;
  int masterPort = -1;
  std::string masterReplId = "8371b4fb1155b71f4a04d3e1bc3e18c4a990aeeb";
  int masterReplOffset = 0;
  std::string dir;
  std::string dbfilename;

  LruStore store{10000};  // default LRU capacity: 10k keys

  std::mutex replicaMutex;
  std::vector<socket_t> replicas;

  bool isMaster() const { return masterPort == -1; }
};

static ServerState g;

// A tiny, best-effort RDB reader: it only checks the file exists and is
// readable. Full RDB decoding is out of scope for this project, so the
// server intentionally starts with an empty dataset rather than
// fabricating data (as the previous implementation did).
static void loadRdbFile(const std::string &path) {
  std::ifstream rdb(path, std::ios::binary);
  if (!rdb) {
    std::cerr << "[rdb] no existing dump at " << path << ", starting empty\n";
    return;
  }
  std::cerr << "[rdb] found dump at " << path
            << " (full RDB parsing not implemented; starting empty)\n";
}

const std::string kEmptyRdb =
    "\x52\x45\x44\x49\x53\x30\x30\x31\x31\xfa\x09\x72\x65\x64\x69\x73\x2d\x76"
    "\x65\x72\x05\x37\x2e\x32\x2e\x30\xfa\x0a\x72\x65\x64\x69\x73\x2d\x62\x69"
    "\x74\x73\xc0\x40\xfa\x05\x63\x74\x69\x6d\x65\xc2\x6d\x08\xbc\x65\xfa\x08"
    "\x75\x73\x65\x64\x2d\x6d\x65\x6d\xc2\xb0\xc4\x10\x00\xfa\x08\x61\x6f\x66"
    "\x2d\x62\x61\x73\x65\xc0\x00\xff\xf0\x6e\x3b\xfe\xc0\xff\x5a\xa2";

static void propagateToReplicas(const std::vector<std::string> &args) {
  std::lock_guard<std::mutex> lock(g.replicaMutex);
  if (g.replicas.empty()) return;
  std::string encoded = respCommandArray(args);
  for (socket_t fd : g.replicas) socketSend(fd, encoded);
}

// ---------------------------------------------------------------------------
// Command dispatch
// ---------------------------------------------------------------------------

static std::string handleCommand(const std::vector<std::string> &args, socket_t clientFd) {
  if (args.empty()) return "";
  std::string cmd = toLower(args[0]);

  if (cmd == "ping") {
    return respSimpleString("PONG");
  }

  if (cmd == "echo") {
    if (args.size() < 2) return respError("wrong number of arguments for 'echo'");
    return respBulkString(args[1]);
  }

  if (cmd == "set") {
    if (args.size() < 3) return respError("wrong number of arguments for 'set'");
    std::optional<int64_t> ttlMillis;
    for (size_t i = 3; i + 1 < args.size(); i += 2) {
      std::string opt = toLower(args[i]);
      if (opt == "px") {
        ttlMillis = std::stoll(args[i + 1]);
      } else if (opt == "ex") {
        ttlMillis = std::stoll(args[i + 1]) * 1000;
      }
    }
    g.store.set(args[1], args[2], ttlMillis);
    if (g.isMaster()) propagateToReplicas(args);
    return respSimpleString("OK");
  }

  if (cmd == "get") {
    if (args.size() < 2) return respError("wrong number of arguments for 'get'");
    auto value = g.store.get(args[1]);
    return value ? respBulkString(*value) : respNilBulkString();
  }

  if (cmd == "del") {
    if (args.size() < 2) return respError("wrong number of arguments for 'del'");
    int removed = 0;
    for (size_t i = 1; i < args.size(); ++i) {
      if (g.store.remove(args[i])) ++removed;
    }
    if (g.isMaster()) propagateToReplicas(args);
    return ":" + std::to_string(removed) + "\r\n";
  }

  if (cmd == "keys") {
    return respArray(g.store.keys());
  }

  if (cmd == "info" && args.size() >= 2 && toLower(args[1]) == "replication") {
    std::string info = g.role + "\n" + "master_replid:" + g.masterReplId + "\n" +
                        "master_repl_offset:" + std::to_string(g.masterReplOffset) + "\n";
    return respBulkString(info);
  }

  if (cmd == "replconf") {
    if (args.size() >= 2 && toLower(args[1]) == "listening-port") {
      std::lock_guard<std::mutex> lock(g.replicaMutex);
      g.replicas.push_back(clientFd);
    }
    return respSimpleString("OK");
  }

  if (cmd == "psync") {
    std::string reply = "+FULLRESYNC " + g.masterReplId + " " +
                         std::to_string(g.masterReplOffset) + "\r\n";
    reply += "$" + std::to_string(kEmptyRdb.size()) + "\r\n" + kEmptyRdb;
    return reply;
  }

  if (cmd == "config" && args.size() >= 3 && toLower(args[1]) == "get") {
    std::string param = toLower(args[2]);
    if (param == "dir") return respArray({"dir", g.dir});
    if (param == "dbfilename") return respArray({"dbfilename", g.dbfilename});
    return respArray({});
  }

  return respError("unknown command '" + args[0] + "'");
}

// ---------------------------------------------------------------------------
// Connection handling
// ---------------------------------------------------------------------------

static void handleClient(socket_t clientFd) {
  std::string inbox;
  char buf[4096];

  while (true) {
    ssize_t n = socketRecv(clientFd, buf, sizeof(buf));
    if (n <= 0) break;
    inbox.append(buf, static_cast<size_t>(n));

    while (auto args = parseRespCommand(inbox)) {
      std::string reply = handleCommand(*args, clientFd);
      if (!reply.empty()) socketSend(clientFd, reply);
    }
  }

  {
    std::lock_guard<std::mutex> lock(g.replicaMutex);
    g.replicas.erase(std::remove(g.replicas.begin(), g.replicas.end(), clientFd),
                      g.replicas.end());
  }
  closeSocket(clientFd);
}

static void expiryReaperLoop() {
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    g.store.reapExpired();
  }
}

static void sendHandshake() {
  socket_t fd = connectTo(g.masterHost, g.masterPort);
  if (fd == kInvalidSocket) {
    std::cerr << "Replica failed to connect to master " << g.masterHost << ":" << g.masterPort
               << "\n";
    return;
  }

  char buf[1024];
  socketSend(fd, respCommandArray({"ping"}));
  socketRecv(fd, buf, sizeof(buf));

  socketSend(fd, respCommandArray({"REPLCONF", "listening-port", std::to_string(g.port)}));
  socketRecv(fd, buf, sizeof(buf));

  socketSend(fd, respCommandArray({"REPLCONF", "capa", "psync2"}));
  socketRecv(fd, buf, sizeof(buf));

  socketSend(fd, respCommandArray({"PSYNC", "?", "-1"}));
  socketRecv(fd, buf, sizeof(buf));

  closeSocket(fd);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
  std::cout << "Logs from your program will appear here!\n";

  if (!networkInit()) {
    std::cerr << "Failed to initialize networking\n";
    return 1;
  }

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      g.port = static_cast<uint16_t>(std::stoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
      g.dir = argv[++i];
    } else if (std::strcmp(argv[i], "--dbfilename") == 0 && i + 1 < argc) {
      g.dbfilename = argv[++i];
    } else if (std::strcmp(argv[i], "--replicaof") == 0 && i + 1 < argc) {
      g.role = "role:slave";
      std::string master = argv[++i];
      size_t spacePos = master.find(' ');
      g.masterHost = master.substr(0, spacePos);
      g.masterPort = std::stoi(master.substr(spacePos + 1));
    }
  }

  if (!g.dir.empty() && !g.dbfilename.empty()) {
    loadRdbFile(g.dir + "/" + g.dbfilename);
  }

  socket_t serverFd = socket(AF_INET, SOCK_STREAM, 0);
  if (serverFd == kInvalidSocket) {
    std::cerr << "Failed to create server socket\n";
    return 1;
  }

  int reuse = 1;
  setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse),
             sizeof(reuse));

  struct sockaddr_in serverAddr {};
  serverAddr.sin_family = AF_INET;
  serverAddr.sin_addr.s_addr = INADDR_ANY;
  serverAddr.sin_port = htons(g.port);

  if (bind(serverFd, reinterpret_cast<struct sockaddr *>(&serverAddr), sizeof(serverAddr)) != 0) {
    std::cerr << "Failed to bind to port " << g.port << "\n";
    return 1;
  }

  if (listen(serverFd, 16) != 0) {
    std::cerr << "listen failed\n";
    return 1;
  }

  std::thread(expiryReaperLoop).detach();

  if (!g.isMaster()) {
    std::thread(sendHandshake).detach();
  }

  std::cout << "Waiting for a client to connect on port " << g.port << "...\n";
  while (true) {
    struct sockaddr_in clientAddr {};
    socklen_t clientAddrLen = sizeof(clientAddr);
    socket_t clientFd =
        accept(serverFd, reinterpret_cast<struct sockaddr *>(&clientAddr), &clientAddrLen);
    if (clientFd == kInvalidSocket) continue;
    std::thread(handleClient, clientFd).detach();
  }

  closeSocket(serverFd);
  networkCleanup();
  return 0;
}
