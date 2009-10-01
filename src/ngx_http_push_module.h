#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <sys/queue.h>

//with the declarations
typedef struct {
	ngx_int_t                       index;
	time_t                          buffer_timeout;
	ngx_flag_t                      buffer_enabled;
	ngx_str_t                       listener_concurrency;
} ngx_http_push_loc_conf_t;

#define NGX_HTTP_PUSH_DEFAULT_BUFFER_TIMEOUT 3600

#define NGX_HTTP_PUSH_LQUEUEING_LASTIN 0
#define NGX_HTTP_PUSH_LQUEUEING_FIRSTIN 1
#define NGX_HTTP_PUSH_LQUEUEING_BROADCAST 2

typedef struct {
	size_t                          shm_size;
} ngx_http_push_main_conf_t;

//message queue
typedef struct {
    ngx_queue_t                     queue;
	ngx_str_t                       content_type;
	ngx_str_t                       charset;
	ngx_buf_t                      *buf;
	time_t                          expires;
} ngx_http_push_msg_t;

typedef struct ngx_http_push_node_s ngx_http_push_node_t;

//cleaning supplies
typedef struct {
	ngx_http_request_t             *request;
	ngx_http_push_node_t           *node;
	ngx_slab_pool_t                *shpool;
}  ngx_http_push_listener_cleanup_t;

struct ngx_http_push_request_s {
	ngx_http_request_t             *request;
	TAILQ_ENTRY(ngx_http_push_request_s)	next;
};

// ngx_http_push_nodes_s: struct for linked list of ngx_http_push_node_s structs
TAILQ_HEAD(ngx_http_push_requests_s, ngx_http_push_request_s);

typedef struct ngx_http_push_request_s ngx_http_push_request_t;
typedef struct ngx_http_push_requests_s ngx_http_push_requests_t;

//our typecast-friendly rbtree node
struct ngx_http_push_node_s {
	ngx_rbtree_node_t                node;
	ngx_str_t                        id;
	ngx_http_push_msg_t              *message_queue;
	ngx_uint_t                       message_queue_size;
	ngx_http_push_requests_t         requests;
	time_t                           last_seen;
	ngx_http_push_listener_cleanup_t *cleanup;
};

//sender stuff
static char *       ngx_http_push_sender(ngx_conf_t *cf, ngx_command_t *cmd, void *conf); //push_sender hook
static 	ngx_int_t   ngx_http_push_sender_handler(ngx_http_request_t * r);
static void         ngx_http_push_sender_body_handler(ngx_http_request_t * r);
static ngx_int_t    ngx_http_push_node_info(ngx_http_request_t *r, ngx_uint_t queue_len, time_t last_seen);

//listener stuff
static char *       ngx_http_push_listener(ngx_conf_t *cf, ngx_command_t *cmd, void *conf); //push_listener hook
static ngx_int_t    ngx_http_push_listener_handler(ngx_http_request_t * r);

static ngx_int_t    ngx_http_push_set_listener_header(ngx_http_request_t *r, ngx_str_t *content_type);
static ngx_chain_t* ngx_http_push_create_output_chain(ngx_http_request_t *r, ngx_buf_t *buf);
static ngx_int_t    ngx_http_push_set_listener_body(ngx_http_request_t *r, ngx_chain_t *out);

static ngx_int_t    ngx_http_push_add_pool_cleaner_delete_file(ngx_pool_t *pool, ngx_file_t *file);

static void         ngx_http_push_listener_cleanup(ngx_http_push_listener_cleanup_t * data); //request pool cleaner

//misc stuff
ngx_shm_zone_t *    ngx_http_push_shm_zone = NULL;
static char *       ngx_http_push_setup_handler(ngx_conf_t *cf, void * conf, ngx_int_t (*handler)(ngx_http_request_t *));
static void *       ngx_http_push_create_main_conf(ngx_conf_t *cf);
static void *       ngx_http_push_create_loc_conf(ngx_conf_t *cf);
static char *       ngx_http_push_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);
static ngx_int_t    ngx_http_push_set_up_shm(ngx_conf_t *cf, size_t shm_size);
static ngx_int_t    ngx_http_push_init_shm_zone(ngx_shm_zone_t * shm_zone, void * data);
static ngx_int_t    ngx_http_push_postconfig(ngx_conf_t *cf);

static ngx_http_push_msg_t * ngx_http_push_dequeue_message(ngx_http_push_node_t * node); // doesn't free associated memory

//missing in nginx < 0.7.?
#ifndef ngx_queue_insert_tail
#define ngx_queue_insert_tail(h, x)                                           \
    (x)->prev = (h)->prev;                                                    \
    (x)->prev->next = x;                                                      \
    (x)->next = h;                                                            \
    (h)->prev = x
#endif

