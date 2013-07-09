#ifndef SRC_COMMON_UTIL_RED_BLACK_TREE_H
#define SRC_COMMON_UTIL_RED_BLACK_TREE_H

/* derived from IOFSL's red-black tree */
#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <stdint.h>

/* defines */

/* colors: red, black, and 0 (for undefined) */
#define RB_TREE_COLOR_BLACK 'B'
#define RB_TREE_COLOR_RED 'R'
#define RB_TREE_COLOR_NONE 0

/* ops: hints for directing how the rank and max values should be updated */
#define RB_TREE_INSERT 0
#define RB_TREE_DELETE 1
#define RB_TREE_RANK_INC 2
#define RB_TREE_RANK_DEC 3

/* overlap cases: what type of overlap was detected */
#define RB_TREE_INTERVAL_NO_OVERLAP 0
#define RB_TREE_INTERVAL_C1 1
#define RB_TREE_INTERVAL_C2 2
#define RB_TREE_INTERVAL_C3 3
#define RB_TREE_INTERVAL_C4 4
#define RB_TREE_INTERVAL_SAME 5

#define RB_TREE_NODE_POOL_SIZE_MAX 16

#define RB_TREE_CONSUME 2

/* data types */

/*
 * red_black_tree_interval_t, describes the range of the interval the node encompasses
 *  start - the start of the interval
 *  end - the end of the interval
 */
struct red_black_tree_interval
{
    size_t start;
    size_t end;
};
typedef struct red_black_tree_interval red_black_tree_interval_t;

/*
 * red_black_tree_key_t, container for the key and value pair stored in the node
 *  key - the key for the node
 *  value - the value associated with the key
 */
struct red_black_tree_key
{
    void * key;
    void * value;
};
typedef struct red_black_tree_key red_black_tree_key_t;

struct red_black_tree_interval_ll
{
    void * data;
    void * key;
    struct red_black_tree_interval_ll * next;
};
typedef struct red_black_tree_interval_ll red_black_tree_interval_ll_t;

/*
 * red_black_tree_node_t, the node structure for the rb tree
 *  key - the key / value pair
 *  left - the left child of the node
 *  right - the right child of the node
 *  parent - the parent of the node
 *  size - the number of descendants of this node (includes itself)
 *  max - the max interval of descendant nodes (includes self)
 *  interval - the range associated with this node
 *  color - the color of the node
 */
struct red_black_tree_node
{
    red_black_tree_key_t key;
    struct red_black_tree_node * left;
    struct red_black_tree_node * right;
    struct red_black_tree_node * parent;
    unsigned int size;
    unsigned int max;
    red_black_tree_interval_t interval;
    red_black_tree_interval_ll_t * ll_head;
    red_black_tree_interval_ll_t * ll_tail;
    char color;
    char node_pool;
    struct red_black_tree_node * consumer;
};
typedef struct red_black_tree_node red_black_tree_node_t;

/* functions */
int red_black_tree_interval_ll_init(red_black_tree_node_t * node, void * data);
int red_black_tree_interval_ll_destroy(red_black_tree_interval_ll_t * ll);
int red_black_tree_destroy_entire_key(red_black_tree_key_t * key);
red_black_tree_node_t * red_black_tree_create_node();
int red_black_tree_destroy_node(red_black_tree_node_t * node);
int red_black_tree_destroy_tree(red_black_tree_node_t * node);
int red_black_tree_insert(red_black_tree_node_t ** node, red_black_tree_key_t key, red_black_tree_node_t ** root);
int red_black_tree_insert_interval(red_black_tree_node_t ** node, red_black_tree_key_t * key, red_black_tree_node_t ** root);
int red_black_tree_print_tree(red_black_tree_node_t * node);
int red_black_tree_print(red_black_tree_node_t * node, int level, char dir);
red_black_tree_key_t red_black_tree_key_find(red_black_tree_node_t * node, size_t k);
red_black_tree_node_t * red_black_tree_node_find(red_black_tree_node_t * node, size_t k);
red_black_tree_node_t * red_black_tree_delete(red_black_tree_node_t ** root, red_black_tree_node_t * node);
red_black_tree_node_t * red_black_tree_find_delete(red_black_tree_node_t ** root, size_t k);
red_black_tree_node_t * red_black_tree_interval_search(red_black_tree_node_t * root, red_black_tree_interval_t interval, unsigned int * oc);
int red_black_tree_merge_intervals(red_black_tree_node_t ** root, red_black_tree_node_t ** n_node, red_black_tree_key_t key);
size_t red_black_tree_size(red_black_tree_node_t * node);
red_black_tree_node_t * red_black_tree_delete_min_node(red_black_tree_node_t ** root);

#ifdef __cplusplus
}
#endif

#endif
