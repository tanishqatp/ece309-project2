# Design Log — Project 2

## Growth factor and amortized cost

I went with doubling. The capacity goes 0 → 1 on the first append and then doubles whenever the array is full (1, 2, 4, 8, ...). A reallocation only happens when `append` is called with `size_ == capacity_`, so the capacity is always a power of two.

The claim is that n appends take O(n) total work, so each one is O(1) amortized. I used the aggregate method. After n appends the capacity C is the smallest power of two ≥ n, so C < 2n. Growing from c to 2c does three things: `new Message[2c]` default-constructs 2c slots, the c existing messages get moved over, and `delete[]` destroys the c old slots. Adding these up over every reallocation:

- moves: 1 + 2 + ... + C/2 = C − 1 < 2n
- default constructions: 1 + 2 + ... + C = 2C − 1 < 4n
- destroying old buffers: C − 1 < 2n

With the n writes for the appended messages, the total is under n + 2n + 4n + 2n = 9n, which is O(n), so O(1) per append.

I considered 1.5, whose main advantage is letting the allocator reuse old freed blocks. With conversations of a few dozen messages that didn't matter, and doubling means fewer reallocations and simpler math. My test `CapacityDoublesAndContentsSurviveReallocation` checks the 1, 2, 4, ... sequence after every append, that 1000 appends cause exactly 11 reallocations, and that every message is still correct afterward.

## Rule of Five evidence

- **Destructor:** just `delete[] data_`. For an empty or moved-from object `data_` is `nullptr`, and `delete[] nullptr` does nothing.
- **Copy constructor:** allocates its own buffer and copies each message. If a string copy throws, the `catch` frees the new buffer before rethrowing.
- **Copy assignment:** copy-and-swap. The copy is built in a temporary first, so if it throws the original is untouched. Self-assignment works without a special check, and the temporary frees the old buffer.
- **Move constructor and move assignment:** both `noexcept`. They take the pointer and reset the source to `{nullptr, 0, 0}`. Move assignment frees its own buffer first and checks for self-move.
- **Growing:** only `new[]` can throw, and it runs before the old buffer is touched. After that I only move elements, and `std::string`'s move assignment is `noexcept`.

My tests check that copies get a different `begin()` and stay unchanged when the original changes, and that moves keep the same `begin()` and leave the source zeroed and reusable. Everything runs under AddressSanitizer, UBSan and LeakSanitizer with no errors. As a sanity check I swapped in a shallow copy on purpose, and `CopyConstructorIsDeep` failed right away.

## Sentinel scanner: bounded pending_ proof

Let s be the sentinel with length L (an empty sentinel is rejected, so L ≥ 1). Each `feed` looks at b = `pending_` + chunk. If s is in b, it returns the text before the first match, reports `sentinel_found`, and clears `pending_`. Otherwise it finds the largest k ≤ min(|b|, L − 1) where the last k characters of b match the first k characters of s, keeps those in `pending_`, and returns the rest.

**Bound:** after any `feed`, `pending_` is empty or exactly k characters with k ≤ L − 1. That doesn't depend on the old `pending_`, so starting from empty it holds after every call by induction, and `flush` empties it. Each call uses O(L + |chunk|) memory instead of the whole reply.

**Correctness:** say the sentinel starts at position p in b. If it fits in b, `find` catches it. If it runs past the end, b[p..] is a proper prefix of s of length at most L − 1, so it's one of the candidate suffixes. Keeping the longest one keeps everything from the earliest possible start, so p is held back, not printed. By induction, nothing printed earlier can be part of a later match.

The spec suggests always keeping the last L − 1 characters. Mine only holds back text that actually matches the start of the sentinel, so normal text prints right away, with the same bound. `ScannerPendingStaysBoundedOnFourMegabyteStream` feeds 4 MiB of near-miss patterns one byte at a time, checks `pending_` never goes over 19, and checks it actually reaches 19.

## What I would change differently

`new Message[cap]` default-constructs every spare slot. That's the only reason `Message` needs a default constructor, and it's the biggest term (4n) in my proof. Next time I'd allocate raw memory with `::operator new`, placement-new only the slots in use, and destroy them by hand. For the scanner, I'd use a KMP failure table to track how much of the sentinel has matched instead of rebuilding `pending_ + chunk` every call, which makes each byte O(1) amortized even for a long sentinel.
