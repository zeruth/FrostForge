#ifndef BLZ_RB_TREE_HPP
#define BLZ_RB_TREE_HPP

// blz::map's red-black tree as the engine lays it out: each node starts {parent, left, right,
// colour (0 red, 1 black)}, and the header is a node whose parent is the root, whose left is
// the leftmost node and whose right is the rightmost; the root's parent is the header.

#include <cstdint>

namespace blz {

struct rb_node_base {
    rb_node_base* m_parent;
    rb_node_base* m_left;
    rb_node_base* m_right;
    uint8_t m_color;
};

// Rebalances after `node` was linked in as a red leaf (FUN_1402f86b0). `root` is the header's
// root slot.
void rb_insert_fixup(rb_node_base** root, rb_node_base* node);

// The in-order successor, as the engine's iterators step (the header ends the walk).
inline rb_node_base* rb_next(rb_node_base* node) {
    rb_node_base* right = node->m_right;
    if (right != nullptr) {
        while (right->m_left != nullptr) {
            right = right->m_left;
        }
        return right;
    }
    rb_node_base* parent = node->m_parent;
    while (node == parent->m_right) {
        node = parent;
        parent = node->m_parent;
    }
    if (node->m_right != parent) {
        node = parent;
    }
    return node;
}

// The in-order predecessor; the header's is the rightmost node.
inline rb_node_base* rb_prev(rb_node_base* node) {
    if (node->m_color == 0 && node->m_parent->m_parent == node) {
        return node->m_right;
    }
    rb_node_base* left = node->m_left;
    if (left != nullptr) {
        while (left->m_right != nullptr) {
            left = left->m_right;
        }
        return left;
    }
    rb_node_base* parent = node->m_parent;
    while (node == parent->m_left) {
        node = parent;
        parent = node->m_parent;
    }
    return parent;
}

}

#endif
