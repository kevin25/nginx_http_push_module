/*
 *	Copyright 2009 Leo Ponomarev.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <ngx_http_push_module.h>
#include <ngx_http_push_rbtree_util.c>
#include <ngx_http_push_module_setup.c>

//#define _push_log_debug(_r, _id, _fmt, ...) ngx_log_error(NGX_LOG_DEBUG, (_r)->connection->log, 0, "[push %s] " _fmt, (_id).data, ##__VA_ARGS__)
#define _push_log_debug(_r, _id, _fmt, ...)


static ngx_http_push_msg_t * ngx_http_push_dequeue_message(ngx_http_push_node_t * node){ //does NOT free associated memory.
	ngx_queue_t                    *sentinel = &node->message_queue->queue; 
	if(ngx_queue_empty(sentinel)) {
		return NULL;
	}
	ngx_queue_t                    *qmsg = ngx_queue_head(sentinel);
	ngx_queue_remove(qmsg);
	node->message_queue_size--;
	return ngx_queue_data(qmsg, ngx_http_push_msg_t, queue);
}

static ngx_int_t ngx_http_push_set_id(ngx_str_t *id, ngx_http_request_t *r, ngx_http_push_loc_conf_t *cf) {
	ngx_http_variable_value_t      *vv = ngx_http_get_indexed_variable(r, cf->index);
	if (vv == NULL || vv->not_found || vv->len == 0) {
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
		              "http_push module: the \"$push_id\" variable is required but is not set");
		return NGX_ERROR;
	}
	if (id!=NULL){
		id->data=vv->data; //no need to copy anything? ok...
		id->len=vv->len;
		return NGX_OK;
	} else {
		return NGX_ERROR;
	}
}

//this is a macro because i don't want to mess with the alloc_function function pointer
#define ngx_http_push_create_buf_copy(buf, cbuf, pool, pool_alloc)            \
	(cbuf) = pool_alloc((pool), sizeof(ngx_buf_t) + (ngx_buf_in_memory((buf)) ? ngx_buf_size((buf)) : 0));\
	if ((cbuf)!=NULL) {                                                       \
		if(ngx_buf_in_memory((buf))) {                                        \
			(cbuf)->pos = ((u_char *) (cbuf)) + sizeof(ngx_buf_t);            \
			(cbuf)->last = (cbuf)->pos + ngx_buf_size((buf));                 \
			(cbuf)->start=(cbuf)->pos;                                        \
			(cbuf)->end = (cbuf)->start + ngx_buf_size((buf));                \
			ngx_memcpy((cbuf)->pos, (buf)->pos, ngx_buf_size((buf)));         \
			(cbuf)->memory=ngx_buf_in_memory_only((buf)) ? 1 : 0;             \
		}                                                                     \
		if ((buf)->in_file) {                                                 \
			(cbuf)->file_pos = (buf)->file_pos;                               \
			(cbuf)->file_last = (buf)->file_last;                             \
			(cbuf)->temp_file = (buf)->temp_file;                             \
			(cbuf)->file = (buf)->file;                                       \
			(cbuf)->in_file = 1;                                              \
		}                                                                     \
	}

inline static void ngx_http_push_remove_requests_locked(ngx_http_push_node_t *node, ngx_slab_pool_t *shpool) {
	ngx_http_push_request_t *request;
	TAILQ_FOREACH(request, &node->requests, next) {
		ngx_slab_free_locked(shpool, request); // note: not freeing request->request, but the q entry only
	}
	TAILQ_INIT(&node->requests);
}

inline static void ngx_http_push_remove_listener_requests_locked(ngx_http_push_node_t *node, ngx_slab_pool_t *shpool) {
	ngx_http_push_remove_requests_locked(node, shpool);
	if(node->cleanup != NULL)
		node->cleanup->node = NULL;
}
	
static ngx_int_t ngx_http_push_listener_handler(ngx_http_request_t *r) {
	ngx_http_push_loc_conf_t       *cf = ngx_http_get_module_loc_conf(r, ngx_http_push_module);
	ngx_slab_pool_t                *shpool = (ngx_slab_pool_t *) ngx_http_push_shm_zone->shm.addr;
	ngx_str_t                       id;
	ngx_http_push_node_t           *node;
	ngx_http_push_msg_t            *msg;
	ngx_http_push_request_t        *request;
	int listener_concurrency;
	
	if (r->method != NGX_HTTP_GET) {
		return NGX_HTTP_NOT_ALLOWED;
	}
	
	if(ngx_http_push_set_id(&id, r, cf) !=NGX_OK) {
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}
	
	_push_log_debug(r, id, "new subscriber/listener");
	
	listener_concurrency = (cf->listener_concurrency.data == NULL 
		|| ngx_strncmp(cf->listener_concurrency.data, "broadcast", cf->listener_concurrency.len) == 0)
		? NGX_HTTP_PUSH_LQUEUEING_BROADCAST :/* enable when NGX_HTTP_PUSH_LQUEUEING_FIRSTIN is implemented
			(cf->listener_concurrency.data
			&& ngx_strncmp(cf->listener_concurrency.data, "first", cf->listener_concurrency.len) == 0)
			? NGX_HTTP_PUSH_LQUEUEING_FIRSTIN :*/ NGX_HTTP_PUSH_LQUEUEING_LASTIN;
	
	ngx_shmtx_lock(&shpool->mutex);
	node = get_node(&id, ngx_http_push_shm_zone->data, shpool, r->connection->log);
	ngx_shmtx_unlock(&shpool->mutex);
	if (node == NULL) { //unable to allocate node
		return NGX_ERROR;
	}
	
	//don't care about the rest of this request
	ngx_http_discard_request_body(r);
	
	// concurrent listeners
	if (!TAILQ_EMPTY(&node->requests)) {
		// check if we are only allowed to keep ONE (or a limit?) request (listener)
		// go forward and send <409 Conflict> to the previous request(s).
		// multiple_listeners is a conf bool called "push_multiple_listeners".
		if (listener_concurrency == NGX_HTTP_PUSH_LQUEUEING_LASTIN) {
			_push_log_debug(r, id, "sending 409 Conflict to previous listener(s)");
			ngx_shmtx_lock(&shpool->mutex);
			//we don't want the listener request cleanup to accidentally access an already freed node on cleanup
			if (node->cleanup != NULL)
				node->cleanup->node = NULL;
			TAILQ_FOREACH(request, &node->requests, next) {
				ngx_http_finalize_request(request->request, NGX_HTTP_CONFLICT);
				ngx_slab_free_locked(shpool, request); // note: not freeing request->request, only the tailq entry
			}
			TAILQ_INIT(&node->requests); // clear queue
			ngx_shmtx_unlock(&shpool->mutex);
		}
		// todo implement: else if (listener_concurrency == NGX_HTTP_PUSH_LQUEUEING_FIRSTIN) {...
	}
	
	ngx_shmtx_lock(&shpool->mutex);
	msg = ngx_http_push_dequeue_message(node); //expired messages are already removed from queue during get_node()
	node->last_seen = ngx_time();
	ngx_shmtx_unlock(&shpool->mutex);
	
	if (msg == NULL) { // message queue is empty
		//this means we must wait for a message.
		ngx_shmtx_lock(&shpool->mutex);
		request = ngx_slab_alloc_locked(shpool, sizeof(ngx_http_push_request_t));
		request->request = r;
		TAILQ_INSERT_TAIL(&node->requests, request, next);
		ngx_shmtx_unlock(&shpool->mutex);
		r->read_event_handler = ngx_http_test_reading; //definitely test to see if the connection got closed or something.
		
		//attach a cleaner to remove the request from the node, if need be
		ngx_pool_cleanup_t *cln = ngx_pool_cleanup_add(r->pool, sizeof(ngx_http_push_listener_cleanup_t));
		if (cln == NULL) //make sure we can.
			return NGX_ERROR;
		ngx_shmtx_lock(&shpool->mutex);
		node->cleanup = ((ngx_http_push_listener_cleanup_t *) cln->data);
		ngx_shmtx_unlock(&shpool->mutex);
		cln->handler = (ngx_pool_cleanup_pt) ngx_http_push_listener_cleanup;
		((ngx_http_push_listener_cleanup_t *) cln->data)->node = node;
		((ngx_http_push_listener_cleanup_t *) cln->data)->request = r;
		((ngx_http_push_listener_cleanup_t *) cln->data)->shpool = shpool;
		
		_push_log_debug(r, id, "waiting for message(s)");
		return NGX_DONE; //and wait.
	}
	else { // there are queued messages
		//output the message		
		ngx_chain_t                *out; //output chain
		ngx_int_t                   rc;
		ngx_shmtx_lock(&shpool->mutex);
		ngx_file_t                 *file = NULL;
		_push_log_debug(r, id, "directly flushing queued message(s)");
		
		if (msg->buf->in_file) {
			ngx_file_t             *bfile = msg->buf->file;
			file = ngx_pcalloc(r->pool, sizeof(ngx_file_t) + bfile->name.len + 1);
			if(file==NULL){
				ngx_shmtx_unlock(&shpool->mutex);
				return NGX_HTTP_INTERNAL_SERVER_ERROR;
			}
			file->offset=bfile->offset;
			file->sys_offset=bfile->sys_offset;
			file->name.len=bfile->name.len;
			file->name.data=(u_char *) file + sizeof(ngx_file_t);
			ngx_memcpy(file->name.data, bfile->name.data, file->name.len); //the null at the end please
			ngx_slab_free_locked(shpool, bfile); 
			ngx_shmtx_unlock(&shpool->mutex);
			
			file->log = r->connection->log;
			//the following assumes file->name.data is already null-terminated
			file->fd = ngx_open_file(file->name.data, NGX_FILE_RDONLY, NGX_FILE_OPEN, NGX_FILE_OWNER_ACCESS); 
			if(file->fd!=NGX_INVALID_FILE){
				ngx_http_push_add_pool_cleaner_delete_file(r->pool, file);
			}
			else {
				ngx_slab_free(shpool, msg->buf);
				ngx_slab_free(shpool, msg); 
				return NGX_HTTP_INTERNAL_SERVER_ERROR;
			}
			
			ngx_shmtx_lock(&shpool->mutex);
		}
		rc = ngx_http_push_set_listener_header(r, &msg->content_type); //content type is copied
		out = ngx_http_push_create_output_chain(r, msg->buf); 	//buffer is copied
		out->buf->file=file;
		//we no longer need the message and can free its shm slab.
		ngx_slab_free_locked(shpool, msg->buf); //separate block, remember?
		ngx_slab_free_locked(shpool, msg); 
		ngx_shmtx_unlock(&shpool->mutex);
		if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
			return rc;
		}
		rc = ngx_http_send_header(r);
		if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
			return rc;
		}
		
		return ngx_http_push_set_listener_body(r, out);
	}
}

static void ngx_http_push_sender_body_handler(ngx_http_request_t * r) {
	ngx_str_t                       id;
	ngx_http_push_loc_conf_t       *cf = ngx_http_get_module_loc_conf(r, ngx_http_push_module);
	ngx_slab_pool_t                *shpool = (ngx_slab_pool_t *) ngx_http_push_shm_zone->shm.addr;
	ngx_buf_t                      *buf, *buf_copy;
	ngx_http_push_node_t           *node;
	ngx_uint_t                      method = r->method;
	
	time_t                          last_seen = 0;
	ngx_uint_t                      queue_size = 0;
	
	// get channel id
	if(ngx_http_push_set_id(&id, r, cf) != NGX_OK) {
		return;
	}
	
	_push_log_debug(r, id, "publish/send");
	
	// get node
	ngx_shmtx_lock(&shpool->mutex);
	node = get_node(&id, (ngx_rbtree_t *) ngx_http_push_shm_zone->data, shpool, r->connection->log);
	if (node == NULL) {
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "push module: unable to allocate new node");
		ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
		return;
	}
	queue_size = node->message_queue_size;
	last_seen = node->last_seen;
	ngx_shmtx_unlock(&shpool->mutex);
	
	// handle POST or PUT
	if(method == NGX_HTTP_POST || method == NGX_HTTP_PUT) {
		_push_log_debug(r, id, "publish/send POST|PUT");
		
		// Empty message. blank buffer, please.
		if(r->headers_in.content_length_n == -1 || r->headers_in.content_length_n == 0) {
			_push_log_debug(r, id, "publish/send empty message");
			buf = ngx_create_temp_buf(r->pool, 0);
		}
		
		// Non-empty body
		else {
			if(r->request_body->temp_file==NULL) {//everything in the first buffer
				buf = r->request_body->bufs->buf;
			}
			else if(r->request_body->bufs->next!=NULL) {
				buf = r->request_body->bufs->next->buf;
			}
			else {
				ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "push module: request body buffer not found");
				ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
				return; 
			}
		}
		if (buf == NULL) {
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "push module: unable to allocate an empty buffer");
			ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
			return; 
		}
		ngx_http_push_msg_t            *msg;
		size_t                          content_type_len; 
		content_type_len = (r->headers_in.content_type == NULL ? 0 : r->headers_in.content_type->value.len);
		_push_log_debug(r, id, "publish/send content-length: %llu", (unsigned long long)content_type_len);
		
		// Case 1/3: No listeners are waiting for the message, and buffers are not disabled. 
		//           create the message in shared memory for storage
		if (TAILQ_EMPTY(&node->requests) && r->method == NGX_HTTP_POST && cf->buffer_enabled != 0) {
			_push_log_debug(r, id, "publish/send no listeners -- queueing message");
			
			msg = ngx_slab_alloc(shpool, sizeof(ngx_http_push_msg_t) + content_type_len);
			if (msg==NULL) {
				ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "push module: unable to allocate message in shared memory");
				ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
				return;
			}
			
			//create a buffer copy in shared mem
			ngx_shmtx_lock(&shpool->mutex);
			ngx_http_push_create_buf_copy(buf, buf_copy, shpool, ngx_slab_alloc_locked);
			ngx_shmtx_unlock(&shpool->mutex);
			if (buf_copy==NULL) {
				ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "push module: unable to allocate buffer in shared memory");
				ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
				return;
			}
			ngx_shmtx_lock(&shpool->mutex);
			if (buf_copy->in_file) {
				ngx_file_t             *file;
				file = ngx_slab_alloc_locked(shpool, sizeof(ngx_file_t) + buf->file->name.len + 1);
				//the +1 is for the null byte at the end --------------------------------------^^
				if(file==NULL){
					ngx_shmtx_unlock(&shpool->mutex);
					ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
					return;
				}
				file->fd=NGX_INVALID_FILE;
				file->log=NULL;
				file->offset=buf->file->offset;
				file->sys_offset=buf->file->sys_offset;
				file->name.len=buf->file->name.len;
				file->name.data=(u_char *) file + sizeof(ngx_file_t);
				ngx_memcpy(file->name.data, buf->file->name.data, file->name.len);
				buf_copy->file=file;
			}
			msg->buf=buf_copy;
			ngx_queue_insert_tail(&node->message_queue->queue, &msg->queue);
			node->message_queue_size++;
			queue_size = node->message_queue_size;

			//store the content-type
			if(content_type_len>0) {
				msg->content_type.len=r->headers_in.content_type->value.len;
				msg->content_type.data=(u_char *) msg + sizeof(ngx_http_push_msg_t);
				ngx_memcpy(msg->content_type.data, r->headers_in.content_type->value.data, msg->content_type.len);
			}
			else {
				msg->content_type.len=0;
				msg->content_type.data=NULL;
			}
			//set the expiration time
			time_t                      timeout = cf->buffer_timeout;
			msg->expires = timeout == 0 ? 0 : (ngx_time() + timeout);
			ngx_shmtx_unlock(&shpool->mutex);
			//okay, done storing. now respond to the sender request
			r->headers_out.status = NGX_HTTP_OK;
			r->headers_out.status_line.len = sizeof("202 Accepted")- 1;
			r->headers_out.status_line.data = (u_char *) "202 Accepted";
		}
		
		// Case 2/3: There are listeners in the channel -- send the message directly to all listeners
		else if (!TAILQ_EMPTY(&node->requests)) {
			ngx_int_t                   rc;
			ngx_http_push_request_t    *request;
			ngx_http_request_t         *r_listener;
			
			_push_log_debug(r, id, "publish/send direct dispatch");
			
			//we don't want the listener request cleanup to accidentally access an already freed node on cleanup
			ngx_shmtx_lock(&shpool->mutex);
			if (node->cleanup != NULL)
				node->cleanup->node = NULL;
			ngx_shmtx_unlock(&shpool->mutex);
			
			// Send the message to each listener 
			TAILQ_FOREACH(request, &node->requests, next) {
				r_listener = request->request;
				rc = ngx_http_push_set_listener_header(r_listener, (content_type_len>0 ? &r->headers_in.content_type->value : NULL));
				if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
					ngx_http_finalize_request(r_listener, rc);
					ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
					return;
				}
				rc = ngx_http_send_header(r_listener);
				if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
					ngx_http_finalize_request(r_listener, rc);
					ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
					return;
				}
				
				_push_log_debug(r, id, "publish/send scheduling listener %p for finalization", r_listener);
				
				// todo: potential problem: can buf be re-sent over and over?
				ngx_http_finalize_request(r_listener, 
					ngx_http_push_set_listener_body(r_listener, 
						ngx_http_push_create_output_chain(r_listener, buf)));
			}
			
			// delete the message file (if any) when the push_sender request finishes
			if (buf->in_file && buf->file->fd != NGX_INVALID_FILE)
				ngx_http_push_add_pool_cleaner_delete_file(r->pool, buf->file);
			
			// set response for the sender
			r->headers_out.status = NGX_HTTP_OK;
			r->headers_out.status_line.len = sizeof("201 Created")- 1;
			r->headers_out.status_line.data = (u_char *) "201 Created";
		}
		
		// Case 3/3: No listeners and method is PUT
		else {	//r_listener==NULL && r->method == NGX_HTTP_PUT is all that remains
			r->headers_out.status = NGX_HTTP_OK;
		}
	}
	
	// handle GET
	else if (method == NGX_HTTP_GET) {
		r->headers_out.status = NGX_HTTP_OK;
		// before return, we use ngx_http_push_node_info to add info to the response
	}
	
	// handle DELETE -- clear the message queue of <node>
	else if (method == NGX_HTTP_DELETE) {
		/*if (node == NULL) {
			r->headers_out.status = NGX_HTTP_NOT_FOUND;
			r->header_only = 1;
		}
		else {*/
		ngx_http_push_msg_t        *msg;
		
		// lock pool
		ngx_shmtx_lock(&shpool->mutex);
		
		// clear queue
		while((msg = ngx_http_push_dequeue_message(node))!=NULL) {
			ngx_slab_free_locked(shpool, msg);
		}
		
		// 401 Gone to any listeners
		if (!TAILQ_EMPTY(&node->requests)) {
			ngx_http_push_request_t  *request;
			TAILQ_FOREACH(request, &node->requests, next) {
				request->request->headers_out.status = 410;
				request->request->headers_out.status_line.len  = sizeof("410 Gone")- 1;
				request->request->headers_out.status_line.data = (u_char *) "410 Gone";
				request->request->headers_out.content_length_n = 0;
				request->request->header_only = 1;
				ngx_http_finalize_request(request->request, ngx_http_send_header(request->request));
			}
		}
		
		// free node/channel
		ngx_http_push_remove_listener_requests_locked(node, shpool);
		ngx_http_push_delete_node((ngx_rbtree_t *)ngx_http_push_shm_zone->data, (ngx_rbtree_node_t *)node, shpool);
		
		// unlock pool and set response status for sender
		ngx_shmtx_unlock(&shpool->mutex);
		r->headers_out.status = NGX_HTTP_OK;
	}
	
	// Respond with Method Not Allowed
	else {
		ngx_http_finalize_request(r, NGX_HTTP_NOT_ALLOWED);
		return;
	}
	
	// Clean response -- finalize request
	if (r->header_only || node==NULL) {
		r->header_only = 1;
		r->headers_out.content_length_n = 0;
		ngx_http_finalize_request(r, ngx_http_send_header(r));	
	} 
	else {
		ngx_http_finalize_request(r, ngx_http_push_node_info(r, queue_size, last_seen));
	}
	return;
}

static ngx_int_t ngx_http_push_node_info(ngx_http_request_t *r, ngx_uint_t queue_size, time_t last_seen) {
	ngx_buf_t                      *b;
	ngx_uint_t                      len;
	len = sizeof("queued: \r\n") + NGX_INT_T_LEN + sizeof("requested: \r\n") + NGX_INT_T_LEN;
	
	// todo: return count of listeners on the node
	
	b = ngx_create_temp_buf(r->pool, len);
	if (b == NULL) {
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}
	b->last = ngx_sprintf(b->last, "queued: %ui\r\n", queue_size);
	if(last_seen==0){
		b->last = ngx_cpymem(b->last, "requested: never\r\n", sizeof("requested: never\r\n") - 1);
	}
	else {
		b->last = ngx_sprintf(b->last, "requested: %ui\r\n", ngx_time() - last_seen);
	}
	
	r->headers_out.content_type.len = sizeof("text/plain") - 1;
	r->headers_out.content_type.data = (u_char *) "text/plain";
	if (ngx_http_send_header(r) > NGX_HTTP_SPECIAL_RESPONSE) {
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}
	
	return ngx_http_push_set_listener_body(r, ngx_http_push_create_output_chain(r, b));
}

static ngx_int_t ngx_http_push_sender_handler(ngx_http_request_t * r) {
	ngx_int_t                       rc;
	
	/* Instruct ngx_http_read_listener_request_body to store the request
	   body entirely in a memory buffer or in a file */
	r->request_body_in_single_buf = 1;
	r->request_body_in_persistent_file = 1;
	r->request_body_in_clean_file = 0;
	r->request_body_file_log_level = 0;

	rc = ngx_http_read_client_request_body(r, ngx_http_push_sender_body_handler);
	if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
		return rc;
	}
	return NGX_DONE;
}

static ngx_int_t ngx_http_push_set_listener_header(ngx_http_request_t *r, ngx_str_t *content_type) {
	//content-type is _copied_
	if (content_type!=NULL && content_type->data!=NULL && content_type->len > 0) {
		r->headers_out.content_type.len=content_type->len;
		r->headers_out.content_type.data = ngx_palloc(r->pool, content_type->len);
		if(r->headers_out.content_type.data==NULL) {
			return NGX_HTTP_INTERNAL_SERVER_ERROR;
		}
		ngx_memcpy(r->headers_out.content_type.data, content_type->data, content_type->len);
	}
	r->headers_out.status=NGX_HTTP_OK;
	return NGX_OK;
}

static ngx_chain_t * ngx_http_push_create_output_chain(ngx_http_request_t *r, ngx_buf_t *buf) {
	//buffer is _copied_
	ngx_chain_t                    *out = ngx_pcalloc(r->pool, sizeof(ngx_chain_t));
	ngx_buf_t                      *buf_copy;
	ngx_http_push_create_buf_copy(buf, buf_copy, r->pool, ngx_pcalloc);
	
	if (out==NULL || buf_copy==NULL) {
		return NULL;
	}
	
	buf_copy->last_buf = 1; 
	out->buf = buf_copy;
	out->next = NULL;
	return out;	
}

static ngx_int_t ngx_http_push_set_listener_body(ngx_http_request_t *r, ngx_chain_t *out)
{
	if (out==NULL) {
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}
	return ngx_http_output_filter(r, out);	
}

static void ngx_http_push_listener_cleanup(ngx_http_push_listener_cleanup_t *data) {
	if (data->node) {
		ngx_shmtx_lock(&data->shpool->mutex);
		ngx_http_push_remove_requests_locked(data->node, data->shpool);
		ngx_shmtx_unlock(&data->shpool->mutex);
	}
}

static ngx_int_t ngx_http_push_add_pool_cleaner_delete_file(ngx_pool_t *pool, ngx_file_t *file) {
	ngx_pool_cleanup_t             *cln = ngx_pool_cleanup_add(pool, sizeof(ngx_pool_cleanup_file_t));
	ngx_pool_cleanup_file_t        *clnf;
	if (cln == NULL) {
		return NGX_ERROR;
	}
	cln->handler = ngx_pool_delete_file;
	clnf = cln->data;
	clnf->fd = file->fd;
	clnf->name = file->name.data;
	clnf->log = file->log;
	return NGX_OK;
}