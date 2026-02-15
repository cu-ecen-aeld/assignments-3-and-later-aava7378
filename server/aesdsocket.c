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


#define PORT "9000"
#define OUTFILE "/var/tmp/aesdsocketdata"
#define BACKLOG 10
#define RECV_CHUNK 1024


static volatile sig_atomic_t g_exit_requested = 0;
static int g_listen_fd = -1;

static void handle_signal(int sig)
{
	(void)sig;
	g_exit_requested = 1;
}

static int setup_listen_socket(void)
{
	struct addrinfo hints;
	struct addrinfo *outputs = NULL, *rp = NULL;
	int sfd = -1;
	int yes = 1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family   = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags    = AI_PASSIVE;

	int rc = getaddrinfo(NULL, PORT, &hints, &outputs);
	if (rc != 0)
	{
		syslog(LOG_ERR, "getaddrinfo failed: %s", gai_strerror(rc));
		return -1;
	}
	for (rp = outputs; rp != NULL; rp = rp->ai_next)
	{
		sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (sfd < 0)
		{
			continue;
		}
		
		if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0)
		{
			close(sfd);
			sfd = -1;
			continue;
		}
		
		if (bind(sfd, rp->ai_addr, rp->ai_addrlen) == 0)
		{
			break;
		}

		close(sfd);
		sfd = -1;
	}
	freeaddrinfo(outputs);

	if(sfd < 0)
	{
		 syslog(LOG_ERR, "Failed to bind to port %s", PORT);
		return -1;
	}

	if (listen(sfd, BACKLOG) < 0)
	{
		syslog(LOG_ERR, "listen failed: %s", strerror(errno));
		close(sfd);
		return -1;
	}
	
	return sfd;

}

static int daemonize_after_bind(void)
{
	pid_t pid = fork();
	if (pid < 0)
	{
		syslog(LOG_ERR, "fork failed: %s", strerror(errno));
		return -1;
	}

	if (pid > 0)
	{
		exit(0);
	}

	if (setsid() < 0)
	{
		syslog(LOG_ERR, "setsid failed: %s", strerror(errno));
		return -1;
	}

	pid = fork();
	if (pid < 0)
	{
		syslog(LOG_ERR, "second fork failed: %s", strerror(errno));
		return -1;
	}
	
	if (pid > 0)
	{
		exit(0);
	}

	umask(0);

	if (chdir("/") < 0)
	{
		syslog(LOG_ERR, "chdir failed: %s", strerror(errno));
		return -1;
	}

	int fd = open("/dev/null", O_RDWR);
        if (fd >= 0)
	{
		(void)dup2(fd, STDIN_FILENO);
                (void)dup2(fd, STDOUT_FILENO);
                (void)dup2(fd, STDERR_FILENO);
		
		if (fd > STDERR_FILENO)
		{
			close(fd);
		}
	}
	return 0;
}

static int append_packet_to_file(const char *buf, size_t len)
{
	int fd = open(OUTFILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd < 0)
	{
		syslog(LOG_ERR, "open(%s) for append failed: %s", OUTFILE, strerror(errno));
		return -1;
	}
	
	size_t off = 0;
	while (off < len)
	{
		ssize_t w = write(fd, buf + off, len - off);
		if (w < 0)
		{
			syslog(LOG_ERR, "write to %s failed: %s", OUTFILE, strerror(errno));
			close(fd);
			return -1;
		}
		off = off + (size_t)w;
	}
	close(fd);
	return 0;
}

static int send_entire_file(int client_fd)
{
	int fd = open(OUTFILE, O_RDONLY);
	
	if (fd < 0)
	{
		if (errno == ENOENT)
		{
			return 0;
		}
		
		syslog(LOG_ERR, "open(%s) for read failed: %s", OUTFILE, strerror(errno));
		return -1;
	}

	char buf[4096];
	while(1)
	{
		ssize_t r = read(fd, buf, sizeof(buf));
		if (r < 0)
		{
			syslog(LOG_ERR, "read(%s) failed: %s", OUTFILE, strerror(errno));
			close(fd);
			return -1;
		}
		
		if (r == 0) { break; }
		
		size_t off = 0;
		while (off < (size_t)r)
		{
			ssize_t s = send(client_fd, buf + off, (size_t)r - off, 0);
			if (s < 0)
			{
				syslog(LOG_ERR, "send failed: %s", strerror(errno));
				close(fd);
				return -1;
			}
			
			off = off + (size_t)s;
		}
	}
	close(fd);
	return 0;
}



int main(int argc, char *argv[])
{
	bool daemon_mode = false;
	if (argc == 2 && strcmp(argv[1], "-d") == 0)
	{
		daemon_mode = true;
	}
	else if (argc != 1)
	{
		fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
		return -1;
	}

	openlog("aesdsocket", LOG_PID, LOG_USER);

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_signal;
	sigemptyset(&sa.sa_mask);
	
	if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0)
	{
		syslog(LOG_ERR, "sigaction failed: %s", strerror(errno));
		closelog();
		return -1;
	}
	
	g_listen_fd = setup_listen_socket();
	if (g_listen_fd < 0)
	{
		closelog();
		return -1;
	}
	
	if (daemon_mode)
	{
		if (daemonize_after_bind() < 0)
		{	
			close(g_listen_fd);
			closelog();
			return -1;
		}
	}

	while (!g_exit_requested)
	{
		struct sockaddr_in client_addr;
		socklen_t client_len = sizeof(client_addr);

		int cfd = accept(g_listen_fd, (struct sockaddr *)&client_addr, &client_len);
		
		if (cfd < 0)
		{
			if (errno == EINTR && g_exit_requested) { break; }
			syslog(LOG_ERR, "accept failed: %s", strerror(errno));
			continue;
		}

		char ipstr[INET_ADDRSTRLEN] = {0};
		inet_ntop(AF_INET, &client_addr.sin_addr, ipstr, sizeof(ipstr));
		syslog(LOG_INFO, "Accepted connection from %s", ipstr);
		char *packet = NULL;
		size_t packet_sz = 0;

		while (!g_exit_requested)
		{
			char tmp[RECV_CHUNK];
			ssize_t r = recv(cfd, tmp, sizeof(tmp), 0);
			if (r < 0)
			{
				if (errno == EINTR && g_exit_requested) { break; }
				syslog(LOG_ERR, "recv failed: %s", strerror(errno));
				break;
			}

			if (r == 0) { break; }
			
			char *newbuf = realloc(packet, packet_sz + (size_t)r);
			if (!newbuf)
			{
				syslog(LOG_ERR, "realloc failed");
				free(packet);
				packet = NULL;
				packet_sz = 0;
				break;
			}
			
			packet = newbuf;
			memcpy(packet + packet_sz, tmp, (size_t)r);
			packet_sz += (size_t)r;

			size_t start = 0;
			for (size_t i = 0; i < packet_sz; i++)
			{
				if (packet[i] == '\n')
				{
					size_t pkt_len = i - start + 1;
					if (append_packet_to_file(packet + start, pkt_len) == 0)
					{
						(void)send_entire_file(cfd);
					}
					start = i + 1;
				}
			}
			
			if (start > 0)
			{
				size_t leftover = packet_sz - start;
				if (leftover > 0)
				{
					memmove(packet, packet + start, leftover);
				}
				
				packet_sz = leftover;
				char *shrunk = realloc(packet, packet_sz);
				if (shrunk || packet_sz == 0)
				{
					packet = shrunk;
				}
			}
		}
		
		free(packet);
		
		shutdown(cfd, SHUT_RDWR);
		close(cfd);

		syslog(LOG_INFO, "Closed connection from %s", ipstr);
	}
	
	syslog(LOG_INFO, "Caught signal, exiting");
	if (g_listen_fd >= 0)
	{
		close(g_listen_fd);
		g_listen_fd = -1;
	}
	if (unlink(OUTFILE) < 0 && errno != ENOENT)
	{
		syslog(LOG_ERR, "unlink(%s) failed: %s", OUTFILE, strerror(errno));
	}

	closelog();
	return 0;
}
