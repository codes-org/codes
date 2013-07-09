#ifndef RB_TREE_H
#define RB_TREE_H

#ifdef __cplusplus
extern "C"
{
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
 * interval_merge_tree_interval_t, describes the range of the interval the node encompasses
 *  start - the start of the interval
 *  end - the end of the interval
 */
    struct interval_merge_tree_interval
    {
        size_t start;
        size_t end;
    };
    typedef struct interval_merge_tree_interval interval_merge_tree_interval_t;

/*
 * interval_merge_tree_key_t, container for the key and value pair stored in the node
 *  key - the key for the node
 *  value - the value associated with the key
 */
    struct interval_merge_tree_key
    {
        void *key;
        void *value;
    };
    typedef struct interval_merge_tree_key interval_merge_tree_key_t;

    struct interval_merge_tree_interval_ll
    {
        void *data;
        void *key;
        struct interval_merge_tree_interval_ll *next;
    };
    typedef struct interval_merge_tree_interval_ll interval_merge_tree_interval_ll_t;

/*
 * interval_merge_tree_node_t, the node structure for the rb tree
 *  key - the key / value pair
 *  left - the left child of the node
 *  right - the right child of the node
 *  parent - the parent of the node
 *  size - the number of descendants of this node (includes itself)
 *  max - the max interval of descendant nodes (includes self)
 *  interval - the range associated with this node
 *  color - the color of the node
 */
    struct interval_merge_tree_node
    {
        interval_merge_tree_key_t key;
        struct interval_merge_tree_node *left;
        struct interval_merge_tree_node *right;
        struct interval_merge_tree_node *parent;
        unsigned int size;
        unsigned int max;
        interval_merge_tree_interval_t interval;
        interval_merge_tree_interval_ll_t *ll_head;
        interval_merge_tree_interval_ll_t *ll_tail;
        char color;
        char node_pool;
        struct interval_merge_tree_node *consumer;
    };
    typedef struct interval_merge_tree_node interval_merge_tree_node_t;

/* functions */
    int interval_merge_tree_interval_ll_init(
    interval_merge_tree_node_t * node,
    void *data);
    int interval_merge_tree_interval_ll_destroy(
    interval_merge_tree_interval_ll_t * ll);
    int interval_merge_tree_destroy_entire_key(
    interval_merge_tree_key_t * key);
    interval_merge_tree_node_t *interval_merge_tree_create_node(
        );
    int interval_merge_tree_destroy_node(
    interval_merge_tree_node_t * node);
    int interval_merge_tree_destroy_tree(
    interval_merge_tree_node_t * node);
    int interval_merge_tree_insert(
    interval_merge_tree_node_t ** node,
    interval_merge_tree_key_t key,
    interval_merge_tree_node_t ** root);
    int interval_merge_tree_insert_interval(
    interval_merge_tree_node_t ** node,
    interval_merge_tree_key_t * key,
    interval_merge_tree_node_t ** root);
    int interval_merge_tree_print_tree(
    interval_merge_tree_node_t * node);
    int interval_merge_tree_print(
    interval_merge_tree_node_t * node,
    int level,
    char dir);
    interval_merge_tree_key_t interval_merge_tree_key_find(
    interval_merge_tree_node_t * node,
    size_t k);
    interval_merge_tree_node_t *interval_merge_tree_node_find(
    interval_merge_tree_node_t * node,
    size_t k);
    interval_merge_tree_node_t *interval_merge_tree_delete(
    interval_merge_tree_node_t ** root,
    interval_merge_tree_node_t * node);
    interval_merge_tree_node_t *interval_merge_tree_find_delete(
    interval_merge_tree_node_t ** root,
    size_t k);
    interval_merge_tree_node_t *interval_merge_tree_node_find_rank(
    interval_merge_tree_node_t * node,
    size_t k,
    int op);
    interval_merge_tree_node_t *interval_merge_tree_interval_search(
    interval_merge_tree_node_t * root,
    interval_merge_tree_interval_t interval,
    unsigned int *oc);
    int interval_merge_tree_merge_intervals(
    interval_merge_tree_node_t ** root,
    interval_merge_tree_node_t ** n_node,
    interval_merge_tree_key_t key);
    size_t interval_merge_tree_size(
    interval_merge_tree_node_t * node);
    interval_merge_tree_node_t *interval_merge_tree_delete_min_node(
    interval_merge_tree_node_t ** root);

#ifdef __cplusplus
}
#endif

#endif
