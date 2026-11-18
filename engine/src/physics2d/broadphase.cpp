#include "mge/physics2d/broadphase.h"

namespace mge::physics2d {

namespace {
AABB grow(const AABB& a, float m) { return {a.min - vec2{m}, a.max + vec2{m}}; }
constexpr float kDisplacementMultiplier = 2.0f;   // predict two steps of motion
}  // namespace

// ---------------------------------------------------------------- DynamicTree

int32_t DynamicTree::allocNode() {
    int32_t id;
    if (freeList_ != kNullProxy) {
        id = freeList_;
        freeList_ = nodes_[size_t(id)].parent;
    } else {
        id = int32_t(nodes_.size());
        nodes_.push_back({});
    }
    nodes_[size_t(id)] = Node{};
    nodes_[size_t(id)].height = 0;
    return id;
}

void DynamicTree::freeNode(int32_t id) {
    Node& n = nodes_[size_t(id)];
    n.parent = freeList_;
    n.child1 = n.child2 = kNullProxy;
    n.height = -1;
    freeList_ = id;
}

int32_t DynamicTree::createProxy(const AABB& aabb, uint64_t userData) {
    MGE_ASSERT(aabb.valid(), "invalid AABB");
    const int32_t id = allocNode();
    nodes_[size_t(id)].aabb = grow(aabb, kAABBMargin);
    nodes_[size_t(id)].userData = userData;
    insertLeaf(id);
    ++proxyCount_;
    return id;
}

void DynamicTree::destroyProxy(int32_t proxy) {
    MGE_ASSERT(proxy >= 0 && size_t(proxy) < nodes_.size() && nodes_[size_t(proxy)].isLeaf() &&
               nodes_[size_t(proxy)].height == 0, "not a proxy");
    removeLeaf(proxy);
    freeNode(proxy);
    --proxyCount_;
}

bool DynamicTree::moveProxy(int32_t proxy, const AABB& aabb, vec2 displacement) {
    MGE_ASSERT(aabb.valid(), "invalid AABB");
    AABB fat = grow(aabb, kAABBMargin);
    const vec2 d = displacement * kDisplacementMultiplier;
    if (d.x < 0.0f) fat.min.x += d.x; else fat.max.x += d.x;
    if (d.y < 0.0f) fat.min.y += d.y; else fat.max.y += d.y;

    const AABB& current = nodes_[size_t(proxy)].aabb;
    if (current.contains(aabb)) {
        // Still inside. Keep it unless the fat box has become much bigger than needed
        // (a fast body that slowed down), which would cause false pairs.
        if (grow(fat, 4.0f * kAABBMargin).contains(current)) return false;
    }
    removeLeaf(proxy);
    nodes_[size_t(proxy)].aabb = fat;
    insertLeaf(proxy);
    return true;
}

void DynamicTree::insertLeaf(int32_t leaf) {
    if (root_ == kNullProxy) {
        root_ = leaf;
        nodes_[size_t(leaf)].parent = kNullProxy;
        return;
    }
    // Walk down to the sibling that makes the tree's total perimeter grow least.
    const AABB leafBox = nodes_[size_t(leaf)].aabb;
    int32_t index = root_;
    while (!nodes_[size_t(index)].isLeaf()) {
        const Node& n = nodes_[size_t(index)];
        const float area = n.aabb.perimeter();
        const float combined = combine(n.aabb, leafBox).perimeter();
        const float cost = 2.0f * combined;                 // new parent here, above n
        const float inherit = 2.0f * (combined - area);     // growth every ancestor below pays
        auto childCost = [&](int32_t c) {
            const Node& cn = nodes_[size_t(c)];
            const float grown = combine(leafBox, cn.aabb).perimeter();
            return (cn.isLeaf() ? grown : grown - cn.aabb.perimeter()) + inherit;
        };
        const float cost1 = childCost(n.child1), cost2 = childCost(n.child2);
        if (cost < cost1 && cost < cost2) break;
        index = cost1 < cost2 ? n.child1 : n.child2;
    }
    const int32_t sibling = index;

    const int32_t oldParent = nodes_[size_t(sibling)].parent;
    const int32_t newParent = allocNode();   // may reallocate nodes_: no references held across
    Node& np = nodes_[size_t(newParent)];
    np.parent = oldParent;
    np.aabb = combine(leafBox, nodes_[size_t(sibling)].aabb);
    np.height = nodes_[size_t(sibling)].height + 1;
    np.child1 = sibling;
    np.child2 = leaf;
    if (oldParent != kNullProxy) {
        Node& op = nodes_[size_t(oldParent)];
        (op.child1 == sibling ? op.child1 : op.child2) = newParent;
    } else {
        root_ = newParent;
    }
    nodes_[size_t(sibling)].parent = newParent;
    nodes_[size_t(leaf)].parent = newParent;

    for (index = nodes_[size_t(leaf)].parent; index != kNullProxy; index = nodes_[size_t(index)].parent) {
        index = balance(index);
        Node& n = nodes_[size_t(index)];
        const Node& c1 = nodes_[size_t(n.child1)];
        const Node& c2 = nodes_[size_t(n.child2)];
        n.height = 1 + mge::max(c1.height, c2.height);
        n.aabb = combine(c1.aabb, c2.aabb);
    }
}

void DynamicTree::removeLeaf(int32_t leaf) {
    if (leaf == root_) {
        root_ = kNullProxy;
        return;
    }
    const int32_t parent = nodes_[size_t(leaf)].parent;
    const int32_t grand = nodes_[size_t(parent)].parent;
    const int32_t sibling = nodes_[size_t(parent)].child1 == leaf ? nodes_[size_t(parent)].child2
                                                                  : nodes_[size_t(parent)].child1;
    if (grand == kNullProxy) {
        root_ = sibling;
        nodes_[size_t(sibling)].parent = kNullProxy;
        freeNode(parent);
        return;
    }
    Node& g = nodes_[size_t(grand)];
    (g.child1 == parent ? g.child1 : g.child2) = sibling;
    nodes_[size_t(sibling)].parent = grand;
    freeNode(parent);
    for (int32_t index = grand; index != kNullProxy; index = nodes_[size_t(index)].parent) {
        index = balance(index);
        Node& n = nodes_[size_t(index)];
        n.aabb = combine(nodes_[size_t(n.child1)].aabb, nodes_[size_t(n.child2)].aabb);
        n.height = 1 + mge::max(nodes_[size_t(n.child1)].height, nodes_[size_t(n.child2)].height);
    }
}

// If one subtree of A is more than one level taller, rotate its root up into A's place.
// Returns the index now at A's position.
int32_t DynamicTree::balance(int32_t iA) {
    Node& A = nodes_[size_t(iA)];
    if (A.isLeaf() || A.height < 2) return iA;
    const int32_t iB = A.child1, iC = A.child2;
    Node& B = nodes_[size_t(iB)];
    Node& C = nodes_[size_t(iC)];
    const int32_t bal = C.height - B.height;

    auto lift = [&](int32_t iUp, Node& up) {   // `up` takes A's place under A's parent
        up.parent = A.parent;
        A.parent = iUp;
        if (up.parent != kNullProxy) {
            Node& p = nodes_[size_t(up.parent)];
            (p.child1 == iA ? p.child1 : p.child2) = iUp;
        } else {
            root_ = iUp;
        }
    };

    if (bal > 1) {   // C is too tall: C goes up, A takes C's shorter child
        const int32_t iF = C.child1, iG = C.child2;
        Node& F = nodes_[size_t(iF)];
        Node& G = nodes_[size_t(iG)];
        C.child1 = iA;
        lift(iC, C);
        const bool keepF = F.height > G.height;
        const int32_t iKeep = keepF ? iF : iG, iGive = keepF ? iG : iF;
        Node& keep = keepF ? F : G;
        Node& give = keepF ? G : F;
        C.child2 = iKeep;
        A.child2 = iGive;
        give.parent = iA;
        A.aabb = combine(B.aabb, give.aabb);
        C.aabb = combine(A.aabb, keep.aabb);
        A.height = 1 + mge::max(B.height, give.height);
        C.height = 1 + mge::max(A.height, keep.height);
        return iC;
    }
    if (bal < -1) {  // B is too tall: B goes up, A takes B's shorter child
        const int32_t iD = B.child1, iE = B.child2;
        Node& D = nodes_[size_t(iD)];
        Node& E = nodes_[size_t(iE)];
        B.child1 = iA;
        lift(iB, B);
        const bool keepD = D.height > E.height;
        const int32_t iKeep = keepD ? iD : iE, iGive = keepD ? iE : iD;
        Node& keep = keepD ? D : E;
        Node& give = keepD ? E : D;
        B.child2 = iKeep;
        A.child1 = iGive;
        give.parent = iA;
        A.aabb = combine(C.aabb, give.aabb);
        B.aabb = combine(A.aabb, keep.aabb);
        A.height = 1 + mge::max(C.height, give.height);
        B.height = 1 + mge::max(A.height, keep.height);
        return iB;
    }
    return iA;
}

float DynamicTree::areaRatio() const {
    if (root_ == kNullProxy) return 0.0f;
    const float rootArea = nodes_[size_t(root_)].aabb.perimeter();
    float total = 0.0f;
    for (const Node& n : nodes_)
        if (n.height >= 0) total += n.aabb.perimeter();
    return rootArea > 0.0f ? total / rootArea : 0.0f;
}

int32_t DynamicTree::validateNode(int32_t id, bool& ok) const {
    const Node& n = nodes_[size_t(id)];
    if (n.isLeaf()) {
        if (n.child2 != kNullProxy || n.height != 0) ok = false;
        return 1;
    }
    const Node& c1 = nodes_[size_t(n.child1)];
    const Node& c2 = nodes_[size_t(n.child2)];
    if (c1.parent != id || c2.parent != id) ok = false;
    if (n.height != 1 + mge::max(c1.height, c2.height)) ok = false;
    if (c1.height - c2.height > 1 || c2.height - c1.height > 1) ok = false;
    if (!n.aabb.contains(c1.aabb) || !n.aabb.contains(c2.aabb)) ok = false;
    return validateNode(n.child1, ok) + validateNode(n.child2, ok);
}

bool DynamicTree::validate() const {
    if (root_ == kNullProxy) return proxyCount_ == 0;
    bool ok = nodes_[size_t(root_)].parent == kNullProxy;
    const int32_t leaves = validateNode(root_, ok);
    int32_t freeCount = 0;
    for (int32_t f = freeList_; f != kNullProxy; f = nodes_[size_t(f)].parent) ++freeCount;
    // A full binary tree of L leaves has 2L - 1 nodes; everything else is on the free list.
    return ok && leaves == proxyCount_ && int32_t(nodes_.size()) == 2 * leaves - 1 + freeCount;
}

// ---------------------------------------------------------------- BroadPhase

void BroadPhase::bufferMove(int32_t proxy) {
    if (size_t(proxy) >= moved_.size()) moved_.resize(size_t(proxy) + 1, 0);
    if (moved_[size_t(proxy)]) return;
    moved_[size_t(proxy)] = 1;
    moveBuffer_.push_back(proxy);
}

void BroadPhase::unbufferMove(int32_t proxy) {
    if (size_t(proxy) >= moved_.size() || !moved_[size_t(proxy)]) return;
    moved_[size_t(proxy)] = 0;
    for (int32_t& m : moveBuffer_)
        if (m == proxy) m = kNullProxy;
}

int32_t BroadPhase::createProxy(const AABB& aabb, uint64_t userData) {
    const int32_t id = tree_.createProxy(aabb, userData);
    bufferMove(id);
    return id;
}

void BroadPhase::destroyProxy(int32_t proxy) {
    unbufferMove(proxy);
    tree_.destroyProxy(proxy);
}

void BroadPhase::moveProxy(int32_t proxy, const AABB& aabb, vec2 displacement) {
    if (tree_.moveProxy(proxy, aabb, displacement)) bufferMove(proxy);
}

void BroadPhase::touchProxy(int32_t proxy) { bufferMove(proxy); }

}  // namespace mge::physics2d
