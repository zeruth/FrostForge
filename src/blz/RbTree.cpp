#include "blz/RbTree.hpp"

namespace blz {

namespace {

void RotateLeft(rb_node_base** root, rb_node_base* node) {
    rb_node_base* right = node->m_right;
    node->m_right = right->m_left;
    if (right->m_left != nullptr) {
        right->m_left->m_parent = node;
    }
    right->m_parent = node->m_parent;
    if (node == *root) {
        *root = right;
    } else if (node == node->m_parent->m_left) {
        node->m_parent->m_left = right;
    } else {
        node->m_parent->m_right = right;
    }
    right->m_left = node;
    node->m_parent = right;
}

void RotateRight(rb_node_base** root, rb_node_base* node) {
    rb_node_base* left = node->m_left;
    node->m_left = left->m_right;
    if (left->m_right != nullptr) {
        left->m_right->m_parent = node;
    }
    left->m_parent = node->m_parent;
    if (node == *root) {
        *root = left;
    } else if (node == node->m_parent->m_right) {
        node->m_parent->m_right = left;
    } else {
        node->m_parent->m_left = left;
    }
    left->m_right = node;
    node->m_parent = left;
}

}

// ref: FUN_1402f86b0
void rb_insert_fixup(rb_node_base** root, rb_node_base* node) {
    while (node != *root) {
        rb_node_base* parent = node->m_parent;
        if (parent->m_color != 0) {
            break;
        }
        rb_node_base* grandparent = parent->m_parent;
        if (parent == grandparent->m_left) {
            rb_node_base* uncle = grandparent->m_right;
            if (uncle != nullptr && uncle->m_color == 0) {
                parent->m_color = 1;
                uncle->m_color = 1;
                grandparent->m_color = 0;
                node = grandparent;
                continue;
            }
            if (node == parent->m_right) {
                RotateLeft(root, parent);
                node = parent;
                parent = node->m_parent;
            }
            parent->m_color = 1;
            grandparent->m_color = 0;
            RotateRight(root, grandparent);
        } else {
            rb_node_base* uncle = grandparent->m_left;
            if (uncle != nullptr && uncle->m_color == 0) {
                parent->m_color = 1;
                uncle->m_color = 1;
                grandparent->m_color = 0;
                node = grandparent;
                continue;
            }
            if (node == parent->m_left) {
                RotateRight(root, parent);
                node = parent;
                parent = node->m_parent;
            }
            parent->m_color = 1;
            grandparent->m_color = 0;
            RotateLeft(root, grandparent);
        }
    }
    (*root)->m_color = 1;
}

}
