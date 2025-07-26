#include "proxy_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>

#define MAX_BYTES 4096
#define MAX_CLIENTS 10
#define MAX_SIZE 200*(1<<20)
#define MAX_ELEMENT_SIZE 10*(1<<20)

typedef struct cache_element cache_element;

struct cache_element{
    char* data;
    int len;
    char* url;
    time_t lru_time_track;
    cache_element* next;
};

cache_element* head;
int cache_size;

pthread_mutex_t lock;
sem_t seamaphore;
pthread_t tid[MAX_CLIENTS];

int port_number = 8080;
int proxy_socketId;

cache_element* find(char* url);
int add_cache_element(char* data,int size,char* url);
void remove_cache_element();

int sendErrorMessage(int socket, int status_code) {
    char str[1024];
    char currentTime[50];
    time_t now = time(0);
    struct tm data = *gmtime(&now);
    strftime(currentTime,sizeof(currentTime),"%a, %d %b %Y %H:%M:%S %Z", &data);

    switch(status_code) {
        case 400: snprintf(str,sizeof(str),
          "HTTP/1.1 400 Bad Request\r\nContent-Length:95\r\nConnection:close\r\nContent-Type:text/html\r\nDate:%s\r\nServer:Proxy\r\n\r\n<HTML><BODY><H1>400 Bad Request</H1></BODY></HTML>",currentTime);
                  send(socket, str, strlen(str), 0);
                  break;
        case 403: snprintf(str,sizeof(str),
          "HTTP/1.1 403 Forbidden\r\nContent-Length:95\r\nConnection:close\r\nContent-Type:text/html\r\nDate:%s\r\nServer:Proxy\r\n\r\n<HTML><BODY><H1>403 Forbidden</H1></BODY></HTML>",currentTime);
                  send(socket, str, strlen(str), 0);
                  break;
        case 404: snprintf(str,sizeof(str),
          "HTTP/1.1 404 Not Found\r\nContent-Length:95\r\nConnection:close\r\nContent-Type:text/html\r\nDate:%s\r\nServer:Proxy\r\n\r\n<HTML><BODY><H1>404 Not Found</H1></BODY></HTML>",currentTime);
                  send(socket, str, strlen(str), 0);
                  break;
        case 500: snprintf(str,sizeof(str),
          "HTTP/1.1 500 Internal Server Error\r\nContent-Length:95\r\nConnection:close\r\nContent-Type:text/html\r\nDate:%s\r\nServer:Proxy\r\n\r\n<HTML><BODY><H1>500 Internal Server Error</H1></BODY></HTML>",currentTime);
                  send(socket, str, strlen(str), 0);
                  break;
        case 501: snprintf(str,sizeof(str),
          "HTTP/1.1 501 Not Implemented\r\nContent-Length:95\r\nConnection:close\r\nContent-Type:text/html\r\nDate:%s\r\nServer:Proxy\r\n\r\n<HTML><BODY><H1>501 Not Implemented</H1></BODY></HTML>",currentTime);
                  send(socket, str, strlen(str), 0);
                  break;
        case 505: snprintf(str,sizeof(str),
          "HTTP/1.1 505 HTTP Version Not Supported\r\nContent-Length:95\r\nConnection:close\r\nContent-Type:text/html\r\nDate:%s\r\nServer:Proxy\r\n\r\n<HTML><BODY><H1>505 HTTP Version Not Supported</H1></BODY></HTML>",currentTime);
                  send(socket, str, strlen(str), 0);
                  break;
        default: return -1;
    }
    return 1;
}

int connectRemoteServer(char* host_addr, int port_num) {
    int remoteSocket = socket(AF_INET, SOCK_STREAM, 0);
    if(remoteSocket<0){
        printf("Error in Creating Socket.\n");
        return -1;
    }
    struct hostent *host = gethostbyname(host_addr);
    if(host==NULL){
        fprintf(stderr, "No such host exists.\n");
        return -1;
    }
    struct sockaddr_in server_addr;
    bzero((char*)&server_addr,sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_num);
    bcopy((char *)host->h_addr,(char *)&server_addr.sin_addr.s_addr,host->h_length);
    if(connect(remoteSocket,(struct sockaddr*)&server_addr,(socklen_t)sizeof(server_addr))<0){
        fprintf(stderr,"Error in connecting !\n");
        return -1;
    }
    return remoteSocket;
}

int handle_request(int clientSocket, ParsedRequest *request, char *key) {
    char *buf = (char*)malloc(sizeof(char)*MAX_BYTES);
    strcpy(buf, "GET ");
    strcat(buf, request->path);
    strcat(buf, " ");
    strcat(buf, request->version);
    strcat(buf, "\r\n");

    size_t len = strlen(buf);
    if (ParsedHeader_set(request, "Connection", "close") < 0)
        printf("set header key not work\n");

    if (ParsedHeader_get(request, "Host") == NULL) {
        if (ParsedHeader_set(request, "Host", request->host) < 0)
            printf("Set Host header key not working\n");
    }

    if (ParsedRequest_unparse_headers(request, buf + len, (size_t)MAX_BYTES - len) < 0)
        printf("unparse failed\n");

    int server_port = 80;
    if(request->port != NULL)
        server_port = atoi(request->port);

    int remoteSocketID = connectRemoteServer(request->host, server_port);
    if(remoteSocketID<0){
        free(buf);
        return -1;
    }

    int bytes_send = send(remoteSocketID, buf, strlen(buf), 0);
    bzero(buf, MAX_BYTES);

    char *temp_buffer = (char*)malloc(sizeof(char)*MAX_BYTES);
    int temp_buffer_size = MAX_BYTES;
    int temp_buffer_index = 0;

    bytes_send = recv(remoteSocketID, buf, MAX_BYTES-1, 0);
    while(bytes_send > 0) {
        send(clientSocket, buf, bytes_send, 0);
        for(int i=0;i<bytes_send;i++){
            temp_buffer[temp_buffer_index++] = buf[i];
        }
        temp_buffer_size += MAX_BYTES;
        temp_buffer = (char*)realloc(temp_buffer,temp_buffer_size);
        bzero(buf, MAX_BYTES);
        bytes_send = recv(remoteSocketID, buf, MAX_BYTES-1, 0);
    }
    temp_buffer[temp_buffer_index] = '\0';
    add_cache_element(temp_buffer, temp_buffer_index, key);
    printf("Done\n");
    free(temp_buffer);
    free(buf);
    close(remoteSocketID);
    return 0;
}

int checkHTTPversion(char *msg) {
    if(strncmp(msg, "HTTP/1.1", 8)==0) return 1;
    if(strncmp(msg, "HTTP/1.0", 8)==0) return 1;
    return -1;
}

void* thread_fn(void* socketNew) {
    sem_wait(&seamaphore);
    int p; sem_getvalue(&seamaphore,&p);
    printf("semaphore value:%d\n",p);
    int socket = *((int*)socketNew);

    char *buffer = (char*)calloc(MAX_BYTES,sizeof(char));
    bzero(buffer, MAX_BYTES);
    int bytes_send_client = recv(socket, buffer, MAX_BYTES, 0);
    while(bytes_send_client > 0) {
        if(strstr(buffer, "\r\n\r\n") == NULL)
            bytes_send_client = recv(socket, buffer+strlen(buffer), MAX_BYTES-strlen(buffer), 0);
        else break;
    }

    if(bytes_send_client > 0) {
        // parse
        ParsedRequest* request = ParsedRequest_create();
        if(ParsedRequest_parse(request, buffer, strlen(buffer))<0) {
            printf("Parsing failed\n");
        } else {
            if(!strcmp(request->method,"GET")) {
                char key[1024];
                snprintf(key, sizeof(key), "%s%s", request->host, request->path);

                struct cache_element* temp = find(key);
                if(temp != NULL) {
                    int size=temp->len;
                    int pos=0;
                    char response[MAX_BYTES];
                    while(pos<size){
                        bzero(response,MAX_BYTES);
                        for(int i=0;i<MAX_BYTES && pos<size;i++){
                            response[i]=temp->data[pos++];
                        }
                        send(socket,response,(pos<size?MAX_BYTES:size%MAX_BYTES),0);
                    }
                    printf("Data retrived from the Cache\n\n");
                } else {
                    if(request->host && request->path && (checkHTTPversion(request->version)==1)){
                        int res = handle_request(socket, request, key);
                        if(res==-1) sendErrorMessage(socket,500);
                    } else {
                        sendErrorMessage(socket,500);
                    }
                }
            } else {
                printf("This code doesn't support any method other than GET\n");
            }
        }
        ParsedRequest_destroy(request);
    }

    shutdown(socket, SHUT_RDWR);
    close(socket);
    free(buffer);
    sem_post(&seamaphore);
    int p2; sem_getvalue(&seamaphore,&p2);
    printf("Semaphore post value:%d\n",p2);
    return NULL;
}

int main(int argc, char * argv[]) {
    sem_init(&seamaphore,0,MAX_CLIENTS);
    pthread_mutex_init(&lock,NULL);

    if(argc==2)
        port_number = atoi(argv[1]);
    else {
        printf("Too few arguments\n");
        exit(1);
    }

    printf("Setting Proxy Server Port : %d\n",port_number);
    proxy_socketId = socket(AF_INET, SOCK_STREAM, 0);
    if(proxy_socketId<0){ perror("Failed to create socket"); exit(1); }

    int reuse=1;
    if(setsockopt(proxy_socketId,SOL_SOCKET,SO_REUSEADDR,(const char*)&reuse,sizeof(reuse))<0)
        perror("setsockopt failed");

    struct sockaddr_in server_addr;
    bzero((char*)&server_addr,sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_number);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    if(bind(proxy_socketId,(struct sockaddr*)&server_addr,sizeof(server_addr))<0){
        perror("Port is not free");
        exit(1);
    }
    printf("Binding on port: %d\n",port_number);
    if(listen(proxy_socketId,MAX_CLIENTS)<0){
        perror("Error while Listening!");
        exit(1);
    }

    int i=0;
    int Connected_socketId[MAX_CLIENTS];
    struct sockaddr_in client_addr;
    int client_len;

    while(1){
        bzero((char*)&client_addr,sizeof(client_addr));
        client_len = sizeof(client_addr);
        int client_socketId = accept(proxy_socketId,(struct sockaddr*)&client_addr,(socklen_t*)&client_len);
        if(client_socketId<0){
            fprintf(stderr,"Error in Accepting connection!\n");
            exit(1);
        } else {
            Connected_socketId[i] = client_socketId;
        }
        struct sockaddr_in* client_pt = (struct sockaddr_in*)&client_addr;
        struct in_addr ip_addr = client_pt->sin_addr;
        char str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET,&ip_addr,str,INET_ADDRSTRLEN);
        printf("Client is connected with port number: %d and ip address: %s\n",ntohs(client_addr.sin_port),str);
        pthread_create(&tid[i],NULL,thread_fn,(void*)&Connected_socketId[i]);
        i++;
    }
    close(proxy_socketId);
    return 0;
}

// Cache functions
cache_element* find(char* url){
    pthread_mutex_lock(&lock);
    cache_element* site = head;
    while(site!=NULL){
        if(!strcmp(site->url,url)){
            printf("LRU Time Track Before : %ld\n", site->lru_time_track);
            site->lru_time_track = time(NULL);
            printf("LRU Time Track After : %ld\n", site->lru_time_track);
            pthread_mutex_unlock(&lock);
            return site;
        }
        site = site->next;
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

void remove_cache_element(){
    pthread_mutex_lock(&lock);
    if(head!=NULL){
        cache_element *p=head, *q=head, *temp=head;
        for(q=head;q->next!=NULL;q=q->next){
            if(q->next->lru_time_track < temp->lru_time_track){
                temp=q->next;
                p=q;
            }
        }
        if(temp==head){
            head=head->next;
        } else {
            p->next=temp->next;
        }
        cache_size = cache_size - (temp->len) - sizeof(cache_element) - strlen(temp->url) - 1;
        free(temp->data);
        free(temp->url);
        free(temp);
    }
    pthread_mutex_unlock(&lock);
}

int add_cache_element(char* data,int size,char* url){
    pthread_mutex_lock(&lock);
    int element_size = size+1+strlen(url)+sizeof(cache_element);
    if(element_size>MAX_ELEMENT_SIZE){
        pthread_mutex_unlock(&lock);
        return 0;
    }
    while(cache_size+element_size>MAX_SIZE){
        remove_cache_element();
    }
    cache_element* element = (cache_element*)malloc(sizeof(cache_element));
    element->data = (char*)malloc(size+1);
    strcpy(element->data,data);
    element->url = (char*)malloc(1+strlen(url));
    strcpy(element->url,url);
    element->lru_time_track = time(NULL);
    element->next = head;
    element->len = size;
    head = element;
    cache_size += element_size;
    pthread_mutex_unlock(&lock);
    return 1;
}
