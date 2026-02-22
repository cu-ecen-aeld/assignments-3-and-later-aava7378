#define _GNU_SOURCE

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <syslog.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <time.h>

#define USE_AESD_CHAR_DEVICE 0

#if USE_AESD_CHAR_DEVICE
#define DATAFILE "/dev/aesdchar"
#else
#define DATAFILE "/var/tmp/aesdsocketdata"
#endif

#define PORT "9000"
#define BACKLOG 10
#define TIMESTAMP_PERIOD_SEC 10


static volatile sig_atomic_t g_exit_requested = 0;

static pthread_mutex_t g_file_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_list_mutex = PTHREAD_MUTEX_INITIALIZER;

struct thread_node {
	pthread_t thread;
	int client_fd;
	bool completed;
	struct thread_node *next;
};

static struct thread_node *g_threads = NULL;

static int g_listen_fd = -1;

static void handle_signal(int sig)
{
	(void)sig;
	g_exit_requested = 1;

	if (g_listen_fd != -1)
	{
		shutdown(g_listen_fd, SHUT_RDWR);
		close(g_listen_fd);
		g_listen_fd = -1;
	}
}

static void daemonize(void)
{
    	pid_t pid = fork();
    	if (pid < 0) 
	{
		exit(EXIT_FAILURE);
	}

    	if (pid > 0)
	{
		exit(EXIT_SUCCESS);
	}

    	if (setsid() < 0)
	{ 
		exit(EXIT_FAILURE);
	}

    	pid = fork();
    	if (pid < 0)
	{ 
		exit(EXIT_FAILURE);
    	}
	
	if (pid > 0) 
	{
		exit(EXIT_SUCCESS);
	}
    
	umask(0);
    	
	if (chdir("/") != 0) 
	{
		syslog(LOG_ERR, "chdir(\"/\") failed: %s", strerror(errno));
	}

    	int fd = open("/dev/null", O_RDWR);
    
	if (fd >= 0) 
	{
        	dup2(fd, STDIN_FILENO);
        	dup2(fd, STDOUT_FILENO);
        	dup2(fd, STDERR_FILENO);
	}        
	
	if (fd > STDERR_FILENO){ close(fd); }
}

static int setup_listen_socket(void)
{
	struct addrinfo hints;
	struct addrinfo *outputs = NULL, *rp = NULL;
	int sfd = -1;
	int rc;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family   = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags    = AI_PASSIVE;

	rc = getaddrinfo(NULL, PORT, &hints, &outputs);
	if (rc != 0)
	{
		syslog(LOG_ERR, "getaddrinfo failed: %s", gai_strerror(rc));
		return -1;
	}
	for (rp = outputs; rp != NULL; rp = rp->ai_next)
	{
		sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (sfd == -1)
		{
			continue;
		}
		
		int optval = 1;
		setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
		
		if (bind(sfd, rp->ai_addr, rp->ai_addrlen) == 0)
		{
			break;
		}

		close(sfd);
		sfd = -1;
	}
	freeaddrinfo(outputs);

	if(sfd == -1)
	{
		 syslog(LOG_ERR, "Failed to bind to port %s", PORT);
		return -1;
	}

	if (listen(sfd, BACKLOG) != 0)
	{
		syslog(LOG_ERR, "listen failed: %s", strerror(errno));
		close(sfd);
		return -1;
	}
	
	return sfd;

}

static ssize_t send_all(int fd, const void *buf, size_t len)
{
    	const uint8_t *p = (const uint8_t *)buf;
    	size_t total = 0;
    	while (total < len) 
	{
        	ssize_t n = send(fd, p + total, len - total, 0);
        	if (n < 0) 
		{
            		if (errno == EINTR){  continue; }
            		return -1;
        	}
        	
		if (n == 0) break;
        	total += (size_t)n;
    	}
    	
	return (ssize_t)total;
}

static int append_and_echo_file_locked(int client_fd, const uint8_t *data, size_t data_len)
{
    	int fd = open(DATAFILE, O_CREAT | O_RDWR | O_APPEND, 0644);
    	if (fd < 0) 
	{
        	syslog(LOG_ERR, "open(%s) failed: %s", DATAFILE, strerror(errno));
        	return -1;
    	}

    	ssize_t w = write(fd, data, data_len);
    	if (w < 0 || (size_t)w != data_len) 
	{
        	syslog(LOG_ERR, "write failed: %s", strerror(errno));
        	close(fd);
        	return -1;
    	}

    	if (lseek(fd, 0, SEEK_SET) < 0) 
	{
        	syslog(LOG_ERR, "lseek failed: %s", strerror(errno));
        	close(fd);
        	return -1;
    	}

    	uint8_t buf[4096];
    	for (;;) 
	{
        	ssize_t r = read(fd, buf, sizeof(buf));
        	if (r < 0) 
		{
            		if (errno == EINTR){ continue; }
            		syslog(LOG_ERR, "read failed: %s", strerror(errno));
            		close(fd);
            		return -1;
        	}

        	if (r == 0){ break; }
        	if (send_all(client_fd, buf, (size_t)r) < 0) 
		{
            		syslog(LOG_ERR, "send failed: %s", strerror(errno));
            		close(fd);
            		return -1;
        	}
    	}

    	close(fd);
    	return 0;
}

static void list_add_node(struct thread_node *node)
{
    	pthread_mutex_lock(&g_list_mutex);
    	node->next = g_threads;
    	g_threads = node;
    	pthread_mutex_unlock(&g_list_mutex);
}

static void join_and_cleanup_completed_threads(void)
{
    	pthread_mutex_lock(&g_list_mutex);

    	struct thread_node *prev = NULL;
    	struct thread_node *cur = g_threads;

    	while (cur) 
	{
        	if (cur->completed) 
		{
            		pthread_t tid = cur->thread;

            		struct thread_node *to_free = cur;
            		if (prev)
			{
				 prev->next = cur->next;
			}            
			else 
			{
				g_threads = cur->next;
			}

            		cur = cur->next;

            		pthread_mutex_unlock(&g_list_mutex);
            		pthread_join(tid, NULL);
            		free(to_free);
            		pthread_mutex_lock(&g_list_mutex);

            		continue;
        	}

        	prev = cur;
        	cur = cur->next;
    	}

    	pthread_mutex_unlock(&g_list_mutex);
}

static void request_all_threads_exit_and_join(void)
{
    	pthread_mutex_lock(&g_list_mutex);
    	for (struct thread_node *n = g_threads; n; n = n->next) 
	{
        	if (n->client_fd != -1) 
		{
            		shutdown(n->client_fd, SHUT_RDWR);
        	}
    	}

    	pthread_mutex_unlock(&g_list_mutex);

    	pthread_mutex_lock(&g_list_mutex);
    	struct thread_node *cur = g_threads;
    	g_threads = NULL;
    	pthread_mutex_unlock(&g_list_mutex);

    	while (cur) 
	{
        	struct thread_node *next = cur->next;
        	pthread_join(cur->thread, NULL);
        	free(cur);
        	cur = next;
    	}
}

static void *client_thread_fn(void *arg)
{
    	struct thread_node *node = (struct thread_node *)arg;
    	const int cfd = node->client_fd;

    	uint8_t *accum = NULL;
    	size_t accum_len = 0;

    	for (;;) 
	{
        	if (g_exit_requested) break;

        	uint8_t buf[1024];
        	ssize_t r = recv(cfd, buf, sizeof(buf), 0);
        	if (r < 0) 
		{
            		if (errno == EINTR) continue;
            		syslog(LOG_ERR, "recv failed: %s", strerror(errno));
            		break;
        	}

        	if (r == 0) 
		{
            		break;
        	}

        	uint8_t *newbuf = realloc(accum, accum_len + (size_t)r);
        	if (!newbuf) 
		{
            		syslog(LOG_ERR, "realloc failed");
            		break;
        	}

        	accum = newbuf;
        	memcpy(accum + accum_len, buf, (size_t)r);
        	accum_len += (size_t)r;

        	for (;;) 
		{
            		void *nl = memchr(accum, '\n', accum_len);
            		if (!nl) break;

            		size_t line_len = (uint8_t *)nl - accum + 1;

            		pthread_mutex_lock(&g_file_mutex);
            		int rc = append_and_echo_file_locked(cfd, accum, line_len);
            		pthread_mutex_unlock(&g_file_mutex);

            		if (rc != 0) 
			{
                		goto done;
            		}

            		size_t remaining = accum_len - line_len;
            		memmove(accum, accum + line_len, remaining);
            		accum_len = remaining;

            		uint8_t *shrunk = realloc(accum, accum_len);
            		if (shrunk || accum_len == 0) 
			{
                		accum = shrunk;
            		}
        	}
    	}

	done:
    		free(accum);
    		if (node->client_fd != -1) 
		{
        		close(node->client_fd);
        		node->client_fd = -1;
    		}

    		node->completed = true;
    		return NULL;
}

static void *timestamp_thread_fn(void *arg)
{
    	(void)arg;

    	while (!g_exit_requested) 
	{
        	for (int i = 0; i < TIMESTAMP_PERIOD_SEC; i++) 
		{
            		if (g_exit_requested) break;
            		sleep(1);
        	}
        	if (g_exit_requested) break;

        	time_t now = time(NULL);
        	struct tm tm_now;
        	localtime_r(&now, &tm_now);

        	char tbuf[128];
        	strftime(tbuf, sizeof(tbuf), "%a, %d %b %Y %T %z", &tm_now);

        	char line[160];
        	int n = snprintf(line, sizeof(line), "timestamp:%s\n", tbuf);
        	if (n < 0) continue;

        	pthread_mutex_lock(&g_file_mutex);
        	int fd = open(DATAFILE, O_CREAT | O_WRONLY | O_APPEND, 0644);
        	if (fd >= 0) 
		{
            		ssize_t w = write(fd, line, (size_t)n);
            		(void)w;
            		close(fd);
        	} 
		else 
		{
            		syslog(LOG_ERR, "timestamp open failed: %s", strerror(errno));
        	}
        	pthread_mutex_unlock(&g_file_mutex);
    	}
    	return NULL;
}



int main(int argc, char *argv[])
{
    	openlog("aesdsocket", LOG_PID, LOG_USER);

    	bool run_as_daemon = false;
    	if (argc == 2 && strcmp(argv[1], "-d") == 0) 
	{
        	run_as_daemon = true;
    	}
	else if (argc > 1) 
	{
        	syslog(LOG_ERR, "Usage: %s [-d]", argv[0]);
        	closelog();
        	return EXIT_FAILURE;
    	}

	#if !USE_AESD_CHAR_DEVICE
    		unlink(DATAFILE);
	#endif

    	struct sigaction sa;
    	memset(&sa, 0, sizeof(sa));
    	sa.sa_handler = handle_signal;
    	sigemptyset(&sa.sa_mask);
    	sigaction(SIGINT, &sa, NULL);
    	sigaction(SIGTERM, &sa, NULL);

    	if (run_as_daemon) 
	{
        	daemonize();
    	}

    	g_listen_fd = setup_listen_socket();
    	
	if (g_listen_fd < 0) 
	{
        	syslog(LOG_ERR, "Failed to setup listen socket");
        	closelog();
        	return EXIT_FAILURE;
    	}


    	pthread_t ts_thread;
    	if (pthread_create(&ts_thread, NULL, timestamp_thread_fn, NULL) != 0) 
	{
        	syslog(LOG_ERR, "Failed to create timestamp thread");
        	close(g_listen_fd);
        	g_listen_fd = -1;
        	closelog();
        	return EXIT_FAILURE;
    	}

    	syslog(LOG_INFO, "Server listening on port %s", PORT);

    	while (!g_exit_requested) 
	{
        	struct sockaddr_in client_addr;
        	socklen_t client_len = sizeof(client_addr);
        	int cfd = accept(g_listen_fd, (struct sockaddr *)&client_addr, &client_len);
        	if (cfd < 0) 
		{
            		if (errno == EINTR) continue;
            		if (g_exit_requested) break;
            		syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            		continue;
        	}

        	char addrbuf[INET_ADDRSTRLEN];
        	inet_ntop(AF_INET, &client_addr.sin_addr, addrbuf, sizeof(addrbuf));
        	syslog(LOG_INFO, "Accepted connection from %s", addrbuf);

        	struct thread_node *node = calloc(1, sizeof(*node));
        	if (!node) 
		{
            		syslog(LOG_ERR, "calloc failed for thread node");
            		close(cfd);
            		continue;
        	}
        
		node->client_fd = cfd;
        	node->completed = false;
        	node->next = NULL;

        	list_add_node(node);

        	int rc = pthread_create(&node->thread, NULL, client_thread_fn, node);
        	if (rc != 0) 
		{
            		syslog(LOG_ERR, "pthread_create failed: %s", strerror(rc));
            		close(cfd);
            		node->client_fd = -1;
            		node->completed = true;
        	}

        	join_and_cleanup_completed_threads();
    	}

    	syslog(LOG_INFO, "Shutdown requested, cleaning up...");

    
    	pthread_join(ts_thread, NULL);

    	request_all_threads_exit_and_join();

    	if (g_listen_fd != -1) 
	{
        	close(g_listen_fd);
        	g_listen_fd = -1;
    	}

	#if !USE_AESD_CHAR_DEVICE
    		unlink(DATAFILE);
	#endif

    	closelog();
    	return EXIT_SUCCESS;
}

