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

#define CUSTOM_EOF '$'
#define BUFSIZE 8192
#define RSIZE 256
#define HEADERSIZE 128

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
 * send_error() - send an error back to the client
 */
void send_error(int connfd, const char * msg)
{
    fprintf(stderr, "Sending error message to client: %s", msg);
    write(connfd, msg, sizeof(msg));
    //write(connfd, EOF, 4);
}

/*
 * - Send() - send wrapper
 */
void Send(int connfd, void * buffer, int length)
{
    unsigned char * buf_location = (unsigned char * )buffer;
    int bytes_sent = 1;
    while(length){
        if ((bytes_sent = write(connfd, buf_location, length)) < 0)
        {
            if (errno != EINTR)
                die("Put_file write error", strerror(errno));
            bytes_sent = 0;
        }
        length -= bytes_sent;
        buf_location += bytes_sent;
    }
}

/*
 * - Send_Int() - uses Send() to send one int
 */
void Send_Int(int connfd, uint32_t val){
    Send(connfd, &val, sizeof(val));
    //return return_value;
    //return ntohl(return_value);
}

/*
 * - Receive() - recv wrapper
 */
unsigned char Receive(int connfd, void * buffer, int length)
{
    unsigned char * buf_location = (unsigned char * )buffer;
    unsigned char return_value = 0;
    int bytes_received = 1;
    while(length && !return_value)
    {
        bytes_received = recv(connfd, buffer, length, 0);
        if(!bytes_received)
        {
            return_value = 1;
        }
        else if(bytes_received == -1)
        {
            if(errno != EINTR)
                die("read error: ", strerror(errno));
            continue;
        }
        else
        {
            length -= bytes_received;
            buf_location += bytes_received;
        }
    }
    return return_value;
}

uint32_t Receive_Int(int connfd)
{
    uint32_t return_value;
    Receive(connfd, &return_value, sizeof(return_value));
    return return_value;
    //return ntohl(return_value);
}

/*
int check_header(const char * str1, const char * str2)
{

}
*/

/*
 * put_file() - send a file to the server accessible via the given socket fd
 */
void put_file(int fd, char *put_name) 
{
    FILE * fp = fopen(put_name, "rb");   
    if(fp == NULL)
    {
        send_error(fd, "GET file not found\n");
        return;
    } 
    fseek(fp, 0, SEEK_END);
    int file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char file_buf[file_size];

    int c;
    int i = 0;
    while ((c = fgetc(fp)) != EOF)
    {
        file_buf[i] = c;
        i++;
    }

    int j = 1, temp_size = file_size / 10; //acount for file_size of 0
    while (temp_size)
    {
        j++;
        temp_size /= 10;
    }

    fclose(fp);

    char str_fsize[j];
    sprintf(str_fsize, "%d", file_size); 
    
    /* create put request */
    uint32_t header_size = strlen("PUT") + strlen(put_name) + strlen(str_fsize) + 3;
    char put[header_size + file_size];
    bzero(put, header_size + file_size);
    strcpy(put, "PUT\n");
    strcat(put, put_name);
    strcat(put, "\n");
    strcat(put, str_fsize);
    strcat(put, "\n");
    strcat(put, file_buf);
    
    Send_Int(fd, header_size);
    Send(fd, put, header_size + file_size);

    uint32_t rec_size = Receive_Int(fd);
    char response[rec_size];
    bzero(response, rec_size);
    if (Receive(fd, response, rec_size) != 0)
        die("Put_file", "Connection closed while reading response");

    if (strncmp(response, "OK\n", rec_size)) //check for OK
        die("Put_file server response error", response);
}

/*
 * get_file() - get a file from the server accessible via the given socket
 *              fd, and save it according to the save_name
 */
void get_file(int fd, char *get_name, char *save_name) 
{
    int size = strlen(get_name) + 5; //GET\n + \0
    char get[size];
    bzero(get, size);
    strcpy(get, "GET\n");
    strcat(get, get_name);

    uint32_t header_size = strlen(get);
    Send_Int(fd, header_size);
    Send(fd, get, header_size);

    /*
    ssize_t bytes_sent = 0;
    size_t bytes_left = strlen(get);
    char * w_ptr = get;
    while (bytes_left > 0)
    {
        if ((bytes_sent = write(fd, w_ptr, bytes_left)) <= 0)
        {
            if (errno != EINTR)
                die("Get_file write error", strerror(errno));
            bytes_sent = 0;
        }
        bytes_left -= bytes_sent;
        w_ptr += bytes_sent;
    }
    */

    /* get size of header */
    uint32_t rec_headsize = Receive_Int(fd);

    /* get header */
    char header_buf[rec_headsize];
    bzero(header_buf, rec_headsize);
    if (Receive(fd, header_buf, rec_headsize) != 0)
        die("Get_file", "Connection closed while reading header");

    //char header_buf[HEADERSIZE];
    //bzero(header_buf, HEADERSIZE);
    //int file_size = 0;
    //char * r_ptr = header_buf;
    //bytes_left = HEADERSIZE;
    /*
    while (bytes_left > 0)
    {
        if ((bytes_sent = read(fd, r_ptr, bytes_left)) < 0)
        {
            if (errno != EINTR)
                die("Get_file read error", strerror(errno));
            continue;
        }
        if (bytes_sent == 0)
        {
            break;
        }
        bytes_left -= bytes_sent;
        r_ptr += bytes_sent;
    }
    */
    
    /* check for OK */
    char * iter_buf = strtok(header_buf, "\n");
    if (strcmp(iter_buf, "OK"))
        die("Get_file server response error", iter_buf);
    off_t offset = strlen("OK\n");
    
    /* check to make sure correct file was sent */
    iter_buf = strtok(NULL, "\n");
    if (strcmp(iter_buf, get_name))
        die("Get_file file error", "Incorrect file retrieved");
    offset += strlen(iter_buf) + 1;

    /* check file size */
    char * t;
    iter_buf = strtok(NULL, "\n");
    offset += strlen(iter_buf) + 1;
    int file_size = strtol(iter_buf, &t, 10);

    /* create new buffer for file content */
    char file_buf[file_size];
    bzero(file_buf, file_size);
    if (Receive(fd, file_buf, file_size) != 0)
        die("Get_file", "Connection closed while reading file");


    /*
    size_t already_read = HEADERSIZE - bytes_left - offset;
    bytes_left = file_size - already_read;
    memcpy(file_buf, &header_buf[offset], already_read);
    r_ptr = file_buf;
    r_ptr += already_read;
    while (bytes_left > 0)
    {
        if ((bytes_sent = read(fd, r_ptr, bytes_left)) < 0)
        {
            if(errno != EINTR)
                die("read error: ", strerror(errno));
            continue;
        }
        /* this shouldn't ever happen... *
        if(bytes_sent == 0)
        {
            fprintf(stderr, "File Buffer so far: %s\n", file_buf);
            return;
        }
        /* this prevents a read hang when the size of the file is less than the assumed header size *
        if (*(r_ptr-1) == CUSTOM_EOF) 
        {
            *r_ptr = 0;
            break;
        }
        /* update pointer *
        bytes_left -= bytes_sent;
        r_ptr += bytes_sent;
    }
    */

    FILE * fp = fopen(save_name, "wb");
    fwrite(file_buf, sizeof(file_buf) + 1, 1, fp);
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
