/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file examples/phase_arena.cpp
 * @brief The bump arena: when everything dies together, stop tracking it.
 *
 * When all the memory of a phase dies at the same time, a general-purpose
 * allocator is doing work nobody asked for: keeping track of every block so it
 * can be returned on its own.  Here, allocating is bumping a pointer and
 * freeing is nothing at all -- the whole phase is released with one write.
 *
 * WHEN NOT TO USE IT, and this matters more than the speed.  A matching
 * lifetime is not enough: the allocation PATTERN has to match too.  An arena
 * cannot reclaim anything until the end, and a growing container allocates a
 * new buffer and abandons the old one every time it grows.  Putting a range
 * analysis in here -- whose lifetime fitted perfectly -- took peak memory from
 * 2,414 MB to 5,455 MB (+126%) in exchange for 4% speed, because a million
 * insertions kept leaving remains behind.
 *
 * So: fixed-size objects, nodes that get linked, single-pass work buffers.
 * For small repeated allocations of containers that grow, use the size-class
 * allocator in `util/host_allocator.h`, which DOES return the old buffer.
 */

#include "util/host_allocator.h"
#include "util/scratch_arena.h"

#include <chrono>
#include <cstdio>
#include <vector>

namespace {

struct Node {
    Node *next;
    int value;
};

/// Build a linked list of @p n nodes and walk it.  Nodes never grow and all
/// die together: the arena's sweet spot.
long long build_and_walk_arena(int n) {
    // Marks on entry, rewinds on exit -- even if the body throws.
    util::ScratchScope phase;
    util::ScratchArena &arena = util::scratch_arena();

    Node *head = nullptr;
    for (int i = 0; i < n; ++i) {
        void *mem = arena.allocate(sizeof(Node), alignof(Node));
        if (mem == nullptr) return -1; // out of memory: say so, do not guess
        Node *node = static_cast<Node *>(mem);
        node->value = i;
        node->next = head;
        head = node;
    }
    long long sum = 0;
    for (Node *p = head; p != nullptr; p = p->next)
        sum += p->value;
    return sum;
    // No frees here.  The scope rewinds the arena on the way out.
}

/// Same thing through the general allocator, for comparison.
long long build_and_walk_general(int n) {
    std::vector<Node *> owned;
    owned.reserve(size_t(n));
    Node *head = nullptr;
    for (int i = 0; i < n; ++i) {
        Node *node = static_cast<Node *>(util::host_alloc(sizeof(Node)));
        if (node == nullptr) return -1;
        node->value = i;
        node->next = head;
        head = node;
        owned.push_back(node);
    }
    long long sum = 0;
    for (Node *p = head; p != nullptr; p = p->next)
        sum += p->value;
    for (Node *p : owned)
        util::host_free(p);
    return sum;
}

template <class F> double time_per_node(F f, int n, int rounds) {
    const auto t0 = std::chrono::steady_clock::now();
    long long acc = 0;
    for (int r = 0; r < rounds; ++r)
        acc += f(n);
    const auto dt = std::chrono::steady_clock::now() - t0;
    if (acc < 0) return -1.0;
    return double(std::chrono::duration_cast<std::chrono::nanoseconds>(dt)
                      .count()) /
           double(n) / double(rounds);
}

} // namespace

int main() {
    std::printf("== phase arena ==\n\n");

    const int n = 50000, rounds = 40;
    // Warm both paths up before timing either of them.
    build_and_walk_arena(n);
    build_and_walk_general(n);

    const double arena = time_per_node(build_and_walk_arena, n, rounds);
    const double general = time_per_node(build_and_walk_general, n, rounds);

    std::printf("%d nodes x %d rounds, allocate + walk + release:\n\n", n,
                rounds);
    std::printf("  scratch arena      %6.2f ns/node\n", arena);
    std::printf("  general allocator  %6.2f ns/node\n", general);
    if (arena > 0.0 && general > 0.0)
        std::printf("  ratio              %6.2fx\n", general / arena);

    std::printf("\nThe arena wins because it does less, not because it is\n"
                "cleverer: no per-block bookkeeping and no free path at all.\n"
                "That is also why it is the wrong choice for anything that\n"
                "grows -- read the note at the top of this file.\n");
    return 0;
}
