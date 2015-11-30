#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/md5.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BUFSIZE 8192

/*
 * help() - Print a help message
 */
void help(char *progname) 
{
    printf("Usage: %s [OPTIONS]\n", progname);
    printf("Perform a PUT or a GET from a network file server\n");
    printf("  -P    PUT file indicated by parameter\n");
    printf("  -G    GET file indicated by parameter\n");
    printf("  -s    server info (IP or hostname)\n");
    printf("  -p    port on which to contact server\n");
    printf("  -S    for GETs, name to use when saving file locally\n");
}

/*
 * die() - print an error and exit the program
 */
void die(const char *msg1, char *msg2) 
{
    fprintf(stderr, "%s, %s\n", msg1, msg2);
    exit(0);
}

/*
 * connect_to_server() - open a connection to the server specified by the
 *                       parameters
 */
int connect_to_server(char *server, int port) 
{
    int clientfd;
    struct hostent *hp;
    struct sockaddr_in serveraddr;
    char errbuf[256];                                   /* for errors */

    /* create a socket */
    if ((clientfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        die("Error creating socket: ", strerror(errno));

    /* Fill in the server's IP address and port */
    if ((hp = gethostbyname(server)) == NULL) 
    {
        sprintf(errbuf, "%d", h_errno);
        die("DNS error: DNS error ", errbuf);
    }
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)hp->h_addr_list[0],
          (char *)&serveraddr.sin_addr.s_addr, hp->h_length);
    serveraddr.sin_port = htons(port);

    /* connect */
    if (connect(clientfd, (struct sockaddr *) &serveraddr, sizeof(serveraddr)) < 0)
        die("Error connecting: ", strerror(errno));

    return clientfd;
}

/*
 * echo_client() - this is dummy code to show how to read and write on a
 *                 socket when there can be short counts.  The code
 *                 implements an "echo" client.
 */
void echo_client(int fd) 
{
    // main loop
    while (1) 
    {
        /* set up a buffer, clear it, and read keyboard input */
        const int MAXLINE = 8192;
        char buf[MAXLINE];
        bzero(buf, MAXLINE);
        if (fgets(buf, MAXLINE, stdin) == NULL) 
        {
            if (ferror(stdin))
                die("fgets error", strerror(errno));
            break;
        }

        /* send keystrokes to the server, handling short counts */
        size_t n = strlen(buf);
        size_t nremain = n;
        ssize_t nsofar;
        char *bufp = buf;
        while (nremain > 0) 
        {
            if ((nsofar = write(fd, bufp, nremain)) <= 0) 
            {
                if (errno != EINTR) 
                {
                    fprintf(stderr, "Write error: %s\n", strerror(errno));
                    exit(0);
                }
                nsofar = 0;
            }
            nremain -= nsofar;
            bufp += nsofar;
        }

        /* read input back from socket (again, handle short counts)*/
        bzero(buf, MAXLINE);
        bufp = buf;
        nremain = MAXLINE;
        while (1) 
        {
            if ((nsofar = read(fd, bufp, nremain)) < 0) 
            {
                if (errno != EINTR)
                    die("read error: ", strerror(errno));
                continue;
            }
            /* in echo, server should never EOF */
            if (nsofar == 0)
                die("Server error: ", "received EOF");
            bufp += nsofar;
            nremain -= nsofar;
            if (*(bufp-1) == '\n') 
            {
                *bufp = 0;
                break;
            }
        }

        /* output the result */
        printf("%s", buf);
    }
}

/*
 * put_file() - send a file to the server accessible via the given socket fd
 */
void put_file(int fd, char *put_name) 
{
    FILE * fp = fopen(put_name, "r");    
    fseek(fp, 0, SEEK_END);
    int file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char file_content[file_size];

    int c;
    int i = 0;
    while ((c = fgetc(fp)) != EOF)
    {
        file_content[i] = c;
        i++;
    }

    int j = 1, temp_size = file_size / 10; //acount for file_size of 0
    while (temp_size)
    {
        j++;
        temp_size /= 10;
    }

    char size[j];
    sprintf(size, "%d", file_size); 
    
    char put[strlen(put_name) + j + file_size + 6]; //PUT\n + \n + \n
    strcpy(put, "PUT\n");
    strcat(put, put_name);
    strcat(put, "\n");
    strcat(put, size);
    strcat(put, "\n");
    strcat(put, file_content);

    if (write(fd, put, strlen(put)) < 0)
    {
        die("Put_file write error", strerror(errno));
    }

    fclose(fp);

    char response[BUFSIZE];
    bzero(response, BUFSIZE);
    if (read(fd, response, BUFSIZE) < 0)
    {
        die("Put_file read error", strerror(errno));
    }
    else
    {
        if (strcmp(response, "OK\n")) //check for OK
            die("Put_file server response error", response);
    }
}

/*
 * get_file() - get a file from the server accessible via the given socket
 *              fd, and save it according to the save_name
 */
void get_file(int fd, char *get_name, char *save_name) 
{
    int size = strlen(get_name) + 5; //GET\n + \0
    char get[size];
    strcpy(get, "GET\n");
    strcat(get, get_name);

    if (write(fd, get, size) < 0)
    {
        die("Get_file write error", strerror(errno));
    }

    char rec_buf[BUFSIZE];
    bzero(rec_buf, BUFSIZE);
    int bytes_sent = 0;
    char * buf;
    FILE * fp = fopen(save_name, "w");

    if ((bytes_sent = read(fd, rec_buf, BUFSIZE - 1)) < 0)
    {
        /* check to make sure correct file was sent */
        buf = strtok(rec_buf, "\n");
        int offset = 0;
        if (strcmp(buf, "OK"))
        {
            die("Get_file server response error", buf);
        }
        else
        {
            offset += strlen(buf);
            buf = strtok(NULL, "\n");
            if (strcmp(buf, get_name))
                die("Get_file file error", "Incorrect file retrieved");
            offset += strlen(buf);
        }

        rec_buf[bytes_sent] = '\0';
        fputs(rec_buf + offset, fp);
        //printf("%s\n\n", rec_buf);
        bzero(rec_buf, BUFSIZE);
    }
    else
    {
        die("Get_file read error", strerror(errno));
    }
    
    while ((bytes_sent = read(fd, rec_buf, BUFSIZE - 1)) > 0)
    {
        if (bytes_sent < BUFSIZE - 1)
            rec_buf[bytes_sent] = '\0';

        fputs(rec_buf, fp);
        //printf("%s\n\n", rec_buf);
        bzero(rec_buf, BUFSIZE);
    } 

    fclose(fp);
}

/*
 * main() - parse command line, open a socket, transfer a file
 */
int main(int argc, char **argv) {
    /* for getopt */
    long  opt;
    char *server = NULL;
    char *put_name = NULL;
    char *get_name = NULL;
    int   port;
    char *save_name = NULL;

    check_team(argv[0]);

    /* parse the command-line options. */
    while ((opt = getopt(argc, argv, "hs:P:G:S:p:")) != -1) {
        switch(opt) {
          case 'h': help(argv[0]); break;
          case 's': server = optarg; break;
          case 'P': put_name = optarg; break;
          case 'G': get_name = optarg; break;
          case 'S': save_name = optarg; break;
          case 'p': port = atoi(optarg); break;
        }
    }

    /* open a connection to the server */
    int fd = connect_to_server(server, port);
    /* put or get, as appropriate */
    if (put_name)
        put_file(fd, put_name);
    else
        get_file(fd, get_name, save_name);

    /* close the socket */
    int rc;
    if ((rc = close(fd)) < 0)
        die("Close error: ", strerror(errno));
    exit(0);
}
