#undef NDEBUG
#include "core/conversation.h"
#include "core/message.h"
#include "core/sentinel_scanner.h"
#include "harness/harness.h"
#include "model/replay_client.h"
#include "model/scripted_client.h"

#include <cassert>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
struct TestCase { const char* name; void (*fn)(); };
TestCase g_tests[64];
int g_count = 0;
struct Registrar {
    Registrar(const char* n, void (*f)()) { g_tests[g_count++] = {n, f}; }
};
}

#define TEST(name)                                   \
    static void name();                              \
    static Registrar name##_reg(#name, &name);       \
    static void name()

struct ConversationTestAccess {
    static std::size_t capacity(const Conversation& c) { return c.capacity_; }
    static const Message* data(const Conversation& c) { return c.data_; }
};
using CTA = ConversationTestAccess;

struct SentinelScannerTestAccess {
    static const std::string& pending(const SentinelScanner& s) { return s.pending_; }
};
using STA = SentinelScannerTestAccess;

namespace {

const std::string kSentinel = "<|end_conversation|>";

class ScriptedInput : public InputSource {
public:
    explicit ScriptedInput(std::deque<std::string> lines) : lines_(std::move(lines)) {}
    std::string read_line() override {
        if (lines_.empty()) {
            eof_ = true;
            return "";
        }
        std::string line = lines_.front();
        lines_.pop_front();
        return line;
    }
    bool is_eof() const override { return eof_; }
    std::size_t remaining() const { return lines_.size(); }

private:
    std::deque<std::string> lines_;
    bool eof_ = false;
};

class CapturingOutput : public OutputSink {
public:
    void write(std::string_view text) override { text_ += text; }
    const std::string& text() const { return text_; }

private:
    std::string text_;
};

std::string temp_path(const std::string& name) {
    return (std::filesystem::temp_directory_path() / ("p2_test_" + name)).string();
}

std::string write_file(const std::string& name, const std::string& contents) {
    std::string path = temp_path(name);
    std::ofstream f(path);
    f << contents;
    return path;
}

const char* role_name(Role r) {
    switch (r) {
        case Role::System: return "system";
        case Role::User: return "user";
        case Role::Assistant: return "assistant";
    }
    return "assistant";
}

void save_transcript(const Conversation& conv, const std::string& path) {
    std::ofstream file(path);
    bool first = true;
    for (const Message& m : conv) {
        if (!first) file << "---\n";
        first = false;
        file << "role: " << role_name(m.role()) << "\n" << m.content() << "\n";
    }
}

std::string feed_all(SentinelScanner& s, const std::string& text, std::size_t chunk,
                      bool& found) {
    std::string out;
    found = false;
    for (std::size_t i = 0; i < text.size() && !found; i += chunk) {
        auto r = s.feed(std::string_view(text).substr(i, chunk));
        out += r.safe_text;
        found = r.sentinel_found;
    }
    if (!found) out += s.flush().safe_text;
    return out;
}

}

TEST(DefaultMessageIsEmptySystem) {
    Message m;
    assert(m.role() == Role::System);
    assert(m.content().empty());
    Message u(Role::User, "hi");
    assert(u.role() == Role::User && u.content() == "hi");
}

TEST(EmptyConversationHasNoElementsAndThrowsOnAt) {
    Conversation c;
    assert(c.size() == 0);
    assert(c.begin() == c.end());
    assert(CTA::data(c) == nullptr && "no allocation until first append");

    int iterations = 0;
    for (const Message& m : c) { (void)m; ++iterations; }
    assert(iterations == 0);

    bool threw = false;
    try { c.at(0); } catch (const std::out_of_range&) { threw = true; }
    assert(threw && "at(0) on empty must throw out_of_range");
}

TEST(AtThrowsExactlyAtSize) {
    Conversation c;
    c.append(Message(Role::User, "a"));
    c.append(Message(Role::Assistant, "b"));
    assert(c.at(1).content() == "b");
    bool threw = false;
    try { c.at(2); } catch (const std::out_of_range&) { threw = true; }
    assert(threw && "at(size()) must throw");
}

TEST(SystemMessageStaysPinnedFirst) {
    Conversation c;
    c.append(Message(Role::System, "Be concise."));
    for (int i = 0; i < 50; ++i) {
        c.append(Message(i % 2 ? Role::Assistant : Role::User, "msg " + std::to_string(i)));
    }
    assert(c.at(0).role() == Role::System);
    assert(c.at(0).content() == "Be concise.");
    for (std::size_t i = 1; i < c.size(); ++i) assert(c.at(i).role() != Role::System);
}

TEST(LateSystemMessageIsRejected) {
    Conversation c;
    c.append(Message(Role::User, "hi"));
    bool threw = false;
    try { c.append(Message(Role::System, "too late")); }
    catch (const std::logic_error&) { threw = true; }
    assert(threw && "system message after other messages must be rejected");
    assert(c.size() == 1 && "rejected append must not change the conversation");
}

TEST(CopyConstructorIsDeep) {
    Conversation a;
    a.append(Message(Role::User, "hello"));
    a.append(Message(Role::Assistant, "hi"));

    Conversation b(a);
    assert(b.begin() != a.begin() && "copy must own a separate buffer");
    assert(b.size() == a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        assert(b.at(i).role() == a.at(i).role());
        assert(b.at(i).content() == a.at(i).content());
    }

    a.append(Message(Role::User, "more"));
    assert(a.size() == 3 && b.size() == 2);

    Conversation empty;
    Conversation empty_copy(empty);
    assert(empty_copy.size() == 0 && CTA::data(empty_copy) == nullptr);
}

TEST(CopyAssignmentIsDeepAndSelfSafe) {
    Conversation a, b;
    a.append(Message(Role::User, "x"));
    b.append(Message(Role::User, "old"));
    b.append(Message(Role::Assistant, "old2"));

    b = a;
    assert(b.size() == 1 && b.at(0).content() == "x");
    assert(b.begin() != a.begin());

    Conversation& alias = b;
    b = alias;
    assert(b.size() == 1 && b.at(0).content() == "x");

    Conversation empty;
    b = empty;
    assert(b.size() == 0 && b.begin() == b.end());
}

TEST(MoveConstructorStealsBuffer) {
    Conversation a;
    a.append(Message(Role::User, "hello"));
    a.append(Message(Role::Assistant, "hi"));
    const Message* original = a.begin();
    std::size_t cap = CTA::capacity(a);

    Conversation b(std::move(a));
    assert(b.begin() == original && "move must steal, not copy");
    assert(b.size() == 2 && CTA::capacity(b) == cap);
    assert(CTA::data(a) == nullptr && a.size() == 0 && CTA::capacity(a) == 0);

    a.append(Message(Role::User, "reused"));
    assert(a.size() == 1 && a.at(0).content() == "reused");
}

TEST(MoveAssignmentStealsAndFreesOld) {
    Conversation a, b;
    a.append(Message(Role::User, "src"));
    b.append(Message(Role::User, "dst-old"));
    const Message* original = a.begin();

    b = std::move(a);
    assert(b.begin() == original);
    assert(b.size() == 1 && b.at(0).content() == "src");
    assert(CTA::data(a) == nullptr && a.size() == 0 && CTA::capacity(a) == 0);

    Conversation& self = b;
    b = std::move(self);
    assert(b.size() == 1 && b.at(0).content() == "src");
}

TEST(CapacityDoublesAndContentsSurviveReallocation) {
    Conversation c;
    std::size_t expected_cap = 0;
    std::size_t reallocations = 0;
    const std::size_t N = 1000;

    for (std::size_t i = 0; i < N; ++i) {
        if (i == expected_cap) {
            expected_cap = expected_cap == 0 ? 1 : expected_cap * 2;
            ++reallocations;
        }
        c.append(Message(Role::User, std::to_string(i)));
        assert(CTA::capacity(c) == expected_cap && "capacity must follow 1,2,4,8,...");
        assert(c.size() == i + 1);
    }
    for (std::size_t i = 0; i < N; ++i) assert(c.at(i).content() == std::to_string(i));
    assert(reallocations == 11);
}

TEST(ScannerPassesCleanTextThrough) {
    const std::string text = "Hello there, nothing special | < > _ here.";
    for (std::size_t chunk = 1; chunk <= text.size(); ++chunk) {
        SentinelScanner s(kSentinel);
        bool found = true;
        std::string out = feed_all(s, text, chunk, found);
        assert(!found && "clean text must never report the sentinel");
        assert(out == text && "clean text must come out unchanged");
    }

    SentinelScanner s(kSentinel);
    auto r = s.feed("plain words");
    assert(r.safe_text == "plain words" && "text that cannot start the sentinel is emitted at once");
    assert(STA::pending(s).empty());
}

TEST(ScannerCatchesWholeSentinelInOneChunk) {
    SentinelScanner s(kSentinel);
    auto r = s.feed("Goodbye." + kSentinel);
    assert(r.sentinel_found);
    assert(r.safe_text == "Goodbye.");
}

TEST(ScannerCatchesSentinelAtEveryBoundary) {
    const std::string text = "Goodbye." + kSentinel;
    for (std::size_t split = 0; split <= text.size(); ++split) {
        SentinelScanner scanner(kSentinel);
        auto out1 = scanner.feed(text.substr(0, split));
        auto out2 = scanner.feed(text.substr(split));
        assert((out1.sentinel_found || out2.sentinel_found) &&
               "sentinel must be caught regardless of split point");
        assert(out1.safe_text + out2.safe_text == "Goodbye.");
        if (split < text.size()) assert(!out1.sentinel_found && "no early match");
    }
}

TEST(ScannerCatchesSentinelAtEveryPairOfBoundaries) {
    const std::string text = "ab<|end_" + kSentinel;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        for (std::size_t j = i; j <= text.size(); ++j) {
            SentinelScanner s(kSentinel);
            auto a = s.feed(text.substr(0, i));
            auto b = s.feed(text.substr(i, j - i));
            auto c = s.feed(text.substr(j));
            bool found = a.sentinel_found || b.sentinel_found || c.sentinel_found;
            assert(found);
            std::string safe = a.safe_text;
            if (!a.sentinel_found) safe += b.safe_text;
            if (!a.sentinel_found && !b.sentinel_found) safe += c.safe_text;
            assert(safe == "ab<|end_");
        }
    }
}

TEST(ScannerOneByteAtATime) {
    const std::string text = "x<|end_<|end_conv" + kSentinel + "trailing";
    SentinelScanner s(kSentinel);
    bool found = false;
    std::string out = feed_all(s, text, 1, found);
    assert(found);
    assert(out == "x<|end_<|end_conv");
}

TEST(ScannerIgnoresFalseAlarms) {
    const char* decoys[] = {
        "<|end_world|>",
        "<|end_conversation|",
        "<|end_conversation >",
        "|end_conversation|>",
        "<<|end_conversatio",
        "<|END_CONVERSATION|>",
    };
    for (const char* d : decoys) {
        const std::string text = std::string("before ") + d + " after";
        for (std::size_t chunk = 1; chunk <= text.size(); ++chunk) {
            SentinelScanner s(kSentinel);
            bool found = true;
            std::string out = feed_all(s, text, chunk, found);
            assert(!found && "partial or altered sentinel must not trigger");
            assert(out == text && "false alarm text must be released unchanged");
        }
    }
}

TEST(ScannerFlushReleasesHeldPrefix) {
    SentinelScanner s(kSentinel);
    auto r = s.feed("Almost done<|end_conv");
    assert(!r.sentinel_found);
    assert(r.safe_text == "Almost done");
    assert(STA::pending(s) == "<|end_conv");
    auto f = s.flush();
    assert(!f.sentinel_found);
    assert(f.safe_text == "<|end_conv");
    assert(STA::pending(s).empty());
}

TEST(ScannerRejectsEmptySentinel) {
    bool threw = false;
    try { SentinelScanner s(""); } catch (const std::invalid_argument&) { threw = true; }
    assert(threw);
}

TEST(ScannerPendingStaysBoundedOnFourMegabyteStream) {
    const std::size_t bound = kSentinel.size() - 1;
    const std::string pattern = "<|end_conversation|<|end_<|<|end_conversatio";
    const std::size_t target = 4u * 1024u * 1024u;

    std::string stream;
    stream.reserve(target + kSentinel.size());
    while (stream.size() < target) stream += pattern;
    const std::string expected = stream;
    stream += kSentinel;

    SentinelScanner s(kSentinel);
    std::size_t emitted = 0;
    std::size_t max_pending = 0;
    bool found = false;
    for (std::size_t i = 0; i < stream.size(); ++i) {
        auto r = s.feed(std::string_view(stream).substr(i, 1));
        emitted += r.safe_text.size();
        std::size_t p = STA::pending(s).size();
        if (p > max_pending) max_pending = p;
        assert(p <= bound && "pending_ must never exceed sentinel.size() - 1");
        if (r.sentinel_found) {
            found = true;
            assert(i == stream.size() - 1 && "sentinel must be found exactly at its last byte");
        }
    }
    assert(found);
    assert(emitted == expected.size() && "every non-sentinel byte must be emitted");
    assert(max_pending == bound && "adversarial stream should reach the bound");
}

TEST(HarnessStopsAtTurnLimit) {
    std::string path = write_file("turnlimit.script",
        "role: assistant\none\n---\nrole: assistant\ntwo\n---\n"
        "role: assistant\nthree\n---\nrole: assistant\nfour\n");
    HarnessConfig cfg;
    cfg.max_turns = 3;
    Harness h(std::make_unique<ScriptedModelClient>(path), cfg);
    ScriptedInput in({"a", "b", "c", "d", "e"});
    CapturingOutput out;

    StopReason r = h.run(in, out);
    assert(r.kind == StopReason::Kind::TurnLimit);
    assert(h.conversation().size() == 6);
    assert(h.conversation().at(5).content() == "three");
    assert(in.remaining() == 2 && "loop must not read input past the limit");
}

TEST(HarnessZeroTurnLimitRunsNothing) {
    std::string path = write_file("zero.script", "role: assistant\nunused\n");
    HarnessConfig cfg;
    cfg.max_turns = 0;
    Harness h(std::make_unique<ScriptedModelClient>(path), cfg);
    ScriptedInput in({"hello"});
    CapturingOutput out;
    assert(h.run(in, out).kind == StopReason::Kind::TurnLimit);
    assert(h.conversation().size() == 0);
}

TEST(HarnessHaltsOnSplitSentinel) {
    std::string path = write_file("sentinel.script",
        "role: system\nBe concise.\n---\n"
        "chunk: 4\nrole: assistant\nFirst reply.\n---\n"
        "chunk: 3\nrole: assistant\nGoodbye." + kSentinel + "\n---\n"
        "role: assistant\nnever reached\n");
    ScriptedModelClient probe(path);
    HarnessConfig cfg;
    cfg.system_message = probe.system_message();
    Harness h(std::make_unique<ScriptedModelClient>(path), cfg);
    ScriptedInput in({"hi", "bye", "extra", "extra2"});
    CapturingOutput out;

    StopReason r = h.run(in, out);
    assert(r.kind == StopReason::Kind::Sentinel);
    assert(r.detail == "stop sentinel after 2 turns");
    assert(in.remaining() == 2 && "loop must halt immediately after the sentinel");
    assert(out.text().find("<|end") == std::string::npos && "sentinel must never be printed");
    assert(out.text().find("Goodbye.") != std::string::npos);

    const Conversation& c = h.conversation();
    assert(c.size() == 5);
    assert(c.at(0).role() == Role::System && c.at(0).content() == "Be concise.");
    assert(c.at(4).content() == "Goodbye." + kSentinel && "stored reply keeps the sentinel");
}

TEST(HarnessEndsCleanlyOnEof) {
    std::string path = write_file("eof.script",
        "role: assistant\nreply one\n---\nrole: assistant\nreply two\n");
    Harness h(std::make_unique<ScriptedModelClient>(path), HarnessConfig{});
    ScriptedInput in({"only line"});
    CapturingOutput out;

    StopReason r = h.run(in, out);
    assert(r.kind == StopReason::Kind::UserExit);
    assert(h.conversation().size() == 2);
    assert(h.conversation().at(1).content() == "reply one");
}

TEST(HarnessReportsClientErrorWhenScriptRunsOut) {
    std::string path = write_file("short.script", "role: assistant\nonly reply\n");
    Harness h(std::make_unique<ScriptedModelClient>(path), HarnessConfig{});
    ScriptedInput in({"one", "two"});
    CapturingOutput out;
    assert(h.run(in, out).kind == StopReason::Kind::ClientError);
}

TEST(TranscriptRoundTripReplaysIdentically) {
    std::string script = write_file("roundtrip.script",
        "role: system\nBe concise.\n---\n"
        "chunk: 5\nrole: assistant\nI am doing well.\nHow can I help?\n---\n"
        "chunk: 7\nrole: assistant\nSure, done.\n---\n"
        "chunk: 6\nrole: assistant\nGoodbye!" + kSentinel + "\n");
    const std::deque<std::string> inputs = {"hi", "do a thing", "bye", "unused"};

    ScriptedModelClient probe(script);
    HarnessConfig cfg1;
    cfg1.system_message = probe.system_message();
    Harness first(std::make_unique<ScriptedModelClient>(script), cfg1);
    ScriptedInput in1(inputs);
    CapturingOutput out1;
    StopReason r1 = first.run(in1, out1);
    assert(r1.kind == StopReason::Kind::Sentinel);

    std::string transcript = temp_path("roundtrip_transcript.txt");
    save_transcript(first.conversation(), transcript);

    auto replay = std::make_unique<ReplayModelClient>(transcript);
    HarnessConfig cfg2;
    cfg2.system_message = replay->system_message();
    Harness second(std::move(replay), cfg2);
    ScriptedInput in2(inputs);
    CapturingOutput out2;
    StopReason r2 = second.run(in2, out2);

    assert(r2.kind == r1.kind && r2.detail == r1.detail);
    assert(out2.text() == out1.text() && "replayed session must print the same text");
    const Conversation& a = first.conversation();
    const Conversation& b = second.conversation();
    assert(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        assert(a.at(i).role() == b.at(i).role());
        assert(a.at(i).content() == b.at(i).content());
    }
}

int main() {
    for (int i = 0; i < g_count; ++i) {
        std::printf("[ RUN  ] %s\n", g_tests[i].name);
        g_tests[i].fn();
        std::printf("[  OK  ] %s\n", g_tests[i].name);
    }
    std::printf("%d tests passed\n", g_count);
    return 0;
}
