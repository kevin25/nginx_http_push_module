#include <sys/types.h>
#include <sys/time.h>
#include <sys/queue.h>
#include <stdlib.h>
#include <err.h>
#include <event.h>
#include <evhttp.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>
#include <pthread.h>
 
#define BUFSIZE 4096
#define NUMCONNS 10000
#define SERVERADDR "217.213.5.37"
#define SERVERPORT 8088
#define SLEEP_MS 10
 
char buf[BUFSIZE];
 
int bytes_recvd = 0;
int chunks_recvd = 0;
int closed = 0;
int connected = 0;
 
// called per chunk received
void chunkcb(struct evhttp_request * req, void * arg) {
	int s = evbuffer_remove( req->input_buffer, &buf, BUFSIZE );
	//printf("Read %d bytes: %s\n", s, &buf);
	bytes_recvd += s;
	chunks_recvd++;
	if(connected >= NUMCONNS && chunks_recvd%10000==0)
		printf(">Chunks: %d\tBytes: %d\tClosed: %d\n", chunks_recvd, bytes_recvd, closed);
}
 
// gets called when request completes
void reqcb(struct evhttp_request * req, void * arg) {
	closed++;
}
 
int main(int argc, char **argv) {
	event_init();
	struct evhttp_connection *evhttp_connection;
	struct evhttp_request *evhttp_request;
	//char addr[16];
	const char *path = "/msgq/listen?channel=pb";
	int i;//,octet;
	
	//for (octet=1; octet <= 17; octet++) {
		//sprintf(&addr, "10.224.0.%d", octet);
	for (i=1;i<=NUMCONNS;i++) {
		evhttp_connection = evhttp_connection_new(SERVERADDR, SERVERPORT);
		// see http://www.mail-archive.com/libevent-users@monkey.org/msg01302.html
		//evhttp_connection_set_local_address(evhttp_connection, &addr);
		evhttp_set_timeout((struct evhttp *)evhttp_connection, 864000); // 10 day timeout
		evhttp_request = evhttp_request_new(reqcb, NULL);
		evhttp_request->chunk_cb = chunkcb;
		if ( i % 100 == 0)
			printf("req[%d]: %s\n", i, path);
		evhttp_make_request(evhttp_connection, evhttp_request, EVHTTP_REQ_GET, path);
		evhttp_connection_set_timeout(evhttp_request->evcon, 864000);
		event_loop( EVLOOP_NONBLOCK );
		if ( connected % 200 == 0 )
			printf("\nChunks: %d\tBytes: %d\tClosed: %d\n", chunks_recvd, bytes_recvd, closed);
		usleep(SLEEP_MS*1000);
	}
	//}
	event_dispatch();
	return 0;
}
