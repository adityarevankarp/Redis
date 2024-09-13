# 🚀 **CrazyRedis** 🚀

### Your ultra-lightweight, super-fast, and *insanely crazy* Redis-like server! 

**CrazyRedis** is a highly experimental, yet bizarrely stable implementation of a Redis-like in-memory key-value store with a built-in LRU cache and basic replication support. It's crazy-fast, crazy-efficient, and well... just **crazy**!

#### ⚡ **Crazy Features** ⚡
- **In-Memory Key-Value Store** 🧠: Just like Redis! Store all your `SET` and `GET` key-value pairs in RAM.
- **Built-in LRU Caching** 💡: You thought your keys would live forever? Think again! Least Recently Used items get kicked out when the cache is full! Because life is too short to remember everything.
- **Basic Replication** 👥: You can be the master! Or the replica! Why not both? Sync your data between servers like a boss.
- **Blazing Fast PING/ECHO** 🏎️: Responds to your pings faster than your mom asking you to do the dishes. `PING` for instant gratification! `ECHO` to hear yourself shout into the void!
- **Configurable Commands** 📦: We don't limit your crazy ideas—customize your Redis clone with easy-to-use `CONFIG` commands. 
- **Built with C++**, so fast it'll make your head spin! 💨

---

## 🤯 **Why Use CrazyRedis?** 🤯

If you want a **crazy**, minimalist alternative to Redis for your low-key projects and don't mind living on the edge, this is the perfect fit for you. Use it in your side projects, test environments, or when you want to impress your friends with your own DIY Redis clone.

## ⚡ **How It Works** ⚡

1. **Start the Server** 🖥️:
```bash
./crazyredis <port>
```
This will spin up a server on the port you specify. Don't be shy—give it a weird port number. Your server doesn't care.

2. **PING the Server** 💥:
```bash
$ redis-cli
> PING
+PONG
```
3. **Set and Get Keys** 🗝️:
```bash
> SET foo bar
+OK
> GET foo
$3
bar
```
4. **Experience LRU Madness** 🧠:
- When the cache limit is hit, the least recently used key goes poof! 🌪️

# 🛠️ Installation 🛠️
You'll need:

- A C++11 or higher compatible compiler (we're not that crazy).
- POSIX-compliant system (like Linux or WSL—none of that non-sense for CrazyRedis!).
- Some guts to run this crazy project.

## Step 1: Clone the Repo! 🌀
```bash
git clone https://github.com/yourusername/CrazyRedis.git
cd CrazyRedis
```
## Step 2: Build it! 🔨
```bash
g++ -std=c++11 -o crazyredis crazyredis.cpp
```
# 🤖 Commands Supported 🤖
- `PING`: Test if the server is awake (Spoiler: It is!).
- `ECHO` <message>: Repeat whatever you say. Instant validation.
- `SET` <key> <value>: Set a key-value pair.
- `GET` <key>: Retrieve the value of a key.
- `LRU` MAGIC 🧙‍♂️: Caches the most recent SET keys—evicts the old-timers when cache limit is reached!
