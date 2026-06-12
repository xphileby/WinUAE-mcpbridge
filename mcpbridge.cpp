// MCP bridge for WinUAE - step 2.
// Listens on 0.0.0.0:7843, single client, NDJSON over TCP.
// JSON-RPC 2.0 dispatcher implementing MCP initialize/tools/list/tools/call.
//
// Input tools (this step):
//   type_text, mouse_move, mouse_button, mouse_click, mouse_scroll,
//   key_down, key_up, key_press, mousehack_status, get_screen_geometry
//
// Drained on the emulation thread via mcpbridge_drain() (called from
// inputdevice_read()).

// winsock2/ws2tcpip must come before sysdeps.h, which pulls in <winsock.h> v1.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "sysconfig.h"
#include "sysdeps.h"
#include "options.h"
#include "uae.h"
#include "keybuf.h"
#include "inputdevice.h"
#include "xwin.h"
#include "picasso96.h"
#include "disk.h"
#include "savestate.h"
#include "memory.h"
#include "newcpu.h"
#include "debug.h"
#include "mcpbridge.h"

// jsmn: include once with JSMN_STATIC so functions are file-local.
#define JSMN_STATIC
#include "jsmn.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

// Defined in od-win32/screenshot.cpp.
extern int screenshot_capture_png(int monid, int imagemode,
    uae_u8 **out_data, size_t *out_len, int *out_w, int *out_h);

namespace {

// ---------------------------------------------------------------------------
// Command queue (producer = receiver thread, consumer = emu thread)
// ---------------------------------------------------------------------------

enum cmd_kind {
    CMD_TYPE_TEXT,           // text
    CMD_MOUSE_MOVE_AMIGA,    // a=x, b=y
    CMD_MOUSE_MOVE_HOST,     // a=x, b=y
    CMD_MOUSE_BUTTON,        // a=button, b=state
    CMD_MOUSE_SCROLL,        // a=dx, b=dy
    CMD_KEY_RAW,             // a=amiga_scancode, b=state (1=down,0=up)
    CMD_RESET,               // a=hardreset, b=keyboardreset
    CMD_PAUSE,               // a=mode (0=resume, 1=pause, -1=toggle)
    CMD_SAVE_STATE,          // text=path
    CMD_LOAD_STATE,          // text=path
    CMD_DISK_INSERT,         // a=drive, text=path
    CMD_DISK_EJECT,          // a=drive
    CMD_SET_CONFIG,          // text=cfgfile line "key=value"
    CMD_MEMORY_WRITE,        // a=addr, text=raw bytes (NOT base64, already decoded)
    CMD_DEBUG,               // text=command; reply via pending_reply
    CMD_DISASSEMBLE,         // a=addr, b=count; reply via pending_reply
    CMD_SCREENSHOT,          // a=imagemode (0=host, 1=native); reply via pending_reply
};

struct mcp_cmd {
    cmd_kind kind;
    int a = 0;
    int b = 0;
    std::string text;
    int reply_id = 0;       // 0 = no reply expected; >0 = key into g_pending
};

// Synchronous-call plumbing: receiver thread enqueues a command and waits
// for the emu thread to fill in the reply, then writes the JSON response.
struct pending_reply {
    std::string result;
    bool done = false;
};
std::map<int, std::shared_ptr<pending_reply>> g_pending;
std::mutex g_pending_mtx;
std::condition_variable g_pending_cv;
std::atomic<int> g_pending_seq{1};

int new_pending() {
    int id = g_pending_seq.fetch_add(1, std::memory_order_relaxed);
    auto pr = std::make_shared<pending_reply>();
    std::lock_guard<std::mutex> g(g_pending_mtx);
    g_pending[id] = pr;
    return id;
}
std::shared_ptr<pending_reply> get_pending(int id) {
    std::lock_guard<std::mutex> g(g_pending_mtx);
    auto it = g_pending.find(id);
    return (it == g_pending.end()) ? nullptr : it->second;
}
void drop_pending(int id) {
    std::lock_guard<std::mutex> g(g_pending_mtx);
    g_pending.erase(id);
}
// Wait up to `timeout_ms` for the reply. Returns true on success.
bool wait_pending(int id, std::string &out, int timeout_ms = 5000) {
    auto pr = get_pending(id);
    if (!pr) return false;
    std::unique_lock<std::mutex> lk(g_pending_mtx);
    g_pending_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                          [&] { return pr->done; });
    if (!pr->done) return false;
    out = std::move(pr->result);
    return true;
}
// Called from the emu-thread drain after running the work.
void deliver_pending(int id, std::string result) {
    auto pr = get_pending(id);
    if (!pr) return;
    {
        std::lock_guard<std::mutex> g(g_pending_mtx);
        pr->result = std::move(result);
        pr->done = true;
    }
    g_pending_cv.notify_all();
}

std::atomic<bool> g_running{false};
std::atomic<bool> g_client_connected{false};
SOCKET g_listen_sock = INVALID_SOCKET;
std::thread g_accept_thr;
std::mutex g_q_mtx;
std::deque<mcp_cmd> g_q;

// ---------------------------------------------------------------------------
// Paced mouse action queue.
//
// Why this exists: without the guest-side mousehack, pointer motion happens
// through the emulated 8-bit MOUSE0DAT hardware counters. The AmigaOS mouse
// driver reads them once per vsync and interprets the difference as signed
// 8-bit, so more than ~127 counts accumulated within one frame alias (e.g.
// +200 reads back as -56). The drain below therefore emits at most one small
// chunk (MOUSE_CHUNK) per drain tick. maybe_read_input() fires the drain up
// to ~3x per frame, so 3 * MOUSE_CHUNK must stay below 127.
//
// Button events go through the same queue so a click queued after a motion
// executes only once the pointer has arrived. delay_ticks spaces button
// transitions far enough apart that the guest's per-vsync input poll is
// guaranteed to observe each state.
// ---------------------------------------------------------------------------

#define MOUSE_CHUNK 40

struct mouse_action {
    int kind;        // 0 = relative motion, 1 = button event
    int dx = 0, dy = 0;          // motion remainder
    int button = 0, state = 0;   // button event
    int delay_ticks = 0;         // ticks to wait before this action runs
    int reply_id = 0;            // pending-reply delivered when action completes
};
std::deque<mouse_action> g_mouse_q;   // guarded by g_q_mtx

void enqueue_mouse(mouse_action &&a)
{
    std::lock_guard<std::mutex> g(g_q_mtx);
    g_mouse_q.emplace_back(std::move(a));
}

// One tick of the mouse engine; called from mcpbridge_drain on the emu thread.
void mouse_engine_tick()
{
    int finished_reply = 0;
    {
        std::unique_lock<std::mutex> lk(g_q_mtx, std::try_to_lock);
        if (!lk.owns_lock() || g_mouse_q.empty())
            return;
        mouse_action &a = g_mouse_q.front();
        if (a.delay_ticks > 0) {
            a.delay_ticks--;
            return;
        }
        if (a.kind == 0) {
            int sx = a.dx > MOUSE_CHUNK ? MOUSE_CHUNK : (a.dx < -MOUSE_CHUNK ? -MOUSE_CHUNK : a.dx);
            int sy = a.dy > MOUSE_CHUNK ? MOUSE_CHUNK : (a.dy < -MOUSE_CHUNK ? -MOUSE_CHUNK : a.dy);
            if (sx) setmousestate(0, 0, sx, 0);
            if (sy) setmousestate(0, 1, sy, 0);
            a.dx -= sx;
            a.dy -= sy;
            if (a.dx == 0 && a.dy == 0) {
                finished_reply = a.reply_id;
                g_mouse_q.pop_front();
            }
        } else {
            setmousebuttonstate(0, a.button, a.state);
            finished_reply = a.reply_id;
            g_mouse_q.pop_front();
        }
    }
    if (finished_reply)
        deliver_pending(finished_reply, "done");
}

// Writer side: all socket sends funnel through this thread so responses and
// spontaneous notifications never interleave at the byte level. The active
// socket pointer is set by handle_client on connect, cleared on disconnect.
std::atomic<SOCKET> g_active_sock{INVALID_SOCKET};
std::mutex g_send_mtx;
std::deque<std::string> g_send_q;          // serialized frames, no trailing \n
std::condition_variable g_send_cv;
std::thread g_writer_thr;

void log_msg(const TCHAR *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    TCHAR buf[512];
    _vsntprintf(buf, sizeof(buf)/sizeof(buf[0]) - 1, fmt, ap);
    buf[sizeof(buf)/sizeof(buf[0]) - 1] = 0;
    va_end(ap);
    write_log(_T("[mcpbridge] %s"), buf);
}

void enqueue(mcp_cmd &&c)
{
    std::lock_guard<std::mutex> g(g_q_mtx);
    g_q.emplace_back(std::move(c));
}

// UTF-8 (or ASCII) std::string -> wchar_t buffer for WinUAE's TCHAR APIs.
std::wstring widen(const std::string &s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string narrow(const wchar_t *w)
{
    if (!w || !*w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(n - 1, '\0'); // n includes terminator
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// RFC 4648 base64 encode/decode. Tiny, no allocator hooks.
const char B64_ENC[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string b64_encode(const uint8_t *p, size_t n)
{
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = ((uint32_t)p[i] << 16);
        int pad = 2;
        if (i + 1 < n) { v |= ((uint32_t)p[i+1] << 8); pad = 1; }
        if (i + 2 < n) { v |=  (uint32_t)p[i+2];        pad = 0; }
        out.push_back(B64_ENC[(v >> 18) & 0x3f]);
        out.push_back(B64_ENC[(v >> 12) & 0x3f]);
        out.push_back(pad >= 2 ? '=' : B64_ENC[(v >> 6) & 0x3f]);
        out.push_back(pad >= 1 ? '=' : B64_ENC[v & 0x3f]);
    }
    return out;
}
bool b64_decode(const std::string &s, std::string &out)
{
    static int8_t T[256]; static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) T[i] = -1;
        for (int i = 0; i < 64; ++i) T[(uint8_t)B64_ENC[i]] = (int8_t)i;
        T[(uint8_t)'='] = 0;
        init = true;
    }
    out.clear();
    int v = 0, bits = 0;
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        int8_t t = T[(uint8_t)c];
        if (t < 0) return false;
        v = (v << 6) | t;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (c != '=') out.push_back((char)((v >> bits) & 0xff));
        }
    }
    return true;
}

// Hex parse: accept 0x prefixed or bare, signed or unsigned, returns uint32.
bool parse_addr(const std::string &s, uint32_t &out)
{
    if (s.empty()) return false;
    const char *p = s.c_str();
    int base = 10;
    if (s.size() > 1 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16; p += 2;
    } else if (s.size() > 1 && p[0] == '$') {
        base = 16; p += 1;
    }
    char *end = nullptr;
    unsigned long v = strtoul(p, &end, base);
    if (end == p) return false;
    out = (uint32_t)v;
    return true;
}

// ---------------------------------------------------------------------------
// JSON helpers (built on jsmn token arrays)
// ---------------------------------------------------------------------------

bool tok_eq(const char *js, const jsmntok_t &t, const char *s)
{
    int len = t.end - t.start;
    return (int)strlen(s) == len && strncmp(js + t.start, s, len) == 0;
}

std::string tok_str(const char *js, const jsmntok_t &t)
{
    return std::string(js + t.start, t.end - t.start);
}

// Tokens follow the same flat ordering jsmn emits.
// For an OBJECT token at index i, its children are key,value pairs
// occupying the next 2*size tokens (with their own subtrees).
// Returns the next-sibling index of token i.
int tok_skip(const jsmntok_t *toks, int i)
{
    int n = 1;
    if (toks[i].type == JSMN_OBJECT) {
        int kids = toks[i].size;
        int idx = i + 1;
        for (int k = 0; k < kids; ++k) {
            idx = tok_skip(toks, idx); // key
            idx = tok_skip(toks, idx); // value
        }
        return idx;
    } else if (toks[i].type == JSMN_ARRAY) {
        int kids = toks[i].size;
        int idx = i + 1;
        for (int k = 0; k < kids; ++k)
            idx = tok_skip(toks, idx);
        return idx;
    }
    (void)n;
    return i + 1;
}

// Find direct child of object token `obj_idx` whose key string == `key`.
// Returns the value's token index, or -1.
int obj_find(const char *js, const jsmntok_t *toks, int obj_idx, const char *key)
{
    if (obj_idx < 0) return -1;
    if (toks[obj_idx].type != JSMN_OBJECT) return -1;
    int kids = toks[obj_idx].size;
    int idx = obj_idx + 1;
    for (int k = 0; k < kids; ++k) {
        if (toks[idx].type == JSMN_STRING && tok_eq(js, toks[idx], key))
            return idx + 1;
        idx = tok_skip(toks, idx);     // skip key
        idx = tok_skip(toks, idx);     // skip value
    }
    return -1;
}

// Convenience: pull a string from object.field, returns "" if missing.
std::string obj_str(const char *js, const jsmntok_t *toks, int obj_idx, const char *key)
{
    int i = obj_find(js, toks, obj_idx, key);
    if (i < 0 || toks[i].type != JSMN_STRING) return std::string();
    // jsmn returns raw bytes; we do NOT unescape backslash sequences here.
    // For the keys/values our tools accept (ASCII keynames, short text), this is fine.
    // type_text users that need control characters should send raw bytes.
    return tok_str(js, toks[i]);
}

// Convenience: pull an int from object.field, returns dflt if missing/invalid.
int obj_int(const char *js, const jsmntok_t *toks, int obj_idx, const char *key, int dflt)
{
    int i = obj_find(js, toks, obj_idx, key);
    if (i < 0 || toks[i].type != JSMN_PRIMITIVE) return dflt;
    std::string s = tok_str(js, toks[i]);
    if (s.empty() || s == "null" || s == "true" || s == "false") return dflt;
    return atoi(s.c_str());
}

// Address-style getter: accepts either {"addr": 0x4} (JSON number) or
// {"addr": "0x4"} / {"addr": "$4"} / {"addr": "4"} (string). The string
// form is needed because JSON numbers can't fully represent 32-bit
// addresses unambiguously in some clients.
bool obj_addr(const char *js, const jsmntok_t *toks, int obj_idx,
              const char *key, uint32_t &out)
{
    int i = obj_find(js, toks, obj_idx, key);
    if (i < 0) return false;
    std::string s = tok_str(js, toks[i]);
    return parse_addr(s, out);
}

// ---------------------------------------------------------------------------
// Outgoing message: JSON serialization helpers
// ---------------------------------------------------------------------------

std::string esc_json(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += (char)c;
            }
        }
    }
    return out;
}

// Send a complete line via the writer thread (queues, returns immediately).
// Old "SOCKET s" parameter retained for the call sites in the handler but
// the socket itself is read from g_active_sock at send time.
void send_line(SOCKET /*ignored*/, const std::string &line)
{
    {
        std::lock_guard<std::mutex> g(g_send_mtx);
        g_send_q.push_back(line);
    }
    g_send_cv.notify_one();
}

// Notification helper: only fires when a client is attached.
void emit_notification(const char *method, const std::string &params_json)
{
    if (g_active_sock.load(std::memory_order_acquire) == INVALID_SOCKET) return;
    std::string out = "{\"jsonrpc\":\"2.0\",\"method\":\"";
    out += method;
    out += "\",\"params\":";
    out += params_json;
    out += "}";
    send_line(INVALID_SOCKET, out);
}

// id_raw is the JSON literal for the id field (e.g. "1", "\"abc\"", "null").
void send_response(SOCKET s, const std::string &id_raw, const std::string &result_obj)
{
    std::string out = "{\"jsonrpc\":\"2.0\",\"id\":";
    out += id_raw;
    out += ",\"result\":";
    out += result_obj;
    out += "}";
    send_line(s, out);
}

void send_error(SOCKET s, const std::string &id_raw, int code, const std::string &msg)
{
    char head[64];
    snprintf(head, sizeof(head), "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":\"",
             id_raw.c_str(), code);
    std::string out = head;
    out += esc_json(msg);
    out += "\"}}";
    send_line(s, out);
}

// Pack {"content":[{"type":"text","text":"<msg>"}]} into a result object.
std::string text_result(const std::string &msg)
{
    std::string out = "{\"content\":[{\"type\":\"text\",\"text\":\"";
    out += esc_json(msg);
    out += "\"}]}";
    return out;
}

// ---------------------------------------------------------------------------
// Symbolic key name -> Amiga scancode
// ---------------------------------------------------------------------------

struct keyent { const char *name; int scancode; };

// Names are matched case-insensitively. Multiple aliases per key are fine.
const keyent KEY_TABLE[] = {
    // Letters
    {"a", 0x20}, {"b", 0x35}, {"c", 0x33}, {"d", 0x22}, {"e", 0x12},
    {"f", 0x23}, {"g", 0x24}, {"h", 0x25}, {"i", 0x17}, {"j", 0x26},
    {"k", 0x27}, {"l", 0x28}, {"m", 0x37}, {"n", 0x36}, {"o", 0x18},
    {"p", 0x19}, {"q", 0x10}, {"r", 0x13}, {"s", 0x21}, {"t", 0x14},
    {"u", 0x16}, {"v", 0x34}, {"w", 0x11}, {"x", 0x32}, {"y", 0x15},
    {"z", 0x31},
    // Digits (main row)
    {"0", 0x0a}, {"1", 0x01}, {"2", 0x02}, {"3", 0x03}, {"4", 0x04},
    {"5", 0x05}, {"6", 0x06}, {"7", 0x07}, {"8", 0x08}, {"9", 0x09},
    // Symbols on main keyboard
    {"backtick", 0x00}, {"grave", 0x00}, {"`", 0x00},
    {"minus", 0x0b}, {"-", 0x0b},
    {"equal", 0x0c}, {"=", 0x0c},
    {"backslash", 0x0d}, {"\\", 0x0d},
    {"leftbracket", 0x1a}, {"[", 0x1a},
    {"rightbracket", 0x1b}, {"]", 0x1b},
    {"semicolon", 0x29}, {";", 0x29},
    {"quote", 0x2b}, {"apostrophe", 0x2b}, {"'", 0x2b},
    {"comma", 0x38}, {",", 0x38},
    {"period", 0x39}, {".", 0x39},
    {"slash", 0x3a}, {"/", 0x3a},
    // Action keys
    {"space", 0x40}, {" ", 0x40},
    {"backspace", 0x41},
    {"tab", 0x42},
    {"enter", 0x44}, {"return", 0x44},
    {"numericenter", 0x43}, {"keypadenter", 0x43},
    {"escape", 0x45}, {"esc", 0x45},
    {"delete", 0x46}, {"del", 0x46},
    {"help", 0x5f},
    // Arrows
    {"up", 0x4c}, {"down", 0x4d}, {"right", 0x4e}, {"left", 0x4f},
    // Function keys
    {"f1", 0x50}, {"f2", 0x51}, {"f3", 0x52}, {"f4", 0x53}, {"f5", 0x54},
    {"f6", 0x55}, {"f7", 0x56}, {"f8", 0x57}, {"f9", 0x58}, {"f10", 0x59},
    // Modifiers
    {"lshift", 0x60}, {"leftshift", 0x60}, {"shift", 0x60},
    {"rshift", 0x61}, {"rightshift", 0x61},
    {"capslock", 0x62}, {"caps", 0x62},
    {"ctrl", 0x63}, {"control", 0x63}, {"lctrl", 0x63},
    {"lalt", 0x64}, {"leftalt", 0x64}, {"alt", 0x64},
    {"ralt", 0x65}, {"rightalt", 0x65},
    {"lamiga", 0x66}, {"leftamiga", 0x66}, {"lmeta", 0x66}, {"lwin", 0x66},
    {"ramiga", 0x67}, {"rightamiga", 0x67}, {"rmeta", 0x67}, {"rwin", 0x67},
};

int key_lookup(const std::string &name)
{
    if (name.empty()) return -1;
    // Numeric? Accept "0x20" or decimal 0..0x7f as raw scancode.
    if (name.size() > 1 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        int v = (int)strtol(name.c_str(), nullptr, 16);
        if (v >= 0 && v <= 0x7f) return v;
        return -1;
    }
    // Case-insensitive lookup.
    std::string lc = name;
    for (auto &c : lc) c = (char)tolower((unsigned char)c);
    for (auto &e : KEY_TABLE)
        if (lc == e.name) return e.scancode;
    return -1;
}

// ---------------------------------------------------------------------------
// Tool dispatch (called from receiver thread; queues work for emu thread)
// ---------------------------------------------------------------------------

struct dispatch_result {
    bool ok;
    std::string result_or_msg;   // result_obj on ok=true; error msg on ok=false
    int error_code;
};

dispatch_result dispatch_tool(const std::string &name, const char *js,
                              const jsmntok_t *toks, int args_idx)
{
    auto err = [](const std::string &m, int c = -32602) {
        return dispatch_result{false, m, c};
    };
    auto ok_text = [](const std::string &m) {
        return dispatch_result{true, text_result(m), 0};
    };
    auto ok_raw = [](const std::string &r) {
        return dispatch_result{true, r, 0};
    };

    // -- type_text -------------------------------------------------------
    if (name == "type_text") {
        std::string txt = obj_str(js, toks, args_idx, "text");
        if (txt.empty()) return err("missing or empty 'text'");
        mcp_cmd c;
        c.kind = CMD_TYPE_TEXT;
        c.text = txt;
        enqueue(std::move(c));
        char buf[64];
        snprintf(buf, sizeof(buf), "queued %zu char(s)", txt.size());
        return ok_text(buf);
    }

    // -- get_pointer_pos -------------------------------------------------
    // Live Intuition pointer position (guest screen pixels). Mode-independent.
    if (name == "get_pointer_pos") {
        int px = 0, py = 0;
        bool ok = mcpbridge_get_pointer_pos(&px, &py);
        char buf[96];
        snprintf(buf, sizeof(buf), "{\"available\":%s,\"x\":%d,\"y\":%d}",
                 ok ? "true" : "false", px, py);
        std::string out = "{\"content\":[{\"type\":\"text\",\"text\":\"";
        out += esc_json(buf);
        out += "\"}]}";
        return ok_raw(out);
    }

    // -- mouse_move ------------------------------------------------------
    // space:'host' (default): true absolute positioning in Intuition screen
    // pixels via CLOSED LOOP — reads the live pointer position and corrects,
    // so it is immune to resolution / interlace / centering changes. Falls
    // back to open-loop pin+walk if Intuition position is unavailable.
    // space:'amiga': legacy direct rtarea write (needs mousehack alive).
    if (name == "mouse_move") {
        int x = obj_int(js, toks, args_idx, "x", INT_MIN);
        int y = obj_int(js, toks, args_idx, "y", INT_MIN);
        if (x == INT_MIN || y == INT_MIN) return err("require 'x' and 'y'");
        std::string space = obj_str(js, toks, args_idx, "space");
        if (space == "amiga") {
            mcp_cmd c;
            c.kind = CMD_MOUSE_MOVE_AMIGA;
            c.a = x; c.b = y;
            enqueue(std::move(c));
            char buf[96];
            snprintf(buf, sizeof(buf), "queued mouse_move x=%d y=%d space=amiga", x, y);
            return ok_text(buf);
        }
        if (x < 0 || x > 4096 || y < 0 || y > 4096)
            return err("'x'/'y' must be 0..4096");

        int cx, cy;
        bool have_pos = mcpbridge_get_pointer_pos(&cx, &cy);
        if (have_pos) {
            // Closed loop. After each correction we settle ~30ms so Intuition's
            // input interrupt has updated MouseX/Y before we re-read (otherwise
            // a stale read looks like non-convergence). Stops on exact hit,
            // when within 1px, or when two settled reads stop improving.
            int prev_err = INT_MAX;
            int stuck = 0;
            for (int pass = 0; pass < 10; ++pass) {
                int dx = x - cx, dy = y - cy;
                if (dx == 0 && dy == 0) break;
                int rid = new_pending();
                { mouse_action a; a.kind = 0; a.dx = dx; a.dy = dy; a.reply_id = rid; enqueue_mouse(std::move(a)); }
                std::string done;
                if (!wait_pending(rid, done, 30000)) { drop_pending(rid); return err("mouse_move timed out"); }
                drop_pending(rid);
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                if (!mcpbridge_get_pointer_pos(&cx, &cy)) break;
                int err_now = abs(x - cx) + abs(y - cy);
                if (err_now <= 1) break;
                if (err_now >= prev_err) { if (++stuck >= 2) break; }
                else stuck = 0;
                prev_err = err_now;
            }
            char buf[96];
            snprintf(buf, sizeof(buf), "pointer at (%d,%d), target (%d,%d)", cx, cy, x, y);
            return ok_text(buf);
        }

        // Open-loop fallback: pin to corner then walk.
        int rid = new_pending();
        { mouse_action a; a.kind = 0; a.dx = -2400; a.dy = -2400; enqueue_mouse(std::move(a)); }
        { mouse_action a; a.kind = 0; a.dx = x; a.dy = y; a.reply_id = rid; enqueue_mouse(std::move(a)); }
        std::string done;
        if (!wait_pending(rid, done, 30000)) { drop_pending(rid); return err("mouse_move did not complete"); }
        drop_pending(rid);
        char buf[112];
        snprintf(buf, sizeof(buf), "pointer ~(%d,%d) (open-loop; Intuition pos unavailable)", x, y);
        return ok_text(buf);
    }

    // -- mouse_move_rel ---------------------------------------------------
    // Paced relative motion; blocks until the full delta has been applied.
    if (name == "mouse_move_rel") {
        int dx = obj_int(js, toks, args_idx, "dx", 0);
        int dy = obj_int(js, toks, args_idx, "dy", 0);
        if (dx == 0 && dy == 0) return err("provide non-zero 'dx' and/or 'dy'");
        if (dx < -4096 || dx > 4096 || dy < -4096 || dy > 4096)
            return err("'dx'/'dy' must be -4096..4096");
        int rid = new_pending();
        { mouse_action a; a.kind = 0; a.dx = dx; a.dy = dy; a.reply_id = rid; enqueue_mouse(std::move(a)); }
        std::string done;
        if (!wait_pending(rid, done, 30000)) {
            drop_pending(rid);
            return err("mouse_move_rel did not complete (is emulation running?)");
        }
        drop_pending(rid);
        char buf[64];
        snprintf(buf, sizeof(buf), "moved by (%d,%d)", dx, dy);
        return ok_text(buf);
    }

    // -- mouse_button ----------------------------------------------------
    // Routed through the mouse queue so it executes AFTER any queued motion.
    if (name == "mouse_button") {
        int button = obj_int(js, toks, args_idx, "button", 0);
        int state  = obj_int(js, toks, args_idx, "state", -1);
        if (state != 0 && state != 1) return err("'state' must be 0 (up) or 1 (down)");
        if (button < 0 || button > 7) return err("'button' out of range");
        int rid = new_pending();
        { mouse_action a; a.kind = 1; a.button = button; a.state = state; a.reply_id = rid; enqueue_mouse(std::move(a)); }
        std::string done;
        if (!wait_pending(rid, done, 15000)) {
            drop_pending(rid);
            return err("mouse_button did not complete");
        }
        drop_pending(rid);
        char buf[64];
        snprintf(buf, sizeof(buf), "button %d %s", button, state ? "down" : "up");
        return ok_text(buf);
    }

    // -- mouse_click -----------------------------------------------------
    // Paced press/release pairs; delay_ticks guarantees the guest's
    // per-vsync poll observes every transition. Blocks until the last
    // release has executed.
    if (name == "mouse_click") {
        int button = obj_int(js, toks, args_idx, "button", 0);
        int count  = obj_int(js, toks, args_idx, "count", 1);
        if (button < 0 || button > 7) return err("'button' out of range");
        if (count < 1 || count > 4)   return err("'count' must be 1..4");
        // Tunable pacing (ticks; ~6.7ms each). press = button-down duration,
        // gap = up-to-next-down spacing within a multi-click.
        int press_t = obj_int(js, toks, args_idx, "press_ticks", 6);
        int gap_t   = obj_int(js, toks, args_idx, "gap_ticks", 6);
        if (press_t < 1) press_t = 1; if (press_t > 60) press_t = 60;
        if (gap_t < 1) gap_t = 1;     if (gap_t > 60) gap_t = 60;
        int rid = new_pending();
        for (int i = 0; i < count; ++i) {
            mouse_action d; d.kind = 1; d.button = button; d.state = 1;
            d.delay_ticks = (i == 0) ? 0 : gap_t;   // gap between clicks
            enqueue_mouse(std::move(d));
            mouse_action u; u.kind = 1; u.button = button; u.state = 0;
            u.delay_ticks = press_t;                // press duration
            if (i == count - 1) u.reply_id = rid;
            enqueue_mouse(std::move(u));
        }
        std::string done;
        if (!wait_pending(rid, done, 15000)) {
            drop_pending(rid);
            return err("mouse_click did not complete");
        }
        drop_pending(rid);
        char buf[64];
        snprintf(buf, sizeof(buf), "%d click(s) on button %d done", count, button);
        return ok_text(buf);
    }

    // -- mouse_scroll ----------------------------------------------------
    if (name == "mouse_scroll") {
        int dx = obj_int(js, toks, args_idx, "dx", 0);
        int dy = obj_int(js, toks, args_idx, "dy", 0);
        if (dx == 0 && dy == 0) return err("provide non-zero 'dx' and/or 'dy'");
        mcp_cmd c; c.kind = CMD_MOUSE_SCROLL; c.a = dx; c.b = dy; enqueue(std::move(c));
        char buf[64]; snprintf(buf, sizeof(buf), "queued scroll dx=%d dy=%d", dx, dy);
        return ok_text(buf);
    }

    // -- key_down / key_up ----------------------------------------------
    if (name == "key_down" || name == "key_up") {
        std::string k = obj_str(js, toks, args_idx, "key");
        int sc = key_lookup(k);
        if (sc < 0) return err("unknown 'key': " + k);
        mcp_cmd c; c.kind = CMD_KEY_RAW; c.a = sc; c.b = (name == "key_down") ? 1 : 0;
        enqueue(std::move(c));
        char buf[96];
        snprintf(buf, sizeof(buf), "queued %s key=%s sc=0x%02x", name.c_str(), k.c_str(), sc);
        return ok_text(buf);
    }

    // -- key_press (down + up, with optional modifiers) -----------------
    if (name == "key_press") {
        std::string k = obj_str(js, toks, args_idx, "key");
        int sc = key_lookup(k);
        if (sc < 0) return err("unknown 'key': " + k);
        // Modifiers come as a JSON array of strings under "modifiers".
        std::vector<int> mods;
        int mods_idx = obj_find(js, toks, args_idx, "modifiers");
        if (mods_idx >= 0 && toks[mods_idx].type == JSMN_ARRAY) {
            int n = toks[mods_idx].size;
            int idx = mods_idx + 1;
            for (int i = 0; i < n; ++i) {
                if (toks[idx].type == JSMN_STRING) {
                    int m = key_lookup(tok_str(js, toks[idx]));
                    if (m < 0) return err("unknown modifier: " + tok_str(js, toks[idx]));
                    mods.push_back(m);
                }
                idx = tok_skip(toks, idx);
            }
        }
        // Press modifiers, press key, release key, release modifiers (LIFO).
        for (int m : mods) {
            mcp_cmd c; c.kind = CMD_KEY_RAW; c.a = m; c.b = 1; enqueue(std::move(c));
        }
        { mcp_cmd c; c.kind = CMD_KEY_RAW; c.a = sc; c.b = 1; enqueue(std::move(c)); }
        { mcp_cmd c; c.kind = CMD_KEY_RAW; c.a = sc; c.b = 0; enqueue(std::move(c)); }
        for (auto it = mods.rbegin(); it != mods.rend(); ++it) {
            mcp_cmd c; c.kind = CMD_KEY_RAW; c.a = *it; c.b = 0; enqueue(std::move(c));
        }
        char buf[96];
        snprintf(buf, sizeof(buf), "queued key_press key=%s (sc=0x%02x) +%zu modifier(s)",
                 k.c_str(), sc, mods.size());
        return ok_text(buf);
    }

    // -- mousehack_status (read-only; safe from receiver thread) --------
    if (name == "mousehack_status") {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "{\"input_tablet\":%d,\"mousehack_alive\":%d,\"magic_mouse\":%s}",
                 currprefs.input_tablet,
                 mousehack_alive(),
                 (currprefs.input_mouse_untrap & MOUSEUNTRAP_MAGIC) ? "true" : "false");
        std::string out = "{\"content\":[{\"type\":\"text\",\"text\":\"";
        out += esc_json(buf);
        out += "\"}],\"isError\":false}";
        return ok_raw(out);
    }

    // -- reset (soft/hard/keyboard) -------------------------------------
    // uae_reset() just sets the quit_program flag for the main loop.
    // Safe to call from receiver thread, and we want it to work even when
    // paused (so the user can recover from a wedged guest).
    if (name == "reset") {
        std::string kind = obj_str(js, toks, args_idx, "kind");
        if (kind.empty()) kind = "soft";
        int hard = 0, kbd = 0;
        if (kind == "soft")          { hard = 0; kbd = 0; }
        else if (kind == "hard")     { hard = 1; kbd = 1; }
        else if (kind == "keyboard") { hard = 0; kbd = 1; }
        else return err("'kind' must be soft|hard|keyboard");
        uae_reset(hard, kbd);
        emit_notification("notifications/reset",
            std::string("{\"kind\":\"") + kind + "\"}");
        return ok_text(std::string("reset kind=") + kind);
    }

    // -- pause / resume / toggle_pause / is_paused ---------------------
    // pausemode() sets a flag the emulation loop reads. MUST be immediate
    // (not queued via drain), because drain only runs while the emulation
    // is unpaused — a queued resume would never execute.
    if (name == "pause") {
        pausemode(1);
        emit_notification("notifications/paused", "{}");
        return ok_text("paused");
    }
    if (name == "resume") {
        pausemode(0);
        emit_notification("notifications/resumed", "{}");
        return ok_text("resumed");
    }
    if (name == "toggle_pause") {
        pausemode(-1);
        emit_notification(pause_emulation ? "notifications/paused" : "notifications/resumed", "{}");
        return ok_text("toggled");
    }

    // -- set_speed (CPU speed / turbo) ---------------------------------
    if (name == "set_speed") {
        std::string mode = obj_str(js, toks, args_idx, "mode");
        if (mode.empty()) return err("missing 'mode' (turbo|max|original|balanced)");
        if (mode == "turbo") {
            currprefs.turbo_emulation = 1;
            changed_prefs.turbo_emulation = 1;
        } else if (mode == "max") {
            currprefs.turbo_emulation = 0;
            changed_prefs.turbo_emulation = 0;
            currprefs.m68k_speed = 0;
            changed_prefs.m68k_speed = 0;
        } else if (mode == "original") {
            currprefs.turbo_emulation = 0;
            changed_prefs.turbo_emulation = 0;
            currprefs.m68k_speed = -1;
            changed_prefs.m68k_speed = -1;
        } else if (mode == "balanced") {
            currprefs.turbo_emulation = 0;
            changed_prefs.turbo_emulation = 0;
            currprefs.m68k_speed = 1;
            changed_prefs.m68k_speed = 1;
        } else {
            return err("'mode' must be turbo|max|original|balanced");
        }
        set_config_changed();
        return ok_text(std::string("set speed mode=") + mode);
    }

    // -- quit (immediate, just sets a flag) ----------------------------
    // uae_quit() sets quit_program; the main loop sees it and tears down.
    // Safe from receiver thread.
    if (name == "quit") {
        uae_quit();
        return ok_text("quit requested");
    }
    if (name == "is_paused") {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"paused\":%s,\"pause_emulation\":%d}",
                 pause_emulation ? "true" : "false", pause_emulation);
        std::string out = "{\"content\":[{\"type\":\"text\",\"text\":\"";
        out += esc_json(buf);
        out += "\"}]}";
        return ok_raw(out);
    }

    // -- save_state / load_state ---------------------------------------
    if (name == "save_state") {
        std::string path = obj_str(js, toks, args_idx, "path");
        if (path.empty()) return err("missing 'path'");
        mcp_cmd c; c.kind = CMD_SAVE_STATE; c.text = path; enqueue(std::move(c));
        return ok_text("queued save_state path=" + path);
    }
    if (name == "load_state") {
        std::string path = obj_str(js, toks, args_idx, "path");
        if (path.empty()) return err("missing 'path'");
        mcp_cmd c; c.kind = CMD_LOAD_STATE; c.text = path; enqueue(std::move(c));
        return ok_text("queued load_state path=" + path);
    }

    // -- disk_insert / disk_eject / disk_list ---------------------------
    if (name == "disk_insert") {
        int drv = obj_int(js, toks, args_idx, "drive", -1);
        std::string path = obj_str(js, toks, args_idx, "path");
        if (drv < 0 || drv > 3) return err("'drive' must be 0..3");
        if (path.empty()) return err("missing 'path'");
        mcp_cmd c; c.kind = CMD_DISK_INSERT; c.a = drv; c.text = path; enqueue(std::move(c));
        char buf[96]; snprintf(buf, sizeof(buf), "queued disk_insert df%d=%s", drv, path.c_str());
        return ok_text(buf);
    }
    if (name == "disk_eject") {
        int drv = obj_int(js, toks, args_idx, "drive", -1);
        if (drv < 0 || drv > 3) return err("'drive' must be 0..3");
        mcp_cmd c; c.kind = CMD_DISK_EJECT; c.a = drv; enqueue(std::move(c));
        char buf[64]; snprintf(buf, sizeof(buf), "queued disk_eject df%d", drv);
        return ok_text(buf);
    }
    if (name == "disk_list") {
        std::string out = "{\"content\":[{\"type\":\"text\",\"text\":\"{\\\"drives\\\":[";
        for (int i = 0; i < 4; ++i) {
            if (i) out += ",";
            std::string p = narrow(currprefs.floppyslots[i].df);
            out += "{\\\"drive\\\":";
            char nb[8]; snprintf(nb, sizeof(nb), "%d", i); out += nb;
            out += ",\\\"path\\\":\\\"";
            // double-escape: inner content is itself inside a JSON string in the outer text field
            for (char c : p) {
                if (c == '\\' || c == '"') { out += '\\'; out += '\\'; }
                out += c;
            }
            out += "\\\",\\\"type\\\":";
            snprintf(nb, sizeof(nb), "%d", currprefs.floppyslots[i].dfxtype); out += nb;
            out += "}";
        }
        out += "]}\"}]}";
        return ok_raw(out);
    }

    // -- set_config ----------------------------------------------------
    if (name == "set_config") {
        // Accept either {"line":"key=value"} or {"key":"...","value":"..."}.
        std::string line = obj_str(js, toks, args_idx, "line");
        if (line.empty()) {
            std::string k = obj_str(js, toks, args_idx, "key");
            std::string v = obj_str(js, toks, args_idx, "value");
            // If "value" wasn't a string, try as int.
            if (v.empty()) {
                int vi = obj_int(js, toks, args_idx, "value", INT_MIN);
                if (vi != INT_MIN) {
                    char nb[32]; snprintf(nb, sizeof(nb), "%d", vi);
                    v = nb;
                }
            }
            if (k.empty()) return err("provide 'line' or 'key'+'value'");
            line = k + "=" + v;
        }
        mcp_cmd c; c.kind = CMD_SET_CONFIG; c.text = line; enqueue(std::move(c));
        return ok_text("queued set_config: " + line);
    }

    // -- memory_read (RAM-safe direct reads on receiver thread) ---------
    if (name == "memory_read") {
        uint32_t addr;
        if (!obj_addr(js, toks, args_idx, "addr", addr))
            return err("missing or invalid 'addr' (use 0xNN string or int)");
        int len = obj_int(js, toks, args_idx, "length", 0);
        if (len <= 0 || len > 1 * 1024 * 1024)
            return err("'length' must be 1..1048576");
        std::vector<uint8_t> buf((size_t)len);
        for (int i = 0; i < len; ++i)
            buf[i] = (uint8_t)get_byte(addr + i);
        std::string b64 = b64_encode(buf.data(), buf.size());
        char hdr[96];
        snprintf(hdr, sizeof(hdr), "{\"addr\":\"0x%x\",\"length\":%d,\"bytes_b64\":\"",
                 addr, len);
        std::string out = "{\"content\":[{\"type\":\"text\",\"text\":\"";
        out += esc_json(std::string(hdr) + b64 + "\"}");
        out += "\"}]}";
        return ok_raw(out);
    }

    // -- memory_write (queued for safety on emu thread) -----------------
    if (name == "memory_write") {
        uint32_t addr;
        if (!obj_addr(js, toks, args_idx, "addr", addr))
            return err("missing or invalid 'addr'");
        std::string b64 = obj_str(js, toks, args_idx, "bytes_b64");
        if (b64.empty()) return err("missing 'bytes_b64'");
        std::string raw;
        if (!b64_decode(b64, raw)) return err("invalid base64");
        if (raw.size() > 1 * 1024 * 1024) return err("write too large (>1MB)");
        size_t n = raw.size();
        mcp_cmd c; c.kind = CMD_MEMORY_WRITE; c.a = (int)addr; c.text = std::move(raw);
        enqueue(std::move(c));
        char buf[64];
        snprintf(buf, sizeof(buf), "queued write %zu byte(s) to 0x%x", n, addr);
        return ok_text(buf);
    }

    // -- get_cpu_state (direct read; regs is global) --------------------
    if (name == "get_cpu_state") {
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "{\"d\":[%u,%u,%u,%u,%u,%u,%u,%u],"
                 "\"a\":[%u,%u,%u,%u,%u,%u,%u,%u],"
                 "\"pc\":%u,\"sr\":%u,\"usp\":%u,\"isp\":%u,\"msp\":%u,\"vbr\":%u,"
                 "\"stopped\":%d,\"halted\":%d,\"intmask\":%d}",
                 regs.regs[0], regs.regs[1], regs.regs[2], regs.regs[3],
                 regs.regs[4], regs.regs[5], regs.regs[6], regs.regs[7],
                 regs.regs[8], regs.regs[9], regs.regs[10], regs.regs[11],
                 regs.regs[12], regs.regs[13], regs.regs[14], regs.regs[15],
                 regs.pc, (unsigned)regs.sr, regs.usp, regs.isp, regs.msp, regs.vbr,
                 (int)regs.stopped, regs.halted, regs.intmask);
        std::string out = "{\"content\":[{\"type\":\"text\",\"text\":\"";
        out += esc_json(buf);
        out += "\"}]}";
        return ok_raw(out);
    }

    // -- debug (queued, sync wait for output text) ----------------------
    if (name == "debug") {
        std::string cmd = obj_str(js, toks, args_idx, "command");
        if (cmd.empty()) return err("missing 'command'");
        mcp_cmd c; c.kind = CMD_DEBUG; c.text = cmd;
        int rid = new_pending();
        c.reply_id = rid;
        enqueue(std::move(c));
        std::string out_text;
        if (!wait_pending(rid, out_text, 5000)) {
            drop_pending(rid);
            return err("debug command timed out");
        }
        drop_pending(rid);
        return ok_text(out_text);
    }

    // -- disassemble (queued, sync wait) --------------------------------
    if (name == "disassemble") {
        uint32_t addr;
        if (!obj_addr(js, toks, args_idx, "addr", addr))
            return err("missing or invalid 'addr'");
        int count = obj_int(js, toks, args_idx, "count", 16);
        if (count < 1 || count > 1024) return err("'count' must be 1..1024");
        mcp_cmd c; c.kind = CMD_DISASSEMBLE; c.a = (int)addr; c.b = count;
        int rid = new_pending();
        c.reply_id = rid;
        enqueue(std::move(c));
        std::string out_text;
        if (!wait_pending(rid, out_text, 5000)) {
            drop_pending(rid);
            return err("disassemble timed out");
        }
        drop_pending(rid);
        return ok_text(out_text);
    }

    // -- find_in_memory (direct scan on receiver thread; bounded) -------
    if (name == "find_in_memory") {
        std::string b64 = obj_str(js, toks, args_idx, "pattern_b64");
        std::string pat;
        if (b64.empty() || !b64_decode(b64, pat) || pat.empty())
            return err("missing or invalid 'pattern_b64'");
        if (pat.size() > 4096) return err("pattern too long (>4096 bytes)");
        uint32_t start = 0, end = 0;
        obj_addr(js, toks, args_idx, "start", start);
        if (!obj_addr(js, toks, args_idx, "end", end)) end = start + 16 * 1024 * 1024;
        if (end <= start) return err("'end' must be > 'start'");
        if (end - start > 64 * 1024 * 1024)
            return err("scan range too large (>64MB)");
        int max_hits = obj_int(js, toks, args_idx, "max_hits", 16);
        if (max_hits < 1 || max_hits > 1024) max_hits = 16;
        std::string out = "{\"content\":[{\"type\":\"text\",\"text\":\"{\\\"hits\\\":[";
        int hits = 0;
        for (uint32_t a = start; a + pat.size() <= end && hits < max_hits; ++a) {
            bool match = true;
            for (size_t k = 0; k < pat.size(); ++k) {
                if ((uint8_t)get_byte(a + (uint32_t)k) != (uint8_t)pat[k]) {
                    match = false; break;
                }
            }
            if (match) {
                char hbuf[32];
                snprintf(hbuf, sizeof(hbuf), "%s\\\"0x%x\\\"", hits ? "," : "", a);
                out += hbuf;
                hits++;
            }
        }
        char tail[64];
        snprintf(tail, sizeof(tail), "],\\\"count\\\":%d,\\\"truncated\\\":%s}",
                 hits, (hits >= max_hits) ? "true" : "false");
        out += tail;
        out += "\"}]}";
        return ok_raw(out);
    }

    // -- wait_for_pixel (sync on receiver thread; polls outbuffer) -----
    if (name == "wait_for_pixel") {
        int x = obj_int(js, toks, args_idx, "x", INT_MIN);
        int y = obj_int(js, toks, args_idx, "y", INT_MIN);
        if (x == INT_MIN || y == INT_MIN) return err("require x and y");
        uint32_t rgb;
        if (!obj_addr(js, toks, args_idx, "rgb", rgb))
            return err("missing 'rgb' (e.g. '0xRRGGBB' string)");
        int tol = obj_int(js, toks, args_idx, "tolerance", 0);
        if (tol < 0) tol = 0;
        if (tol > 255) tol = 255;
        int timeout = obj_int(js, toks, args_idx, "timeout_ms", 5000);
        if (timeout < 0) timeout = 0;
        if (timeout > 60000) timeout = 60000;
        int want_r = (int)((rgb >> 16) & 0xff);
        int want_g = (int)((rgb >>  8) & 0xff);
        int want_b = (int)( rgb        & 0xff);
        auto start = std::chrono::steady_clock::now();
        bool matched = false;
        int got_r = -1, got_g = -1, got_b = -1;
        for (;;) {
            struct vidbuf_description *vid = &adisplays[0].gfxvidinfo;
            struct vidbuffer *vb = &vid->drawbuffer;
            if (vb && vb->bufmem && x >= 0 && y >= 0 &&
                x < vb->outwidth && y < vb->outheight && vb->pixbytes >= 3) {
                uae_u8 *p = vb->bufmem + (size_t)y * vb->rowbytes + (size_t)x * vb->pixbytes;
                // outbuffer is BGRA when pixbytes==4 (PNG path uses TRANSFORM_BGR).
                got_b = p[0]; got_g = p[1]; got_r = p[2];
                if (abs(got_r - want_r) <= tol &&
                    abs(got_g - want_g) <= tol &&
                    abs(got_b - want_b) <= tol) {
                    matched = true;
                    break;
                }
            }
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() >= timeout)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "{\"matched\":%s,\"elapsed_ms\":%lld,\"got_rgb\":\"0x%02x%02x%02x\"}",
                 matched ? "true" : "false", (long long)elapsed,
                 got_r < 0 ? 0 : got_r, got_g < 0 ? 0 : got_g, got_b < 0 ? 0 : got_b);
        std::string out = "{\"content\":[{\"type\":\"text\",\"text\":\"";
        out += esc_json(buf);
        out += "\"}]}";
        return ok_raw(out);
    }

    // -- wait_for_region_change (sync; hash and poll) ------------------
    if (name == "wait_for_region_change") {
        int x = obj_int(js, toks, args_idx, "x", INT_MIN);
        int y = obj_int(js, toks, args_idx, "y", INT_MIN);
        int w = obj_int(js, toks, args_idx, "w", 0);
        int h = obj_int(js, toks, args_idx, "h", 0);
        if (x == INT_MIN || y == INT_MIN || w <= 0 || h <= 0)
            return err("require x, y, w (>0), h (>0)");
        int timeout = obj_int(js, toks, args_idx, "timeout_ms", 5000);
        if (timeout < 0) timeout = 0;
        if (timeout > 60000) timeout = 60000;

        auto hash_region = [&](uint64_t &out_h) -> bool {
            struct vidbuf_description *vid = &adisplays[0].gfxvidinfo;
            struct vidbuffer *vb = &vid->drawbuffer;
            if (!vb || !vb->bufmem) return false;
            if (x < 0 || y < 0 || x + w > vb->outwidth || y + h > vb->outheight)
                return false;
            // FNV-1a 64-bit over the region's raw pixels.
            uint64_t hh = 1469598103934665603ULL;
            for (int j = 0; j < h; ++j) {
                uae_u8 *row = vb->bufmem + (size_t)(y + j) * vb->rowbytes +
                              (size_t)x * vb->pixbytes;
                for (int i = 0; i < w * vb->pixbytes; ++i) {
                    hh ^= row[i];
                    hh *= 1099511628211ULL;
                }
            }
            out_h = hh;
            return true;
        };

        uint64_t baseline = 0;
        if (!hash_region(baseline)) return err("region out of bounds or no buffer");
        auto start = std::chrono::steady_clock::now();
        bool changed = false;
        uint64_t cur = baseline;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            uint64_t tmp;
            if (hash_region(tmp)) {
                cur = tmp;
                if (cur != baseline) { changed = true; break; }
            }
            // hash read failure is treated as a transient skip, not a fatal.
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() >= timeout)
                break;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "{\"changed\":%s,\"elapsed_ms\":%lld,\"baseline_hash\":\"0x%016llx\",\"final_hash\":\"0x%016llx\"}",
                 changed ? "true" : "false", (long long)elapsed,
                 (unsigned long long)baseline, (unsigned long long)cur);
        std::string out = "{\"content\":[{\"type\":\"text\",\"text\":\"";
        out += esc_json(buf);
        out += "\"}]}";
        return ok_raw(out);
    }

    // -- wait_for_idle (sync; polls regs.stopped for continuous quiet) -
    if (name == "wait_for_idle") {
        int timeout = obj_int(js, toks, args_idx, "timeout_ms", 5000);
        if (timeout < 0) timeout = 0;
        if (timeout > 60000) timeout = 60000;
        int quiet = obj_int(js, toks, args_idx, "quiet_ms", 200);
        if (quiet < 10) quiet = 10;
        if (quiet > timeout) quiet = timeout;
        auto start = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point quiet_start = start;
        bool quiet_active = false;
        bool became_idle = false;
        for (;;) {
            bool idle_now = (regs.stopped != 0);
            auto now = std::chrono::steady_clock::now();
            if (idle_now) {
                if (!quiet_active) { quiet_active = true; quiet_start = now; }
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - quiet_start).count() >= quiet) {
                    became_idle = true;
                    break;
                }
            } else {
                quiet_active = false;
            }
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() >= timeout)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "{\"idle\":%s,\"elapsed_ms\":%lld,\"stopped\":%d}",
                 became_idle ? "true" : "false", (long long)elapsed, (int)regs.stopped);
        std::string out = "{\"content\":[{\"type\":\"text\",\"text\":\"";
        out += esc_json(buf);
        out += "\"}]}";
        return ok_raw(out);
    }

    // -- screenshot (queued, sync wait; expensive PNG encode) ----------
    if (name == "screenshot") {
        std::string scale_s = obj_str(js, toks, args_idx, "scale");
        int imagemode = (scale_s == "host" || scale_s == "window") ? 0 : 1; // default native
        mcp_cmd c; c.kind = CMD_SCREENSHOT; c.a = imagemode;
        int rid = new_pending();
        c.reply_id = rid;
        enqueue(std::move(c));
        std::string out_body;
        if (!wait_pending(rid, out_body, 10000)) {
            drop_pending(rid);
            return err("screenshot timed out");
        }
        drop_pending(rid);
        return ok_raw(out_body);
    }

    // -- get_screen_geometry (read-only) --------------------------------
    if (name == "get_screen_geometry") {
        // Pull from monitor 0. These reads cross thread boundaries but the
        // fields are stable across a frame; worst case we get a slightly
        // stale value, which is fine for the AI's purposes.
        struct amigadisplay *ad = &adisplays[0];
        struct vidbuf_description *vid = &ad->gfxvidinfo;
        int host_w = vid->outbuffer ? vid->outbuffer->outwidth  : 0;
        int host_h = vid->outbuffer ? vid->outbuffer->outheight : 0;
        bool rtg = ad->picasso_on;
        int amiga_w = 0, amiga_h = 0;
#ifdef PICASSO96
        if (rtg) {
            struct picasso96_state_struct *ps = &picasso96_state[0];
            amiga_w = ps->Width;
            amiga_h = ps->Height;
        }
#endif
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "{\"host_width\":%d,\"host_height\":%d,\"rtg\":%s,"
                 "\"rtg_width\":%d,\"rtg_height\":%d,"
                 "\"gfx_resolution\":%d,\"gfx_linemode\":%d,"
                 "\"ntsc\":%s,\"chipset_refreshrate\":%.4f}",
                 host_w, host_h, rtg ? "true" : "false",
                 amiga_w, amiga_h,
                 currprefs.gfx_resolution, currprefs.gfx_vresolution,
                 currprefs.ntscmode ? "true" : "false",
                 currprefs.chipset_refreshrate);
        std::string out = "{\"content\":[{\"type\":\"text\",\"text\":\"";
        out += esc_json(buf);
        out += "\"}]}";
        return ok_raw(out);
    }

    return err("unknown tool: " + name, -32601);
}

// ---------------------------------------------------------------------------
// Tools/list JSON (static — we never mutate the tool surface at runtime)
// ---------------------------------------------------------------------------

const char *TOOLS_LIST_JSON =
"{\"tools\":["
  "{\"name\":\"type_text\","
    "\"description\":\"Type ASCII text into the running Amiga via keybuf_inject. Only chars representable on the US Amiga keyboard work; newline = Return.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}},"
  "{\"name\":\"mouse_move\","
    "\"description\":\"Move the pointer to absolute Intuition SCREEN PIXEL coordinates (0,0 = screen top-left, same coordinate space as get_pointer_pos). CLOSED-LOOP: reads the live pointer position and self-corrects, so it is accurate regardless of resolution/interlace/centering. BLOCKS until arrived. (space='amiga' selects the legacy rtarea path that needs mousehack alive; the default/host path is recommended.)\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"},\"space\":{\"type\":\"string\",\"enum\":[\"amiga\",\"host\"]}},\"required\":[\"x\",\"y\"]}},"
  "{\"name\":\"get_pointer_pos\","
    "\"description\":\"Returns the live Intuition pointer position {available, x, y} in screen pixels. Mode-independent. Use to verify mouse_move or to locate the pointer before a relative move.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
  "{\"name\":\"mouse_move_rel\","
    "\"description\":\"Move the pointer by a relative delta in guest pixels. Vsync-paced (no 8-bit counter aliasing); blocks until the full delta has been applied.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"dx\":{\"type\":\"integer\"},\"dy\":{\"type\":\"integer\"}},\"required\":[\"dx\",\"dy\"]}},"
  "{\"name\":\"mouse_button\","
    "\"description\":\"Press or release a mouse button. button: 0=left, 1=right, 2=middle. state: 1=down, 0=up. Executes after any queued pointer motion; blocks until applied.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"button\":{\"type\":\"integer\"},\"state\":{\"type\":\"integer\"}},\"required\":[\"button\",\"state\"]}},"
  "{\"name\":\"mouse_click\","
    "\"description\":\"Click (or N clicks, count<=4) of the given button. Press/release transitions are vsync-paced so the guest reliably observes each one; count=2 produces a guest-recognized double-click. Blocks until done.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"button\":{\"type\":\"integer\"},\"count\":{\"type\":\"integer\"}}}},"
  "{\"name\":\"mouse_scroll\","
    "\"description\":\"Mouse wheel delta. dy = vertical, dx = horizontal. Use multiples of 120 for one notch.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"dx\":{\"type\":\"integer\"},\"dy\":{\"type\":\"integer\"}}}},"
  "{\"name\":\"key_down\","
    "\"description\":\"Press a key (does not release). key: symbolic name (Enter, F10, LShift, a, 1, ...) or '0xNN' Amiga scancode.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"}},\"required\":[\"key\"]}},"
  "{\"name\":\"key_up\","
    "\"description\":\"Release a key. See key_down for the 'key' format.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"}},\"required\":[\"key\"]}},"
  "{\"name\":\"key_press\","
    "\"description\":\"Press and release a key, optionally holding modifiers. modifiers: array of LShift/RShift/Ctrl/LAlt/RAlt/LAmiga/RAmiga/etc.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"},\"modifiers\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[\"key\"]}},"
  "{\"name\":\"mousehack_status\","
    "\"description\":\"Returns input_tablet/mousehack_alive/magic_mouse flags so the client can tell whether absolute mouse positioning will visibly move the Amiga pointer.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
  "{\"name\":\"get_screen_geometry\","
    "\"description\":\"Returns host output buffer size, RTG state and resolution, gfx_resolution/linemode/ntsc/refreshrate. Use this to plan mouse_move coordinates.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
  "{\"name\":\"reset\","
    "\"description\":\"Reset the emulated machine. kind: 'soft' (CPU reset, default), 'hard' (clears memory), 'keyboard' (Ctrl-Amiga-Amiga style).\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"kind\":{\"type\":\"string\",\"enum\":[\"soft\",\"hard\",\"keyboard\"]}}}},"
  "{\"name\":\"pause\","
    "\"description\":\"Pause the emulation.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
  "{\"name\":\"resume\","
    "\"description\":\"Resume the emulation.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
  "{\"name\":\"toggle_pause\","
    "\"description\":\"Toggle pause/resume.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
  "{\"name\":\"is_paused\","
    "\"description\":\"Returns {paused: bool, pause_emulation: int}.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
  "{\"name\":\"save_state\","
    "\"description\":\"Save the full emulator state to a .uss file at 'path'. Synchronous; new file overwrites existing.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},"
  "{\"name\":\"load_state\","
    "\"description\":\"Restore emulator state from a .uss file at 'path'.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},"
  "{\"name\":\"disk_insert\","
    "\"description\":\"Insert a floppy image into df0..df3. 'drive': 0-3. 'path': .adf/.ipf/.scp/etc. Empty string to leave empty (use disk_eject instead).\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"drive\":{\"type\":\"integer\"},\"path\":{\"type\":\"string\"}},\"required\":[\"drive\",\"path\"]}},"
  "{\"name\":\"disk_eject\","
    "\"description\":\"Eject the floppy in df0..df3.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"drive\":{\"type\":\"integer\"}},\"required\":[\"drive\"]}},"
  "{\"name\":\"disk_list\","
    "\"description\":\"Returns the current floppy configuration: drives[] with {drive, path, type}.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
  "{\"name\":\"set_config\","
    "\"description\":\"Apply a WinUAE config option at runtime. Pass either {line:'key=value'} or {key:'...',value:'...'}. Examples: input_tablet=1 (enables tablet mode for visible mouse_move), magic_mouse=true, cpu_speed=max. Most options apply immediately; some require a reset.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"line\":{\"type\":\"string\"},\"key\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"}}}},"
  "{\"name\":\"memory_read\","
    "\"description\":\"Read 'length' bytes from CPU-virtual memory at 'addr'. Returns base64. addr accepts '0xNN' / '$NN' / decimal. length max 1MB. Safe on RAM; reading MMIO regions can have side effects.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"addr\":{\"type\":\"string\"},\"length\":{\"type\":\"integer\"}},\"required\":[\"addr\",\"length\"]}},"
  "{\"name\":\"memory_write\","
    "\"description\":\"Write base64-decoded bytes to CPU-virtual memory at 'addr'. Max 1MB. Queued for emu-thread safety. Use with care.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"addr\":{\"type\":\"string\"},\"bytes_b64\":{\"type\":\"string\"}},\"required\":[\"addr\",\"bytes_b64\"]}},"
  "{\"name\":\"get_cpu_state\","
    "\"description\":\"Returns 68k register state: d[0..7], a[0..7], pc, sr, usp, isp, msp, vbr, stopped, halted, intmask.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
  "{\"name\":\"debug\","
    "\"description\":\"Passthrough to WinUAE's built-in debugger CLI (debug_parser). Unlocks disassembler ('d ADDR'), memory dump ('m ADDR'), register dump ('r'), breakpoints ('f ADDR'), memory search ('s'), and many more. 5s timeout.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}},"
  "{\"name\":\"disassemble\","
    "\"description\":\"Convenience over debug('d ADDR COUNT'). Returns text disassembly of 'count' instructions starting at 'addr'.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"addr\":{\"type\":\"string\"},\"count\":{\"type\":\"integer\"}},\"required\":[\"addr\"]}},"
  "{\"name\":\"find_in_memory\","
    "\"description\":\"Linear byte scan for a base64 pattern. start/end as '0xNN' strings (defaults to start..start+16MB). max_hits default 16. Scan range capped at 64MB. Pattern max 4096 bytes.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"pattern_b64\":{\"type\":\"string\"},\"start\":{\"type\":\"string\"},\"end\":{\"type\":\"string\"},\"max_hits\":{\"type\":\"integer\"}},\"required\":[\"pattern_b64\"]}},"
  "{\"name\":\"screenshot\","
    "\"description\":\"Capture the current Amiga screen as PNG. scale='native' (default) returns the chipset-native buffer at native Amiga pixels (matches get_screen_geometry). scale='host' returns the host-window scaled output. Returns an MCP image content item + a text metadata item with width/height/scale/png_bytes.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"scale\":{\"type\":\"string\",\"enum\":[\"native\",\"host\"]}}}},"
  "{\"name\":\"quit\","
    "\"description\":\"Cleanly quit the emulator. Sets the quit_program flag; the main loop tears down on the next tick.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
  "{\"name\":\"wait_for_pixel\","
    "\"description\":\"Block (up to timeout_ms, capped 60s) until the chipset-native pixel at (x,y) matches rgb (e.g. '0xRRGGBB'). 'tolerance' allows per-channel slack. Returns {matched, elapsed_ms, got_rgb}.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"},\"rgb\":{\"type\":\"string\"},\"tolerance\":{\"type\":\"integer\"},\"timeout_ms\":{\"type\":\"integer\"}},\"required\":[\"x\",\"y\",\"rgb\"]}},"
  "{\"name\":\"wait_for_region_change\","
    "\"description\":\"Block until a rectangular region (chipset coords) hashes differently than its baseline. Use when you don't care what the target looks like, just that *something* there changed (e.g. a requester opens).\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"},\"w\":{\"type\":\"integer\"},\"h\":{\"type\":\"integer\"},\"timeout_ms\":{\"type\":\"integer\"}},\"required\":[\"x\",\"y\",\"w\",\"h\"]}},"
  "{\"name\":\"wait_for_idle\","
    "\"description\":\"Block until the 68k CPU has been in STOP state continuously for 'quiet_ms' (default 200) or until timeout. Useful for 'wait until boot finished'.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"timeout_ms\":{\"type\":\"integer\"},\"quiet_ms\":{\"type\":\"integer\"}}}},"
  "{\"name\":\"set_speed\","
    "\"description\":\"Set CPU emulation speed. mode: 'turbo' (frame-uncapped fast-forward), 'max' (run at host speed, no throttle), 'original' (cycle-accurate 68000 timing), 'balanced' (default mixed mode).\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"mode\":{\"type\":\"string\",\"enum\":[\"turbo\",\"max\",\"original\",\"balanced\"]}},\"required\":[\"mode\"]}}"
"]}";

const char *INIT_RESULT_JSON =
"{\"protocolVersion\":\"2024-11-05\","
 "\"capabilities\":{\"tools\":{}},"
 "\"serverInfo\":{\"name\":\"winuae-mcpbridge\",\"version\":\"0.2\"}}";

// ---------------------------------------------------------------------------
// JSON-RPC dispatch (called per line)
// ---------------------------------------------------------------------------

void handle_jsonrpc(SOCKET s, const std::string &line)
{
    jsmn_parser p;
    jsmn_init(&p);
    // Pre-count tokens.
    int r = jsmn_parse(&p, line.c_str(), line.size(), nullptr, 0);
    if (r < 1) {
        send_error(s, "null", -32700, "parse error");
        return;
    }
    std::vector<jsmntok_t> toks(r + 4);
    jsmn_init(&p);
    r = jsmn_parse(&p, line.c_str(), line.size(), toks.data(), (unsigned)toks.size());
    if (r < 1 || toks[0].type != JSMN_OBJECT) {
        send_error(s, "null", -32700, "parse error / not an object");
        return;
    }

    const char *js = line.c_str();

    // Pull id raw (as JSON literal) so we can echo it back exactly.
    std::string id_raw = "null";
    int id_tok = obj_find(js, toks.data(), 0, "id");
    if (id_tok >= 0) id_raw = tok_str(js, toks[id_tok]);
    // Add quotes around STRING ids — tok_str returns the raw bytes between quotes.
    if (id_tok >= 0 && toks[id_tok].type == JSMN_STRING)
        id_raw = std::string("\"") + esc_json(id_raw) + "\"";

    // method
    int m_tok = obj_find(js, toks.data(), 0, "method");
    if (m_tok < 0 || toks[m_tok].type != JSMN_STRING) {
        send_error(s, id_raw, -32600, "missing method");
        return;
    }
    std::string method = tok_str(js, toks[m_tok]);

    // Notifications (id absent): never reply.
    bool is_notification = (id_tok < 0);

    if (method == "initialize") {
        if (!is_notification) send_response(s, id_raw, INIT_RESULT_JSON);
        return;
    }
    if (method == "notifications/initialized") {
        // No response per JSON-RPC.
        return;
    }
    if (method == "ping") {
        if (!is_notification) send_response(s, id_raw, "{}");
        return;
    }
    if (method == "tools/list") {
        if (!is_notification) send_response(s, id_raw, TOOLS_LIST_JSON);
        return;
    }
    if (method == "tools/call") {
        int params_idx = obj_find(js, toks.data(), 0, "params");
        if (params_idx < 0 || toks[params_idx].type != JSMN_OBJECT) {
            if (!is_notification) send_error(s, id_raw, -32602, "missing params");
            return;
        }
        std::string tname = obj_str(js, toks.data(), params_idx, "name");
        if (tname.empty()) {
            if (!is_notification) send_error(s, id_raw, -32602, "missing tool name");
            return;
        }
        // arguments is optional; pass -1 if missing — obj_find/obj_str/obj_int
        // all guard on the index and return defaults when not found.
        int args_idx = obj_find(js, toks.data(), params_idx, "arguments");
        dispatch_result dr = dispatch_tool(tname, js, toks.data(), args_idx);
        if (is_notification) return;
        if (dr.ok) send_response(s, id_raw, dr.result_or_msg);
        else       send_error(s, id_raw, dr.error_code, dr.result_or_msg);
        return;
    }

    if (!is_notification) send_error(s, id_raw, -32601, "method not found: " + method);
}

// ---------------------------------------------------------------------------
// Listener
// ---------------------------------------------------------------------------

void handle_client(SOCKET s)
{
    log_msg(_T("client connected\n"));
    g_active_sock.store(s, std::memory_order_release);
    std::string buf;
    char tmp[4096];
    while (g_running.load(std::memory_order_relaxed)) {
        int n = recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, n);
        for (;;) {
            size_t pos = buf.find('\n');
            if (pos == std::string::npos) break;
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            handle_jsonrpc(s, line);
        }
    }
    // Clear active socket BEFORE closing so the writer thread stops trying to use it.
    g_active_sock.store(INVALID_SOCKET, std::memory_order_release);
    // Drop any pending sends queued for this now-gone client.
    {
        std::lock_guard<std::mutex> g(g_send_mtx);
        g_send_q.clear();
    }
    closesocket(s);
    g_client_connected.store(false, std::memory_order_release);
    log_msg(_T("client disconnected\n"));
}

// Writer thread: drains g_send_q to the active socket, and polls
// mousehack_alive once per tick so we can emit a transition notification.
void writer_loop()
{
    int last_mh = -1;
    while (g_running.load(std::memory_order_relaxed)) {
        std::deque<std::string> local;
        {
            std::unique_lock<std::mutex> lk(g_send_mtx);
            g_send_cv.wait_for(lk, std::chrono::milliseconds(250),
                [] { return !g_send_q.empty() || !g_running.load(std::memory_order_relaxed); });
            local.swap(g_send_q);
        }
        SOCKET s = g_active_sock.load(std::memory_order_acquire);
        if (s != INVALID_SOCKET) {
            for (auto &line : local) {
                std::string out = line + '\n';
                int n = (int)out.size();
                const char *p = out.c_str();
                while (n > 0) {
                    int w = send(s, p, n, 0);
                    if (w <= 0) { n = 0; break; }
                    p += w;
                    n -= w;
                }
            }
        }
        // Mousehack alive transition watcher.
        int mh = mousehack_alive() > 0 ? 1 : 0;
        if (last_mh >= 0 && mh != last_mh) {
            emit_notification("notifications/mousehack_changed",
                std::string("{\"alive\":") + (mh ? "true" : "false") + "}");
        }
        last_mh = mh;
    }
}

void accept_loop(int port)
{
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) {
        log_msg(_T("WSAStartup failed\n"));
        return;
    }
    g_listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_sock == INVALID_SOCKET) {
        log_msg(_T("socket() failed: %d\n"), WSAGetLastError());
        WSACleanup();
        return;
    }
    BOOL on = TRUE;
    setsockopt(g_listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((u_short)port);

    if (bind(g_listen_sock, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        log_msg(_T("bind() port %d failed: %d\n"), port, WSAGetLastError());
        closesocket(g_listen_sock);
        g_listen_sock = INVALID_SOCKET;
        WSACleanup();
        return;
    }
    if (listen(g_listen_sock, 1) == SOCKET_ERROR) {
        log_msg(_T("listen() failed: %d\n"), WSAGetLastError());
        closesocket(g_listen_sock);
        g_listen_sock = INVALID_SOCKET;
        WSACleanup();
        return;
    }
    log_msg(_T("listening on 0.0.0.0:%d\n"), port);

    while (g_running.load(std::memory_order_relaxed)) {
        sockaddr_in cli = {};
        int clilen = sizeof(cli);
        SOCKET c = accept(g_listen_sock, (sockaddr *)&cli, &clilen);
        if (c == INVALID_SOCKET) break;
        bool expected = false;
        if (!g_client_connected.compare_exchange_strong(expected, true)) {
            // JSON-RPC-style refusal so an MCP client gets a clean error.
            const char msg[] =
                "{\"jsonrpc\":\"2.0\",\"id\":null,"
                "\"error\":{\"code\":-32000,\"message\":"
                "\"single-client server; already connected\"}}\n";
            send(c, msg, (int)sizeof(msg) - 1, 0);
            closesocket(c);
            log_msg(_T("rejected second client\n"));
            continue;
        }
        std::thread(handle_client, c).detach();
    }
    if (g_listen_sock != INVALID_SOCKET) {
        closesocket(g_listen_sock);
        g_listen_sock = INVALID_SOCKET;
    }
    WSACleanup();
}

} // namespace

extern "C" void mcpbridge_init(int port)
{
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true)) return;
    if (port <= 0 || port > 65535) port = 7843;
    g_accept_thr = std::thread(accept_loop, port);
    g_accept_thr.detach();
    g_writer_thr = std::thread(writer_loop);
    g_writer_thr.detach();
}

extern "C" void mcpbridge_shutdown(void)
{
    if (!g_running.exchange(false)) return;
    if (g_listen_sock != INVALID_SOCKET) {
        closesocket(g_listen_sock);
        g_listen_sock = INVALID_SOCKET;
    }
}

extern "C" void mcpbridge_drain(void)
{
    if (!g_running.load(std::memory_order_relaxed)) return;

    // One paced step of the mouse engine per drain tick (see comments at
    // the mouse_action queue for the 8-bit counter aliasing rationale).
    mouse_engine_tick();

    std::deque<mcp_cmd> local;
    {
        std::unique_lock<std::mutex> lk(g_q_mtx, std::try_to_lock);
        if (!lk.owns_lock()) return;
        if (g_q.empty()) return;
        local.swap(g_q);
    }
    for (auto &c : local) {
        switch (c.kind) {
        case CMD_TYPE_TEXT:
            keybuf_inject(c.text.c_str());
            break;
        case CMD_MOUSE_MOVE_AMIGA:
            // Bypass host->amiga transform; write directly to rtarea mousehack region.
            inputdevice_mh_abs(c.a, c.b, mousehack_alive() ? 0 : 0);
            break;
        case CMD_MOUSE_MOVE_HOST:
        case CMD_MOUSE_BUTTON:
            // Superseded by the paced mouse_action queue; kept for ABI
            // stability of the enum only.
            break;
        case CMD_MOUSE_SCROLL:
            if (c.a) setmousestate(0, 3, c.a, 0);   // horizontal wheel
            if (c.b) setmousestate(0, 2, c.b, 0);   // vertical wheel
            break;
        case CMD_KEY_RAW: {
            // record_key_direct expects (scancode<<1) | release_bit.
            int kc = (c.a << 1) | (c.b ? 0 : 1);
            record_key_direct(kc, true);
            break;
        }
        case CMD_RESET:
            uae_reset(c.a, c.b);
            break;
        case CMD_PAUSE:
            pausemode(c.a);
            break;
        case CMD_SAVE_STATE: {
            std::wstring w = widen(c.text);
            // save_state(path, description) takes effect immediately.
            save_state(w.c_str(), STATE_SAVE_DESCRIPTION);
            break;
        }
        case CMD_LOAD_STATE: {
            std::wstring w = widen(c.text);
            // restore_state(path) is also immediate.
            restore_state(w.c_str());
            break;
        }
        case CMD_DISK_INSERT: {
            std::wstring w = widen(c.text);
            disk_insert(c.a, w.c_str());
            break;
        }
        case CMD_DISK_EJECT:
            disk_eject(c.a);
            break;
        case CMD_MEMORY_WRITE: {
            const uint32_t base = (uint32_t)c.a;
            const uint8_t *p = (const uint8_t *)c.text.data();
            size_t n = c.text.size();
            for (size_t i = 0; i < n; ++i)
                put_byte(base + (uint32_t)i, p[i]);
            break;
        }
        case CMD_DEBUG: {
            std::wstring win = widen(c.text);
            const size_t outsz = 64 * 1024;
            std::vector<wchar_t> wout(outsz);
            wout[0] = 0;
            debug_parser(win.c_str(), wout.data(), (uae_u32)outsz);
            std::string out = narrow(wout.data());
            if (c.reply_id) deliver_pending(c.reply_id, out);
            break;
        }
        case CMD_DISASSEMBLE: {
            wchar_t cmdbuf[64];
            _snwprintf(cmdbuf, 64, L"d %x %d", (unsigned)c.a, c.b);
            const size_t outsz = 64 * 1024;
            std::vector<wchar_t> wout(outsz);
            wout[0] = 0;
            debug_parser(cmdbuf, wout.data(), (uae_u32)outsz);
            std::string out = narrow(wout.data());
            if (c.reply_id) deliver_pending(c.reply_id, out);
            break;
        }
        case CMD_SCREENSHOT: {
            uae_u8 *png_data = nullptr;
            size_t png_len = 0;
            int w = 0, h = 0;
            int rc = screenshot_capture_png(0, c.a, &png_data, &png_len, &w, &h);
            std::string out;
            if (rc != 0 || !png_data || png_len == 0) {
                out = "{\"isError\":true,\"content\":[{\"type\":\"text\",\"text\":\"screenshot failed (rc=";
                char nb[16]; snprintf(nb, sizeof(nb), "%d", rc); out += nb;
                out += ")\"}]}";
            } else {
                std::string b64 = b64_encode(png_data, png_len);
                char meta[160];
                snprintf(meta, sizeof(meta),
                         "{\"width\":%d,\"height\":%d,\"scale\":\"%s\",\"png_bytes\":%zu}",
                         w, h, c.a ? "native" : "host", png_len);
                out = "{\"content\":[{\"type\":\"image\",\"data\":\"";
                out += b64;
                out += "\",\"mimeType\":\"image/png\"},{\"type\":\"text\",\"text\":\"";
                out += esc_json(meta);
                out += "\"}]}";
            }
            if (png_data) free(png_data);
            if (c.reply_id) deliver_pending(c.reply_id, out);
            break;
        }
        case CMD_SET_CONFIG: {
            std::wstring w = widen(c.text);
            // Shortcut: cfgfile_parse_line silently drops `absolute_mouse=N`
            // in our build (the value is recognized but never written through
            // to currprefs.input_tablet). Apply it directly so the AI's
            // request "make absolute mouse work" actually takes effect.
            if (c.text.rfind("absolute_mouse=", 0) == 0) {
                std::string v = c.text.substr(15);
                int n = -1;
                if      (v == "none")      n = 0;
                else if (v == "mousehack") n = 1;
                else if (v == "tablet")    n = 2;
                if (n >= 0) {
                    changed_prefs.input_tablet = n;
                    currprefs.input_tablet     = n;
                }
            }
            // gfx_* options are applied by the main loop's
            // check_prefs_changed_gfx() which DIFFS changed_prefs against
            // currprefs — writing both would erase the diff and the change
            // would never apply (e.g. fullscreen switching). Everything else
            // goes to both structs so read-mostly fields update immediately.
            bool gfx_key = c.text.rfind("gfx_", 0) == 0;
            cfgfile_parse_line(&changed_prefs, const_cast<wchar_t *>(w.c_str()), 0);
            if (!gfx_key) {
                cfgfile_parse_line(&currprefs, const_cast<wchar_t *>(w.c_str()), 0);
            }
            set_config_changed();
            if (!gfx_key) {
                inputdevice_updateconfig(&changed_prefs, &currprefs);
            }
            emit_notification("notifications/config_changed",
                std::string("{\"line\":\"") + esc_json(c.text) + "\"}");
            break;
        }
        }
    }
}
