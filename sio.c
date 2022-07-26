#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <setjmp.h>
#include <signal.h>
#include <dirent.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/vmd.h"
#include "spdk/nvme_zns.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/log.h"

typedef struct sockaddr SA;

#define RIO_BUFSIZE 8192
typedef struct {
    int rio_fd;
    int rio_cnt;
    char *rio_bufptr;
    char rio_buf[RIO_BUFSIZE];
} rio_t;

struct ctrlr_entry {
	struct spdk_nvme_ctrlr		*ctrlr;
	TAILQ_ENTRY(ctrlr_entry)	link;
	char				        name[1024];
};

struct ns_entry {
	struct spdk_nvme_ctrlr	*ctrlr;
	struct spdk_nvme_ns  	*ns;
	TAILQ_ENTRY(ns_entry)	link;
	struct spdk_nvme_qpair	*qpair;
} *ns_entry;

static TAILQ_HEAD(, ctrlr_entry) g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);
static TAILQ_HEAD(, ns_entry) g_namespaces = TAILQ_HEAD_INITIALIZER(g_namespaces);
static struct spdk_nvme_transport_id g_trid = {};

static bool g_vmd = false;

static void register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns) {
	struct ns_entry *entry;
	if (!spdk_nvme_ns_is_active(ns)) {
		return;
	}
	entry = malloc(sizeof(struct ns_entry));
	if (entry == NULL) {
		perror("ns_entry malloc");
		exit(1);
	}
	entry->ctrlr = ctrlr;
	entry->ns = ns;
	TAILQ_INSERT_TAIL(&g_namespaces, entry, link);
	printf("  Namespace ID: %d size: %juGB\n", spdk_nvme_ns_get_id(ns), spdk_nvme_ns_get_size(ns) / 1000000000);
}

struct hello_world_sequence {
	struct ns_entry	*ns_entry;
	char		    *buf;
	unsigned        using_cmb_io;
	int		        is_completed;
} sequence;

static void read_complete(void *arg, const struct spdk_nvme_cpl *completion) {
	struct hello_world_sequence *sequence = arg;
	sequence->is_completed = 1;
	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(sequence->ns_entry->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Read I/O failed, aborting run\n");
		sequence->is_completed = 2;
		exit(1);
	}
}

static void write_complete(void *arg, const struct spdk_nvme_cpl *completion) {
	struct hello_world_sequence	*sequence = arg;
	sequence->is_completed = 1;
	struct ns_entry *ns_entry = sequence->ns_entry;
	int rc;
	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(sequence->ns_entry->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Write I/O failed, aborting run\n");
		sequence->is_completed = 2;
		exit(1);
	}
	if (sequence->using_cmb_io) {
		spdk_nvme_ctrlr_unmap_cmb(ns_entry->ctrlr);
	} else {
		spdk_free(sequence->buf);
	}
}

static void reset_zone_complete(void *arg, const struct spdk_nvme_cpl *completion) {
	struct hello_world_sequence *sequence = arg;
	sequence->is_completed = 1;
	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(sequence->ns_entry->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Reset zone I/O failed, aborting run\n");
		sequence->is_completed = 2;
		exit(1);
	}
}

static void reset_zone_and_wait_for_completion(struct hello_world_sequence *sequence) {
	if (spdk_nvme_zns_reset_zone(sequence->ns_entry->ns, sequence->ns_entry->qpair, 0, false, reset_zone_complete, sequence)) {
		fprintf(stderr, "starting reset zone I/O failed\n");
		exit(1);
	}
	while (!sequence->is_completed) {
		spdk_nvme_qpair_process_completions(sequence->ns_entry->qpair, 0);
	}
	sequence->is_completed = 0;
}

static void hello_world(void) {
	int rc;
	size_t sz;
	TAILQ_FOREACH(ns_entry, &g_namespaces, link) {
		ns_entry->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, NULL, 0);
		if (ns_entry->qpair == NULL) {
			printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
			return;
		}
		sequence.using_cmb_io = 1;
		sequence.buf = spdk_nvme_ctrlr_map_cmb(ns_entry->ctrlr, &sz);
		if (sequence.buf == NULL || sz < 0x1000) {
			sequence.using_cmb_io = 0;
			sequence.buf = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
		}
		if (sequence.buf == NULL) {
			printf("ERROR: write buffer allocation failed\n");
			return;
		}
		if (sequence.using_cmb_io) {
			printf("INFO: using controller memory buffer for IO\n");
		} else {
			printf("INFO: using host memory buffer for IO\n");
		}
		sequence.is_completed = 0;
		sequence.ns_entry = ns_entry;
		if (spdk_nvme_ns_get_csi(ns_entry->ns) == SPDK_NVME_CSI_ZNS) {
			reset_zone_and_wait_for_completion(&sequence);
		}
	}
}

static bool probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid, struct spdk_nvme_ctrlr_opts *opts) {
	printf("Attaching to %s\n", trid->traddr);
	return true;
}

static void attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid, struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts) {
	int nsid;
	struct ctrlr_entry *entry;
	struct spdk_nvme_ns *ns;
	const struct spdk_nvme_ctrlr_data *cdata;
	entry = malloc(sizeof(struct ctrlr_entry));
	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}
	printf("Attached to %s\n", trid->traddr);
	cdata = spdk_nvme_ctrlr_get_data(ctrlr);
	snprintf(entry->name, sizeof(entry->name), "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);
	entry->ctrlr = ctrlr;
	TAILQ_INSERT_TAIL(&g_controllers, entry, link);
	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
	     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (ns == NULL) {
			continue;
		}
		register_ns(ctrlr, ns);
	}
}

static void cleanup(void) {
	struct ns_entry *ns_entry, *tmp_ns_entry;
	struct ctrlr_entry *ctrlr_entry, *tmp_ctrlr_entry;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;
	TAILQ_FOREACH_SAFE(ns_entry, &g_namespaces, link, tmp_ns_entry) {
		TAILQ_REMOVE(&g_namespaces, ns_entry, link);
		free(ns_entry);
	}
	TAILQ_FOREACH_SAFE(ctrlr_entry, &g_controllers, link, tmp_ctrlr_entry) {
		TAILQ_REMOVE(&g_controllers, ctrlr_entry, link);
		spdk_nvme_detach_async(ctrlr_entry->ctrlr, &detach_ctx);
		free(ctrlr_entry);
	}
	if (detach_ctx) {
		spdk_nvme_detach_poll(detach_ctx);
	}
}

int init_spdk() {
	int rc;
	struct spdk_env_opts opts;
	spdk_env_opts_init(&opts);
	spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
	snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);
	opts.name = "hello_world";
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}
	printf("Initializing NVMe Controllers\n");
	if (g_vmd && spdk_vmd_init()) {
		fprintf(stderr, "Failed to initialize VMD."
			" Some NVMe devices can be unavailable.\n");
	}
	rc = spdk_nvme_probe(&g_trid, NULL, probe_cb, attach_cb, NULL);
	if (rc != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		rc = 1;
		goto exit;
	}
	if (TAILQ_EMPTY(&g_controllers)) {
		fprintf(stderr, "no NVMe controllers found\n");
		rc = 1;
		goto exit;
	}
	printf("Initialization complete.\n");
	hello_world();
	return rc;
exit:
	cleanup();
	spdk_env_fini();
	return rc;
}

int open_listenfd(char *port)  {
    struct addrinfo hints, *listp, *p;
    int listenfd, rc, optval=1;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG;
    hints.ai_flags |= AI_NUMERICSERV;
    if ((rc = getaddrinfo(NULL, port, &hints, &listp)) != 0) {
        fprintf(stderr, "getaddrinfo failed (port %s): %s\n", port, gai_strerror(rc));
        return -2;
    }
    for (p = listp; p; p = p->ai_next) {
        if ((listenfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0) 
            continue;
        setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval , sizeof(int));
        if (bind(listenfd, p->ai_addr, p->ai_addrlen) == 0)
            break;
        if (close(listenfd) < 0) {
            fprintf(stderr, "open_listenfd close failed: %s\n", strerror(errno));
            return -1;
        }
    }
    freeaddrinfo(listp);
    if (!p)
        return -1;
    if (listen(listenfd, 1024) < 0) {
        close(listenfd);
	    return -1;
    }
    return listenfd;
}

ssize_t rio_writen(int fd, void *usrbuf, size_t n)  {
    size_t nleft = n;
    ssize_t nwritten;
    char *bufp = usrbuf;
    while (nleft > 0) {
	    if ((nwritten = write(fd, bufp, nleft)) <= 0) {
	        if (errno == EINTR)
		        nwritten = 0;
	        else
		        return -1;
    	}
	    nleft -= nwritten;
	    bufp += nwritten;
    }
    return n;
}

static ssize_t rio_read(rio_t *rp, char *usrbuf, size_t n) {
    int cnt;
    while (rp->rio_cnt <= 0) {
		rp->rio_cnt = read(rp->rio_fd, rp->rio_buf, sizeof(rp->rio_buf));
		if (rp->rio_cnt < 0) {
	    	if (errno != EINTR)
				return -1;
		} else if (rp->rio_cnt == 0)
	    	return 0;
		else 
	    	rp->rio_bufptr = rp->rio_buf;
    }
    cnt = n;          
    if (rp->rio_cnt < n)   
		cnt = rp->rio_cnt;
    memcpy(usrbuf, rp->rio_bufptr, cnt);
    rp->rio_bufptr += cnt;
    rp->rio_cnt -= cnt;
    return cnt;
}

void rio_readinitb(rio_t *rp, int fd) {
    rp->rio_fd = fd;  
    rp->rio_cnt = 0;  
    rp->rio_bufptr = rp->rio_buf;
}

ssize_t rio_readlineb(rio_t *rp, void *usrbuf, size_t maxlen) {
    int n, rc;
    char c, *bufp = usrbuf;
    for (n = 1; n < maxlen; n++) { 
        if ((rc = rio_read(rp, &c, 1)) == 1) {
	    	*bufp++ = c;
	    	if (c == '\n') {
            	n++;
     			break;
       		}
		} else if (rc == 0) {
	    	if (n == 1)
				return 0;
	    	else
				break;
		} else
	    	return -1;
    }
    *bufp = 0;
    return n-1;
}

ssize_t rio_readnb(rio_t *rp, void *usrbuf, size_t n) {
    size_t nleft = n;
    ssize_t nread;
    char *bufp = usrbuf;
    while (nleft > 0) {
		if ((nread = rio_read(rp, bufp, nleft)) < 0) 
        	return -1;
		else if (nread == 0)
		    break;
		nleft -= nread;
		bufp += nread;
    }
    return (n - nleft);
}

void bad_request(int fd) {
    char buf[1024];
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Server: Tiny Web Server\r\n");
    rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n", 4);
    rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: %s\r\n\r\n", "text/plain");
    rio_writen(fd, buf, strlen(buf));   
    sprintf(buf, "aaaa");
    rio_writen(fd, buf, strlen(buf));
}

ssize_t read_requesthdrs(rio_t *rp) {
	int length = -1;
    char buf[8192];
    rio_readlineb(rp, buf, 8192);
    printf("%s", buf);
    while(strcmp(buf, "\r\n")) {
 		if (strstr(buf, "Length:")) {
    		sscanf(buf, "Content-Length: %d", &length);
		}
		rio_readlineb(rp, buf, 8192);
		printf("%s", buf);
    }
	return length == -1 ? 0 : length;
}

void doit(int fd) {
    char buf[8192], method[8192], uri[8192], version[8192];
    rio_t rio;
	int rc;
	int lba;
	int is_write = 0;
	rio_readinitb(&rio, fd);
    if (!rio_readlineb(&rio, buf, 8192))
        return;
    sscanf(buf, "%s %s %s", method, uri, version);
	ssize_t num = read_requesthdrs(&rio);
    if (strcasecmp(method, "GET") && strcasecmp(method, "POST")) {
        bad_request(fd);
        return;
    }
    if (strcasecmp(method, "POST") == 0)
    	is_write = 1;
	if (strstr(uri, "/wondfs?lba=")) {
		sscanf(uri, "/wondfs?lba=%d", &lba);
		printf("lba: %d\n", lba);
	} else {
		bad_request(fd);
        return;
	}
	if (is_write) {
		if (!rio_readnb(&rio, buf, num))
        	return;
		TAILQ_FOREACH(ns_entry, &g_namespaces, link) {
			sequence.is_completed = 0;
			sequence.buf = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
			snprintf(sequence.buf, 0x1000, "%s", buf);
			rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qpair, sequence.buf, lba, 4, write_complete, &sequence, 0);
			if (rc != 0) {
				fprintf(stderr, "starting write I/O failed\n");
				exit(1);
			}
			while (!sequence.is_completed) {
				spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
			}
		}
		return;
	}
	TAILQ_FOREACH(ns_entry, &g_namespaces, link) {
		sequence.is_completed = 0;
		sequence.buf = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
		rc = spdk_nvme_ns_cmd_read(ns_entry->ns, ns_entry->qpair, sequence.buf, lba, 4, read_complete, &sequence, 0);
		if (rc != 0) {
			fprintf(stderr, "starting read I/O failed\n");
			exit(1);
		}
		while (!sequence.is_completed) {
			spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
		}
		printf("%s\n", sequence.buf);
	}
	bad_request(fd);
}

int main(int argc, char **argv) {
	init_spdk();
    int listenfd, connfd;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    if (argc != 2) {
	    fprintf(stderr, "usage: %s <port>\n", argv[0]);
	    exit(1);
    }
    listenfd = open_listenfd(argv[1]);
    while (1) {
	    clientlen = sizeof(clientaddr);
	    connfd = accept(listenfd, (SA *)&clientaddr, &clientlen);
        doit(connfd);
        close(connfd);
    }
}