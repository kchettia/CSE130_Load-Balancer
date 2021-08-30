#include<err.h>
#include<arpa/inet.h>
#include<netdb.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<sys/socket.h>
#include<sys/types.h>
#include<unistd.h>
#include <stdbool.h>
#include <err.h>
#include <ctype.h>
#include "queue.h"
#include <pthread.h>
#include <sys/time.h>

#define MAXSIZE 4096

pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

pthread_mutex_t healthMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t healthCond = PTHREAD_COND_INITIALIZER;

pthread_mutex_t bestMutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct workerInfoObj{
    Queue client_Q;
    int* bestServer;
}WorkerObj;

typedef struct server_info{
    int port;
    bool status;
    int entries;
    int errors;
}ServerInfo;

typedef struct servers{
    int* bestServer;
    ServerInfo *servInfo;
    int numServers;
    int reqFreq;
}Servers;

bool isValidArg(char*,int);
bool argParser(int,char**,Servers*,int*, int*);
void *thread_func(void*);
void *health_thread(void*);
bool validateHealthCheck(char*,char*,char*,char*);
void updateServerStatus(ServerInfo*,int);
int bridge_connections(int, int);
void bridge_loop(int, int);
int client_connect(uint16_t);
void reqSignaller(Servers*,int);

int main(int argc, char** argv) {
  
    struct sockaddr_in server_addr;
    int clientPort,numThreads;
    WorkerObj workObj;
    Servers servers;
    memset(&workObj,0,sizeof(WorkerObj));
    memset(&servers,0,sizeof(servers));

    workObj.client_Q = newQueue();
    workObj.bestServer= malloc(sizeof(int));
    servers.bestServer=workObj.bestServer;

    if(argc<3){
        printf("Client and Server Port must be specified\n");
        return EXIT_FAILURE;
    }

    if(argParser(argc,argv,&servers,&clientPort,&numThreads)){
        
        pthread_t worker_threads[numThreads+1];
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(clientPort);
        server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        socklen_t addrlen = sizeof(server_addr);
            

        for(int i = 0 ; i<numThreads;++i){
            pthread_create(&worker_threads[i],NULL,thread_func,&workObj);
        }
        pthread_create(&worker_threads[numThreads],NULL,health_thread,&servers);
      
        int server_sockd = socket(AF_INET, SOCK_STREAM, 0);
     
        if (server_sockd < 0) 
            perror("socket");
        
    
        int enable = 1;
     
        int ret = setsockopt(server_sockd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  
        ret = bind(server_sockd, (struct sockaddr *) &server_addr, addrlen);

        ret = listen(server_sockd, SOMAXCONN); 

        if (ret < 0) 
            return EXIT_FAILURE;
        

        struct sockaddr client_addr;
        memset(&client_addr, 0, sizeof(client_addr));
        socklen_t client_addrlen=sizeof(client_addr);
        int *client_sockd;

        while(true){
            printf("[+] server is waiting...\n");
          
            client_sockd= malloc(sizeof(int));
            *client_sockd= accept(server_sockd, &client_addr, &client_addrlen);
           
            if (*client_sockd < 0) 
                perror("accept(Client Socket)");
            
            else{
                pthread_mutex_lock(&queue_mutex);
                enqueue(workObj.client_Q,client_sockd);
                pthread_mutex_unlock(&queue_mutex);
                pthread_cond_signal(&cond);
                reqSignaller(&servers,1);
            }
        }
    }
    free(servers.servInfo);
    freeQueue(&workObj.client_Q);
    free(workObj.bestServer);
    return EXIT_SUCCESS;
}
//health thread should conduct health check on each server
void *health_thread(void *servs){
    struct timespec timeToWait=(struct timespec){ 0 };
    struct timeval now=(struct timeval){ 0 };
    Servers servers= *(Servers*)servs;
    ServerInfo *servInfo=servers.servInfo;
    int serverFD[servers.numServers];
    int bestServ=0;

    while(true){

        for(int idx=0;idx<servers.numServers;++idx){
            servInfo[idx].status=true;
            serverFD[idx]=client_connect(servInfo[idx].port);
            if(serverFD[idx]==-1){
                servInfo[idx].status=false;
                continue;
            }
            
            updateServerStatus(&(servInfo[idx]),serverFD[idx]);
            
            if(servInfo[idx].status){
                if(servInfo[bestServ].status==false)
                    bestServ=idx;
                else if(servInfo[bestServ].entries>servInfo[idx].entries)
                    bestServ=idx;
                else if(servInfo[bestServ].entries==servInfo[idx].entries&&servInfo[bestServ].errors > servInfo[idx].errors)
                    bestServ=idx;
            }

            if(close(serverFD[idx])==-1)
                perror("Close error:");
        }
        
        pthread_mutex_lock(&bestMutex);
        if(servInfo[bestServ].status)
            *(servers.bestServer)=servInfo[bestServ].port;
        else
            *(servers.bestServer)=-1;
        pthread_mutex_unlock(&bestMutex);
        gettimeofday(&now,NULL);
        timeToWait.tv_sec = now.tv_sec+2;
        reqSignaller(&servers,2);

        pthread_mutex_lock(&healthMutex);
        pthread_cond_timedwait(&healthCond, &healthMutex, &timeToWait);
        pthread_mutex_unlock(&healthMutex);
    }


}
//send and recv healthcheck.Updates entries and errors
void updateServerStatus(ServerInfo *servInfo,int serv_sockd){
    fd_set set;
    struct timeval timeout;
    char buffer[300],message[300];
    char http[100],stat_code[100],stat_msg[100],cl_text[100],
        cl_number[100],entries[100],errors[100];
    timeout.tv_sec = 0;
    timeout.tv_usec = 500000;
    FD_ZERO (&set);
    FD_SET (serv_sockd, &set);
    dprintf(serv_sockd,"GET /healthcheck HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
    memset(buffer,0,sizeof(buffer));
    memset(message,0,sizeof(message));
    memset(http,0,sizeof(http));
    memset(stat_code,0,sizeof(stat_code));
    memset(stat_msg,0,sizeof(stat_msg));
    memset(cl_text,0,sizeof(cl_text));
    memset(cl_number,0,sizeof(cl_number));
    memset(entries,0,sizeof(entries));
    memset(errors,0,sizeof(errors));

   if(select(FD_SETSIZE, &set, NULL, NULL, &timeout)<=0){
       servInfo->status=false;
       return;
    }
    else if(FD_ISSET(serv_sockd, &set)){
       while(read(serv_sockd,buffer,sizeof(buffer)-1)>0){
           strcat(buffer,"\0");
           strcat(message,buffer);
           memset(buffer,0,sizeof(buffer));
       }
 
        if(sscanf(message,"%s %s %s\r\n %s %s\r\n\r\n %s\n%s",http,stat_code,stat_msg,cl_text,cl_number,entries,errors)!=7){
            servInfo->status=false;
            return;
        }
 
        else if(!(validateHealthCheck(http,stat_code,stat_msg,cl_text))){
           servInfo->status=false;
          return;
       }
        servInfo->entries=atoi(entries);
        servInfo->errors=atoi(errors);
   }

}

void *thread_func(void *workObject){
    WorkerObj workObj =*(WorkerObj*)workObject;
    int *client_sockd=NULL;
    int server_sockd;
    while(true){
        pthread_mutex_lock(&queue_mutex);
        if(isEmpty(workObj.client_Q)){
            pthread_cond_wait(&cond, &queue_mutex);
        }
        client_sockd = (int*)(back(workObj.client_Q));
        dequeue(workObj.client_Q);
        pthread_mutex_unlock(&queue_mutex);

        pthread_mutex_lock(&bestMutex);
        server_sockd=*(workObj.bestServer);
        pthread_mutex_unlock(&bestMutex);

        if(server_sockd>=0)
            server_sockd=client_connect(server_sockd);
        if(server_sockd==-1){
            perror("Failed to connect: ");
            dprintf(*client_sockd,"HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n");
        }
        else{
            bridge_loop(*client_sockd,server_sockd);
            if(close (server_sockd)==-1)
                perror("server close error");

        }
        if(close (*client_sockd)==-1)
            perror("client close error");
        if(client_sockd!=NULL)  
            free(client_sockd);
    
    }
    return 0;

}

bool argParser(int argc, char** argv,Servers *servers,int* clientPort, int *numThreads){
    int c;
    char* tempStr=NULL;
    *numThreads = 4;
    int portCounter=argc;
    bool thread_flag=false,req_flag=false,port_flag;
    servers->reqFreq=-1;
    
    if(argc<2)
        return false;

    while ((c = getopt (argc, argv, "N:R:")) != -1){
        
        switch (c){
        case 'N':
            if(!thread_flag){
                tempStr = optarg;
                thread_flag=isValidArg(tempStr,2);
                if(thread_flag)
                    *numThreads = atoi(tempStr);   
                else
                    return false; 
                portCounter--;
            }
            else{
                printf("More than one thread arg\n");
                return false; 
            }
            break;
        case 'R':
            if(!req_flag){
                tempStr = optarg;
                req_flag=isValidArg(tempStr,2);
                if(req_flag)
                    servers->reqFreq = atoi(tempStr);
                 else
                    return false; 
                 portCounter--;
            }
            else{
                printf("More than one request arg\n");
                return false; 
            }

            break;
        case '?':
            if (optopt == 'N' || (optopt == 'l')){
                 fprintf (stderr, "Option -%c requires an argument.\n", optopt);
                return false;
            }
            else {
                fprintf (stderr, "Unknown option `-%c'.\n", optopt);
                return false;
            }
        default:
            abort ();
        }
    }

    servers->servInfo=malloc(sizeof(ServerInfo)*(portCounter-1));
    int counter=0;
    for (; optind < argc; optind++){
        
        tempStr = argv[optind];
        port_flag=isValidArg(tempStr,1);
        if(port_flag==false){
            printf ("\nNon-option argument: %s\n", argv[optind]);
            return false;
        }
        else if(counter==0)
            *clientPort=atoi(tempStr);
        
        else
            servers->servInfo[counter-1].port=atoi(tempStr);

        ++counter;
    }

    if(counter<2 &&portCounter!=counter)
        return false;
    servers->numServers=counter-1;

    return true;
}

bool isValidArg(char* temp,int mode){
    for(size_t port_index =0;port_index<strlen(temp);port_index++){
        if(!isdigit(temp[port_index])){
             printf("Args should only have integers");
             return false;
        }
    }
    switch(mode){
        case 1:
            if(atoi(temp)>65535 || atoi(temp)<0){
                printf("Invalid Port / Args Specified\n");
                return false;
            }
           break; 
        case 2:
            if(atoi(temp)<0){
                printf("Thread arg should be an integer greater than 0\n");
                return false; 
            }
         break; 
         default:
            break;
    }
    return true;
}

bool validateHealthCheck(char* http,char* stat_code,char* stat_msg,char* cl_text){

    if(strcmp(stat_code,"200")!=0 || strcmp(stat_msg,"OK")!=0)
        return false;

    if(strcmp(http,"HTTP/1.1")!=0)
        return false;
    
    if(strcmp(cl_text,"Content-Length:")!=0)
        return false;

    return true;
}

int bridge_connections(int fromfd, int tofd) {
    char recvline[MAXSIZE];
    int n = recv(fromfd, recvline, MAXSIZE-1, 0);
    if (n < 0) {
        printf("connection error receiving\n");
        return -1;
    } else if (n == 0) {
        printf("receiving connection ended\n");
        return 0;
    }
    recvline[n] = '\0';
    n = send(tofd, recvline, n, 0);
    if (n < 0) {
        printf("connection error sending\n");
        return -1;
    } else if (n == 0) {
        printf("sending connection ended\n");
        return 0;
    }
    return n;
}

void bridge_loop(int sockfd1, int sockfd2) {
    fd_set set;
    struct timeval timeout;

    int fromfd, tofd;
    while(1) {
        // set for select usage must be initialized before each select call
        // set manages which file descriptors are being watched
        FD_ZERO (&set);
        FD_SET (sockfd1, &set);
        FD_SET (sockfd2, &set);

        // same for timeout
        // max time waiting, 5 seconds, 0 microseconds
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        // select return the number of file descriptors ready for reading in set
        switch (select(FD_SETSIZE, &set, NULL, NULL, &timeout)) {
            case -1:
                printf("error during select, exiting\n");
                return;
            case 0:
                printf("both channels are idle, waiting again\n");
                continue;
            default:
                if (FD_ISSET(sockfd1, &set)) {
                    fromfd = sockfd1;
                    tofd = sockfd2;
                } else if (FD_ISSET(sockfd2, &set)) {
                    fromfd = sockfd2;
                    tofd = sockfd1;
                } else {
                    dprintf(sockfd1,"HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n");
                    return;
                }
        }
        if (bridge_connections(fromfd, tofd) <= 0)
            return;
    }
}

int client_connect(uint16_t connectport) {
    int connfd;
    struct sockaddr_in servaddr;

    connfd=socket(AF_INET,SOCK_STREAM,0);
    if (connfd < 0){
        return -1;
    }
    memset(&servaddr, 0, sizeof servaddr);

    servaddr.sin_family=AF_INET;
    servaddr.sin_port=htons(connectport);

    /* For this assignment the IP address can be fixed */
    inet_pton(AF_INET,"127.0.0.1",&(servaddr.sin_addr));

    if(connect(connfd,(struct sockaddr *)&servaddr,sizeof(servaddr)) < 0){
        if(close(connfd)==-1)
            perror("Close error:");
        return -1;
    }
    return connfd;
}

void reqSignaller(Servers* servers,int mode){
    if(servers->reqFreq==-1)
        return;
    static int requests;
    pthread_mutex_lock(&healthMutex);
    switch(mode){
        case 1:
            ++requests;
            if(requests==servers->reqFreq){
                pthread_cond_signal(&healthCond);
                requests=0;
            }
            break;
        case 2:
            requests=0;
            break;
    }
    pthread_mutex_unlock(&healthMutex);
}
