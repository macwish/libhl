#include <hashtable.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <limits.h>
#include <strings.h>

#define PERL_HASH_FUNC_ONE_AT_A_TIME
#define PERL_STATIC_INLINE static inline
#define U8  uint8_t
#define U16 uint16_t
#define U32 uint32_t
#define I32 int32_t
#define STRLEN ssize_t
#define STMT_START
#define STMT_END
#include <hv_func.h>

#ifdef THREAD_SAFE
#define MUTEX_LOCK(__mutex) pthread_mutex_lock(__mutex) 
#define MUTEX_UNLOCK(__mutex) pthread_mutex_unlock(__mutex) 
#else
#define MUTEX_LOCK(__mutex)
#define MUTEX_UNLOCK(__mutex)
#endif

#define HT_GROW_THRESHOLD 16

typedef struct __ht_item {
    uint32_t hash;
    void    *key;
    size_t   klen;
    void    *data;
    size_t   dlen;
} ht_item_t;

typedef struct __ht_iterator_arg {
    uint32_t  index;
    bool      set;
    ht_item_t item;
} ht_iterator_arg_t;

struct __hashtable {
    uint32_t size;
    uint32_t max_size;
    uint32_t count;
    linked_list_t **items; 
#ifdef THREAD_SAFE
    pthread_mutex_t lock;
#endif
    ht_free_item_callback_t free_item_cb;
};

typedef struct __ht_iterator_callback {
    void (*cb)();
    void *user;
    uint32_t count;
    hashtable_t *table;
} ht_iterator_callback_t;

typedef struct __ht_collector_arg {
    linked_list_t *output;
    uint32_t count;
} ht_collector_arg_t;

hashtable_t *ht_create(uint32_t initial_size, uint32_t max_size, ht_free_item_callback_t cb) {
    hashtable_t *table = (hashtable_t *)calloc(1, sizeof(hashtable_t));

    if (table)
        ht_init(table, initial_size, max_size, cb);

    return table;
}

void ht_init(hashtable_t *table, uint32_t initial_size, uint32_t max_size, ht_free_item_callback_t cb) {
    table->size = initial_size > 256 ? initial_size : 256;
    table->max_size = max_size;
    table->items = (linked_list_t **)calloc(table->size,
                                            sizeof(linked_list_t *));

    if (table->items) {
#ifdef THREAD_SAFE
        pthread_mutex_init(&table->lock, NULL);
#endif
    }
    ht_set_free_item_callback(table, cb);
}

void ht_set_free_item_callback(hashtable_t *table, ht_free_item_callback_t cb)
{
    MUTEX_LOCK(&table->lock);
    table->free_item_cb = cb;
    MUTEX_UNLOCK(&table->lock);
}

static int _destroy_item(void *item, uint32_t idx, void *user) {
    if (idx) { } // suppress warnings
    ht_free_item_callback_t cb = (ht_free_item_callback_t)user;
    ht_item_t *ht_item = (ht_item_t *)item;
    if (cb)
        cb(ht_item->data);
    free(ht_item->key);
    return 1; 
}

void ht_clear(hashtable_t *table) {
    uint32_t i;
    MUTEX_LOCK(&table->lock);
    for (i = 0; i < table->size; i++) {
        linked_list_t *list = table->items[i];
        if (list) {
            uint32_t count = list_count(list);
            foreach_list_value(list, _destroy_item,
                    (void *)table->free_item_cb);
            destroy_list(list);
            table->items[i] = NULL;
            table->count -= count;
        }
    }
    MUTEX_UNLOCK(&table->lock);
}

void ht_destroy(hashtable_t *table) {
    ht_clear(table);
    free(table->items);
#ifdef THREAD_SAFE
    pthread_mutex_destroy(&table->lock);
#endif
    free(table);
}

static int _get_item(void *item, uint32_t idx, void *user) {
    ht_iterator_arg_t *arg = (ht_iterator_arg_t *)user;
    ht_item_t *ht_item = (ht_item_t *)item;
    if (/*ht_item->hash == arg->item.hash && */
        ((char *)ht_item->key)[0] == ((char *)arg->item.key)[0] &&
        ht_item->klen == arg->item.klen &&
        memcmp(ht_item->key, arg->item.key, ht_item->klen) == 0)
    {
        char *data = ht_item->data;
        if (arg->set) {
            ht_item->data = arg->item.data;
            ht_item->dlen = arg->item.dlen;
        }
        arg->item.data = data;
        arg->item.dlen = ht_item->dlen;
        arg->index = idx;
        return 0;
    }
    return 1;
}

typedef struct __ht_copy_helper {
    linked_list_t **items;
    uint32_t size;
} ht_copy_helper;

static int _copyItems(void *item, uint32_t idx, void *user) {
    if (idx) { } // suppress warnings
    ht_copy_helper *helper = (ht_copy_helper *)user;
    ht_item_t *ht_item = (ht_item_t *)item;
    linked_list_t *list = helper->items[ht_item->hash % helper->size];
    if (!list) {
        list = create_list();
        set_free_value_callback(list, free);
        helper->items[ht_item->hash % helper->size] = list;
    }
    push_value(list, ht_item); 
    return 1;
}

void ht_grow_table(hashtable_t *table) {
    // if we need to extend the table, better locking it globally
    // preventing any operation on the actual one
    MUTEX_LOCK(&table->lock);

    if (table->max_size && table->size >= table->max_size) {
        MUTEX_UNLOCK(&table->lock);
        return;
    }

    uint32_t i;
    uint32_t newSize = table->size << 1;

    if (table->max_size && newSize > table->max_size)
        newSize = table->max_size;

    //fprintf(stderr, "Growing table from %u to %u\n", table->size, newSize);

    linked_list_t **items_list =
        (linked_list_t **)calloc(newSize, sizeof(linked_list_t *));

    if (!items_list) {
        //fprintf(stderr, "Can't create new items array list: %s\n", strerror(errno));
        return;
    }
    ht_copy_helper helper = {
        .items = items_list,
        .size = newSize
    };
    for (i = 0; i < table->size; i++) {
        linked_list_t *items = table->items[i];
        if (items) {
            foreach_list_value(items, _copyItems, (void *)&helper);
            set_free_value_callback(items, NULL);
            destroy_list(items);
        }
    }
    free(table->items);
    table->items = helper.items;

    table->size = newSize;
    MUTEX_UNLOCK(&table->lock);
}

int _ht_set_internal(hashtable_t *table, void *key, size_t klen,
        void *data, size_t dlen, void **prev_data, size_t *prev_len, int copy)
{
    uint32_t hash;
    uint32_t actual_size;
    void *prev = NULL;
    size_t plen = 0;

    PERL_HASH(hash, key, klen);
    MUTEX_LOCK(&table->lock);

    actual_size = table->size;

    linked_list_t *list = table->items[hash%table->size];
    if (!list) {
        list = create_list();
        set_free_value_callback(list, free);
        table->items[hash%table->size] = list;
    }

    // we want to lock the list now to avoid it being destroyed
    // by a concurrent ht_delete() call. 
    // Note that inserting/removing values from the list is thread-safe. 
    // The problem would exist if the list is destroyed while we are 
    // still accessing it so we need to lock it for this purpose

    list_lock(list);

    // we can anyway unlock the table to allow operations which 
    // don't involve the actual linklist

    MUTEX_UNLOCK(&table->lock);

    ht_iterator_arg_t arg = {
        .index = UINT_MAX,
        .set   = true,
        .item  = {
            .hash  = hash,
            .key   = key,
            .klen  = klen,
            .data  = data,
            .dlen  = dlen
        }
    };

    if (list_count(list))
        foreach_list_value(list, _get_item, &arg);

    if (arg.index != UINT_MAX) {
        prev = arg.item.data;
        plen = arg.item.dlen;
    } else {
        ht_item_t *item = (ht_item_t *)calloc(1, sizeof(ht_item_t));
        if (!item) {
            //fprintf(stderr, "Can't create new item: %s\n", strerror(errno));
            list_unlock(list); 
            return -1;
        }
        item->hash = hash;
        item->klen = klen;
        item->key = malloc(klen);
        memcpy(item->key, key, klen);
        if (!item->key) {
            //fprintf(stderr, "Can't copy key: %s\n", strerror(errno));
            list_unlock(list);
            free(item);
            return -1;
        }

        if (copy) {
            item->data = malloc(dlen);
            memcpy(item->data, data, dlen);
        } else {
            item->data = data;
        }
        item->dlen = dlen;

        if (push_value(list, item) == 0) {
            __sync_add_and_fetch(&table->count, 1);
        } else {
            //fprintf(stderr, "Can't push new value for key: %s\n", strerror(errno));
            list_unlock(list);
            free(item->key);
            free(item);
            return -1;
        }
    }

    list_unlock(list);

    if (ht_count(table) > actual_size + HT_GROW_THRESHOLD && 
        (!table->max_size || actual_size < table->max_size))
    {
        ht_grow_table(table);
    }

    if (prev) {
        if (prev_data)
            *prev_data = prev;
        else if (table->free_item_cb)
            table->free_item_cb(prev);
    } else if (prev_data) {
        *prev_data = NULL;
    }

    if (prev_len)
        *prev_len = plen;

    return 0;
}

int ht_set(hashtable_t *table, void *key, size_t len, void *data, size_t dlen, void **prev_data, size_t *prev_len) {
    return _ht_set_internal(table, key, len, data, dlen, prev_data, prev_len, 0);
}

int ht_set_copy(hashtable_t *table, void *key, size_t len, void *data, size_t dlen, void **prev_data, size_t *prev_len) {
    return _ht_set_internal(table, key, len, data, dlen, prev_data, prev_len, 1);
}

int ht_unset(hashtable_t *table, void *key, size_t len, void **prev_data, size_t *prev_len) {
    uint32_t hash;
    void *data = NULL;

    PERL_HASH(hash, key, len);

    MUTEX_LOCK(&table->lock);
    linked_list_t *list = table->items[hash%table->size];

    if (!list) {
        MUTEX_UNLOCK(&table->lock);
        return -1;
    }

    // TODO : maybe this lock is not necessary
    list_lock(list);
    MUTEX_UNLOCK(&table->lock);

    ht_iterator_arg_t arg = {
        .item = {
            .hash = hash,
            .key = key,
            .klen = len,
            .data = NULL,
            .dlen = 0
        },
        .set = true
    };
    foreach_list_value(list, _get_item, &arg);
    list_unlock(list);

    data = arg.item.data;

    if (prev_data)
        *prev_data = data;
    else if (table->free_item_cb)
        table->free_item_cb(data);

    if (prev_len)
        *prev_len = arg.item.dlen;
    return 0;
}

int ht_delete(hashtable_t *table, void *key, size_t len, void **prev_data, size_t *prev_len) {
    uint32_t hash;
    PERL_HASH(hash, key, len);
    MUTEX_LOCK(&table->lock);
    linked_list_t *list = table->items[hash%table->size];

    if (!list) {
        MUTEX_UNLOCK(&table->lock);
        return -1;
    }

    list_lock(list);
    MUTEX_UNLOCK(&table->lock);

    ht_iterator_arg_t arg = {
        .item = {
            .hash = hash,
            .key = key,
            .klen = len,
        },
        .index = UINT_MAX,
        .set   = false
    };
    foreach_list_value(list, _get_item, &arg);
    if (arg.index != UINT_MAX) {
        ht_item_t *item = (ht_item_t *)fetch_value(list, arg.index);
        if (item) {
            if (prev_data)
                *prev_data = item->data;
            else if (table->free_item_cb)
                table->free_item_cb(item->data);

            if (prev_len)
                *prev_len = item->dlen;

            free(item->key);
            free(item);
            __sync_sub_and_fetch(&table->count, 1);
        }
    }

    list_unlock(list);
    return 0;
}

void *_ht_get_internal(hashtable_t *table, void *key, size_t len,
        size_t *dlen, int copy, ht_deep_copy_callback_t copy_cb)
{
    uint32_t hash = 0;
    void *data = NULL; 
    PERL_HASH(hash, key, len);
    MUTEX_LOCK(&table->lock);
    linked_list_t *list = table->items[hash%table->size];
    if (!list) {
        MUTEX_UNLOCK(&table->lock);
        return NULL;
    }

    list_lock(list);
    MUTEX_UNLOCK(&table->lock);

    ht_iterator_arg_t arg = {
        .item = {
            .hash = hash,
            .key = key,
            .klen = len,
        },
        .index = UINT_MAX,
        .set   = false
    };
    foreach_list_value(list, _get_item, &arg);
    if (arg.index != UINT_MAX) {
        if (copy) {
            if (copy_cb) {
                data = copy_cb(arg.item.data, arg.item.dlen);
            } else {
                data = malloc(arg.item.dlen);
                memcpy(data, arg.item.data, arg.item.dlen);
            }
        } else {
            data = arg.item.data;
        }
        if (dlen)
            *dlen = arg.item.dlen;
    }

    list_unlock(list);
    return data;
}

void *ht_get(hashtable_t *table, void *key, size_t klen, size_t *dlen) {
    return _ht_get_internal(table, key, klen, dlen, 0, NULL);
}

void *ht_get_copy(hashtable_t *table, void *key, size_t klen, size_t *dlen) {
    return _ht_get_internal(table, key, klen, dlen, 1, NULL);
}

void *ht_get_deep_copy(hashtable_t *table, void *key, size_t klen,
        size_t *dlen, ht_deep_copy_callback_t copy_cb)
{
    return _ht_get_internal(table, key, klen, dlen, 1, copy_cb);
}

static int _collect_key(void *item, uint32_t idx, void *user) {
    if (idx) { } // suppress warnings
    ht_item_t *ht_item = (ht_item_t *)item;
    ht_collector_arg_t *arg = (ht_collector_arg_t *)user;
    push_value(arg->output, ht_item->key);
    return list_count(arg->output) >= arg->count ? 0 : 1;
}

linked_list_t *ht_get_all_keys(hashtable_t *table) {
    uint32_t i;
    ht_collector_arg_t arg = {
        .output = create_list(),
        .count = table->count
    };
    for (i = 0; i < table->size; i++) {
        MUTEX_LOCK(&table->lock);
        linked_list_t *items = table->items[i];

        if (!items) {
            MUTEX_UNLOCK(&table->lock);
            continue;
        }

        list_lock(items);
        MUTEX_UNLOCK(&table->lock);
        foreach_list_value(items, _collect_key, &arg);
        list_unlock(items);
    }
    return arg.output;
}

static int _collect_value(void *item, uint32_t idx, void *user) {
    if (idx) { } // suppress warnings
    ht_item_t *ht_item = (ht_item_t *)item;
    ht_collector_arg_t *arg = (ht_collector_arg_t *)user;
    push_value(arg->output, ht_item->data);
    return list_count(arg->output) >= arg->count ? 0 : 1;
}

linked_list_t *ht_get_all_values(hashtable_t *table) {
    uint32_t i;
    ht_collector_arg_t arg = {
        .output = create_list(),
        .count = table->count
    };
    for (i = 0; i < table->size; i++) {
        MUTEX_LOCK(&table->lock);
        linked_list_t *items = table->items[i];

        if (!items) {
            MUTEX_UNLOCK(&table->lock);
            continue;
        }

        list_lock(items);
        MUTEX_UNLOCK(&table->lock);
        foreach_list_value(items, _collect_value, &arg);
        list_unlock(items);
    }
    return arg.output;
}

static int _keyIterator(void *item, uint32_t idx, void *user) {
    if (idx) { } // suppress warnings
    ht_iterator_callback_t *arg = (ht_iterator_callback_t *)user;
    ht_item_t *ht_item = (ht_item_t *)item;
    arg->cb(arg->table, ht_item->key, ht_item->klen, arg->user);
    return 1;
}

void ht_foreach_key(hashtable_t *table, ht_key_iterator_callback_t cb, void *user) {
    uint32_t i;

    ht_iterator_callback_t arg = { 
        .cb = cb,
        .user = user,
        .table = table
    };

    for (i = 0; i < table->size; i++) {
        linked_list_t *items = table->items[i];
        MUTEX_LOCK(&table->lock);

        if (!items) {
            MUTEX_UNLOCK(&table->lock);
            continue;
        }
        
        // Note: once we acquired the linklist, we don't need to lock the whole
        // table anymore. We need to lock the list though to prevent it from
        // being destroyed while we are still accessing it (or iterating over)

        list_lock(items);
        MUTEX_UNLOCK(&table->lock);

        foreach_list_value(items, _keyIterator, (void *)&arg);

        list_unlock(items);
    }
}

static int _valueIterator(void *item, uint32_t idx, void *user) {
    if (idx) { } // suppress warnings
    ht_iterator_callback_t *arg = (ht_iterator_callback_t *)user;
    ht_item_t *ht_item = (ht_item_t *)item;
    arg->cb(arg->table, ht_item->data, ht_item->dlen, arg->user);
    return 1;
}

void ht_foreach_value(hashtable_t *table, ht_value_iterator_callback_t cb, void *user) {
    uint32_t i;

    ht_iterator_callback_t arg = { 
        .cb = cb,
        .user = user,
        .table = table
    };

    for (i = 0; i < table->size; i++) {
        MUTEX_LOCK(&table->lock);
        linked_list_t *items = table->items[i];

        if (!items) {
            MUTEX_UNLOCK(&table->lock);
            continue;
        }

        // Note: once we acquired the linklist, we don't need to lock the whole
        // table anymore. We need to lock the list though to prevent it from
        // being destroyed while we are still accessing it (or iterating over)
        list_lock(items);

        MUTEX_UNLOCK(&table->lock);
        foreach_list_value(items, _valueIterator, (void *)&arg);

        list_unlock(items);
    }
}

static int _pair_iterator(void *item, uint32_t idx, void *user) {
    if (idx) { } // suppress warnings
    ht_iterator_callback_t *arg = (ht_iterator_callback_t *)user;
    ht_item_t *ht_item = (ht_item_t *)item;
    arg->cb(arg->table, ht_item->key, ht_item->klen, ht_item->data, ht_item->dlen, arg->user);
    return 1;
}

void ht_foreach_pair(hashtable_t *table, ht_pair_iterator_callback_t cb, void *user) {
    uint32_t i;
    ht_iterator_callback_t arg = { 
        .cb = cb,
        .user = user,
        .table = table
    };
    for (i = 0; i < table->size; i++) {
        MUTEX_LOCK(&table->lock);
        linked_list_t *items = table->items[i];

        if (!items) {
            MUTEX_UNLOCK(&table->lock);
            continue;
        }

        // Note: once we acquired the linklist, we don't need to lock the whole
        // table anymore. We need to lock the list though to prevent it from
        // being destroyed while we are still accessing it (or iterating over)
        list_lock(items);

        MUTEX_UNLOCK(&table->lock);
        foreach_list_value(items, _pair_iterator, (void *)&arg);

        list_unlock(items);
    }
}

uint32_t ht_count(hashtable_t *table) {
    return __sync_add_and_fetch(&table->count, 0);
}

