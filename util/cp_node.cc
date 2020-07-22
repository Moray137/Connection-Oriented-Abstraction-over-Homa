/* Copyright (c) 2019-2020 Stanford University
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/* This file contains a program that runs on one node, as part of
 * the cluster_perf test.
 */

#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#include "dist.h"
#include "homa.h"
#include "test_utils.h"
#include "time_trace.h"

using std::string;

/* Command-line parameter values (note: changes to default values must
 * also be reflected in client and server constructors): */
uint32_t client_max = 1;
uint32_t client_port_max = 1;
int client_ports = 0;
int first_port = 4000;
int first_server = 1;
bool is_server = false;
int id = -1;
double net_bw = 0.0;
bool tcp_trunc = true;
int port_receivers = 1;
int port_threads = 1;
const char *protocol;
int server_nodes = 1;
int server_ports = 1;
bool verbose = false;
const char *workload = "100";

/** @rand_gen: random number generator. */
std::mt19937 rand_gen(
		std::chrono::system_clock::now().time_since_epoch().count());

/**
 * @server_addrs: Internet addresses for each of the server threads available
 * to receive a Homa RPC.
 */
std::vector<struct sockaddr_in> server_addrs;

/** @message_id: used to generate unique identifiers for outgoing messages.*/
std::atomic<uint32_t> message_id;

/**
 * @last_stats_time: time (in rdtsc cycles) when we last printed
 * staticsics. Zero means that none of the statistics below are valid.
 */
uint64_t last_stats_time;

/**
 * @last_client_rpcs: total number of client RPCS completed by this
 * application as of the last time we printed statistics.
 */
uint64_t last_client_rpcs;

/**
 * @last_client_data: total amount of data in client RPCS completed by this
 * application as of the last time we printed statistics.
 */
uint64_t last_client_data;

/**
 * @last_total_elapsed: total amount of elapsed time for all client RPCs
 * issued by this application (in units of rdtsc cycles), as of the last
 * time we printed statistics.
 */
uint64_t last_total_rtt;

/**
 * @last_lag: total lag across all clients (measured in rdtsc cycles)
 * as of the last time we printed statistics.
 */
uint64_t last_lag;

/**
 * @last_server_rpcs: total number of server RPCS handled by this
 * application as of the last time we printed statistics.
 */
uint64_t last_server_rpcs;

/**
 * @last_server_data: total amount of data in server RPCS handled by this
 * application as of the last time we printed statistics.
 */
uint64_t last_server_data;

/**
 * @last_per_server_rpcs: server->requests for each individual server,
 * as of the last time we printed statistics.
 */
std::vector<uint64_t> last_per_server_rpcs;

/** @log_file: where log messages get printed. */
FILE* log_file = stdout;

enum Msg_Type {NORMAL, VERBOSE};

/** @log_level: only print log messages if they have a level <= this value. */
Msg_Type log_level = NORMAL;

extern void log(Msg_Type type, const char *format, ...)
	__attribute__((format(printf, 2, 3)));

/**
 * @cmd_lock: held whenever a command is executing.  Used to ensure that
 * operations such as statistics printing don't run when commands such
 * as "stop" are changing the client or server structure.
 */
std::mutex cmd_lock;

/**
 * @fd_locks: used to synchronize concurrent accesses to the same fd
 * (indexed by fd).
 */

#define MAX_FDS 10000
std::atomic_bool fd_locks[MAX_FDS];

/**
 * print_help() - Print out usage information for this program.
 * @name:   Name of the program (argv[0])
 */
void print_help(const char *name)
{
	printf("Usage: cp_node [command]\n\n"
		"If there are command-line options, they constitute a single command\n"
		"to execute, after which cp_node will print statistics every second.\n\n"
		"If there are no command-line options, then cp_node enters a loop reading\n"
		"lines from standard input and executing them as commands. The following\n"
		"commands are supported, each followed by a list of options supported\n"
		"by that command:\n\n"
		"client [options]      Start one or more client threads\n"
		"    --client-max      Maximum number of outstanding requests from a single\n"
		"                      client machine (divided equally among client ports)\n"
		"                      (default: %d)\n"
		"    --first-port      Lowest port number to use for each server (default: %d)\n"
		"    --first-server    Id of first server node (default: %d, meaning node-%d)\n"
		"    --id              Id of this node; a value of I >= 0 means requests will\n"
		"                      not be sent to node-I (default: -1)\n"
		"    --net-bw          Target network utilization, including only message data,\n"
		"                      GB/s; 0 means send continuously (default: %.1f)\n"
		"    --no-trunc        For TCP, allow messages longer than Homa's limit\n"
		"    --ports           Number of ports on which to send requests (one\n"
		"                      sending thread per port (default: %d)\n"
		"    --port-receivers  Number of threads to listen for responses on each\n"
		"                      port (default: %d)\n"
		"    --protocol        Transport protocol to use: homa or tcp (default: %s)\n"
		"    --server-nodes    Number of nodes running server threads (default: %d)\n"
		"    --server-ports    Number of server ports on each server node\n"
		"                      (default: %d)\n"
		"    --workload        Name of distribution for request lengths (e.g., 'w1')\n"
		"                      or integer for fixed length (default: %s)\n\n"
		"dump_times file       Log RTT times (and lengths) to file\n\n"
		"exit                  Exit the application\n\n"
		"log [options] [msg]   Configure logging as determined by the options. If\n"
		"                      there is an \"option\" that doesn't start with \"--\",\n"
		"                      then it and all of the remaining words are printed to\n"
		"                      the log as a message.\n"
		"    --file            Name of log file to use for future messages (\"-\"\n"
		"                      means use standard output)\n"
		"    --level           Log level: either normal or verbose\n\n"
		"server [options]      Start serving requests on one or more ports\n"
		"    --first-port      Lowest port number to use (default: %d)\n"
		"    --protocol        Transport protocol to use: homa or tcp (default: %s)\n"
		"    --port-threads    Number of server threads to service each port\n"
		"                      (Homa only, default: %d)\n"
		"    --ports           Number of ports to listen on (default: %d)\n\n"
		"stop [options]        Stop existing client and/or server threads; each\n"
		"                      option must be either 'clients' or 'servers'\n",
		client_max, first_port, first_server, first_server, net_bw,
		client_ports, port_receivers, protocol,
		server_nodes, server_ports, workload,
		first_port, protocol, port_threads, server_ports);
}

/**
 * log() - Print a message to the current log file
 * @type:   Kind of message (NORMAL or VERBOSE); used to control degree of
 *          log verbosity
 * @format: printf-style format string, followed by printf-style arguments.
 */
void log(Msg_Type type, const char *format, ...)
{
	char buffer[1000];
	struct timespec now;
	va_list args;

	if (type > log_level)
		return;
	va_start(args, format);
	clock_gettime(CLOCK_REALTIME, &now);

	vsnprintf(buffer, sizeof(buffer), format, args);
	fprintf(log_file, "%010lu.%09lu %s", now.tv_sec, now.tv_nsec, buffer);
}

/**
 * parse_float() - Parse an floating-point value from an argument word.
 * @words:  Words of a command being parsed.
 * @i:      Index within words of a word expected to contain a floating-
 *          point value (may be outside the range of words, in which case an
 *          error message is printed).
 * @value:  The value corresponding to @words[i] is stored here,
 *          if the function completes successfully.
 * @option: Name of option being parsed (for use in error messages).
 * Return:  Nonzero means success, zero means an error occurred (and a
 *          message was printed).
 */
int parse_float(std::vector<string> &words, unsigned i, double *value,
		const char *option)
{
	double num;
	char *end;
	
	if (i >= words.size()) {
		printf("No value provided for %s\n", option);
		return 0;
	}
	num = strtod(words[i].c_str(), &end);
	if (*end != 0) {
		printf("Bad value '%s' for %s; must be floating-point "
				"number\n", words[i].c_str(), option);
		return 0;
	}
	*value = num;
	return 1;
}

/**
 * parse_int() - Parse an integer value from an argument word.
 * @words:  Words of a command being parsed.
 * @i:      Index within words of a word expected to contain an integer
 *          value (may be outside the range of words, in which case an
 *          error message is printed).
 * @value:  The integer value corresponding to @words[i] is stored here,
 *          if the function completes successfully.
 * @option: Name of option being parsed (for use in error messages).
 * Return:  Nonzero means success, zero means an error occurred (and a
 *          message was printed).
 */
int parse_int(std::vector<string> &words, unsigned i, int *value,
		const char *option)
{
	int num;
	char *end;
	
	if (i >= words.size()) {
		printf("No value provided for %s\n", option);
		return 0;
	}
	num = strtol(words[i].c_str(), &end, 0);
	if (*end != 0) {
		printf("Bad value '%s' for %s; must be integer\n",
				words[i].c_str(), option);
		return 0;
	}
	*value = num;
	return 1;
}

/**
 * log_affinity() - Log a message listing the core affinity for the
 * current thread.
 */
void log_affinity()
{
	cpu_set_t cores;
	if (sched_getaffinity(0, sizeof(cores), &cores) != 0) {
		log(NORMAL, "ERROR: couldn't read core affinities: %s",
				strerror(errno));
		return;
	}
	int total = CPU_COUNT(&cores);
	std::string list = "";
	for (int i = 0; total > 0; i++) {
		if (!CPU_ISSET(i, &cores))
			continue;
		total--;
		if (!list.empty())
			list.append(" ");
		list.append(std::to_string(i));
	}
	log(NORMAL, "Core affinities: %s\n", list.c_str());
}

/**
 * struct message_header - The first few bytes of each message (request or
 * response) have the structure defined here. The client initially specifies
 * this information in the request, and the server returns the information
 * in the response.
 */
struct message_header {
	/**
	 * @length: total number of bytes in the message, including this
	 * header.
	 */
	int length;
	
	/**
	 * @start_time: the time when the client initiated the request.
	 * This is the low-order 32 bits of a rdtsc value.
	 */
	uint32_t start_time;
	
	/**
	 * @server_id: the index in @server_addrs (on the client) of
	 * the server for this request.
	 */
	int server_id;
	
	/**
	 * @msg_id: unique identifier for this message among all those
	 * from a given client machine.
	 */
	uint32_t msg_id;
};

/**
 * init_server_addrs() - Set up the server_addrs table (addresses of the
 * server/port combinations that clients will communicate with), based on
 * current configuration parameters. Any previous contents of the table
 * are discarded
 */
void init_server_addrs(void)
{
	server_addrs.clear();
	for (int node = first_server; node < first_server + server_nodes;
			node++) {
		char host[100];
		struct addrinfo hints;
		struct addrinfo *matching_addresses;
		struct sockaddr_in *dest;

		if (node == id)
			continue;
		snprintf(host, sizeof(host), "node-%d", node);
		memset(&hints, 0, sizeof(struct addrinfo));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		int status = getaddrinfo(host, NULL, &hints,
				&matching_addresses);
		if (status != 0) {
			log(NORMAL, "FATAL: couldn't look up address "
					"for %s: %s\n",
					host, gai_strerror(status));
			exit(1);
		}
		dest = reinterpret_cast<struct sockaddr_in *>
				(matching_addresses->ai_addr);
		for (int thread = 0; thread < server_ports; thread++) {
			dest->sin_port = htons(first_port + thread);
			server_addrs.push_back(*dest);
		}
	}
}

/**
 * class spin_lock - Implements simple spin lock guards: lock is acquired by
 * constructor, released by destructor.
 */
class spin_lock {
    public:
	spin_lock(std::atomic_bool *mutex)
		: mutex(mutex)
	{
		do {
		    /* mutex.exchange() always invalidates the cache line
		     * mutex resides in, regardless of whether it succeeded
		     * in updating the value. To reduce cache invalidation
		     * traffic, wait until we observe the lock to be free.
		     */
		    while (mutex->load(std::memory_order_relaxed)) {
			/* Do nothing */
		    }
		} while (mutex->exchange(1, std::memory_order_acquire));
	}
		
	~spin_lock()
	{
		mutex->store(0, std::memory_order_release);
	}
	
    protected:
	std::atomic_bool *mutex;
};

/**
 * class tcp_connection - Handles the reading and writing of TCP messages
 * from/to a given peer. Incoming messages may arrive in several chunks
 * spaced out in time, and outgoing messages may have to be sent in
 * multiple chunks because the stream backed up. This class keeps track
 * of the state of partial messages.
 */
class tcp_connection {
    public:
	tcp_connection(int fd, uint32_t epoll_id, int port,
			struct sockaddr_in peer);
	size_t pending();
	int read(bool loop, std::function<void (message_header *header)> func);
	bool send_message(message_header *header);
	void set_epoll_events(int epoll_fd, uint32_t events);
	bool xmit();
	
	/** @fd: File descriptor to use for reading and writing data. */
	int fd;
	
	/**
	 * @epoll_id: identifier for this connection, which will be stored
	 * in the u32 field of the data for epoll events for this
	 * connection. */
	uint32_t epoll_id;
	
	/**
	 * @port: Port number associated with this connection (listen port
	 * for servers, outgoing port for clients). Used for error messages.
	 */
	int port;
	
	/**
	 * @peer: Address of the machine on the other end of this connection.
	 */
	struct sockaddr_in peer;
	
	/**
	 * @bytes_received: nonzero means we have read part of an incoming
	 * request; the value indicates how many bytes have been received
	 * so far.
	 */
	int bytes_received;
	
	/**
	 * @header: will eventually hold the first bytes of an incoming
	 * message. If @bytes_received is less than the size of this value,
	 * then it has not yet been fully read.
	 */
	message_header header;
	
	/**
	 * @outgoing: queue of headers for messages waiting to be
	 * transmitted. The first entry may have been partially transmitted.
	 */
	std::deque<message_header> outgoing;
	
	/*
	 * @bytes_sent: Nonzero means we have sent part of the first message
	 * in outgoing; the value indicates how many bytes have been
	 * successfully transmitted.
	 */
	int bytes_sent;
	
	/**
	 * @epoll_events: OR-ed combination of epoll events such as EPOLLIN
	 * currently enabled for this connection.
	 */
	uint32_t epoll_events;
	
	/**
	 * @error_message: holds human-readable error information after
	 * an error.
	 */
	char error_message[200];
};

/**
 * tcp_connection:: tcp_connection() - Constructor for tcp_connection objects.
 * @fd:        File descriptor from which to read data.
 * @epoll_id:  Identifier to store in the u32 data field of epoll events
 *             for this connection.
 * @port:      Port number associated with this connection; used for messages.
 * @peer:      Address of the machine we're reading from; used for messages.
 */
tcp_connection::tcp_connection(int fd, uint32_t epoll_id, int port,
		struct sockaddr_in peer)
	: fd(fd)
	, epoll_id(epoll_id)
        , port(port)
	, peer(peer)
	, bytes_received(0)
        , header()
        , outgoing()
        , bytes_sent(0)
        , epoll_events(0)
{
}

/**
 * pending() - Return a count of the number of messages currently
 * waiting to be transmitted (nonzero means the connection is backed up).
 */
inline size_t tcp_connection::pending()
{
	return outgoing.size();
}

/**
 * tcp_connection::read() - Reads more data from a TCP connection and calls
 * a function to handle complete messages, if any.
 * @loop:      If true, this function will read repeatedly from the
 *             socket, stopping only when there is no more data to read
 *             (the socket must be in nonblocking mode). If false, only
 *             one read call will be issued.
 * @func:      Function to call when there is a complete message; the argument
 *             to the function is a pointer to the standard header from the
 *             message. Func may be called multiple times in a single
 *             invocation of this method.
 * Return:     Zero means success; nonzero means the socket was closed
 *             by the peer, or there was an error; a human-readable message
 *	       will be left in @error_message.
 */
int tcp_connection::read(bool loop,
		std::function<void (message_header *header)> func)
{
	char buffer[100000];
	char *next;
	
	while (1) {
		int count = ::read(fd, buffer, sizeof(buffer));
		if (count <= 0) {
			if ((count < 0) && ((errno == EAGAIN)
					|| (errno == EWOULDBLOCK))) {
				tt("read failed: EWOULDBLOCK");
				return 0;
			}
			if ((count == 0) || ((count < 0)
					&& (errno == ECONNRESET))) {
				/* Connection was closed by the client. */
				snprintf(error_message, sizeof(error_message),
						"TCP connection on port %d "
						"(fd %d) closed by peer %s",
						port, fd, print_address(&peer));
				return 1;
			}

			/* At this point count < 0. */
			if (errno == EFAULT) {
				/* As of 6/2020, the system call above
				 * sometimes returns EFAULT for no apparent
				 * reason (particularly under high load).
				 * Retrying seems to work...
				 */
				log(NORMAL, "WARNING: tcp_connect::read "
						"retrying after EFAULT\n");
				continue;
			}
			log(NORMAL, "ERROR: read failed for TCP connection on "
					"port %d (fd %d) to %s: %s (%d)\n",
					port, fd, print_address(&peer),
					strerror(errno), errno);
			snprintf(error_message, sizeof(error_message),
					"Error reading from TCP connection on "
					"port %d (fd %d) to %s: %s", port, fd,
					print_address(&peer), strerror(errno));
			return 1;
			
		}
	
		/*
		 * Process incoming bytes (could contains parts of multiple
		 * requests). The first 4 bytes of each request give its
		 * length.
		 */
		next = buffer;
		while (count > 0) {
			/* First, fill in the message header with incoming data
			 * (there's no guarantee that a single read will return
			 * all of the bytes needed for these).
			 */
			int header_bytes = sizeof32(message_header)
				- bytes_received;
			if (header_bytes > 0) {
				if (count < header_bytes)
					header_bytes = count;
				char *dst = reinterpret_cast<char *>(&header);
				memcpy(dst + bytes_received, next, header_bytes);
				bytes_received += header_bytes;
				next += header_bytes;
				count -= header_bytes;
				tt("Added %d bytes to header", header_bytes);
				if (bytes_received < sizeof32(message_header))
					break;
				tt("Header complete for message %d: length %d bytes",
						header.msg_id, header.length);
			}

			/* At this point we know the request length, so read until
			 * we've got a full request.
			 */
			int needed = header.length - bytes_received;
			if (count < needed) {
				bytes_received += count;
				tt("Incomplete message: have %d/%d bytes",
						bytes_received, header.length);
				break;
			}
			tt("Message %d received: %d bytes",
					header.msg_id, header.length);

			/* We now have a full request. */
			count -= needed;
			next += needed;
			func(&header);
			bytes_received = 0;
		}
		if (!loop)
			return 0;
	}
}

/**
 * tcp_connection::set_epoll_events() - Convenience method to set events
 * for epolling on this connection.
 * @epoll_fd:  File descriptor on which epoll events are collected and
 *             waited for.
 * @event:     OR-ed combination of EPOLLIN, EPOLLOUT, etc.
 */
void tcp_connection::set_epoll_events(int epoll_fd, uint32_t events)
{
	struct epoll_event ev;
	
	if (events == epoll_events)
		return;
	ev.events = events;
	ev.data.u32 = epoll_id;
	if (epoll_ctl(epoll_fd, (epoll_events == 0) ? EPOLL_CTL_ADD
			: EPOLL_CTL_MOD, fd, &ev) < 0) {
		log(NORMAL, "FATAL: couldn't add/modify epoll event: %s\n",
				strerror(errno));
		exit(1);
	}
	epoll_events = events;
}

/**
 * tcp_connection::send_message() - Begin the process of sending a message
 * to a peer; the message may not be completely transmitted at the time this
 * method returns.
 * @header:     Transmitted as the first bytes of the message.
 *              If the size isn't at least as large as the header,
 *              we'll round it up.
 * Return:  true means the message was completely transmitted; false means
 * it has not been fully transmitted (xmit will need to be called later to
 * finish the job).
 */
bool tcp_connection::send_message(message_header *header)
{
	if (header->length < sizeof32(*header))
		header->length = sizeof32(*header);
	outgoing.emplace_back(*header);
	if (outgoing.size() > 1)
		return false;
	return xmit();
}

/**
 * tcp_connection::xmit() - Transmit as much data as possible on this
 * connection.
 * Return:  true means all available data has been sent; false means
 *          there is data that couldn't be sent because the stream
 *          backed up.
 */
bool tcp_connection::xmit()
{
	char buffer[100000];
	struct message_header *header;
	int start;
	int send_length;
	ssize_t result;
	
	while (true) {
		if (outgoing.size() == 0)
			return true;
		header = &outgoing[0];
		if (bytes_sent < sizeof32(*header)) {
			*(reinterpret_cast<message_header *>(buffer))
					= *header;
			start = bytes_sent;
		} else
			start = 0;
		send_length = header->length - bytes_sent;
		if (send_length > (sizeof32(buffer) - start))
			send_length = sizeof32(buffer) - start;
		tt("Sending %d bytes at offset %d/%d for message id %d",
				send_length, bytes_sent, header->length,
				header->msg_id);
		result = send(fd, buffer + start, send_length,
				MSG_NOSIGNAL|MSG_DONTWAIT);
		if (result >= 0)
			bytes_sent += result;
		else {
			if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
				return false;
			if ((errno == EPIPE) || (errno == ECONNRESET))
				bytes_sent = header->length;
			else {
				log(NORMAL, "FATAL: error sending TCP message "
						"to %s: %s (port %d)\n",
						print_address(&peer),
						strerror(errno), port);
				exit(1);
			}
		}
		tt("After send, bytes_sent now %d/%d", bytes_sent,
				header->length);
		if (bytes_sent < header->length)
			continue;
		bytes_sent = 0;
		tt("Finished sending message id %d (length %d), %u messages "
				"still to send", header->msg_id,
				header->length, outgoing.size());
		outgoing.pop_front();
	}
}

/**
 * class server_metrics - Keeps statistics for a single server thread
 * (i.e. all the requests arriving via one Homa port or one TCP listen
 * socket).
 */
class server_metrics {
    public:
	/** @requests: Total number of requests handled so far. */
	uint64_t requests;
	
	/**
	 * @data: Total number of bytes of data in requests handled
	 * so far.
	 */
	uint64_t data;
	
	server_metrics() :requests(0), data(0) {}
};

/** @metrics: keeps track of metrics for all servers (whether Homa or TCP). */
std::vector<server_metrics *> metrics;

/**
 * class homa_server - Holds information about a single Homa server
 * thread, which handles requests on a given port. There may be more
 * than one thread on the same port.
 */
class homa_server {
    public:
	homa_server(int port, int id);
	~homa_server();
	void server();
	
	/** @id: Identifying number of this server. */
	int id;
	
	/** @fd: File descriptor for Homa socket. */
	int fd;
	
	/** @metrics: Performance statistics. */
	server_metrics metrics;
	
	/** @thread: Background thread that services requests. */
	std::thread thread;
};

/** @homa_servers: keeps track of all existing Homa clients. */
std::vector<homa_server *> homa_servers;

/**
 * homa_server::homa_server() - Constructor for homa_server objects.
 * @fd:  File descriptor for Homa socket to use for receiving
 *       requests.
 */
homa_server::homa_server(int fd, int id)
	: id(id)
	, fd(fd)
        , metrics()
	, thread(&homa_server::server, this)
{
}

/**
 * homa_server::~homa_server() - Destructor for homa_servers; terminates
 * the background thread.
 */
homa_server::~homa_server()
{
	shutdown(fd, SHUT_RDWR);
	close(fd);
	thread.join();
}

/**
 * homa_server::server() - Handles incoming requests arriving on a Homa
 * socket. Normally invoked as top-level method in a thread.
 */
void homa_server::server(void)
{
	int message[1000000];
	struct sockaddr_in source;
	int length;
	char buffer[50];
	
	snprintf(buffer, sizeof(buffer), "S%d", id);
	time_trace::create_thread_buffer(buffer);
	while (1) {
		uint64_t id = 0;
		int result;
		
		while (1) {
			length = homa_recv(fd, message, sizeof(message),
				HOMA_RECV_REQUEST, &id,
				(struct sockaddr *) &source, sizeof(source));
			if (length >= 0)
				break;
			if ((errno == EBADF) || (errno == ESHUTDOWN))
				return;
			else if ((errno != EINTR) && (errno != EAGAIN))
				log(NORMAL, "homa_recv failed: %s\n",
						strerror(errno));
		}

		result = homa_reply(fd, message, length,
			(struct sockaddr *) &source, sizeof(source), id);
		if (result < 0) {
			log(NORMAL, "FATAL: homa_reply failed: %s\n",
					strerror(errno));
			exit(1);
		}
		metrics.requests++;
		metrics.data += length;
	}
}

/**
 * class tcp_server - Holds information about a single TCP server,
 * which consists of a thread that handles requests on a given port.
 */
class tcp_server {
    public:
	tcp_server(int port, int id, int num_threads);
	~tcp_server();
	void accept(int epoll_fd);
	void read(int fd);
	void server(int thread_id);
	
	/**
	 * @mutex: For synchronizing access to server-wide state, such
	 * as listen_fd.
	 */
	std::atomic_bool mutex;
	
	/** @port: Port on which we listen for connections. */
	int port;
	
	/** @id: Unique identifier for this server. */
	int id;
	
	/** @listen_fd: File descriptor for the listen socket. */
	int listen_fd;
	
	/** @epoll_fd: File descriptor used for epolling. */
	int epoll_fd;
	
	/**
	 * @epollet: EPOLLET if this flag should be used, or 0 otherwise.
	 * We only use edge triggering if there are multiple receiving
	 * threads (it's unneeded if there's only a single thread, and
	 * it's faster not to use it).
	 */
	int epollet;
	
	/**
	 * @connections: Entry i contains information for a client
	 * connection on fd i, or NULL if no such connection.
	 */
	tcp_connection *connections[MAX_FDS];
	
	/** @metrics: Performance statistics. */
	server_metrics metrics;
	
	/**
	 * @thread: Background threads that both accept connections and
	 * service requests on them.
	 */
	std::vector<std::thread> threads;
	
	/** @stop: True means that background threads should exit. */
	bool stop;
};

/** @tcp_servers: keeps track of all existing Homa clients. */
std::vector<tcp_server *> tcp_servers;

/**
 * tcp_server::tcp_server() - Constructor for tcp_server objects.
 * @port:         Port number on which this server should listen for incoming
 *                requests.
 * @id:           Unique identifier for this server.
 * @num_threads:  Number of threads to service this listening socket and
 *                all of the other sockets excepted from it.
 */
tcp_server::tcp_server(int port, int id, int num_threads)
	: mutex(0)
	, port(port)
        , id(id)
	, listen_fd(-1)
	, epoll_fd(-1)
        , epollet((num_threads > 0) ? EPOLLET : 0)
        , connections()
        , metrics()
        , threads()
        , stop(false)
{
	memset(connections, 0, sizeof(connections));
	listen_fd = socket(PF_INET, SOCK_STREAM, 0);
	if (listen_fd == -1) {
		log(NORMAL, "FATAL: couldn't open server socket: %s\n",
				strerror(errno));
		exit(1);
	}
	int option_value = 1;
	if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &option_value,
			sizeof(option_value)) != 0) {
		log(NORMAL, "FATAL: couldn't set SO_REUSEADDR on listen "
				"socket: %s",
				strerror(errno));
		exit(1);
	}
	if (fcntl(listen_fd, F_SETFL, O_NONBLOCK) != 0) {
		log(NORMAL, "FATAL: couldn't set O_NONBLOCK on listen "
				"socket: %s",
				strerror(errno));
		exit(1);
	}
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = INADDR_ANY;
	if (bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr))
			== -1) {
		log(NORMAL, "FATAL: couldn't bind to port %d: %s\n", port,
				strerror(errno));
		exit(1);
	}
	if (listen(listen_fd, 1000) == -1) {
		log(NORMAL, "FATAL: couldn't listen on socket: %s",
				strerror(errno));
		exit(1);
	}
	
	epoll_fd = epoll_create(10);
	if (epoll_fd < 0) {
		log(NORMAL, "FATAL: couldn't create epoll instance for "
				"TCP server: %s\n",
				strerror(errno));
		exit(1);
	}
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = listen_fd;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
		log(NORMAL, "FATAL: couldn't add listen socket to epoll: %s\n",
				strerror(errno));
		exit(1);
	}
	
	for (int i = 0; i < num_threads; i++)
		threads.emplace_back(&tcp_server::server, this, i);
}

/**
 * tcp_server::~tcp_server() - Destructor for TCP servers. Terminates the
 * server's background thread.
 */
tcp_server::~tcp_server()
{
	int fds[2];
	
	stop = true;
	
	/* In order to wake up the background threads, open a file that is
	 * readable and add it to the epoll set.
	 */
	if (pipe2(fds, 0) < 0) {
		log(NORMAL, "FATAL: couldn't create pipe to shutdown TCP "
				"server: %s\n", strerror(errno));
		exit(1);
	}
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = fds[0];
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fds[0], &ev);
	if (write(fds[1], "xxxx", 4) < 0) {
		log(NORMAL, "FATAL: couldn't write to TCP shutdown pipe: %s\n",
				strerror(errno));
		exit(1);
	}
	
	for (size_t i = 0; i < threads.size(); i++)
		threads[i].join();
	close(listen_fd);
	close(epoll_fd);
	close(fds[0]);
	close(fds[1]);
	for (unsigned i = 0; i < MAX_FDS; i++) {
		if (connections[i] != NULL) {
			if (close(i) < 0)
				log(NORMAL, "Error closing TCP connection to "
						"%s: %s\n",
						print_address(
						&connections[i]->peer),
						strerror(errno));
			delete connections[i];
			log(NORMAL, "Deleted connection at 0x%p, size %lu\n",
				connections[i], sizeof(*connections[i]));
			connections[i] = NULL;
		}
	}
}

/**
 * tcp_server::server() - Handles incoming TCP requests on a listen socket
 * and all of the connections accepted via that socket. Normally invoked as
 * top-level method in a thread. There can potentially be multiple instances
 * of this function running simultaneously.
 * @thread_id:  Unique id for this particular thread among all of the
 *              threads in this server.
 */
void tcp_server::server(int thread_id)
{
	char buffer[50];
	
	snprintf(buffer, sizeof(buffer), "S%d.%d", id, thread_id);
	time_trace::create_thread_buffer(buffer);
	
	/* Each iteration through this loop processes a batch of epoll events. */
	while (1) {
#define MAX_EVENTS 20
		struct epoll_event events[MAX_EVENTS];
		int num_events;
		
		while (1) {
			num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
			if (stop)
				return;
			if (num_events >= 0)
				break;
			if ((errno == EAGAIN) || (errno == EINTR))
				continue;
			log(NORMAL, "FATAL: epoll_wait failed: %s\n",
					strerror(errno));
			exit(1);
		}
		for (int i = 0; i < num_events; i++) {
			int fd = events[i].data.u32;
			if (fd == listen_fd) {
				spin_lock lock_guard(&mutex);
				accept(epoll_fd);
			} else {
				spin_lock lock_guard(&fd_locks[fd]);
				if ((events[i].events & EPOLLIN) &&
						(connections[fd] != NULL))
					read(fd);
				if ((events[i].events & EPOLLOUT) &&
						(connections[fd] != NULL)) {
					if (connections[fd]->xmit())
						connections[fd]->set_epoll_events(
								epoll_fd,
								EPOLLIN|epollet);
				}
			}
		}
	}
}

/**
 * tcp_server::accept() - Accepts a new incoming TCP connection and
 * initializes state for that connection.
 * @epoll_fd:   Used to arrange for epolling on the new connection.
 */
void tcp_server::accept(int epoll_fd)
{
	int fd;
	struct sockaddr_in client_addr;
	socklen_t addr_len = sizeof(client_addr);
	
	fd = ::accept4(listen_fd, reinterpret_cast<sockaddr *>(&client_addr),
			&addr_len, SOCK_NONBLOCK);
	if (fd < 0) {
		if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
			return;
		log(NORMAL, "FATAL: couldn't accept incoming TCP connection: "
				"%s\n", strerror(errno));
		exit(1);
	}
	log(NORMAL, "tcp_server on port %d accepted connection from %s, fd %d\n",
			port, print_address(&client_addr), fd);
	int flag = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
	if (fd >= MAX_FDS) {
		log(NORMAL, "FATAL: TCP socket fd %d is greater than MAX_FDS\n",
				fd);
		exit(1);
	}
	spin_lock lock_guard(&fd_locks[fd]);
	tcp_connection *connection = new tcp_connection(fd, fd, port,
			client_addr);
	connections[fd] = connection;
	connection->set_epoll_events(epoll_fd, EPOLLIN|epollet);
}

/**
 * tcp_server::read() - Reads available data from a TCP connection; once an
 * entire request has been read, sends an appropriate response.
 * @fd:        File descriptor for connection; connections must hold
 *             state information for this descriptor.
 */
void tcp_server::read(int fd)
{
	int error = connections[fd]->read(epollet,
			[this, fd](message_header *header) {
		metrics.requests++;
		metrics.data += header->length;
		if (!connections[fd]->send_message(header))
			connections[fd]->set_epoll_events(epoll_fd,
					EPOLLIN|EPOLLOUT|epollet);
	});
	if (error) {
		log(NORMAL, "Closing client connection: %s\n",
				connections[fd]->error_message);
		spin_lock lock_guard(&mutex);
		if (close(fd) < 0) {
			log(NORMAL, "Error closing TCP connection to %s: %s\n",
					print_address(&connections[fd]->peer),
					strerror(errno));
		}
		delete connections[fd];
		connections[fd] = NULL;
	}
}

/**
 * class client - Holds information that is common to both Homa clients
 * and TCP clients. 
 */
class client {
    public:
	client(int id);
	virtual ~client();
	void check_completion(const char *protocol);
	void record(int length, uint32_t rtt, int server_id);
	virtual void stop_sender(void) {}
	
	/**
	 * @id: unique identifier for this client (index starting at
	 * 0 for the first client.
	 */
	int id;
	    
	/**
	 * @receivers_running: number of receiving threads that have
	 * initialized and are ready to receive responses.
	 */
	std::atomic<size_t> receivers_running;
	
	/**
	 * @request_servers: a randomly chosen collection of indexes into
	 * server_addrs; used to select the server for each outgoing request.
	 */
	std::vector<int16_t> request_servers;

	/**
	 * @next_server: index into request_servers of the server to use for
	 * the next outgoing RPC.
	 */
	uint32_t next_server;
	
	/**
	 * @request_lengths: a randomly chosen collection of lengths to
	 * use for outgoing RPCs. Precomputed to save time during the
	 * actual measurements, and based on a given distribution.
	 * Note: lengths are always at least 4 (this is needed in order
	 * to include a 32-bit timestamp in the request).
	 */
	std::vector<int> request_lengths;

	/**
	 * @cnext_length: index into request_lengths of the length to use for
	 * the next outgoing RPC.
	 */
	uint32_t next_length;
	
	/**
	 * @request_intervals: a randomly chosen collection of inter-request
	 * intervals, measured in rdtsc cycles. Precomputed to save time
	 * during the actual measurements, and chosen to achieve a given
	 * network utilization, assuming a given distribution of request
	 * lengths.
	 */
	std::vector<int> request_intervals;

	/**
	 * @next_interval: index into request_intervals of the value to use
	 * for the next outgoing RPC.
	 */
	std::atomic<uint32_t> next_interval;
	
	/**
	 * @actual_lengths: a circular buffer that holds the actual payload
	 * sizes used for the most recent RPCs.
	 */
	std::vector<int> actual_lengths;
	
	/**
	 * @actual_rtts: a circular buffer that holds the actual round trip
	 * times (measured in rdtsc cycles) for the most recent RPCs. Entries
	 * in this array correspond to those in @actual_lengths.
	 */
	std::vector<uint32_t> actual_rtts;

	/**
	 * define NUM_CLENT_STATS: number of records in actual_lengths
	 * and actual_rtts.
	 */
#define NUM_CLIENT_STATS 500000
	
	/** @requests: total number of RPCs issued so far for each server. */
	std::vector<uint64_t> requests;
	
	/** @responses: total number of responses received so far from
	 * each server. Dynamically allocated (as of 3/2020, can't use
	 * vector with std::atomic).
	 */
	std::atomic<uint64_t> *responses;
	
	/** @num_servers: Number of entries in @responses. */
	size_t num_servers;
	
	/**
	 * @total_requests: total number of RPCs issued so far across all
	 * servers.
	 */
	uint64_t total_requests;
	
	/**
	 * @total_responses: total number of responses received so far from all
	 * servers.
	 */
	std::atomic<uint64_t> total_responses;
	
	/**
	 * @response_data: total number of bytes of data in responses
	 * received so far.
	 */
	std::atomic<uint64_t> response_data;
	
	/**
	 * @total_rtt: sum of round-trip times (in rdtsc cycles) for
	 * all responses received so far.
	 */
	std::atomic<uint64_t> total_rtt;
	
	/**
	 * @lag: time in rdtsc cycles by which we are running behind
	 * because client_port_max was exceeded (i.e., the request
	 * we just sent should have been sent @lag cycles ago).
	 */
	uint64_t lag;
};

/** @clients: keeps track of all existing clients. */
std::vector<client *> clients;
	
/**
 * client::client() - Constructor for client objects.
 *
 * @id: Unique identifier for this client (index starting at 0?)
 */
client::client(int id)
	: id(id)
	, receivers_running(0)
	, request_servers()
	, next_server(0)
	, request_lengths()
	, next_length(0)
	, request_intervals()
	, next_interval(0)
	, actual_lengths(NUM_CLIENT_STATS, 0)
	, actual_rtts(NUM_CLIENT_STATS, 0)
	, requests()
	, responses()
        , num_servers(server_addrs.size())
	, total_requests(0)
	, total_responses(0)
	, response_data(0)
        , total_rtt(0)
        , lag(0)
{
	/* Precompute information about the requests this client will
	 * generate. Pick a different prime number for the size of each
	 * vector, so that they will wrap at different times, giving
	 * different combinations of values over time.
	 */
#define NUM_SERVERS 4729
#define NUM_LENGTHS 7207
#define NUM_INTERVALS 8783
	std::uniform_int_distribution<int> server_dist(0,
			static_cast<int>(num_servers - 1));
	for (int i = 0; i < NUM_SERVERS; i++) {
		int server = server_dist(rand_gen);
		request_servers.push_back(server);
	}
	if (!dist_sample(workload, &rand_gen, NUM_LENGTHS, &request_lengths)) {
		printf("FATAL: invalid workload '%s'\n", workload);
		exit(1);
	}
	if (net_bw == 0.0)
		request_intervals.push_back(0);
	else {
		double lambda = 1e09*net_bw/(dist_mean(workload,
				HOMA_MAX_MESSAGE_LENGTH)*client_ports);
		double cycles_per_second = get_cycles_per_sec();
		std::exponential_distribution<double> interval_dist(lambda);
		for (int i = 0; i < NUM_INTERVALS; i++) {
			double seconds = interval_dist(rand_gen);
			int cycles = int(seconds*cycles_per_second);
			request_intervals.push_back(cycles);
		}
	}
	requests.resize(server_addrs.size());
	responses = new std::atomic<uint64_t>[num_servers];
	for (size_t i = 0; i < num_servers; i++)
		responses[i] = 0;
	double avg_length = 0;
	for (size_t i = 0; i < request_lengths.size(); i++)
		avg_length += request_lengths[i];
	avg_length /= NUM_LENGTHS;
	uint64_t interval_sum = 0;
	for (size_t i = 0; i < request_intervals.size(); i++)
		interval_sum += request_intervals[i];
	double rate = ((double) NUM_INTERVALS)/to_seconds(interval_sum);
	log(NORMAL, "Average message length %.1f KB (expected %.1fKB), "
			"rate %.2f K/sec, expected BW %.1f MB/sec\n",
			avg_length*1e-3, dist_mean(workload,
			HOMA_MAX_MESSAGE_LENGTH)*1e-3, rate*1e-3,
			avg_length*rate*1e-6);
}

/**
 * Destructor for clients.
 */
client::~client()
{
	delete[] responses;
}

/**
 * check_completion() - Make sure that all outstanding requests have
 * completed; if not, generate a log message.
 * @protocol:  String that identifies the current protocol for the log
 *             message, if any.
 */
void client::check_completion(const char *protocol)
{
	string server_info;
	int incomplete = total_requests - total_responses;
	for (size_t i = 0; i < requests.size(); i++) {
		char buffer[100];
		int diff = requests[i] - responses[i];
		if (diff == 0)
			continue;
		if (!server_info.empty())
			server_info.append(", ");
		snprintf(buffer, sizeof(buffer), "s%lu: %d", i, diff);
		server_info.append(buffer);
	}
	if ((incomplete != 0) || !server_info.empty())
		log(NORMAL, "ERROR: %d incomplete %s requests (%s)\n",
				incomplete, protocol, server_info.c_str());
}

/**
 * record() - Records statistics about a particular request.
 * @length:     Size of the request and response messages for the request,
 *              in bytes.
 * @rtt:        Total round-trip time to complete the request, in rdtsc cycles.
 * @server_id:  Index of the server for this request in @server_addrs.
 */
void client::record(int length, uint32_t rtt, int server_id)
{
	int slot = total_responses.fetch_add(1) % NUM_CLIENT_STATS;
	responses[server_id].fetch_add(1);
	response_data += length;
	total_rtt += rtt;
	actual_lengths[slot] = length;
	actual_rtts[slot] = rtt;
}

/**
 * class homa_client - Holds information about a single Homa client,
 * which consists of one thread issuing requests and one or more threads
 * receiving responses. 
 */
class homa_client : public client {
    public:
	homa_client(int id);
	virtual ~homa_client();
	void receiver(int id);
	void sender(void);
	virtual void stop_sender(void);
	bool wait_response(uint64_t id);
	
	/** @fd: file descriptor for Homa socket. */
	int fd;
	
	/** @stop_sending: true means the sending thread should exit ASAP. */
	bool exit_sender;
	
	/** @stop: true means receiving threads should exit ASAP. */
	bool exit_receivers;
	
	/** @server_exited:  just what you'd guess from the name. */
	bool sender_exited;
	
	/** @receiver: threads that receive responses. */
	std::vector<std::thread> receiving_threads;
	
	/**
	 * @sender: thread that sends requests (may also receive
	 * responses if port_receivers is 0).
	 */
	std::optional<std::thread> sending_thread;
};

/**
 * homa_client::homa_client() - Constructor for homa_client objects.
 *
 * @id: Unique identifier for this client (index starting at 0?)
 */
homa_client::homa_client(int id)
	: client(id)
	, fd(-1)
        , exit_sender(false)
        , exit_receivers(false)
        , sender_exited(false)
        , receiving_threads()
        , sending_thread()
{
	fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_HOMA);
	if (fd < 0) {
		log(NORMAL, "Couldn't open Homa socket: %s\n", strerror(errno));
		exit(1);
	}
	
	for (int i = 0; i < port_receivers; i++) {
		receiving_threads.emplace_back(&homa_client::receiver, this, i);
	}
	while (receivers_running < receiving_threads.size()) {
		/* Wait for the receivers to begin execution before
		 * starting the sender; otherwise the initial RPCs
		 * may appear to take a long time.
		 */
	}
	sending_thread.emplace(&homa_client::sender, this);
}

/**
 * homa_client::~homa_client() - Destructor for homa_client objects;
 * will terminate threads created for this client.
 */
homa_client::~homa_client()
{
	uint64_t start = rdtsc();
	exit_sender = true;
	exit_receivers = true;
	while (!sender_exited || (total_responses != total_requests)) {
		if (to_seconds(rdtsc() - start) > 2.0)
			break;
	}
	shutdown(fd, SHUT_RDWR);
	close(fd);
	if (sending_thread)
		sending_thread->join();
	for (std::thread &thread: receiving_threads)
		thread.join();
	check_completion("homa");
}

/**
 * homa_client::stop_sender() - Ask the sending thread to stop sending,
 * and wait until it exits (but give up if that takes too long).
 */
void homa_client::stop_sender(void)
{
	uint64_t start = rdtsc();
	exit_sender = true;
	while (1) {
		if (sender_exited) {
			if (sending_thread) {
				sending_thread->join();
				sending_thread.reset();
			}
		}
		if (to_seconds(rdtsc() - start) > 0.5)
			break;
	}
}

/**
 * homa_client::weight_response() - Wait for a response to arrive and
 * update statistics.
 * @id     Id of a specific RPC to wait for, or 0 for "any response".
 * Return: True means that a response was received; false means the client
 *         has been stopped and the socket has been shut down.
 */
bool homa_client::wait_response(uint64_t id)
{
	char response[1000000];
	message_header *header = reinterpret_cast<message_header *>(response);
	struct sockaddr_in server_addr;
	
	id = 0;
	int length;
	do {
		length = homa_recv(fd, response, sizeof(response),
				HOMA_RECV_RESPONSE, &id,
				(struct sockaddr *) &server_addr,
				sizeof(server_addr));
	} while ((length < 0) && ((errno == EAGAIN) || (errno == EINTR)));
	if (length < 0) {
		if (exit_receivers)
			return false;
		log(NORMAL, "FATAL: error in homa_recv: %s (id %lu, server %s)\n",
				strerror(errno), id,
				print_address(&server_addr));
		exit(1);
	}
	uint32_t end_time = rdtsc() & 0xffffffff;
	tt("Received response from server %d with %d bytes", header->server_id,
			length);
	record(length, end_time - header->start_time, header->server_id);
	return true;
}

/**
 * homa_client::sender() - Invoked as the top-level method in a thread;
 * invokes a pseudo-random stream of RPCs continuously.
 */
void homa_client::sender()
{
	char request[1000000];
	message_header *header = reinterpret_cast<message_header *>(request);
	uint64_t next_start = rdtsc();
	char buffer[50];
	
	snprintf(buffer, sizeof(buffer), "C%d", id);
	time_trace::create_thread_buffer(buffer);
	
	while (1) {
		uint64_t now;
		uint64_t id;
		int server;
		
		/* Wait until (a) we have reached the next start time
		 * and (b) there aren't too many requests outstanding.
		 */
		while (1) {
			if (exit_sender) {
				sender_exited = true;
				return;
			}
			now = rdtsc();
			if (now < next_start)
				continue;
			if ((total_requests - total_responses) < client_port_max)
				break;
		}
		
		server = request_servers[next_server];
		next_server++;
		if (next_server >= request_servers.size())
			next_server = 0;
		
		header->length = request_lengths[next_length];
		if (header->length > HOMA_MAX_MESSAGE_LENGTH)
			header->length = HOMA_MAX_MESSAGE_LENGTH;
		if (header->length < sizeof32(*header))
			header->length = sizeof32(*header);
		header->start_time = now & 0xffffffff;
		header->server_id = server;
		tt("sending to server %d, length %d", server, header->length);
		int status = homa_send(fd, request, header->length,
			reinterpret_cast<struct sockaddr *>(
			&server_addrs[server]),
			sizeof(server_addrs[0]), &id);
		if (status < 0) {
			log(NORMAL, "FATAL: error in homa_send: %s (request "
					"length %d)\n", strerror(errno),
					header->length);
			exit(1);
		}
		requests[server]++;
		total_requests++;
		next_length++;
		if (next_length >= request_lengths.size())
			next_length = 0;
		lag = now - next_start;
		next_start = next_start + request_intervals[next_interval];
		next_interval++;
		if (next_interval >= request_intervals.size())
			next_interval = 0;
		
		if (receivers_running == 0) {
			/* There isn't a separate receiver thread; wait for
			 * the response here. */
			wait_response(id);
		}
	}
}

/**
 * homa_client::receiver() - Invoked as the top-level method in a thread
 * that waits for RPC responses and then logs statistics about them.
 * @id:   Unique id for this receiver within its client.
 */
void homa_client::receiver(int receiver_id)
{	
	char buffer[50];
	snprintf(buffer, sizeof(buffer), "R%d.%d", id, receiver_id);
	time_trace::create_thread_buffer(buffer);
	
	receivers_running++;
	while (wait_response(0)) {}
}

/**
 * class tcp_client - Holds information about a single TCP client,
 * which consists of one thread issuing requests and one thread receiving
 * responses. 
 */
class tcp_client : public client {
    public:
	tcp_client(int id);
	virtual ~tcp_client();
	void read(tcp_connection *connection);
	void receiver(int id);
	void sender(void);
	
	/** 
	 * @connections: One entry for each server in server_addrs; used to
	 * communicate with that server.
	 */
	std::vector<tcp_connection *> connections;
	
	/**
	 * @blocked: Contains all of the connections for which there is
	 * pending output that couldn't be sent because the connection
	 * was backed up.
	 */
	std::vector<tcp_connection *> blocked;
	
	/**
	 * @epoll_fd: File descriptor used by @receiving_thread to
	 * wait for epoll events.
	 */
	int epoll_fd;
	
	/**
	 * @epollet: EPOLLET if this flag should be used, or 0 otherwise.
	 * We only use edge triggering if there are multiple receiving
	 * threads (it's unneeded if there's only a single thread, and
	 * it's faster not to use it).
	 */
	int epollet;
	
	/** @stop:  True means background threads should exit. */
	bool stop;
	
	/** @receiver: threads that receive responses. */
	std::vector<std::thread> receiving_threads;
	
	/**
	 * @sender: thread that sends requests (may also receive
	 * responses if port_receivers is 0).
	 */
	std::optional<std::thread> sending_thread;
};

/**
 * tcp_client::tcp_client() - Constructor for tcp_client objects.
 *
 * @id: Unique identifier for this client (index starting at 0?)
 */
tcp_client::tcp_client(int id)
	: client(id)
	, connections()
        , blocked()
        , epoll_fd(-1)
	, epollet((port_receivers > 1) ? EPOLLET : 0)
        , stop(false)
        , receiving_threads()
        , sending_thread()
{
	epoll_fd = epoll_create(10);
	if (epoll_fd < 0) {
		log(NORMAL, "FATAL: tcp_client couldn't create epoll "
				"instance: %s\n", strerror(errno));
		exit(1);
	}
	
	for (uint32_t i = 0; i < server_addrs.size(); i++) {
		int fd = socket(PF_INET, SOCK_STREAM, 0);
		if (fd == -1) {
			log(NORMAL, "FATAL: couldn't open TCP client "
					"socket: %s\n",
					strerror(errno));
			exit(1);
		}
		if (connect(fd, reinterpret_cast<struct sockaddr *>(
				&server_addrs[i]),
				sizeof(server_addrs[i])) == -1) {
			log(NORMAL, "FATAL: client couldn't connect "
					"to %s: %s\n",
					print_address(&server_addrs[i]),
					strerror(errno));
			exit(1);
		}
		int flag = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
		if (fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
			log(NORMAL, "FATAL: couldn't set O_NONBLOCK on socket "
					"to server %s: %s",
					print_address(&server_addrs[i]),
					strerror(errno));
			exit(1);
		}
		struct sockaddr_in addr;
		socklen_t length = sizeof(addr);
		if (getsockname(fd, reinterpret_cast<struct sockaddr *>(&addr),
				&length)) {
			log(NORMAL, "FATAL: getsockname failed for TCP client: "
					"%s\n", strerror(errno));
			exit(1);
		}
		connections.emplace_back(new tcp_connection(fd, i,
				ntohs(addr.sin_port), server_addrs[i]));
		connections[connections.size()-1]->set_epoll_events(epoll_fd,
				EPOLLIN|epollet);
	}
	
	for (int i = 0; i < port_receivers; i++) {
		receiving_threads.emplace_back(&tcp_client::receiver, this, i);
	}
	while (receivers_running < receiving_threads.size()) {
		/* Wait for the receivers to begin execution before
		 * starting the sender; otherwise the initial RPCs
		 * may appear to take a long time.
		 */
	}
	sending_thread.emplace(&tcp_client::sender, this);
}

/**
 * tcp_client::~tcp_client() - Destructor for tcp_client objects;
 * will terminate threads created for this client.
 */
tcp_client::~tcp_client()
{
	int fds[2];
	
	stop = true;
	
	/* In order to wake up the background thread, open a file that is
	 * readable and add it to the epoll set.
	 */
	if (pipe2(fds, 0) < 0) {
		log(NORMAL, "FATAL: couldn't create pipe to shutdown TCP "
				"server: %s\n", strerror(errno));
		exit(1);
	}
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = fds[0];
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fds[0], &ev);
	if (write(fds[1], "xxxx", 4) < 0) {
		log(NORMAL, "FATAL: couldn't write to TCP shutdown "
				"pipe: %s\n", strerror(errno));
		exit(1);
	}
	
	if (sending_thread)
		sending_thread->join();
	for (std::thread& thread: receiving_threads)
		thread.join();
	
	close(fds[0]);
	close(fds[1]);
	close(epoll_fd);
	for (tcp_connection *connection: connections) {
		close(connection->fd);
		delete connection;
	}
}

/**
 * tcp_client::sender() - Invoked as the top-level method in a thread;
 * invokes a pseudo-random stream of RPCs continuously.
 */
void tcp_client::sender()
{
	char buffer[50];
	
	snprintf(buffer, sizeof(buffer), "C%d", id);
	time_trace::create_thread_buffer(buffer);
	
	uint64_t next_start = rdtsc();
	message_header header;
	size_t max_pending = 1;
	
	/* Index of the next connection in blocked on which to try sending. */
	size_t next_blocked = 0;
	
	while (1) {
		uint64_t now;
		int server;
		
		/* Wait until (a) we have reached the next start time
		 * and (b) there aren't too many requests outstanding.
		 */
		while (1) {
			if (stop)
				return;
			now = rdtsc();
			if ((now >= next_start)
					&& ((total_requests - total_responses)
					< client_port_max))
				break;
			
			/* Try to finish I/O on backed up connections. */
			if (blocked.size() == 0)
				continue;
			if (next_blocked >= blocked.size())
				next_blocked = 0;
			if (blocked[next_blocked]->xmit())
				blocked.erase(blocked.begin() + next_blocked);
			else
				next_blocked++;
		}
		
		server = request_servers[next_server];
		next_server++;
		if (next_server >= request_servers.size())
			next_server = 0;
		
		header.length = request_lengths[next_length];
		if ((header.length > HOMA_MAX_MESSAGE_LENGTH) && tcp_trunc)
			header.length = HOMA_MAX_MESSAGE_LENGTH;
		header.start_time = now & 0xffffffff;
		header.server_id = server;
		header.msg_id = message_id.fetch_add(1);
		size_t old_pending = connections[server]->pending();
		tt("sending message id %u to server %d, length %d",
				header.msg_id, server, header.length);
		if ((!connections[server]->send_message(&header))
				&& (old_pending == 0)) {
			blocked.push_back(connections[server]);
			if (connections[server]->pending() > max_pending) {
				max_pending = connections[server]->pending();
				log(NORMAL, "max_pending now %lu for "
						"tcp_client %d\n",
						max_pending, id);
			}
		}
		if (verbose)
			log(NORMAL, "tcp_client %d sent request to server port "
					"%d, length %d\n",
					id, header.server_id,
					request_lengths[next_length]);
		requests[server]++;
		total_requests++;
		next_length++;
		if (next_length >= request_lengths.size())
			next_length = 0;
		lag = now - next_start;
		next_start += request_intervals[next_interval];
		next_interval++;
		if (next_interval >= request_intervals.size())
			next_interval = 0;
	}
}

/**
 * tcp_client::receiver() - Invoked as the top-level method in a thread
 * that waits for RPC responses and then logs statistics about them.
 * @receiver_id:  Id of this receiver (among those for the same port).
 */
void tcp_client::receiver(int receiver_id)
{
	char buffer[50];
	
	snprintf(buffer, sizeof(buffer), "R%d.%d", id, receiver_id);
	time_trace::create_thread_buffer(buffer);
	receivers_running++;
	
	/* Each iteration through this loop processes a batch of incoming
	 * responses
	 */
	while (1) {
#define MAX_EVENTS 20
		struct epoll_event events[MAX_EVENTS];
		int num_events;
		
		while (1) {
			num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
			if (stop)
				return;
			if (num_events > 0)
				break;
			if ((errno == EAGAIN) || (errno == EINTR))
				continue;
			log(NORMAL, "FATAL: epoll_wait failed in tcp_client: "
					"%s\n",
					strerror(errno));
			exit(1);
		}
		for (int i = 0; i < num_events; i++) {
			int fd = events[i].data.fd;
			tcp_connection *connection = connections[fd];
			if (events[i].events & EPOLLIN) {
				spin_lock lock_guard(&fd_locks[fd]);
				read(connection);
			}
		}
	}
}

/**
 * tcp_client::read() - Is available data from a TCP connection; if an
 * entire response has now been read, records statistics for that request.
 * @connection:  TCP connection that has data available to read.
 */
void tcp_client::read(tcp_connection *connection)
{
	int error = connection->read(epollet, [this](message_header *header) {
		uint32_t end_time = rdtsc() & 0xffffffff;
		record(header->length, end_time - header->start_time,
				header->server_id);
	});
	if (error) {
		log(NORMAL, "FATAL: %s (client)\n",
				connection->error_message);
		exit(1);
	}
}

/**
 * server_stats() -  Prints recent statistics collected from all
 * servers.
 * @now:   Current time in rdtsc cycles (used to compute rates for
 *         statistics).
 */
void server_stats(uint64_t now)
{
	char details[10000];
	int offset = 0;
	int length;
	uint64_t server_rpcs = 0;
	uint64_t server_data = 0;
	details[0] = 0;
	for (uint32_t i = 0; i < metrics.size(); i++) {
		server_metrics *server = metrics[i];
		server_rpcs += server->requests;
		server_data += server->data;
		length = snprintf(details + offset, sizeof(details) - offset,
				"%s%lu", (offset != 0) ? " " : "",
				server->requests - last_per_server_rpcs[i]);
		offset += length;
		if (i > last_per_server_rpcs.size())
			printf("last_per_server_rpcs has %lu entries, needs %lu\n",
					last_per_server_rpcs.size(),
					metrics.size());
		last_per_server_rpcs[i] = server->requests;
	}
	if ((last_stats_time != 0) && (server_data != last_server_data)) {
		double elapsed = to_seconds(now - last_stats_time);
		double rpcs = (double) (server_rpcs - last_server_rpcs);
		double data = (double) (server_data - last_server_data);
		log(NORMAL, "Servers: %.2f Kops/sec, %.2f MB/sec, "
				"avg. length %.1f bytes\n",
				rpcs/(1000.0*elapsed), data/(1e06*elapsed),
				data/rpcs);
		log(NORMAL, "RPCs per server: %s\n", details);
	}
	last_server_rpcs = server_rpcs;
	last_server_data = server_data;
}

/**
 * client_stats() -  Prints recent statistics collected by all existing
 * clients (either TCP or Homa).
 * @now:       Current time in rdtsc cycles (used to compute rates for
 *             statistics).
 */
void client_stats(uint64_t now)
{
#define CDF_VALUES 100000
	uint64_t client_rpcs = 0;
	uint64_t client_data = 0;
	uint64_t total_rtt = 0;
	uint64_t lag = 0;
	uint64_t outstanding_rpcs = 0;
	uint64_t cdf_times[CDF_VALUES];
	int times_per_client;
	int cdf_index = 0;
	
	if (clients.size() == 0)
		return;
	
	times_per_client = CDF_VALUES/clients.size();
	if (times_per_client > NUM_CLIENT_STATS)
		times_per_client = NUM_CLIENT_STATS;
	for (client *client: clients) {
		for (size_t i = 0; i < client->num_servers; i++)
			client_rpcs += client->responses[i];
		client_data += client->response_data;
		total_rtt += client->total_rtt;
		lag += client->lag;
		outstanding_rpcs += client->total_requests
			- client->total_responses;
		for (int i = 1; i <= times_per_client; i++) {
			/* Collect the most recent RTTs from the client for
			 * computing a CDF.
			 */
			int src = (client->total_responses - i)
					% NUM_CLIENT_STATS;
			if (client->actual_rtts[src] == 0) {
				/* Client hasn't accumulated times_per_client
				 * entries yet; just use what it has. */
				break;
			}
			cdf_times[cdf_index] = client->actual_rtts[src];
			cdf_index++;
		}
	}
	std::sort(cdf_times, cdf_times + cdf_index);
	if ((last_stats_time != 0) && (client_data != last_client_data)) {
		double elapsed = to_seconds(now - last_stats_time);
		double rpcs = (double) (client_rpcs - last_client_rpcs);
		double data = (double) (client_data - last_client_data);
		log(NORMAL, "Clients: %.2f Kops/sec, %.2f MB/sec, RTT (us) "
				"P50 %.2f P99 %.2f P99.9 %.2f, avg. length "
				"%.1f bytes\n",
				rpcs/(1000.0*elapsed), data/(1e06*elapsed),
				to_seconds(cdf_times[cdf_index/2])*1e06,
				to_seconds(cdf_times[99*cdf_index/100])*1e06,
				to_seconds(cdf_times[999*cdf_index/1000])*1e06,
			        data/rpcs);
		double lag_fraction;
		if (lag > last_lag)
			lag_fraction = (to_seconds(lag - last_lag)/elapsed)
				/ clients.size();
		else
			lag_fraction = -(to_seconds(last_lag - lag)/elapsed)
				/ clients.size();
		if (lag_fraction >= .01)
			log(NORMAL, "Lag due to overload: %.1f%%\n",
					lag_fraction*100.0);
	}
	if (outstanding_rpcs != 0)
		log(NORMAL, "Outstanding client RPCs: %lu\n", outstanding_rpcs);
	last_client_rpcs = client_rpcs;
	last_client_data = client_data;
	last_total_rtt = total_rtt;
	last_lag = lag;
}

/**
 * log_stats() - Enter an infinite loop printing statistics to the
 * log every second. This function never returns.
 */
void log_stats()
{
	while (1) {
		sleep(1);
		std::lock_guard<std::mutex> lock(cmd_lock);
		uint64_t now = rdtsc();
		server_stats(now);
		client_stats(now);

		last_stats_time = now;
	}
}

/**
 * client_cmd() - Parse the arguments for a "client" command and execute it.
 * @words:  Command arguments (including the command name as @words[0]).
 * 
 * Return:  Nonzero means success, zero means there was an error.
 */
int client_cmd(std::vector<string> &words)
{
	client_max = 1;
	client_ports = 1;
	first_port = 4000;
	first_server = 1;
	net_bw = 0.0;
	port_receivers = 1;
	protocol = "homa";
	server_nodes = 1;
	tcp_trunc = true;
	workload = "100";
	for (unsigned i = 1; i < words.size(); i++) {
		const char *option = words[i].c_str();

		if (strcmp(option, "--client-max") == 0) {
			if (!parse_int(words, i+1, (int *) &client_max,
					option))
				return 0;
			i++;
		} else if (strcmp(option, "--first-port") == 0) {
			if (!parse_int(words, i+1, &first_port, option))
				return 0;
			i++;
		} else if (strcmp(option, "--first-server") == 0) {
			if (!parse_int(words, i+1, &first_server, option))
				return 0;
			i++;
		} else if (strcmp(option, "--id") == 0) {
			if (!parse_int(words, i+1, &id, option))
				return 0;
			i++;
		} else if (strcmp(option, "--net-bw") == 0) {
			if (!parse_float(words, i+1, &net_bw, option))
				return 0;
			i++;
		} else if (strcmp(option, "--no-trunc") == 0) {
			tcp_trunc = false;
		} else if (strcmp(option, "--ports") == 0) {
			if (!parse_int(words, i+1, &client_ports, option))
				return 0;
			i++;
		} else if (strcmp(option, "--port-receivers") == 0) {
			if (!parse_int(words, i+1, &port_receivers, option))
				return 0;
			i++;
		} else if (strcmp(option, "--protocol") == 0) {
			if ((i + 1) >= words.size()) {
				printf("No value provided for %s\n",
						option);
				return 0;
			}
			protocol = words[i+1].c_str();
			i++;
		} else if (strcmp(option, "--server-nodes") == 0) {
			if (!parse_int(words, i+1, &server_nodes, option))
				return 0;
			i++;
		} else if (strcmp(option, "--server-ports") == 0) {
			if (!parse_int(words, i+1, &server_ports, option))
				return 0;
			i++;
		} else if (strcmp(option, "--workload") == 0) {
			if ((i + 1) >= words.size()) {
				printf("No value provided for %s\n",
						option);
				return 0;
			}
			workload = words[i+1].c_str();
			i++;
		} else {
			printf("Unknown option '%s'\n", option);
			return 0;
		}
	}
	init_server_addrs();
	client_port_max = client_max/client_ports;
	if (client_port_max < 1)
		client_port_max = 1;

	/* Create clients. */
	for (int i = 0; i < client_ports; i++) {
		if (strcmp(protocol, "homa") == 0)
			clients.push_back(new homa_client(i));
		else
			clients.push_back(new tcp_client(i));
	}
	last_stats_time = 0;
	return 1;
}

/**
 * dump_times_cmd() - Parse the arguments for a "dump_times" command and
 * execute it.
 * @words:  Command arguments (including the command name as @words[0]).
 * 
 * Return:  Nonzero means success, zero means there was an error.
 */
int dump_times_cmd(std::vector<string> &words)
{
	FILE *f;
	time_t now;
	char time_buffer[100];
	
	if (words.size() != 2) {
		printf("Wrong # args; must be 'dump_times file'\n");
		return 0;
	}
	f = fopen(words[1].c_str(), "w");
	if (f == NULL) {
		printf("Couldn't open file %s: %s\n", words[1].c_str(),
				strerror(errno));
		return 0;
	}
	
	time(&now);
	strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S",
			localtime(&now));
	fprintf(f, "# Round-trip times measured by cp_node at %s\n",
			time_buffer);
	fprintf(f, "# --protocol %s, --workload %s, --net-bw %.1f --threads %d,\n",
			protocol, workload, net_bw, client_ports);
	fprintf(f, "# --server-nodes %d --server-ports %d, --client-max %d\n",
			server_nodes, server_ports, client_max);
	fprintf(f, "# Length   RTT (usec)\n");
	for (client *client: clients) {
		__u32 start = client->total_responses % NUM_CLIENT_STATS;
		__u32 i = start;
		while (1) {
			if (client->actual_rtts[i] != 0) {
				fprintf(f, "%8d %12.2f\n",
						client->actual_lengths[i],
						1e06*to_seconds(
						client->actual_rtts[i]));
				client->actual_rtts[i] = 0;
			}
			i++;
			if (i >= client->actual_rtts.size())
				i = 0;
			if (i == start)
				break;
		}
	}
	fclose(f);
	return 1;
}

/**
 * log_cmd() - Parse the arguments for a "log" command and execute it.
 * @words:  Command arguments (including the command name as @words[0]).
 * 
 * Return:  Nonzero means success, zero means there was an error.
 */
int log_cmd(std::vector<string> &words)
{
	for (unsigned i = 1; i < words.size(); i++) {
		const char *option = words[i].c_str();
		
		if (strncmp(option, "--", 2) != 0) {
			string message;
			for (unsigned j = i; j < words.size(); j++) {
				if (j != i)
					message.append(" ");
				message.append(words[j]);
			}
			message.append("\n");
			log(NORMAL, "%s", message.c_str());
			return 1;
		}

		if (strcmp(option, "--file") == 0) {
			FILE *f;
			if ((i + 1) >= words.size()) {
				printf("No value provided for %s\n",
						option);
				return 0;
			}
			const char *name = words[i+1].c_str();
			if (strcmp(name, "-") == 0)
				f = stdout;
			else {
				f = fopen(name, "w");
				if (f == NULL) {
					printf("Couldn't open %s: %s\n", name,
							strerror(errno));
					return 0;
				}
				setlinebuf(f);
			}
			if (log_file != stdout)
				fclose(log_file);
			log_file = f;
			i++;
		} else if (strcmp(option, "--level") == 0) {
			if ((i + 1) >= words.size()) {
				printf("No value provided for %s\n",
						option);
				return 0;
			}
			if (words[i+1].compare("normal") == 0)
				log_level = NORMAL;
			else if (words[i+1].compare("verbose") == 0)
				log_level = VERBOSE;
			else {
				printf("Unknown log level '%s'; must be "
						"normal or verbose\n",
						words[i+1].c_str());
				return 0;
			}
			log(NORMAL, "Log level is now %s\n",
					words[i+1].c_str());
			i++;
		} else {
			printf("Unknown option '%s'\n", option);
			return 0;
		}
	}
	return 1;
}

/**
 * server_cmd() - Parse the arguments for a "server" command and execute it.
 * @words:  Command arguments (including the command name as @words[0]).
 * 
 * Return:  Nonzero means success, zero means there was an error.
 */
int server_cmd(std::vector<string> &words)
{
	first_port = 4000;
        protocol = "homa";
	port_threads = 1;
	server_ports = 1;
	
	for (unsigned i = 1; i < words.size(); i++) {
		const char *option = words[i].c_str();

		if (strcmp(option, "--first-port") == 0) {
			if (!parse_int(words, i+1, &first_port, option))
				return 0;
			i++;
		} else if (strcmp(option, "--port-threads") == 0) {
			if (!parse_int(words, i+1, &port_threads, option))
				return 0;
			i++;
		} else if (strcmp(option, "--ports") == 0) {
			if (!parse_int(words, i+1, &server_ports, option))
				return 0;
			i++;
		} else if (strcmp(option, "--protocol") == 0) {
			if ((i + 1) >= words.size()) {
				printf("No value provided for %s\n",
						option);
				return 0;
			}
			protocol = words[i+1].c_str();
			i++;
		} else {
			printf("Unknown option '%s'\n", option);
			return 0;
		}
	}

	if (strcmp(protocol, "homa") == 0) {
		for (int i = 0; i < server_ports; i++) {
			struct sockaddr_in addr_in;
			int fd, j, port;

			fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_HOMA);
			if (fd < 0) {
				log(NORMAL, "FATAL: couldn't open Homa socket: "
						"%s\n",
						strerror(errno));
				exit(1);
			}

			port = first_port + i;
			memset(&addr_in, 0, sizeof(addr_in));
			addr_in.sin_family = AF_INET;
			addr_in.sin_port = htons(port);
			if (bind(fd, (struct sockaddr *) &addr_in,
					sizeof(addr_in)) != 0) {
				log(NORMAL, "FATAL: couldn't bind socket "
						"to Homa port %d: %s\n", port,
						strerror(errno));
				exit(1);
			}
			log(NORMAL, "Successfully bound to Homa port %d\n",
					port);
			for (j = 0; j < port_threads; j++) {
				homa_server *server = new homa_server(
						fd, homa_servers.size());
				homa_servers.push_back(server);
				metrics.push_back(&server->metrics);
			}
		}
	} else {
		for (int i = 0; i < server_ports; i++) {
			tcp_server *server = new tcp_server(first_port + i,
					i, port_threads);
			tcp_servers.push_back(server);
			metrics.push_back(&server->metrics);
		}
	}
	last_per_server_rpcs.resize(server_ports*port_threads, 0);
	last_stats_time = 0;
	return 1;
}

/**
 * stop_cmd() - Parse the arguments for a "stop" command and execute it.
 * @words:  Command arguments (including the command name as @words[0]).
 * 
 * Return:  Nonzero means success, zero means there was an error.
 */
int stop_cmd(std::vector<string> &words)
{	
	for (unsigned i = 1; i < words.size(); i++) {
		const char *option = words[i].c_str();
		if (strcmp(option, "clients") == 0) {
			for (client *client: clients)
				delete client;
			clients.clear();
		} else if (strcmp(option, "senders") == 0) {
			for (client *client: clients)
				client->stop_sender();
		} else if (strcmp(option, "servers") == 0) {
			for (homa_server *server: homa_servers)
				delete server;
			homa_servers.clear();
			for (tcp_server *server: tcp_servers)
				delete server;
			tcp_servers.clear();
			last_per_server_rpcs.clear();
			metrics.clear();
		} else {
			printf("Unknown option '%s'; must be clients, senders, "
				"or servers\n", option);
			return 0;
		}
	}
	return 1;
}

/**
 * tt_cmd() - Parse the arguments for a "tt" command and execute it.
 * @words:  Command arguments (including the command name as @words[0]).
 * 
 * Return:  Nonzero means success, zero means there was an error.
 */
int tt_cmd(std::vector<string> &words)
{	
	const char *option = words[1].c_str();
	if (strcmp(option, "print") == 0) {
		if (words.size() < 3) {
			printf("No file name provided for %s\n", option);
			return 0;
		}
		int error = time_trace::print_to_file(words[2].c_str());
		if (error) {
			printf("Couldn't open time trace file '%s': %s",
				words[2].c_str(), strerror(error));
			return 0;
		}
	} else {
		printf("Unknown option '%s'; must be print\n", option);
		return 0;
	}
	return 1;
}

/**
 * exec_words() - Given a command that has been parsed into words,
 * execute the command corresponding to the words.
 * @words:  Each entry represents one word of the command, like argc/argv.
 * 
 * Return:  Nonzero means success, zero means there was an error. 
 */
int exec_words(std::vector<string> &words)
{
	std::lock_guard<std::mutex> lock(cmd_lock);
	if (words.size() == 0)
		return 1;
	if (words[0].compare("client") == 0) {
		return client_cmd(words);
	} else if (words[0].compare("dump_times") == 0) {
		return dump_times_cmd(words);
	} else if (words[0].compare("log") == 0) {
		return log_cmd(words);
	} else if (words[0].compare("exit") == 0) {
		if (log_file != stdout)
			log(NORMAL, "cp_node exiting (exit command)\n");
		exit(0);
	} else if (words[0].compare("server") == 0) {
		return server_cmd(words);
	} else if (words[0].compare("stop") == 0) {
		return stop_cmd(words);
	} else if (words[0].compare("tt") == 0) {
		return tt_cmd(words);
	} else {
		printf("Unknown command '%s'\n", words[0].c_str());
		return 0;
	}
}

/**
 * exec_string() - Given a string, parse it into words and execute the
 * resulting command.
 * @cmd:  Command to execute.
 */
void exec_string(const char *cmd)
{
	const char *p = cmd;
	std::vector<string> words;
	
	if (log_file != stdout)
		log(NORMAL, "Command: %s\n", cmd);
	
	while (1) {
		int word_length = strcspn(p, " \t\n");
		if (word_length > 0)
			words.emplace_back(p, word_length);
		p += word_length;
		if (*p == 0)
			break;
		p++;
	}
	exec_words(words);
}

/**
 * error_handler() - This method is invoked after a terminal error such
 * as a segfault; it logs a backtrace and exits.
 * @signal    Signal number that caused this method to be invoked.
 * @info      Details about the cause of the signal; used to find the
 *            faulting address for segfaults.
 * @ucontext  CPU context at the time the signal occurred.
 */
void error_handler(int signal, siginfo_t* info, void* ucontext)
{
	ucontext_t* uc = static_cast<ucontext_t*>(ucontext);
	void* caller_address = reinterpret_cast<void*>(
			uc->uc_mcontext.gregs[REG_RIP]);

	log(NORMAL, "Signal %d (%s) at address %p from %p\n",
			signal, strsignal(signal), info->si_addr,
			caller_address);

	const int max_frames = 128;
	void* return_addresses[max_frames];
	int frames = backtrace(return_addresses, max_frames);

	// Overwrite sigaction with caller's address.
	return_addresses[1] = caller_address;

	char** symbols = backtrace_symbols(return_addresses, frames);
	if (symbols == NULL) {
		/* If the malloc failed we might be able to get the backtrace out
		 * to stderr still.
		 */
		log(NORMAL, "backtrace_symbols failed; trying "
				"backtrace_symbols_fd\n");
		backtrace_symbols_fd(return_addresses, frames, 2);
		return;
	}

	log(NORMAL, "Backtrace:\n");
	for (int i = 1; i < frames; ++i)
		log(NORMAL, "%s\n", symbols[i]);
	fflush(log_file);
	while(1) {}

	/* Use abort, rather than exit, to dump core/trap in gdb. */
	abort();
}

int main(int argc, char** argv)
{
	setlinebuf(stdout);
	signal(SIGPIPE, SIG_IGN);
	struct rlimit limits;
	if (getrlimit(RLIMIT_NOFILE, &limits) != 0) {
		log(NORMAL, "FATAL: couldn't read file descriptor limits: "
				"%s\n", strerror(errno));
		exit(1);
	}
	limits.rlim_cur = limits.rlim_max;
	if (setrlimit(RLIMIT_NOFILE, &limits) != 0) {
		log(NORMAL, "FATAL: couldn't increase file descriptor limit: "
				"%s\n", strerror(errno));
		exit(1);
	}
	struct sigaction action;
	action.sa_sigaction = error_handler;
	action.sa_flags = SA_RESTART | SA_SIGINFO;
	if (sigaction(SIGSEGV, &action, NULL) != 0)
		log(VERBOSE, "Couldn't set signal handler for SIGSEGV; "
				"continuing anyway\n");

	if ((argc >= 2) && (strcmp(argv[1], "--help") == 0)) {
		print_help(argv[0]);
		exit(0);
	}
	
	if (argc > 1) {
		std::vector<string> words;
		for (int i = 1; i < argc; i++)
			words.emplace_back(argv[i]);
		if (!exec_words(words))
			exit(1);
		
		/* Instead of going interactive, just print stats.
		 * every second.
		 */
		log_stats();
	}
	
//	cpu_set_t cores;
//	CPU_ZERO(&cores);
//	for (int i = 2; i < 18; i++)
//		CPU_SET(i, &cores);
//	if (sched_setaffinity(0, sizeof(cores), &cores) != 0)
//		log(NORMAL, "ERROR: couldn't set core affinity: %s\n",
//				strerror(errno));
	
	std::thread logger(log_stats);
	while (1) {
		string line;
		
		printf("%% ");
		fflush(stdout);
		if (!std::getline(std::cin, line)) {
			if (log_file != stdout)
				log(NORMAL, "cp_node exiting (EOF on stdin)\n");
			exit(0);
		}
		exec_string(line.c_str());
	}
}
