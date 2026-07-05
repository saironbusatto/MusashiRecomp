/* text_xlate.cpp — on-the-fly string translation / localization (framework).
 * See text_xlate.h + docs/STRING_TRANSLATION.md. */

#include "text_xlate.h"
#include "cpu_state.h"

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <filesystem>

#include "toml.hpp"

extern "C" uint8_t* memory_get_ram_ptr(void);
extern "C" { extern uint64_t s_frame_count; }

namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Guest RAM access (little-endian, no swizzle). Main RAM is 2 MB, mirrored
// across [0,0x800000). Returns 0 / no-op for out-of-range.
// ---------------------------------------------------------------------------
constexpr uint32_t kRamSize = 2u * 1024u * 1024u;

inline bool ram_fold(uint32_t va, uint32_t* pa_out) {
    uint32_t p = va & 0x1FFFFFFFu;
    if (p < 0x00800000u) { *pa_out = p & (kRamSize - 1u); return true; }
    return false;  // I/O / BIOS / scratchpad — not translatable text storage
}
inline uint8_t grb(uint8_t* ram, uint32_t va) {
    uint32_t pa; return ram_fold(va, &pa) ? ram[pa] : 0u;
}
inline void gwb(uint8_t* ram, uint32_t va, uint8_t v) {
    uint32_t pa; if (ram_fold(va, &pa)) ram[pa] = v;
}
inline bool va_in_ram(uint32_t va) { uint32_t pa; return ram_fold(va, &pa); }

// ---------------------------------------------------------------------------
// FNV-1a 64 over raw source bytes — the translation KV key.
// ---------------------------------------------------------------------------
inline uint64_t fnv1a(const uint8_t* p, uint32_t n) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// ===========================================================================
// EncodingProfile — general text-encoding abstraction (validator + terminator
// rules + UTF-8 -> game-glyph transcoder). A title selects one; Tsumu uses
// shift_jis. Keeps the capture/apply core encoding-agnostic.
// ===========================================================================
constexpr uint32_t kSrcMax = 512;   // max source record bytes captured/hashed

enum class Term { None, Nul, FFFF };

struct EncodingProfile {
    const char* name;
    // Cheap first-byte gate: could `c` begin a text record?
    bool (*first_byte_textish)(uint8_t c);
    // Read a NUL/terminator-delimited record from guest RAM at va. Fills
    // out[0..*len) with the raw content bytes (terminator excluded), sets
    // *term. Returns true only for a plausible text record (>= min real chars).
    bool (*read_record)(uint8_t* ram, uint32_t va, uint8_t* out, uint32_t* len, Term* term);
    // Transcode a UTF-8 target string into game glyph bytes appended to `out`.
    void (*transcode)(const std::string& utf8, std::vector<uint8_t>& out);
    // Authoring quality gate: is this decoded record worth recording in the
    // always-on capture inventory (vs. binary that passed the reader by chance)?
    bool (*capture_worthy)(const uint8_t* bytes, uint32_t len);
};

// ---- Shift-JIS profile (Tsumu) --------------------------------------------
inline bool sj_lead(uint8_t c)  { return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC); }
inline bool sj_trail(uint8_t c) { return (c >= 0x40 && c <= 0x7E) || (c >= 0x80 && c <= 0xFC); }
inline bool sj_kana(uint8_t c)  { return c >= 0xA1 && c <= 0xDF; }               // halfwidth katakana
inline bool sj_ascii(uint8_t c) { return c >= 0x20 && c <= 0x7E; }

bool sj_first_byte_textish(uint8_t c) {
    return sj_ascii(c) || sj_kana(c) || sj_lead(c);
}

// A record ends at NUL (menu/UI labels) or the game's 0xFFFF message terminator
// (framed multi-line messages). Content bytes are kept VERBATIM — including the
// game's control framing: a lone SJIS-lead byte with no valid trail, 0x8x/0xFE,
// and the "FE FF" line-break code — so the hash is byte-exact and the original
// renderer sees identical framing. Binary structs are filtered by requiring a
// minimum count of decodable 2-byte SJIS units.
bool sj_read_record(uint8_t* ram, uint32_t va, uint8_t* out, uint32_t* len, Term* term) {
    if (!va_in_ram(va)) return false;
    uint8_t b0 = grb(ram, va);
    if (!sj_first_byte_textish(b0)) return false;
    uint32_t n = 0, real = 0, two = 0;  // real=text units; two=decodable 2-byte SJIS
    Term t = Term::None;
    for (uint32_t i = 0; i < kSrcMax; ++i) {
        uint8_t c = grb(ram, va + i);
        if (c == 0x00) { t = Term::Nul; break; }
        if (c == 0xFF && grb(ram, va + i + 1) == 0xFF) { t = Term::FFFF; break; }
        if (sj_lead(c) && sj_trail(grb(ram, va + i + 1))) {
            if (n + 2 > kSrcMax) break;
            out[n++] = c; out[n++] = grb(ram, va + i + 1); ++i; ++real; ++two; continue;
        }
        if (sj_kana(c) || sj_ascii(c) || c == 0x0A) {
            if (n + 1 > kSrcMax) break;
            out[n++] = c; if (c != 0x0A) ++real; continue;
        }
        // Control / framing byte: lone SJIS lead (0x81..0x9F/0xE0..0xFC with no
        // valid trail — e.g. the "81 FF" / "FE FF" framing), 0x01..0x1F, 0x80,
        // 0xFD/0xFE, or a lone 0xFF (second half of a FE-FF line break). Keep it.
        if (n + 1 > kSrcMax) break;
        out[n++] = c;
    }
    // Reject binary: a genuine string carries at least two decodable 2-byte SJIS
    // chars (or, for a short pure label, at least three text units).
    if (t == Term::None) return false;
    if (two < 2 && real < 3) return false;
    *len = n; *term = t;
    return true;
}

// UTF-8 -> fullwidth-Shift-JIS. Tsumu's font already contains fullwidth Latin +
// digits (proven by "ＴＳＵＭＵ ｌｉｇｈｔ"), so English renders with zero glyph
// injection. '\n' -> raw 0x0A (the drawer's line break); unknown codepoints ->
// fullwidth space. (Halfwidth-ASCII path is a future EncodingProfile variant
// once MOJI.BIN confirms a halfwidth Latin block.)
uint16_t fw_sjis_for_ascii(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return (uint16_t)(0x8260 + (cp - 'A'));
    if (cp >= 'a' && cp <= 'z') return (uint16_t)(0x8281 + (cp - 'a'));
    if (cp >= '0' && cp <= '9') return (uint16_t)(0x824F + (cp - '0'));
    switch (cp) {
        case ' ':  return 0x8140;  // fullwidth (ideographic) space
        case '!':  return 0x8149;
        case '?':  return 0x8148;
        case '.':  return 0x8144;
        case ',':  return 0x8143;
        case ':':  return 0x8146;
        case ';':  return 0x8147;
        case '\'': return 0x8166;
        case '"':  return 0x8168;
        case '(':  return 0x8169;
        case ')':  return 0x816A;
        case '[':  return 0x816D;
        case ']':  return 0x816E;
        case '-':  return 0x817C;  // fullwidth minus
        case '/':  return 0x815E;
        case '&':  return 0x8195;
        case '%':  return 0x8193;
        case '+':  return 0x817B;
        case '=':  return 0x8181;
        case '*':  return 0x8196;
        case '@':  return 0x8197;
        case '<':  return 0x8183;
        case '>':  return 0x8184;
        case '_':  return 0x8151;
        default:   return 0x8140;  // unknown -> fullwidth space
    }
}

void sj_transcode(const std::string& utf8, std::vector<uint8_t>& out) {
    size_t i = 0;
    while (i < utf8.size()) {
        uint8_t c = (uint8_t)utf8[i];
        uint32_t cp; size_t adv;
        if (c < 0x80)                    { cp = c;                                   adv = 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
            cp = ((c & 0x1F) << 6) | (utf8[i+1] & 0x3F);                             adv = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
            cp = ((c & 0x0F) << 12) | ((utf8[i+1] & 0x3F) << 6) | (utf8[i+2] & 0x3F); adv = 3;
        } else                           { cp = ' ';                                 adv = 1; }
        i += adv;
        if (cp == '\n') { out.push_back(0x0A); continue; }
        // A codepoint already in the fullwidth SJIS range would be authored as
        // raw JP; here targets are Latin. Map ASCII-range codepoints; fullwidth
        // Unicode letters (U+FF01..) map back to their ASCII then to SJIS.
        if (cp >= 0xFF01 && cp <= 0xFF5E) cp = cp - 0xFF00 + 0x20;
        if (cp == 0x3000) cp = ' ';
        uint16_t g = fw_sjis_for_ascii(cp <= 0x7E ? cp : ' ');
        // Endianness of the 16-bit glyph code as the game's drawer reads it.
        // EXE menu labels are big-endian SJIS; the message-table text is stored
        // little-endian. Selectable per run (PSX_XLATE_LE=1) until confirmed;
        // once known this becomes an EncodingProfile flag.
        static const bool le = [] { const char* e = std::getenv("PSX_XLATE_LE");
                                    return e && e[0] == '1'; }();
        if (le) { out.push_back((uint8_t)(g & 0xFF)); out.push_back((uint8_t)(g >> 8)); }
        else    { out.push_back((uint8_t)(g >> 8));   out.push_back((uint8_t)(g & 0xFF)); }
    }
}

// Capture-inventory quality gate (authoring signal only — APPLY is KV-gated and
// unaffected). Real Tsumu text is hiragana/katakana/fullwidth-alnum heavy, whose
// SJIS lead bytes concentrate in the 0x82/0x83 rows; vertex/coordinate binary
// that passes the record reader by chance has diverse leads. Require >= 2 such
// chars so the always-on inventory stays a clean enumeration of drawn strings.
bool sj_capture_worthy(const uint8_t* b, uint32_t n) {
    uint32_t good = 0;
    for (uint32_t i = 0; i + 1 < n; ) {
        uint8_t c = b[i];
        if (sj_lead(c) && sj_trail(b[i + 1])) {
            if (c == 0x83 || (c == 0x82 && b[i + 1] >= 0x4F)) ++good;  // kana/fullwidth
            i += 2;
        } else i += 1;
    }
    return good >= 2;
}

const EncodingProfile kShiftJisProfile = {
    "shift_jis", sj_first_byte_textish, sj_read_record, sj_transcode, sj_capture_worthy
};

// ===========================================================================
// Module state
// ===========================================================================
struct TableEntry {
    std::string target;   // active-language UTF-8 target (empty => fall back to source)
    Term        term = Term::Nul;
};
struct CapRec {
    uint8_t  bytes[96];
    uint8_t  sample_len;   // bytes stored in `bytes` (<=96)
    uint16_t full_len;     // full record length
    uint32_t addr;         // last source VA seen
    uint32_t pc;           // a dispatch target that carried it
    uint32_t ra;
    uint32_t first_frame;
    uint64_t count;
    Term     term;         // terminator that delimited the record (nul|ffff)
    bool     translated;
};

const EncodingProfile* g_prof = &kShiftJisProfile;

std::unordered_map<uint64_t, TableEntry> g_table;      // hash -> translation
std::unordered_map<uint64_t, CapRec>     g_inv;        // hash -> capture record
std::mutex g_mtx;

std::atomic<bool>     g_apply_armed{false};   // table non-empty AND language enabled
std::atomic<bool>     g_capture_on{true};     // always-on inventory (default)
std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_hits{0};
std::string           g_lang = "en";
std::string           g_dir;                  // translations/ directory (for reload)

// Recently-written scratch VAs — so our own transcoded (fullwidth-SJIS) English
// scratch isn't re-captured as a bogus "Japanese" record.
uint32_t g_scratch_ring[32] = {0};
std::atomic<uint32_t> g_scratch_pos{0};
bool scratch_recent(uint32_t va) {
    for (uint32_t i = 0; i < 32; ++i) {
        uint32_t s = g_scratch_ring[i];
        if (s && (va >= s) && (va < s + kSrcMax + 4)) return true;
    }
    return false;
}
void scratch_note(uint32_t va) {
    g_scratch_ring[g_scratch_pos.fetch_add(1, std::memory_order_relaxed) & 31u] = va;
}

// ---------------------------------------------------------------------------
// Table loading (all translations/*.toml). Hot-reloadable via debug "reload".
// ---------------------------------------------------------------------------
std::vector<uint8_t> hex_to_bytes(const std::string& hex) {
    std::vector<uint8_t> b;
    auto hv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        int hi = hv(hex[i]), lo = hv(hex[i+1]);
        if (hi < 0 || lo < 0) { b.clear(); break; }
        b.push_back((uint8_t)((hi << 4) | lo));
    }
    return b;
}

void load_tables_locked() {
    g_table.clear();
    for (auto& kv : g_inv) kv.second.translated = false;
    if (g_dir.empty() || !fs::exists(g_dir)) { g_apply_armed.store(false); return; }
    const bool lang_off = g_lang.empty() || g_lang == "jp" || g_lang == "off";
    size_t files = 0, entries = 0;
    std::error_code ec;
    for (auto& de : fs::directory_iterator(g_dir, ec)) {
        if (!de.is_regular_file()) continue;
        if (de.path().extension() != ".toml") continue;
        toml::value data;
        try { data = toml::parse(de.path().string()); }
        catch (...) { continue; }
        ++files;
        if (!data.contains("entry")) continue;
        const auto& arr = toml::find(data, "entry");
        if (!arr.is_array()) continue;
        for (const auto& e : arr.as_array()) {
            if (!e.contains("src_hex")) continue;
            std::string hex = toml::find_or<std::string>(e, "src_hex", "");
            auto bytes = hex_to_bytes(hex);
            if (bytes.empty()) continue;
            std::string tgt;
            if (e.contains(g_lang)) tgt = toml::find_or<std::string>(e, g_lang, "");
            else if (e.contains("en")) tgt = toml::find_or<std::string>(e, "en", "");
            if (tgt.empty()) continue;  // no translation for this lang => source shown
            Term term = Term::Nul;
            std::string ts = toml::find_or<std::string>(e, "term", "nul");
            if (ts == "ffff" || ts == "FFFF") term = Term::FFFF;
            uint64_t key = fnv1a(bytes.data(), (uint32_t)bytes.size());
            g_table[key] = TableEntry{ std::move(tgt), term };
            ++entries;
        }
    }
    g_apply_armed.store(!lang_off && !g_table.empty(), std::memory_order_relaxed);
    // Mark inventory records that now have a translation.
    for (auto& kv : g_inv)
        kv.second.translated = (g_table.find(kv.first) != g_table.end());
    std::fprintf(stderr,
        "[xlate] loaded %zu entries from %zu file(s) in %s (lang=%s apply=%s)\n",
        entries, files, g_dir.c_str(), g_lang.c_str(),
        g_apply_armed.load() ? "on" : "off");
}

// ---------------------------------------------------------------------------
// Capture inventory upsert.
// ---------------------------------------------------------------------------
void inv_upsert_locked(uint64_t key, const uint8_t* bytes, uint32_t len,
                       uint32_t addr, uint32_t pc, uint32_t ra, Term term) {
    auto it = g_inv.find(key);
    if (it != g_inv.end()) { it->second.count++; it->second.addr = addr;
                             it->second.pc = pc; it->second.ra = ra; return; }
    if (g_inv.size() >= 60000) return;  // sane bound
    CapRec r{};
    uint32_t n = len < 96 ? len : 96;
    std::memcpy(r.bytes, bytes, n);
    r.sample_len = (uint8_t)n; r.full_len = (uint16_t)len; r.term = term;
    r.addr = addr; r.pc = pc; r.ra = ra;
    r.first_frame = (uint32_t)s_frame_count; r.count = 1;
    r.translated = (g_table.find(key) != g_table.end());
    g_inv[key] = r;
}

// ---------------------------------------------------------------------------
// Apply: transcode + write guest scratch + repoint the arg register.
// ---------------------------------------------------------------------------
bool apply_to_reg(uint8_t* ram, CPUState* cpu, uint32_t sp, uint32_t* reg,
                  const TableEntry& te, Term src_term) {
    std::vector<uint8_t> enc;
    g_prof->transcode(te.target, enc);
    if (enc.empty()) return false;
    Term t = (te.term != Term::Nul) ? te.term : src_term;  // preserve source terminator
    // For framed (0xFFFF) messages the game's line break is the "FE FF" code, not
    // a raw 0x0A — remap so multi-line English reflows in the text box.
    std::vector<uint8_t> body;
    body.reserve(enc.size() + 8);
    for (uint8_t c : enc) {
        if (c == 0x0A && t == Term::FFFF) { body.push_back(0xFE); body.push_back(0xFF); }
        else if (c == 0x0A)               { /* NUL-label: drop stray newline */ }
        else                               body.push_back(c);
    }
    if (body.empty() || body.size() > kSrcMax) return false;
    // Scratch below the caller's $sp, 8-byte aligned, with headroom.
    if (sp < 0x80001000u) return false;
    uint32_t scratch = (sp - 0x900u) & ~7u;
    if (!va_in_ram(scratch) || !va_in_ram(scratch + (uint32_t)body.size() + 2)) return false;
    for (size_t i = 0; i < body.size(); ++i) gwb(ram, scratch + (uint32_t)i, body[i]);
    if (t == Term::FFFF) { gwb(ram, scratch + (uint32_t)body.size(),     0xFF);
                           gwb(ram, scratch + (uint32_t)body.size() + 1, 0xFF); }
    else                 { gwb(ram, scratch + (uint32_t)body.size(),     0x00); }
    scratch_note(scratch);
    *reg = scratch;
    return true;
}

} // namespace

// ===========================================================================
// Public API
// ===========================================================================
extern "C" void text_xlate_on_dispatch(CPUState* cpu, uint32_t target) {
    const bool cap = g_capture_on.load(std::memory_order_relaxed);
    const bool app = g_apply_armed.load(std::memory_order_relaxed);
    if (!cap && !app) return;
    uint8_t* ram = memory_get_ram_ptr();
    if (!ram) return;
    g_calls.fetch_add(1, std::memory_order_relaxed);

    const uint32_t sp = cpu->gpr[29];
    // Scan the argument registers a0..a3 for source-text pointers. KV-gated
    // apply means swapping only the reg that pointed at a KNOWN record — a
    // coincidental non-text arg won't be in the table, so it is never touched.
    uint32_t* argregs[4] = { &cpu->gpr[4], &cpu->gpr[5], &cpu->gpr[6], &cpu->gpr[7] };
    for (int a = 0; a < 4; ++a) {
        uint32_t va = *argregs[a];
        if (!va_in_ram(va)) continue;
        if (!g_prof->first_byte_textish(grb(ram, va))) continue;
        if (scratch_recent(va)) continue;  // don't recapture our own English scratch
        uint8_t buf[kSrcMax]; uint32_t len = 0; Term term = Term::None;
        if (!g_prof->read_record(ram, va, buf, &len, &term)) continue;
        uint64_t key = fnv1a(buf, len);

        if (cap && (!g_prof->capture_worthy || g_prof->capture_worthy(buf, len))) {
            std::lock_guard<std::mutex> lk(g_mtx);
            inv_upsert_locked(key, buf, len, va, target, cpu->gpr[31], term);
        }
        if (app) {
            // Safety: by default only substitute standalone framed (0xFFFF)
            // messages. NUL-terminated records are frequently fixed-width struct
            // fields with binary params packed after the text (e.g. the "Tutorial
            // N" label carries trailing coordinate bytes); replacing those
            // corrupts the struct and derails the game. A NUL record is only
            // safe when its table entry opts in with term="nul" AND matches
            // byte-for-byte (the author vetted it). PSX_XLATE_ALLOW_NUL=1 lifts
            // the gate for experimentation.
            TableEntry te;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                auto it = g_table.find(key);
                if (it == g_table.end()) continue;
                te = it->second;
            }
            static const bool allow_nul = [] {
                const char* e = std::getenv("PSX_XLATE_ALLOW_NUL"); return e && e[0] == '1'; }();
            if (term != Term::FFFF && !allow_nul) continue;  // framed messages only
            if (apply_to_reg(ram, cpu, sp, argregs[a], te, term))
                g_hits.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

extern "C" void text_xlate_init(const char* project_root, const char* language) {
    const char* env = std::getenv("PSX_LANG");
    if (env && *env) g_lang = env;
    else if (language && *language) g_lang = language;
    if (project_root && *project_root)
        g_dir = (fs::path(project_root) / "translations").string();
    const char* capenv = std::getenv("PSX_XLATE_CAPTURE");
    if (capenv && capenv[0] == '0') g_capture_on.store(false);
    std::lock_guard<std::mutex> lk(g_mtx);
    load_tables_locked();
}

extern "C" int text_xlate_debug_json(const char* subcmd, char* out, int cap) {
    if (!out || cap <= 0) return 0;
    std::string sc = subcmd ? subcmd : "stats";
    std::lock_guard<std::mutex> lk(g_mtx);

    if (sc == "reload") { load_tables_locked();
        return std::snprintf(out, cap, "{\"reloaded\":true,\"entries\":%zu}", g_table.size()); }

    if (sc == "stats") {
        return std::snprintf(out, cap,
            "{\"lang\":\"%s\",\"apply\":%s,\"capture\":%s,\"table_entries\":%zu,"
            "\"distinct_captured\":%zu,\"calls\":%llu,\"hits\":%llu}",
            g_lang.c_str(), g_apply_armed.load() ? "true" : "false",
            g_capture_on.load() ? "true" : "false",
            g_table.size(), g_inv.size(),
            (unsigned long long)g_calls.load(), (unsigned long long)g_hits.load());
    }

    // dump / todo: JSON array of captured records.
    const bool todo_only = (sc == "todo");
    int w = 0;
    w += std::snprintf(out + w, cap - w, "[");
    bool first = true;
    for (auto& kv : g_inv) {
        const CapRec& r = kv.second;
        if (todo_only && r.translated) continue;
        if (w > cap - 512) break;   // leave room; truncate cleanly
        if (!first) w += std::snprintf(out + w, cap - w, ",");
        first = false;
        w += std::snprintf(out + w, cap - w,
            "{\"hash\":\"%016llx\",\"addr\":\"%08x\",\"pc\":\"%08x\",\"ra\":\"%08x\","
            "\"len\":%u,\"count\":%llu,\"term\":\"%s\",\"xl\":%s,\"hex\":\"",
            (unsigned long long)kv.first, r.addr, r.pc, r.ra,
            (unsigned)r.full_len, (unsigned long long)r.count,
            r.term == Term::FFFF ? "ffff" : "nul",
            r.translated ? "true" : "false");
        for (uint32_t i = 0; i < r.sample_len && w < cap - 4; ++i)
            w += std::snprintf(out + w, cap - w, "%02x", r.bytes[i]);
        w += std::snprintf(out + w, cap - w, "\"}");
    }
    w += std::snprintf(out + w, cap - w, "]");
    return w;
}
