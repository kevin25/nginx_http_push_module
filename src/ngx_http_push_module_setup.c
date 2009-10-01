static ngx_command_t  ngx_http_push_commands[] = {

    { ngx_string("push_message_timeout"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_sec_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_push_loc_conf_t, buffer_timeout),
      NULL },

    { ngx_string("push_queue_messages"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_push_loc_conf_t, buffer_enabled),
      NULL },

    { ngx_string("push_multiple_listeners"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_push_loc_conf_t, multiple_listeners),
      NULL },

    { ngx_string("push_buffer_size"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_push_main_conf_t, shm_size),
      NULL },

	{ ngx_string("push_sender"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_push_sender,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("push_listener"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_push_listener,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_push_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_push_postconfig,              /* postconfiguration */
    ngx_http_push_create_main_conf,        /* create main configuration */
    NULL,                                  /* init main configuration */
    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */
    ngx_http_push_create_loc_conf,         /* create location configuration */
    ngx_http_push_merge_loc_conf,          /* merge location configuration */
};

ngx_module_t  ngx_http_push_module = {
    NGX_MODULE_V1,
    &ngx_http_push_module_ctx,             /* module context */
    ngx_http_push_commands,                /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,					               /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_int_t	ngx_http_push_postconfig(ngx_conf_t *cf) {
	//initialize shared memory
	ngx_http_push_main_conf_t	*conf = ngx_http_conf_get_module_main_conf(cf, ngx_http_push_module);
	size_t                       shm_size;
	if(conf->shm_size==NGX_CONF_UNSET_SIZE) {
		conf->shm_size=3145728; //3megabytes
	}
	shm_size = ngx_align(conf->shm_size, ngx_pagesize);
	if (shm_size < 8 * ngx_pagesize) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0, "The push_shm_size value must be at least %udKiB", (8 * ngx_pagesize) >> 10);
        shm_size = 8 * ngx_pagesize;
    }
	if(ngx_http_push_shm_zone && ngx_http_push_shm_zone->shm.size != shm_size) {
		ngx_conf_log_error(NGX_LOG_WARN, cf, 0, "Cannot change memory area size without restart, ignoring change");
	}
	ngx_conf_log_error(NGX_LOG_INFO, cf, 0, "Using %udKiB of shared memory for push module", shm_size >> 10);
	return ngx_http_push_set_up_shm(cf, shm_size);
}

//shared memory
static ngx_str_t	ngx_push_shm_name = ngx_string("push_module"); //shared memory segment name
static ngx_int_t	ngx_http_push_set_up_shm(ngx_conf_t *cf, size_t shm_size) {
    ngx_http_push_shm_zone = ngx_shared_memory_add(cf, &ngx_push_shm_name, shm_size, &ngx_http_push_module);
    if (ngx_http_push_shm_zone == NULL) {
        return NGX_ERROR;
    }
	ngx_http_push_shm_zone->init = ngx_http_push_init_shm_zone;
	ngx_http_push_shm_zone->data = (void *) 1;
    return NGX_OK;
}
// shared memory zone initializer
static ngx_int_t	ngx_http_push_init_shm_zone(ngx_shm_zone_t * shm_zone, void *data) {
	if(data) { /* zone already initialized */
		shm_zone->data = data;
		return NGX_OK;
	}

    ngx_slab_pool_t                *shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;
    ngx_rbtree_node_t              *sentinel;
    ngx_rbtree_t                   *tree;
	
    shm_zone->data = ngx_slab_alloc(shpool, sizeof(ngx_rbtree_t));
	tree = shm_zone->data;
    if(tree == NULL) {
        return NGX_ERROR;
    }
    sentinel = ngx_slab_alloc(shpool, sizeof(ngx_rbtree_node_t));
    if(sentinel == NULL) {
        return NGX_ERROR;
    }
	ngx_rbtree_init(tree, sentinel, ngx_http_push_rbtree_insert);
    return NGX_OK;
}



//main config
static void * 		ngx_http_push_create_main_conf(ngx_conf_t *cf) {
	ngx_http_push_main_conf_t      *mcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_push_main_conf_t));
	if(mcf == NULL) {
		return NGX_CONF_ERROR;
	}
	mcf->shm_size=NGX_CONF_UNSET_SIZE;
	return mcf;
}

//location config stuff
static void *		ngx_http_push_create_loc_conf(ngx_conf_t *cf) {
	ngx_http_push_loc_conf_t       *lcf;
	lcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_push_loc_conf_t));
	if(lcf == NULL) {
		return NGX_CONF_ERROR;
	}
	lcf->buffer_timeout = NGX_CONF_UNSET;
	lcf->buffer_enabled = NGX_CONF_UNSET;
	lcf->multiple_listeners = NGX_CONF_UNSET;
	return lcf;
}
static char *	ngx_http_push_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child) {
	ngx_http_push_loc_conf_t       *prev = parent;
	ngx_http_push_loc_conf_t       *conf = child;
	ngx_conf_merge_sec_value(conf->buffer_timeout, prev->buffer_timeout, NGX_HTTP_PUSH_DEFAULT_BUFFER_TIMEOUT);
	ngx_conf_merge_value(conf->buffer_enabled, prev->buffer_enabled, 1);
	ngx_conf_merge_value(conf->multiple_listeners, prev->multiple_listeners, 1);
	return NGX_CONF_OK;
}


static ngx_str_t  ngx_http_push_id = ngx_string("push_id"); //id variable name
//sender and listener handlers now.
static char *ngx_http_push_setup_handler(ngx_conf_t *cf, void * conf, ngx_int_t (*handler)(ngx_http_request_t *)) {
	ngx_http_core_loc_conf_t       *clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module); 
	ngx_http_push_loc_conf_t       *plcf = conf;                                    
    clcf->handler = handler;                                       
	plcf->index = ngx_http_get_variable_index(cf, &ngx_http_push_id);         
    if (plcf->index == NGX_ERROR) {                                           
        return NGX_CONF_ERROR;                                                
    }                                                                         
    return NGX_CONF_OK;
}

static char *ngx_http_push_sender(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
	return ngx_http_push_setup_handler(cf, conf, &ngx_http_push_sender_handler);
}

static char *ngx_http_push_listener(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
	return ngx_http_push_setup_handler(cf, conf, &ngx_http_push_listener_handler);
}
