/*
 * An event-driven server that handles simple commands from multiple clients.
 * If no command is received for 60 seconds, the client will be disconnected.
 *
 * Note that evbuffer_readline() is a potential source of denial of service, as
 * it does an O(n) scan for a newline character each time it is called.  One
 * solution would be checking the length of the buffer and dropping the
 * connection if the buffer exceeds some limit (dropping the data is less
 * desirable, as the client is clearly not speaking our protocol anyway).
 * Another (more ideal) solution would be starting the newline search at the
 * end of the existing buffer.  The server won't crash with really long lines
 * within the limits of system RAM (tested using lines up to 1GB in length), it
 * just runs slowly.
 *
 * Created Dec. 19-21, 2010 while learning to use libevent 1.4.
 * (C)2010 Mike Bourgeous, licensed under 2-clause BSD
 * Contact: mike on nitrogenlogic (it's a dot com domain)
 *
 * References used:
 * Socket code from previous personal projects
 * http://monkey.org/~provos/libevent/doxygen-1.4.10/
 * http://tupleserver.googlecode.com/svn-history/r7/trunk/tupleserver.c
 * http://abhinavsingh.com/blog/2009/12/how-to-build-a-custom-static-file-serving-http-server-using-libevent-in-c/
 * http://www.wangafu.net/~nickm/libevent-book/Ref6_bufferevent.html
 * http://publib.boulder.ibm.com/infocenter/iseries/v5r3/index.jsp?topic=%2Frzab6%2Frzab6xacceptboth.htm
 *
 * Useful commands for testing:
 * valgrind --leak-check=full --show-reachable=yes --track-fds=yes --track-origins=yes --read-var-info=yes ./cliserver
 * echo "info" | eval "$(for f in `seq 1 100`; do echo -n nc -q 10 localhost 14310 '| '; done; echo nc -q 10 localhost 14310)"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <event.h>
#include <stdbool.h>

#include "zmodem.h"
#include "zm.h"

// Behaves similarly to printf(...), but adds file, line, and function
// information.  I omit do ... while(0) because I always use curly braces in my
// if statements.
#define INFO_OUT(...) {\
	printf("%s:%d: %s():\t", __FILE__, __LINE__, __FUNCTION__);\
	printf(__VA_ARGS__);\
}

// Behaves similarly to fprintf(stderr, ...), but adds file, line, and function
// information.
#define ERROR_OUT(...) {\
	fprintf(stderr, "\e[0;1m%s:%d: %s():\t", __FILE__, __LINE__, __FUNCTION__);\
	fprintf(stderr, __VA_ARGS__);\
	fprintf(stderr, "\e[0m");\
}

// Behaves similarly to perror(...), but supports printf formatting and prints
// file, line, and function information.
#define ERRNO_OUT(...) {\
	fprintf(stderr, "\e[0;1m%s:%d: %s():\t", __FILE__, __LINE__, __FUNCTION__);\
	fprintf(stderr, __VA_ARGS__);\
	fprintf(stderr, ": %d (%s)\e[0m\n", errno, strerror(errno));\
}

// Size of array (Caution: references its parameter multiple times)
#define ARRAY_SIZE(array) (sizeof((array)) / sizeof((array)[0]))

// Prints a message and returns 1 if o is NULL, returns 0 otherwise
#define CHECK_NULL(o) ( (o) == NULL ? ( fprintf(stderr, "\e[0;1m%s is null.\e[0m\n", #o), 1 ) : 0 )

struct cmdsocket {
	// The file descriptor for this client's socket
	int fd;

	// Whether this socket has been shut down
	int shutdown;

	// zmodem start flag
	int zm_start;

	// The client's socket address
	struct sockaddr_in6 addr;

	// The server's event loop
	struct event_base *evloop;

	// The client's buffered I/O event
	struct bufferevent *buf_event;

	// The client's output buffer (commands should write to this buffer,
	// which is flushed at the end of each command processing loop)
	struct evbuffer *buffer;

	// Doubly-linked list (so removal is fast) for cleaning up at shutdown
	struct cmdsocket *prev, *next;

	struct zmr_state_s *pzmr;
};

struct command {
	char *name;
	char *desc;
	void (*func)(struct cmdsocket *cmdsocket, struct command *command, const char *params);
};

static void echo_func(struct cmdsocket *cmdsocket, struct command *command, const char *params);
static void help_func(struct cmdsocket *cmdsocket, struct command *command, const char *params);
static void info_func(struct cmdsocket *cmdsocket, struct command *command, const char *params);
static void quit_func(struct cmdsocket *cmdsocket, struct command *command, const char *params);
static void kill_func(struct cmdsocket *cmdsocket, struct command *command, const char *params);

static void shutdown_cmdsocket(struct cmdsocket *cmdsocket);

static struct command commands[] = {
	{ "echo", "Prints the command line.", echo_func },
	{ "help", "Prints a list of commands and their descriptions.", help_func },
	{ "info", "Prints connection information.", info_func },
	{ "quit", "Disconnects from the server.", quit_func },
	{ "kill", "Shuts down the server.", kill_func },
};

// List of open connections to be cleaned up at server shutdown
static struct cmdsocket cmd_listhead = { .next = NULL };
static struct cmdsocket * const socketlist = &cmd_listhead;




static void echo_func(struct cmdsocket *cmdsocket, struct command *command, const char *params)
{
	INFO_OUT("%s %s\n", command->name, params);
	evbuffer_add_printf(cmdsocket->buffer, "%s\n", params);
}

static void help_func(struct cmdsocket *cmdsocket, struct command *command, const char *params)
{
	int i;

	INFO_OUT("%s %s\n", command->name, params);

	for(i = 0; i < ARRAY_SIZE(commands); i++) {
		evbuffer_add_printf(cmdsocket->buffer, "%s:\t%s\n", commands[i].name, commands[i].desc);
	}
}

static void info_func(struct cmdsocket *cmdsocket, struct command *command, const char *params)
{
	char addr[INET6_ADDRSTRLEN];
	const char *addr_start;

	INFO_OUT("%s %s\n", command->name, params);

	addr_start = inet_ntop(cmdsocket->addr.sin6_family, &cmdsocket->addr.sin6_addr, addr, sizeof(addr));
	if(!strncmp(addr, "::ffff:", 7) && strchr(addr, '.') != NULL) {
		addr_start += 7;
	}

	evbuffer_add_printf(
			cmdsocket->buffer,
			"Client address: %s\nClient port: %hu\n",
			addr_start,
			cmdsocket->addr.sin6_port
			);
}

static void quit_func(struct cmdsocket *cmdsocket, struct command *command, const char *params)
{
	INFO_OUT("%s %s\n", command->name, params);
	shutdown_cmdsocket(cmdsocket);
}

static void kill_func(struct cmdsocket *cmdsocket, struct command *command, const char *params)
{
	INFO_OUT("%s %s\n", command->name, params);

	INFO_OUT("Shutting down server.\n");
	if(event_base_loopexit(cmdsocket->evloop, NULL)) {
		ERROR_OUT("Error shutting down server\n");
	}
	
	shutdown_cmdsocket(cmdsocket);
}

static void add_cmdsocket(struct cmdsocket *cmdsocket)
{
	cmdsocket->prev = socketlist;
	cmdsocket->next = socketlist->next;
	if(socketlist->next != NULL) {
		socketlist->next->prev = cmdsocket;
	}
	socketlist->next = cmdsocket;
}

static struct cmdsocket *create_cmdsocket(int sockfd, struct sockaddr_in6 *remote_addr, struct event_base *evloop)
{
	struct cmdsocket *cmdsocket;

	cmdsocket = calloc(1, sizeof(struct cmdsocket));
	if(cmdsocket == NULL) {
		ERRNO_OUT("Error allocating command handler info");
		close(sockfd);
		return NULL;
	}
	cmdsocket->fd = sockfd;
	cmdsocket->addr = *remote_addr;
	cmdsocket->evloop = evloop;

	add_cmdsocket(cmdsocket);

	return cmdsocket;
}

static void free_cmdsocket(struct cmdsocket *cmdsocket)
{
	if(CHECK_NULL(cmdsocket)) {
		abort();
	}

	// Remove socket info from list of sockets
	if(cmdsocket->prev->next == cmdsocket) {
		cmdsocket->prev->next = cmdsocket->next;
	} else {
		ERROR_OUT("BUG: Socket list is inconsistent: cmdsocket->prev->next != cmdsocket!\n");
	}
	if(cmdsocket->next != NULL) {
		if(cmdsocket->next->prev == cmdsocket) {
			cmdsocket->next->prev = cmdsocket->prev;
		} else {
			ERROR_OUT("BUG: Socket list is inconsistent: cmdsocket->next->prev != cmdsocket!\n");
		}
	}

	// Close socket and free resources
	if(cmdsocket->buf_event != NULL) {
		bufferevent_free(cmdsocket->buf_event);
	}
	if(cmdsocket->buffer != NULL) {
		evbuffer_free(cmdsocket->buffer);
	}
	if(cmdsocket->fd >= 0) {
		shutdown_cmdsocket(cmdsocket);
		if(close(cmdsocket->fd)) {
			ERRNO_OUT("Error closing connection on fd %d", cmdsocket->fd);
		}
	}
	free(cmdsocket);
}

static void shutdown_cmdsocket(struct cmdsocket *cmdsocket)
{
	if(!cmdsocket->shutdown && shutdown(cmdsocket->fd, SHUT_RDWR)) {
		ERRNO_OUT("Error shutting down client connection on fd %d", cmdsocket->fd);
	}
	cmdsocket->shutdown = 1;
}

static int set_nonblock(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL);
	if(flags == -1) {
		ERRNO_OUT("Error getting flags on fd %d", fd);
		return -1;
	}
	flags |= O_NONBLOCK;
	if(fcntl(fd, F_SETFL, flags)) {
		ERRNO_OUT("Error setting non-blocking I/O on fd %d", fd);
		return -1;
	}

	return 0;
}

// str must have at least len bytes to copy
static char *strndup_p(const char *str, size_t len)
{
	char *newstr;

	newstr = malloc(len + 1);
	if(newstr == NULL) {
		ERRNO_OUT("Error allocating buffer for string duplication");
		return NULL;
	}

	memcpy(newstr, str, len);
	newstr[len] = 0;

	return newstr;
}

static void send_prompt(struct cmdsocket *cmdsocket)
{
	if(evbuffer_add_printf(cmdsocket->buffer, "> ") < 0) {
		ERROR_OUT("Error sending prompt to client.\n");
	}
}

static void flush_cmdsocket(struct cmdsocket *cmdsocket)
{
	if(bufferevent_write_buffer(cmdsocket->buf_event, cmdsocket->buffer)) {
		ERROR_OUT("Error sending data to client on fd %d\n", cmdsocket->fd);
	}
}

static void process_command(size_t len, char *cmdline, struct cmdsocket *cmdsocket)
{
	size_t cmdlen;
	char *cmd;
	int i;

	// Skip leading whitespace, then find command name
	cmdline += strspn(cmdline, " \t");
	cmdlen = strcspn(cmdline, " \t");
	if(cmdlen == 0) {
		// The line was empty -- no command was given
		send_prompt(cmdsocket);
		return;
	} else if(len == cmdlen) {
		// There are no parameters
		cmd = cmdline;
		cmdline = "";
	} else {
		// There may be parameters
		cmd = strndup_p(cmdline, cmdlen);
		cmdline += cmdlen + 1; // Skip first space after command name
	}

	INFO_OUT("Command received: %s\n", cmd);

	// Execute the command, if it is valid
	for(i = 0; i < ARRAY_SIZE(commands); i++) {
		if(!strcmp(cmd, commands[i].name)) {
			INFO_OUT("Running command %s\n", commands[i].name);
			commands[i].func(cmdsocket, &commands[i], cmdline);
			break;
		}
	}
	if(i == ARRAY_SIZE(commands)) {
		ERROR_OUT("Unknown command: %s\n", cmd);
		evbuffer_add_printf(cmdsocket->buffer, "Unknown command: %s\n", cmd);
	}
		
	send_prompt(cmdsocket);

	if(cmd != cmdline && len != cmdlen) {
		free(cmd);
	}
}

static void cmd_read(struct bufferevent *buf_event, void *arg)
{
	struct cmdsocket *cmdsocket = (struct cmdsocket *)arg;
	char *cmdline;
	size_t len;
	if(!cmdsocket->shutdown){
		cmdline = evbuffer_readln(buf_event->input,&len,EVBUFFER_EOL_ANY);
		INFO_OUT("Read a line of length %zd from client on fd %d: %s\n", len, cmdsocket->fd, cmdline);
		if(strcmp(cmdline,"rz") == 0){
			cmdsocket->zm_start = 1;
			INFO_OUT("get rz, start ....\n");
			evbuffer_add_printf(cmdsocket->buffer, "start");
			
			if(bufferevent_write_buffer(cmdsocket->buf_event, cmdsocket->buffer)) {
				ERROR_OUT("Error sending data to client on fd %d\n", cmdsocket->fd);
			}			
			free(cmdline);
			goto done;
		}
		if(cmdsocket->zm_start == 1){
			memcpy(cmdsocket->pzmr->cmn.rcvbuf,cmdline,len);
			zmr_receive(cmdsocket->pzmr,len);
			
			INFO_OUT("zmodem parse done....\n");
		}
	}
done:
	return;
#if 0
	// Process up to 10 commands at a time
	for(i = 0; i < 10 && !cmdsocket->shutdown; i++) {
		cmdline = evbuffer_readln(buf_event->input,&len,EVBUFFER_EOL_ANY);
		if(cmdline == NULL) {
			// No data, or data has arrived, but no end-of-line was found
			break;
		}	
		INFO_OUT("Read a line of length %zd from client on fd %d: %s\n", len, cmdsocket->fd, cmdline);
		
		process_command(len, cmdline, cmdsocket);
		free(cmdline);
	}

	// Send the results to the client
	flush_cmdsocket(cmdsocket);
#endif
}

static void cmd_error(struct bufferevent *buf_event, short error, void *arg)
{
	struct cmdsocket *cmdsocket = (struct cmdsocket *)arg;

	if(error & EVBUFFER_EOF) {
		INFO_OUT("Remote host disconnected from fd %d.\n", cmdsocket->fd);
		cmdsocket->shutdown = 1;
	} else if(error & EVBUFFER_TIMEOUT) {
		INFO_OUT("Remote host on fd %d timed out.\n", cmdsocket->fd);
	} else {
		ERROR_OUT("A socket error (0x%hx) occurred on fd %d.\n", error, cmdsocket->fd);		
	}	
	free_cmdsocket(cmdsocket);
}
#if 0
static void
conn_writecb(struct bufferevent *buf_event, void *user_data)
{

}
#endif

static void
conn_eventcb(struct bufferevent *buf_event, short events, void *arg)
{
	if (events & BEV_EVENT_EOF) {
		printf("Connection closed.\n");
	} else if (events & BEV_EVENT_ERROR) {
		printf("Got an error on the connection: %s\n",
		    strerror(errno));/*XXX win32*/
		cmd_error(buf_event,errno,arg);
	}
	/* None of the other events can happen here, since we haven't enabled
	 * timeouts */
	bufferevent_free(buf_event);
}


char* barray2hexstr (const unsigned char* data, size_t datalen) {
  size_t final_len = datalen * 2;
  char* chrs = (char *) malloc((final_len + 1) * sizeof(*chrs));
  unsigned int j = 0;
  for(j = 0; j<datalen; j++) {
    chrs[2*j] = (data[j]>>4)+48;
    chrs[2*j+1] = (data[j]&15)+48;
    if (chrs[2*j]>57) chrs[2*j]+=7;
    if (chrs[2*j+1]>57) chrs[2*j+1]+=7;
  }
  chrs[2*j]='\0';
  return chrs;
}

size_t zmodem_write(void *arg, const uint8_t *buffer, size_t buflen){
	
#if 1
	int i;
	char* hexbuffer = barray2hexstr(buffer,buflen);
	for(i = 0; i < strlen(hexbuffer); i++){
		printf("%c.",hexbuffer[i]);
	}
	printf("\n");
	free(hexbuffer);
#endif

	struct cmdsocket *cmdsocket = (struct cmdsocket *)arg;
	evbuffer_add_printf(cmdsocket->buffer, "%s",buffer);
	
	if(bufferevent_write_buffer(cmdsocket->buf_event, cmdsocket->buffer)) {
		ERROR_OUT("Error sending data to client on fd %d\n", cmdsocket->fd);
	}
	return 0;
} 
size_t zmodem_read(void *arg, const uint8_t *buffer, size_t buflen){
	//struct cmdsocket *cmdsocket = container_of(pzmr,struct cmdsocket,pzmr);
	
	return 0;
}
size_t zmodem_on_receive(void *arg, const uint8_t *buffer, size_t buflen,bool zcnl){
	//struct cmdsocket *cmdsocket = container_of(pzmr,struct cmdsocket,pzmr);
	printf("%s\n",buffer);

	return 0;
}

static void setup_connection(int sockfd, struct sockaddr_in6 *remote_addr, struct event_base *evloop)
{
	struct cmdsocket *cmdsocket;

	if(set_nonblock(sockfd)) {
		ERROR_OUT("Error setting non-blocking I/O on an incoming connection.\n");
	}

	// Copy connection info into a command handler info structure
	cmdsocket = create_cmdsocket(sockfd, remote_addr, evloop);
	if(cmdsocket == NULL) {
		close(sockfd);
		return;
	}

	// Initialize a buffered I/O event
	cmdsocket->buf_event = bufferevent_socket_new(evloop,sockfd,BEV_OPT_DEFER_CALLBACKS);
	//cmdsocket->buf_event = bufferevent_new(sockfd, cmd_read, NULL, cmd_error, cmdsocket);
	if(CHECK_NULL(cmdsocket->buf_event)) {
		ERROR_OUT("Error initializing buffered I/O event for fd %d.\n", sockfd);
		free_cmdsocket(cmdsocket);
		return;
	}
	bufferevent_base_set(evloop, cmdsocket->buf_event);
	bufferevent_setcb(cmdsocket->buf_event,cmd_read,NULL/*conn_writecb*/,conn_eventcb,cmdsocket);
	bufferevent_settimeout(cmdsocket->buf_event, 60, 0);
	if(bufferevent_enable(cmdsocket->buf_event, EV_READ)) {
		ERROR_OUT("Error enabling buffered I/O event for fd %d.\n", sockfd);
		free_cmdsocket(cmdsocket);
		return;
	}

	// Create the outgoing data buffer
	cmdsocket->buffer = evbuffer_new();
	if(CHECK_NULL(cmdsocket->buffer)) {
		ERROR_OUT("Error creating output buffer for fd %d.\n", sockfd);
		free_cmdsocket(cmdsocket);
		return;
	}
	
	send_prompt(cmdsocket);
	flush_cmdsocket(cmdsocket);
	cmdsocket->pzmr = zmr_initialize(zmodem_write,zmodem_read,zmodem_on_receive,cmdsocket);
	if(CHECK_NULL(cmdsocket->pzmr)) {
		ERROR_OUT("Error creating zmr_state_s for fd %d.\n", sockfd);
		free_cmdsocket(cmdsocket);
	}
}

static void cmd_connect(int listenfd, short evtype, void *arg)
{
	struct sockaddr_in6 remote_addr;
	socklen_t addrlen = sizeof(remote_addr);
	int sockfd;
	int i;

	if(!(evtype & EV_READ)) {
		ERROR_OUT("Unknown event type in connect callback: 0x%hx\n", evtype);
		return;
	}
	
	// Accept and configure incoming connections (up to 10 connections in one go)
	for(i = 0; i < 10; i++) {
		sockfd = accept(listenfd, (struct sockaddr *)&remote_addr, &addrlen);
		if(sockfd < 0) {
			if(errno != EWOULDBLOCK && errno != EAGAIN) {
				ERRNO_OUT("Error accepting an incoming connection");
			}
			break;
		}

		INFO_OUT("Client connected on fd %d\n", sockfd);

		setup_connection(sockfd, &remote_addr, (struct event_base *)arg);
	}
}

// Used only by signal handler
static struct event_base *server_loop;

static void sighandler(int signal)
{
	INFO_OUT("Received signal %d: %s.  Shutting down.\n", signal, strsignal(signal));

	if(event_base_loopexit(server_loop, NULL)) {
		ERROR_OUT("Error shutting down server\n");
	}
}

int main(int argc, char *argv[])
{
	struct event_base *evloop;
	struct event connect_event;
	
	unsigned short listenport = 14310;
	struct sockaddr_in6 local_addr;
	int listenfd;

	// Set signal handlers
	sigset_t sigset;
	sigemptyset(&sigset);
	struct sigaction siginfo = {
		.sa_handler = sighandler,
		.sa_mask = sigset,
		.sa_flags = SA_RESTART,
	};
	sigaction(SIGINT, &siginfo, NULL);
	sigaction(SIGTERM, &siginfo, NULL);

	// Initialize libevent
	INFO_OUT("libevent version: %s\n", event_get_version());
	evloop = event_base_new();
	if(CHECK_NULL(evloop)) {
		ERROR_OUT("Error initializing event loop.\n");
		return -1;
	}
	server_loop = evloop;
	INFO_OUT("libevent is using %s for events.\n", event_base_get_method(evloop));

	// Initialize socket address
	memset(&local_addr, 0, sizeof(local_addr));
	local_addr.sin6_family = AF_INET6;
	local_addr.sin6_port = htons(listenport);
	local_addr.sin6_addr = in6addr_any;

	// Begin listening for connections
	listenfd = socket(AF_INET6, SOCK_STREAM, 0);
	if(listenfd == -1) {
		ERRNO_OUT("Error creating listening socket");
		return -1;
	}
	int tmp_reuse = 1;
	if(setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &tmp_reuse, sizeof(tmp_reuse))) {
		ERRNO_OUT("Error enabling socket address reuse on listening socket");
		return -1;
	}
	if(bind(listenfd, (struct sockaddr *)&local_addr, sizeof(local_addr))) {
		ERRNO_OUT("Error binding listening socket");
		return -1;
	}
	if(listen(listenfd, 8)) {
		ERRNO_OUT("Error listening to listening socket");
		return -1;
	}

	// Set socket for non-blocking I/O
	if(set_nonblock(listenfd)) {
		ERROR_OUT("Error setting listening socket to non-blocking I/O.\n");
		return -1;
	}

	// Add an event to wait for connections
	event_set(&connect_event, listenfd, EV_READ | EV_PERSIST, cmd_connect, evloop);
	event_base_set(evloop, &connect_event);
	if(event_add(&connect_event, NULL)) {
		ERROR_OUT("Error scheduling connection event on the event loop.\n");
	}


	// Start the event loop
	if(event_base_dispatch(evloop)) {
		ERROR_OUT("Error running event loop.\n");
	}

	INFO_OUT("Server is shutting down.\n");

	// Clean up and close open connections
	while(socketlist->next != NULL) {
		free_cmdsocket(socketlist->next);
	}

	// Clean up libevent
	if(event_del(&connect_event)) {
		ERROR_OUT("Error removing connection event from the event loop.\n");
	}
	event_base_free(evloop);
	if(close(listenfd)) {
		ERRNO_OUT("Error closing listening socket");
	}

	INFO_OUT("Goodbye.\n");

	return 0;
}

