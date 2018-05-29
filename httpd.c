/*
 * 这是一个简单的web服务器的实现
 * 只支持linux
 * 实现
 * 1. 静态文件服务器
 * 2. COMMON GATEWAY INTERFACE
 * 3. 例子用ruby脚本实现
 * 很多实现都是参考 J. David's webserver　
 *　https://sourceforge.net/projects/tinyhttpd/
 */

#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <signal.h>

#define SERVER_STRING "Server: simplehttpd/0.1.0\r\n"

#define MAX_COMMANDLINE_FLAGS 1
#define PORT_BUFF_SIZE 100
#define BACKLOG 5
#define FOLDER "www"

void *accept_request(void *pclient);
void bad_req(int fd);
void inner_error_req(int fd);
void notfound_req(int fd);
void unimp_method_req(int fd);
void sv_static_file(int clientfd, char *path);
void execcgi(int fd, const char *path, const char *method, char *querystr);
int startup();
void setupwait();
u_short get_sin_port(struct sockaddr *sa);
void *get_in_addr(struct sockaddr *);
int get_line(int sock, char *buf, int size);
void sigchld_handler(int s);
u_short port;

/* http 是通过\n 或者 CRLF \r\n来分割 */
/* socket 一般不检测EOF,　只有对方socket关闭, 才有eof */
/* 所以一般都是在协议中规定长度，例如 http CONTENT-LENGTH */
int get_line(int sock, char *buf, int size) {
  int i = 0;
  char c = '\0';
  int n;
  while ((i < size - 1) && (c != '\n')) {
    n = recv(sock, &c, 1, 0);
    if (n > 0) {
      if (c == '\r')
        {
          n = recv(sock, &c, 1, MSG_PEEK);
          if ((n > 0) && (c == '\n'))
            recv(sock, &c, 1, 0);
          else
            c = '\n';
        }
      buf[i] = c;
      i++;
    }
    else
      c = '\n';
  }
  buf[i] = '\0';
  return i;
}

/* 500 */
void inner_error_req(int clientfd) {
  char buf[1024];

  sprintf(buf, "HTTP/1.0 500 Internal Server Error\r\n");
  send(clientfd, buf, strlen(buf), 0);
  sprintf(buf, "Content-type: text/html\r\n");
  send(clientfd, buf, strlen(buf), 0);

  sprintf(buf, "\r\n");
  send(clientfd, buf, strlen(buf), 0);

  sprintf(buf, "<P>Inner error.\r\n");
  send(clientfd, buf, strlen(buf), 0);
}

void bad_req(int clientfd) {
  char buf[1024];

  sprintf(buf, "HTTP/1.0 400 BAD REQUEST\r\n");
  send(clientfd, buf, sizeof(buf), 0);
  sprintf(buf, "Content-type: text/html\r\n");
  send(clientfd, buf, sizeof(buf), 0);

  sprintf(buf, "\r\n");
  send(clientfd, buf, sizeof(buf), 0);

  sprintf(buf, "<P>A bad request");
  send(clientfd, buf, sizeof(buf), 0);
}

/* 404 */
void notfound_req(int clientfd) {
  char buf[1024];

  /* headers */
  sprintf(buf, "HTTP/1.0 404 NOT FOUND\r\n");
  send(clientfd, buf, strlen(buf), 0);
  sprintf(buf, SERVER_STRING);
  send(clientfd, buf, strlen(buf), 0);
  sprintf(buf, "Content-Type: text/html\r\n");
  send(clientfd, buf, strlen(buf), 0);

  /* blank line */
  sprintf(buf, "\r\n");
  send(clientfd, buf, strlen(buf), 0);

  /* body */
  sprintf(buf, "<p>Not Found</p>\r\n");
  send(clientfd, buf, strlen(buf), 0);
}

/* not support method */
void unimp_method_req(int clientfd) {
  char buf[1024];
  /* 501 method没有实现 */
  sprintf(buf, "HTTP/1.0 501 Method Not Implemented\r\n");
  send(clientfd, buf, strlen(buf), 0);
  sprintf(buf, SERVER_STRING);
  send(clientfd, buf, strlen(buf), 0);
  sprintf(buf, "Content-Type: text/html\r\n");
  send(clientfd, buf, strlen(buf), 0);

  sprintf(buf, "\r\n");
  send(clientfd, buf, strlen(buf), 0);

  sprintf(buf, "<p>Method Not Implemented</p>\r\n");
  send(clientfd, buf, strlen(buf), 0);

  sprintf(buf, "<p>Support GET, POST, PUT, DELETE</p>\r\n");
  send(clientfd, buf, strlen(buf), 0);
}

/* 执行CGI */
void execcgi(int clientfd, const char *path, const char *method, char *querystr) {
  char buf[1024];
  int cgi_output[2];
  int cgi_input[2];
  pid_t pid;
  int i;
  char c;
  int numchars = 1;
  int content_length = -1;

  if (strcasecmp(method, "GET") == 0)
    while ((numchars > 0) && strcmp("\n", buf))
      numchars = get_line(clientfd, buf, sizeof(buf));
  else if(strcasecmp(method, "POST") == 0 || strcasecmp(method, "PUT") == 0) {
    numchars = get_line(clientfd, buf, sizeof(buf));
    /* 读取首部　headers, 由于headers是通过一个空行来和body分隔的 */
    while ((numchars > 0) && strcmp("\n", buf)) {
        buf[15] = '\0';
        if (strcasecmp(buf, "Content-Length:") == 0)
          content_length = atoi(&(buf[16]));
        numchars = get_line(clientfd, buf, sizeof(buf));
    }
    if (content_length == -1) {
      bad_req(clientfd);
      return;
    }
  }

  if (pipe(cgi_output) < 0) {
    perror("pipe");
    inner_error_req(clientfd);
    return;
  }

  if (pipe(cgi_input) < 0) {
    perror("pipe");
    inner_error_req(clientfd);
    return;
  }

  if ((pid = fork()) < 0) {
    perror("fork");
    inner_error_req(clientfd);
    return;
  }

  if (pid == 0) {
    char meth_env[255];
    char query_env[255];
    char length_env[255];

    /* 重定向标准输出 */
    dup2(cgi_output[1], 1);
    /* 重定向标准输入 */
    dup2(cgi_input[0], 0);
    close(cgi_output[0]);
    close(cgi_input[1]);

    /* 通过环境变量来和CGI来传递　REQUEST_METHOD QUERY_STRING CONTENT_LENGTH */
    /* 这个CGI规定的方式 */
    sprintf(meth_env, "REQUEST_METHOD=%s", method);
    putenv(meth_env);
    if (strcasecmp(method, "GET") == 0) {
      sprintf(query_env, "QUERY_STRING=%s", querystr);
      putenv(query_env);
    }
    else {
      sprintf(length_env, "CONTENT_LENGTH=%d", content_length);
      putenv(length_env);
    }
    if(execl(path, path, NULL) == -1) {
      perror("execl");
    }
    exit(0);
  } else {    /* 父进程 */
    close(cgi_output[1]);
    close(cgi_input[0]);
    /* 发送body通过管道 */
    /* 通过Content-Length来取body */
    if(strcasecmp(method, "POST") == 0 || strcasecmp(method, "PUT") == 0)
      for (i = 0; i < content_length; i++) {
        recv(clientfd, &c, 1, 0);
        write(cgi_input[1], &c, 1);
      }
    /* 写入reponse的首行 */
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    send(clientfd, buf, strlen(buf), 0);
    while (read(cgi_output[0], &c, 1) > 0)
      send(clientfd, &c, 1, 0);
    close(cgi_output[0]);
    close(cgi_input[1]);
  }
}

void sv_static_file(int clientfd, char *path) {
  FILE *resource = NULL;
  int numchars = 1;
  char buf[1024];

  /* 丢弃掉所有的其他http数据 */
  while ((numchars > 0) && strcmp("\n", buf))
    numchars = get_line(clientfd, buf, sizeof(buf));

  //打开文件
  resource = fopen(path, "r");
  if (resource == NULL)
    //如果文件不存在，则返回 not found
    notfound_req(clientfd);
  else {
    char buf[1024];

    /* headers */
    strcpy(buf, "HTTP/1.0 200 OK\r\n");
    send(clientfd, buf, strlen(buf), 0);
    strcpy(buf, SERVER_STRING);
    send(clientfd, buf, strlen(buf), 0);
    sprintf(buf, "Content-Type: text/html\r\n");
    send(clientfd, buf, strlen(buf), 0);

    /* 空白行 */
    strcpy(buf, "\r\n");
    send(clientfd, buf, strlen(buf), 0);

    /* body */
    fgets(buf, sizeof(buf), resource);
    while (!feof(resource)) {
      //读取并发送文件内容
      send(clientfd, buf, strlen(buf), 0);
      fgets(buf, sizeof(buf), resource);
    }
  }
}

void *accept_request(void *pclient) {
  int client = *(int *) pclient;
  char buf[1024];
  int numchars;
  char method[255];
  char url[255];
  char path[512];
  size_t i, j;
  struct stat st;
  int cgi = 0;      /* becomes true if server decides this is a CGI
                     * program */
  char *query_string = NULL;
  /* 获得首行 */
  /* <method> <url> <version> */
  numchars = get_line(client, buf, sizeof(buf));
  i = 0; j = 0;

  /* 获得 method */
  while (!isspace(buf[j]) && (i < sizeof(method) - 1)) {
      method[i] = buf[j];
      i++; j++;
  }
  method[i] = '\0';

  if(strcasecmp(method, "GET") && strcasecmp(method, "POST") && strcasecmp(method, "PUT") && strcasecmp(method, "DELETE")) {
    unimp_method_req(client);
    goto complement;
  }

  /* Get可能访问静态文件 */
  if(strcasecmp(method, "GET") != 0)
    cgi = 1;

  i = 0;
  /* 去掉多余的空格 */
  while (isspace(buf[j]) && (j < sizeof(buf)))
    j++;

  /* 获得url */
  while (!isspace(buf[j]) && (i < sizeof(url) - 1) && (j < sizeof(buf))) {
      url[i] = buf[j];
      i++; j++;
  }
  url[i] = '\0';

  /* 获得　query_string */
  query_string = url;
  while ((*query_string != '?') && (*query_string != '\0'))
    query_string++;
  if (*query_string == '?') {
    cgi = 1;
    *query_string = '\0';
    query_string++;
  }
  sprintf(path, "%s%s", FOLDER, url);
  printf("path: %s, url: %s\n", path, url);

  if (path[strlen(path) - 1] == '/')
    strcat(path, "index.html");
  printf("path: %s, url: %s\n", path, url);

  if(stat(path, &st) == -1) {
    while ((numchars > 0)  && strcmp("\n", buf))
      numchars = get_line(client, buf, sizeof(buf));
    notfound_req(client);
    goto complement;
  } else {
    /* 检测是否是目录　*/
    if ((st.st_mode & S_IFMT) == S_IFDIR)
      strcat(path, "/index.html");
    /* 检测执行权 */
    if ((st.st_mode & S_IXUSR) ||
        (st.st_mode & S_IXGRP) ||
        (st.st_mode & S_IXOTH))
      cgi = 1;

    if (!cgi) {
      /* 静态文件服务器 */
      sv_static_file(client, path);
    } else {
      /* 执行CGI */
      execcgi(client, path, method, query_string);
    }
  }
 complement:
  close(client);
  return NULL;
}

static int usage(char **argv) {
        printf("\nUsage:\t%s -p portnum\n"
               "\t-p set server port number to listen.\n"
               "\tfor example : httpd -p 3355\n" "More help in README file\n\n", argv[0]);
        return EXIT_FAILURE;
}

void sigchld_handler(int s) {
  while(waitpid(-1, NULL, WNOHANG) > 0);
}

// 取得 sockaddr，IPv4 或 IPv6：
u_short get_sin_port(struct sockaddr *sa)
{
  if (sa->sa_family == AF_INET) {
    return ((struct sockaddr_in*)sa)->sin_port;
  }
  return ((struct sockaddr_in6*)sa)->sin6_port;
}

// 取得 sockaddr，IPv4 或 IPv6：
void *get_in_addr(struct sockaddr *sa)
{
  if (sa->sa_family == AF_INET) {
    return &(((struct sockaddr_in*)sa)->sin_addr);
  }
  return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int startup() {
  int sockfd, rv, yes;
  char portbuf[PORT_BUFF_SIZE];
  struct addrinfo hints, *servinfo, *p;
  struct sockaddr_storage name;
  yes = 1;
  socklen_t namelen = sizeof(name);

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE; // 使用我的 IP
  sprintf(portbuf, "%u", port);

  if ((rv = getaddrinfo(NULL, portbuf, &hints, &servinfo)) != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
    return 1;
  }

  for(p = servinfo; p != NULL; p = p->ai_next) {
    if ((sockfd = socket(p->ai_family, p->ai_socktype,
                         p->ai_protocol)) == -1) {
      perror("server: socket");
      continue;
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,
                   sizeof(int)) == -1) {
      perror("setsockopt");
      exit(1);
    }

    if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
      close(sockfd);
      perror("server: bind");
      continue;
    }
    break;
  }

  if (p == NULL) {
    fprintf(stderr, "server: failed to bind\n");
    exit(2);
  }

  if(port == 0) {
    if(getsockname(sockfd, (struct sockaddr *)&name, &namelen) == -1) {
      perror("getsockname");
      exit(1);
    }
    port = ntohs(get_sin_port((struct sockaddr *)&name));
  }

  freeaddrinfo(servinfo);

  if (listen(sockfd, BACKLOG) == -1) {
    perror("listen");
    exit(1);
  }

  return sockfd;
}

void setupwait() {
  struct sigaction sa;
  sa.sa_handler = sigchld_handler; // 收拾全部死掉的 processes
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  if (sigaction(SIGCHLD, &sa, NULL) == -1) {
    perror("sigaction");
    exit(1);
  }
}

int main(int argc, char *argv[]) {
  int serverfd, clientfd, start_argv, i;
  struct sockaddr_storage their_addr; // 连接者的地址资料
  socklen_t sin_size = sizeof(their_addr);
  char s[INET6_ADDRSTRLEN];
  pthread_t threadid;
  serverfd = -1;
  start_argv = 1;
  i = 0;
  /* 查看是否有-p　option来设置 参数*/
  for(i=0;i<MAX_COMMANDLINE_FLAGS;i++) {
    if(start_argv < argc && (*(argv+start_argv))[0] == '-') {
      if((*(argv+start_argv))[1] == 'p') {
        start_argv++;
        if(start_argv < argc)
          port = atoi(*(argv+start_argv));
        else
          return usage(argv);
      }
    }
  }

  serverfd = startup();
  setupwait();

  printf("httpd running on port %d\n", port);

  while(1) {
    clientfd = accept(serverfd, (struct sockaddr *)&their_addr, &sin_size);
    if(clientfd == -1) {
      perror("accept");
      continue;
    }
    inet_ntop(their_addr.ss_family,
              get_in_addr((struct sockaddr *)&their_addr),
              s, sizeof s);
    printf("server: got connection from %s port: %u\n", s, ntohs(get_sin_port((struct sockaddr *)&their_addr)));
    if (pthread_create(&threadid, NULL, accept_request, (void*)&clientfd) != 0)
        perror("pthread_create");
  }
  close(serverfd);
  return 0;
}
