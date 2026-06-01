// Tiny SAX-ish JSON parser/encoder for the line-delimited JSON wire.
//
// Ported verbatim from smo_archipelago/switch-mod/src/util/Json.hpp,
// renamed to dread::ap::json. Same heap-avoidance rationale: libstdc++'s
// allocator NULL-derefs in long-running subsdk9 sessions; fixed-size
// LineBuffer keeps every encode/decode out of the allocator.
//
// 8 KiB matches the wire-format per-line cap shared with the PC.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dread::ap::json {

class LineBuffer {
public:
    static constexpr std::size_t kCap = 8 * 1024;

    void clear() { len_ = 0; trunc_ = false; }

    const char* data() const { return buf_; }
    std::size_t size() const { return len_; }
    bool empty() const { return len_ == 0; }
    bool truncated() const { return trunc_; }

    void append(char c) {
        if (len_ < kCap) { buf_[len_++] = c; } else { trunc_ = true; }
    }
    void append(const char* s, std::size_t n) {
        std::size_t take = (len_ + n <= kCap) ? n : (kCap - len_);
        for (std::size_t i = 0; i < take; ++i) buf_[len_ + i] = s[i];
        len_ += take;
        if (take < n) trunc_ = true;
    }
    void append(std::string_view sv) { append(sv.data(), sv.size()); }

private:
    char buf_[kCap];
    std::size_t len_ = 0;
    bool trunc_ = false;
};

class Encoder {
public:
    static constexpr int kMaxDepth = 16;

    explicit Encoder(LineBuffer& out) : out_(out) {}

    Encoder& beginObject();
    Encoder& endObject();
    Encoder& beginArray();
    Encoder& endArray();
    Encoder& key(std::string_view k);
    Encoder& value(std::string_view s);
    Encoder& value(const char* s) { return value(std::string_view(s)); }
    Encoder& value(std::int64_t v);
    Encoder& value(int v);
    Encoder& value(bool v);

private:
    void maybeComma();
    void pushFrame();
    void popFrame();
    void markNeedsComma();
    void clearNeedsComma();

    LineBuffer& out_;
    bool needs_comma_stack_[kMaxDepth]{};
    int depth_ = 0;
};

// Minimal scan API. Returns false on malformed input.
// String escapes are decoded IN PLACE — pass a writable buffer, not a literal.
class Reader {
public:
    Reader(const char* data, std::size_t len);

    bool nextString(std::string_view& out);
    bool nextInt(std::int64_t& out);
    bool nextBool(bool& out);
    bool isNull();

    bool enterObject();
    bool exitObject();
    bool nextField(std::string_view& out_key);

    bool enterArray();
    bool exitArray();
    bool hasMoreInArray() const;

private:
    void skipWs();
    bool fail();
    bool prepareValue();
    void markValueDone();
    bool readString(std::string_view& out);

    struct Frame { bool is_object; bool needs_comma; };
    static constexpr int kMaxDepth = 8;

    const char* p_;
    const char* end_;
    Frame stack_[kMaxDepth]{};
    int depth_ = 0;
    bool error_ = false;
};

}  // namespace dread::ap::json
