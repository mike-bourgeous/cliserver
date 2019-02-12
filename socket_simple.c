#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "zmodem.h"
#include "zm.h"
struct socket_pri
{
    /* data */
    int fd;
    struct zmr_state_s *pzmr;
};


size_t zmodem_write(void *arg, const uint8_t *buffer, size_t buflen){
	struct socket_pri *sp = (struct socket_pri *)arg;
    int len;
#if 1
	int i;
	//char* hexbuffer = barray2hexstr(buffer,buflen);
	for(i = 0; i < buflen; i++){
		printf("%02x ",buffer[i]);
	}
	printf("\n");
	//free(hexbuffer);
#endif	
    printf("-");
    len = write(sp->fd,buffer,buflen);
	return len;
} 
size_t zmodem_read(void *arg, const uint8_t *buffer, size_t buflen){
	struct socket_pri *sp = (struct socket_pri *)arg;
    int len;
    printf("%d +",sp->fd);
    //len = recv(sp->fd,buffer,buflen,0);
    len = read(sp->fd,(void*)buffer,buflen);

#if 1
	int i;
	//char* hexbuffer = barray2hexstr(buffer,buflen);
	for(i = 0; i < len; i++){
		printf("%02x ",buffer[i]);
	}
	printf("\n");
	//free(hexbuffer);
#endif	
	return len;
}

size_t zmodem_on_receive(void *arg, const uint8_t *buffer, size_t buflen,bool zcnl){
	//struct cmdsocket *cmdsocket = container_of(pzmr,struct cmdsocket,pzmr);
	printf("%s\n",buffer);

	return 0;
}


int main(){
    int serv_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    struct sockaddr_in serv_addr;
    int ret;
    int on=1;

    struct socket_pri *sp = calloc(1, sizeof(struct socket_pri));
	if(sp == NULL) {
		printf("Error allocating command handler info");
		return 0;
	}

	if (setsockopt(serv_sock, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof (on)) < 0) {
		printf("setsockopt (reuse address)");
        return 0;
	}

    memset(&serv_addr, 0, sizeof(serv_addr));  
    serv_addr.sin_family = AF_INET;  
    serv_addr.sin_addr.s_addr = inet_addr("127.0.0.1"); 
    serv_addr.sin_port = htons(1234);  
    bind(serv_sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
    listen(serv_sock, 20);
    struct sockaddr_in clnt_addr;
    socklen_t clnt_addr_size = sizeof(clnt_addr);
    int clnt_sock = accept(serv_sock, (struct sockaddr*)&clnt_addr, &clnt_addr_size);
    char str[16] = {0};
    while(1){
        ret = read(clnt_sock,str,2);
        printf("%d %d %s\n",clnt_sock, ret, str);
        if(strcmp(str,"rz") == 0){
            printf("1\n");
            sp->fd = clnt_sock;
            sp->pzmr = zmr_initialize(zmodem_write,zmodem_read,zmodem_on_receive,sp);
            if(sp->pzmr == NULL){
                printf("zmr_initialize failed\n");
                break;
            }
        }
        if(sp->pzmr != NULL){
            printf("2\n");
            while(1){
                if(zmr_receive(sp->pzmr,0) != 0){
                    break;
                }
                sleep(1);
            }
        }
    }

    //关闭套接字
    close(clnt_sock);
    close(serv_sock);
    return 0;
}
