#pragma once
// physics2d broadphase (M1 #18).
//  - DynamicTree: bounding volume hierarchy of fat AABBs. Leaves are inserted where they grow
//    the tree's surface area the least, and every ancestor is rebalanced with AVL-style
//    rotations, so queries stay O(log n). A proxy only moves in the tree when its shape leaves
//    its fat AABB (margin + predicted displacement), so resting and slow bodies cost nothing.
//  - BroadPhase: the tree plus a move buffer. updatePairs() reports each candidate pair once,
//    only for proxies that were created or re-inserted since the last call.
// Nodes live in one array (indices, not pointers) and queries use a fixed stack: no per-frame
// allocation once capacity is reached.
#include <algorithm>
#include <cstdint>
#include <vector>
#include "mge/assert.h"
#include "mge/physics2d/shapes.h"

namespace mge::physics2d {

constexpr int32_t kNullProxy = -1;

class DynamicTree {
public:
    int32_t createProxy(const AABB& aabb, uint64_t userData);
    void destroyProxy(int32_t proxy);
    // `aabb` is the shape's tight box, `displacement` its expected motion this step. Returns true
    // when the proxy had to be re-inserted (its fat AABB changed).
    bool moveProxy(int32_t proxy, const AABB& aabb, vec2 displacement = {});

    const AABB& fatAABB(int32_t proxy) const { return nodes_[size_t(proxy)].aabb; }
    uint64_t userData(int32_t proxy) const { return nodes_[size_t(proxy)].userData; }
    bool isLeaf(int32_t proxy) const { return nodes_[size_t(proxy)].isLeaf(); }

    // callback(int32_t proxy) -> bool (false stops the query).
    template <class F> void query(const AABB& aabb, F&& callback) const;
    // callback(const RayInput& clipped, int32_t proxy) -> float: the new max fraction
    // (0 stops, the input's maxFraction keeps going, a hit fraction clips the ray).
    template <class F> void rayCast(const RayInput& in, F&& callback) const;

    int32_t height() const { return root_ == kNullProxy ? 0 : nodes_[size_t(root_)].height; }
    int32_t proxyCount() const { return proxyCount_; }
    // Sum of all node perimeters over the root's (lower is a better tree).
    float areaRatio() const;
    // Checks parent links, heights, balance and AABB containment. For tests.
    bool validate() const;

private:
    struct Node {
        AABB aabb;
        uint64_t userData = 0;
        int32_t parent = kNullProxy;   // doubles as the free-list link
        int32_t child1 = kNullProxy, child2 = kNullProxy;
        int32_t height = -1;           // leaf = 0, free = -1
        bool isLeaf() const { return child1 == kNullProxy; }
    };
    int32_t allocNode();
    void freeNode(int32_t id);
    void insertLeaf(int32_t leaf);
    void removeLeaf(int32_t leaf);
    int32_t balance(int32_t a);
    int32_t validateNode(int32_t id, bool& ok) const;

    std::vector<Node> nodes_;
    int32_t root_ = kNullProxy;
    int32_t freeList_ = kNullProxy;
    int32_t proxyCount_ = 0;

    static constexpr int kStackSize = 256;
};

template <class F> void DynamicTree::query(const AABB& aabb, F&& callback) const {
    int32_t stack[kStackSize];
    int sp = 0;
    if (root_ != kNullProxy) stack[sp++] = root_;
    while (sp > 0) {
        const int32_t id = stack[--sp];
        const Node& n = nodes_[size_t(id)];
        if (!overlaps(n.aabb, aabb)) continue;
        if (n.isLeaf()) {
            if (!callback(id)) return;
        } else {
            MGE_ASSERT(sp + 2 <= kStackSize, "tree too deep");
            stack[sp++] = n.child1;
            stack[sp++] = n.child2;
        }
    }
}

template <class F> void DynamicTree::rayCast(const RayInput& in, F&& callback) const {
    RayInput ray = in;
    const vec2 d = in.p2 - in.p1;
    if (lengthSq(d) <= 0.0f) return;
    int32_t stack[kStackSize];
    int sp = 0;
    if (root_ != kNullProxy) stack[sp++] = root_;
    while (sp > 0) {
        const int32_t id = stack[--sp];
        const Node& n = nodes_[size_t(id)];
        if (!physics2d::rayCast(n.aabb, ray)) continue;
        if (n.isLeaf()) {
            const float f = callback(ray, id);
            if (f == 0.0f) return;
            if (f > 0.0f && f < ray.maxFraction) ray.maxFraction = f;
        } else {
            MGE_ASSERT(sp + 2 <= kStackSize, "tree too deep");
            stack[sp++] = n.child1;
            stack[sp++] = n.child2;
        }
    }
}

class BroadPhase {
public:
    int32_t createProxy(const AABB& aabb, uint64_t userData);
    void destroyProxy(int32_t proxy);
    // Re-inserts only when the tight box leaves the fat box; then the proxy is queued for pairing.
    void moveProxy(int32_t proxy, const AABB& aabb, vec2 displacement = {});
    // Queue for pairing without moving (e.g. a filter change).
    void touchProxy(int32_t proxy);

    bool testOverlap(int32_t a, int32_t b) const { return overlaps(tree_.fatAABB(a), tree_.fatAABB(b)); }
    const DynamicTree& tree() const { return tree_; }
    int32_t moveCount() const { return int32_t(moveBuffer_.size()); }

    // callback(uint64_t userA, uint64_t userB) for each pair whose fat AABBs overlap and where at
    // least one proxy is in the move buffer; each pair once, ordered by proxy id. Clears the buffer.
    template <class F> void updatePairs(F&& callback);

private:
    struct Pair { int32_t a, b; };
    void bufferMove(int32_t proxy);
    void unbufferMove(int32_t proxy);

    DynamicTree tree_;
    std::vector<int32_t> moveBuffer_;
    std::vector<uint8_t> moved_;      // per proxy id: in the move buffer
    std::vector<Pair> pairs_;
};

template <class F> void BroadPhase::updatePairs(F&& callback) {
    pairs_.clear();
    for (int32_t q : moveBuffer_) {
        if (q == kNullProxy) continue;
        tree_.query(tree_.fatAABB(q), [&](int32_t other) {
            if (other == q) return true;
            // Both moved: only the lower id reports, so the pair appears once.
            if (moved_[size_t(other)] && other < q) return true;
            pairs_.push_back({q < other ? q : other, q < other ? other : q});
            return true;
        });
    }
    for (int32_t q : moveBuffer_)
        if (q != kNullProxy) moved_[size_t(q)] = 0;
    moveBuffer_.clear();
    std::sort(pairs_.begin(), pairs_.end(), [](Pair x, Pair y) { return x.a < y.a || (x.a == y.a && x.b < y.b); });
    for (size_t i = 0; i < pairs_.size(); ++i) {
        if (i > 0 && pairs_[i].a == pairs_[i - 1].a && pairs_[i].b == pairs_[i - 1].b) continue;
        callback(tree_.userData(pairs_[i].a), tree_.userData(pairs_[i].b));
    }
}

}  // namespace mge::physics2d
