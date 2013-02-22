#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <libguile.h>

char *mgetline(int sock, char term) {
	static char buf[65536];
	char *pt;
	int n;
	pt = buf;
	n = 0;
	while (n < 65536) {
		if (recv(sock, pt, 1, 0) < 1) {
			*++pt = '\0';
			return buf;
			}
		if (*pt == term) {
			*++pt = '\0';
			return buf;
			}
		pt++;
		n++;
		}
	return buf;
	}

static void parse_line(char *line, int reqline) {
	printf("%s\n", line);
	}

static SCM dispatch(void *data) {
	int size;
	int sock, conn, reqline, eoh;
	SCM lines;
	char *mark, *pt, buf[65536];
	struct sockaddr_in client;
printf("dispatch\n");
	size = sizeof(struct sockaddr_in);
	sock = *((int *)data);
	conn = accept(sock, (struct sockaddr *)&client, &size);
	printf("I got a connection from (%s , %d)\n",
                   inet_ntoa(client.sin_addr),ntohs(client.sin_port));
	lines = SCM_EOL;
	reqline = 1;
	eoh = 0;
	while (!eoh) {
		if (recv(conn, buf, 65536, 0) < 1) {
			printf("bad recv\n");
			break;
			}
		mark = buf;
		while ((pt = index(mark, '\n')) != NULL) {
			*pt = '\0';
			if (*(pt - 1) == '\r') *(pt - 1) = '\0';
			if (strlen(mark) == 0) {
				eoh = 1;
				break;
				}
			parse_line(mark, reqline);
			reqline = 0;
			mark = pt + 1;
			}
		/*else lines = scm_cons(
			scm_from_locale_string(rdata), lines);*/
		}
	printf("reply...\n");
	sprintf(buf, "HTTP/1.1 200 OK\r\ncontent-type: text/plain\r\n\r\nfoo!\n");
	send(conn, buf, strlen(buf), 0);
	close(conn);
	/*scm_c_define("lines", lines);
	scm_c_eval_string("(display (reverse lines))");*/
	return SCM_UNSPECIFIED;
	}

int main() {
	int port;
	fd_set fds;
        int sock, conn;  
	int hisock, fdin, c;
	char buf[1024];
	SCM lines, display, newline, expr, obj;
        char *recv_data;       
        struct sockaddr_in server_addr,client_addr;    
        int sin_size, n;
	port = 8080;
	sock = socket(AF_INET, SOCK_STREAM, 0);
        server_addr.sin_family = AF_INET;         
        server_addr.sin_port = htons(port);
        server_addr.sin_addr.s_addr = INADDR_ANY; 
        bzero(&(server_addr.sin_zero), 8); 
        bind(sock, (struct sockaddr *)&server_addr,
		sizeof(struct sockaddr));
        listen(sock, 5);
	printf("TCPServer Waiting for client on port %d\n", port);
        fflush(stdout);
	scm_init_guile();
	hisock = fdin = fileno(stdin);
	if (sock > hisock) hisock = sock;
	printf("in> ");
	fflush(stdout);
	display = scm_c_eval_string("display");
	newline = scm_c_eval_string("newline");
        while(1) {  
		FD_ZERO(&fds);
		FD_SET(sock, &fds);
		FD_SET(fdin, &fds);
		select(hisock + 1, &fds, NULL, NULL, NULL);
printf("hit\n");
		if (FD_ISSET(fdin, &fds)) {
			expr = scm_read(scm_current_input_port());
			obj = scm_primitive_eval(expr);
			if (scm_eof_object_p(obj) == SCM_BOOL_T) break;
			scm_call_1(display, obj);
			scm_call_0(newline);
			printf("in> ");
			fflush(stdout);
			}
		if (FD_ISSET(sock, &fds))
			scm_spawn_thread(dispatch, (void *)&sock,
						NULL, NULL);
		}
	close(sock);
	return 0;
	} 
