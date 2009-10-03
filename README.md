# Nginx HTTP push module

Turn nginx into a _long-polling push_ server that relays messages. 

If you want a long-polling server but don't want to wait on idle connections 
via upstream proxies, use this module to have nginx accept and hold 
long-polling client connections. Send responses to those clients by sending 
an HTTP request to a different location.

## Example config

	http {
	  # Sending
	  server {
	    listen   localhost:8089;
	    location / {
	      default_type  text/plain;
	      set $push_id $arg_channel; #/?channel=a or somesuch
	      push_sender;
	      push_message_timeout 2h; #buffered messages expire after 2 hours
	    }
	  }
	
	  # Receiving
	  server {
	    listen       8088;
	    location / {
	      default_type  text/plain;
	      set $push_id $arg_channel; #/?channel=a or somesuch
	      push_listener;
	    }
	  }
	}

See section "Alternative example config" further down for a simpler example 
where no distinction or security is needed for posting messages.

## Practical example

_This assumes you have built nginx with this module._

First time we need to create nginx log files or it will cry like a little baby:

	$ mkdir -p logs
	$ touch logs/access.log logs/error.log

Now, lets start nginx with the example configuration (nginx.conf). `cd` to the 
directory of this module's source and:

	$ nginx -c nginx.conf -p `pwd`/

_Note that nginx will stay in foreground (daemon set to off in nginx.conf)_

In a new terminal, listen for a message:

	$ curl localhost:8088/?channel=a

In another new terminal, send a message:

	$ curl -d 'hello' localhost:8089/?channel=a

You can try to launch several listeners on the same channel and broadcast
a message to them.


## Configuration directives & variables

### Directives

#### `push_sender`

	push_sender
	  default: none
	  context: server, location

Defines a server or location as the sender. Requests from a sender will be 
treated as messages to send to listeners.See protocol documentation 
for more info.

#### `push_listener`

	push_listener
	  default: none
	  context: server, location
  
Defines a server or location as a listener. Requests from a listener will 
not be responded to until a message for the listener (identified by 
$push_id) becomes available. See protocol documentation for more info.

#### `push_listener_concurrency`

	push_listener_concurrency  last | broadcast
	  default: last
	  context: http, server, location
  
Controls how listeners to the same channel id are handled. If `last` (default) is set, only the most recently connected listener to a channel will receive a message. If the value is set to `broadcast` all listeners to a channel will receive a message.

#### `push_queue_messages`

	push_queue_messages  on | off
	  default: on
	  context: http, server, location
  
Whether or not message queuing is enabled. If set to off, messages will be 
delivered only if a push_listener connection is already present for the id. 
Applicable only if a push_sender is present in this or a child context. 

#### `push_message_timeout`

	push_message_timeout  <time>
	  default: 1h
	  context: http, server, location

How long a message may be queued before it is considered expired. If you do 
not want messages to expire, set this to 0. Applicable only if a push_sender 
is present in this or a child context. 

#### `push_buffer_size`

	push_buffer_size  <size>
	  default: 3M
	  context: http

The size of the memory chunk this module will use for all message queuing 
and buffering. 


### Variables

#### `$push_id`

The channel id associated with a `push_listener` or `push_sender`. Must be present next
to said directives.

Identifies the channel of communication:

                sender
                  ||
                  ||
               [message]
                  ||
                  \/
        ----------------------
          channel {$push_id} 
        ----------------------
          ||      ||      ||
          \/      ||      \/
       listener   ||   listener
                  \/
               listener

Config example:

	set $push_id $arg_id #$push_id is now the url parameter "id"


## Operation

Assuming the example config given above:
Clients will `GET http://example.com:8088/?id=...` and have the 
response delayed until a message is `POST`ed to `http://localhost:8089/?id=...`
Messages can be sent to clients that have not yet connected, i.e. they are 
queued.

Upon sending a request to a `push_sender` location, the server will respond with 
a `201 Created` if the message has been sent. If it must be queued up (i.e. the 
`push_listener` with this id is presently connected), a `202 Accepted` will be sent.
 
If you indend to have the `push_sender` be a server-side application, 
it's a damn good idea to make sure the `push_server` location is not visible
publically, as it is intended for use only by your application.


## Building & Installation

Configure and build nginx with this module. Example:

	$ ./configure --add-module=path/to/nginx_http_push_module
	$ make


## "Protocol" spec

See [queuing-long-poll-relay-protocol](http://wiki.github.com/slact/nginx_http_push_module/queuing-long-poll-relay-protocol)


## Todo

- Add "first" (first in gets the message) functionality/switch to
  `push_listener_concurrency` (see prepared code in `ngx_http_push_listener_handler`)

- Add other mechanisms of server pushing. The list should include
  "long-poll" (default), "interval-poll".

- Add a `push_accomodate_strangers` setting (presently defaulting to on). 
  When set to off, requests with a previously-unseen `$push_id` 
  will be rejected. 

- When `POST`ing to `push_server`, if Content-Type is "message/http", the 
  response sent to `$push_id` should be created from the body of the request.


## Note on this branch of the source

This branch have been developed by Rasmus Andersson to introduce concurrent 
listeners. Essentially the `push_listener_concurrency` option has been added.


## Alternative example config

If there is no need for network-level security to protect sending/posting of 
messages, you can use the same server for sending and listening:

	http {
	  server {
	    listen        localhost:8088;
	    default_type  text/plain;
	    location /send {
	      set $push_id $arg_channel; #/?channel=a or somesuch
	      push_sender;
	    }
	    location /listen {
	      set $push_id $arg_channel; #/?channel=a or somesuch
	      push_listener;
	    }
	  }
	}
