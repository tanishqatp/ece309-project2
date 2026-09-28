# ECE 309 Project 2: The Conversation Loop

`miniharness` is a small C++17 LLM harness: the program that sits between a language model and the user. It keeps the conversation history, streams the model's reply to the terminal as it arrives, and ends the session when the model sends the stop sentinel `<|end_conversation|>`, when a turn limit is reached, or when the user presses Ctrl-D. The "model" is a `.script` file, so every run is deterministic and testable.

## What I implemented

- **`Message`** (`include/core/message.h`): a role (System, User, or Assistant) plus text content.
- **`Conversation`** (`include/core/conversation.h`, `src/conversation.cpp`): a growable array of messages written without `std::vector`. It doubles its capacity when full, so appends are amortized O(1). It follows the Rule of Five, with deep copies and pointer-stealing moves, and it keeps a system message pinned first.
- **`SentinelScanner`** (`include/core/sentinel_scanner.h`, `src/sentinel_scanner.cpp`): detects the stop sentinel in a stream that arrives in arbitrary chunks, even when it's split across them. It never holds back more than `sentinel length - 1` characters, and it never prints the sentinel.
- **Tests** (`tests/p2/test_p2.cpp`): 25 assert-based tests covering the container, the scanner (including a 4 MB byte-at-a-time stress test), and the harness (turn limit, sentinel halt, EOF, and transcript round-trip).
- **Design log** (`docs/design-log-p2.md`): the amortized-cost proof, Rule of Five reasoning, and the bounded-buffer proof for the scanner.

The model clients, the `Harness` run loop, and `main.cpp` were provided as starter code and are unchanged.

## Build and run

Requires CMake and a C++17 compiler. The build enables AddressSanitizer and UBSan.

```bash
cmake -S . -B build
cmake --build build
./build/test_p2
```

Try a conversation:

```bash
./build/miniharness --script scripts/greeting.script --save transcript.txt
```

The options are `--script <file>` for the scripted model replies, `--max-turns N` (default 20), and `--save <file>` to write the transcript. Press Ctrl-D to end early.
