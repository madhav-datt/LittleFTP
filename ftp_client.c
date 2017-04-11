#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <libgen.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define BUFFER_SIZE 1024
#define CREATION_REPLY 257
#define PASSIVEMODE 227
#define FILE_NOT_FOUND 550


struct sockaddr_in server;
char user[20];
char pass[20];
int data_port;

void errorReport(char* err_info) {
    printf("# %s\n", err_info);
    exit(-1);
}

void sendCommand(int sock_fd, const char* cmd, const char* info) {
    char buf[BUFFER_SIZE] = {0};
    strcpy(buf, cmd);
    strcat(buf, info);
    strcat(buf, "\r\n");
    // printf("COMMAND SENT= %s\n",buf);
    if (send(sock_fd, buf, strlen(buf), 0) < 0){
        printf("Errro\n");
        printf("Exiting..\n");
        exit(-1);
    }
}

int getReplyCode(int sockfd) {
    int r_code, bytes;
    char buf[BUFFER_SIZE] = {0};
    char nbuf[5] = {0};
    if ((bytes = read(sockfd, buf, BUFFER_SIZE - 2)) > 0) {
        r_code = atoi(buf);
        // printf("rcode: %d\n", r_code);
        printf("Buff: %s\n", buf);
        buf[bytes] = '\0';
    }
    else
        return -1;
    if (buf[3] == '-') {
        char* newline = strchr(buf, '\n');
        if (*(newline+1) == '\0') {
            while ((bytes = read(sockfd, buf, BUFFER_SIZE - 2)) > 0) {
                buf[bytes] = '\0';
                printf("%s", buf);
                if (atoi(buf) == r_code)
                    break;
            }
        }
    }
    if (r_code == PASSIVEMODE) {
        char* begin = strrchr(buf, ',')+1;
        char* end = strrchr(buf, ')');
        strncpy(nbuf, begin, end - begin);
        nbuf[end-begin] = '\0';
        data_port = atoi(nbuf);
        buf[begin-1-buf] = '\0';
        end = begin - 1;
        begin = strrchr(buf, ',')+1;
        strncpy(nbuf, begin, end - begin);
        nbuf[end-begin] = '\0';
        data_port += 256 * atoi(nbuf);
    }

    return r_code;
}

int find_i(char* cmd){
    int i = 0;
    while (i < strlen(cmd) && cmd[i] != ' ') i++;
    if (i == strlen(cmd)) {
        printf("Erronous Command: %s\n", cmd);
        return -1;
    }
    while (i < strlen(cmd) && cmd[i] == ' ') i++;
    if (i == strlen(cmd)) {
        printf("Erronous Command: %s\n", cmd);
        return -1;
    }
    return i;
}


void get(int sockfd, char* cmd) {
    int data_sock, bytes;
    FILE* file;
    char filename[BUFFER_SIZE], buf[BUFFER_SIZE];
    int i = find_i(cmd);
    strncpy(filename, cmd+i, strlen(cmd+i)+1);

    // sendCommand(sockfd, "TYPE ", "I");
    char temp[] = "TYPE I\r\n";
    if (send(sockfd, temp, strlen(temp), 0) < 0){
        printf("Error\n");
        return;
    }
    getReplyCode(sockfd);
    // sendCommand(sockfd, "PASV", "");
    char temp2[] = "PASV\r\n";
    if (send(sockfd, temp2, strlen(temp2), 0) < 0){
        printf("Error\n");
        return;
    }
    if (getReplyCode(sockfd) != PASSIVEMODE) {
        printf("Error!\n");
        return;
    }
    server.sin_port = htons(data_port);
    if ((data_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0){
        printf("Error\n");
        return;
    }

    if (connect(data_sock, (struct sockaddr*)&server, sizeof(server)) < 0){
        printf("Error, can't connect to server\n");
    }


    sendCommand(sockfd, "RETR ", filename);
    if (!(getReplyCode(sockfd) != FILE_NOT_FOUND)) {
        close(sockfd);
        return;
    }

    if ((file = fopen(filename, "wb")) == NULL) {
        printf("Error! File not found\n");
        close(sockfd);
        return;
    }
    bytes = read(data_sock, buf, BUFFER_SIZE);
    while (bytes>0){
        fwrite(buf, 1, bytes, file);
        bytes = read(data_sock, buf, BUFFER_SIZE);
    }
    fclose(file);
    close(data_sock);
    getReplyCode(sockfd);
}


void put(int sockfd, char* cmd) {
    FILE* file;
    int data_sock;
    char filename[BUFFER_SIZE], buf[BUFFER_SIZE];
    int i = find_i(cmd);
    strncpy(filename, cmd+i, strlen(cmd+i)+1);

    // sendCommand(sockfd, "PASV", "");
    char temp2[] = "PASV\r\n";
    if (send(sockfd, temp2, strlen(temp2), 0) < 0){
        printf("Error, file not found\n");
        return;
    }


    if (getReplyCode(sockfd) != PASSIVEMODE) {
        printf("Error!");
        return;
    }

    if ((file = fopen(filename, "rb")) == NULL) {
        printf("Error, file not found!\n");
        return;
    }
    server.sin_port = htons(data_port);
    if ((data_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        printf("Socket creation error\n");    
    }
    if (connect(data_sock, (struct sockaddr*)&server, sizeof(server)) < 0){
        printf("No connection to server\n");
        exit(-1);
    }
    // printf("Data connection successfully: %s:%d\n", inet_ntoa(server.sin_addr), ntohs(server.sin_port));
    sendCommand(sockfd, "STOR ", filename);
    if (getReplyCode(sockfd) == FILE_NOT_FOUND) {
        close(data_sock);
        fclose(file);
        return;
    }
    int bytes = fread(buf, 1, BUFFER_SIZE, file);
    while (bytes > 0){
        send(data_sock, buf, bytes, 0);
        bytes = fread(buf, 1, BUFFER_SIZE, file);
    }
    fclose(file);
    close(data_sock);
    getReplyCode(sockfd);
}



void pwd(int sockfd) {
    // sendCommand(sockfd, "PWD", "");
    char temp2[] = "PWD\r\n";
    if (send(sockfd, temp2, strlen(temp2), 0) < 0){
        printf("Error\n");
        printf("Exiting..\n");
        return;
    }
    if (getReplyCode(sockfd) != CREATION_REPLY){
        printf("Bad reply...");
        exit(-1);
    }
}

void do_cd(char* cmd){
    if(cmd[5] != ' '){
        printf("Error in cd command\n");
    }
    int i;
    for(i=5; i<strlen(cmd);i++){
        if(cmd[i]!=' ')
            break;
    }
    if(i==strlen(cmd)){
        printf("Error in cmd\n");
        return;
    }
    char dir[50];
    strncpy(dir, cmd+i, strlen(cmd)-i);
    dir[strlen(cmd)-i] = '\0';
    // printf("%s\n", dir);
    int cd_fail = chdir(dir);
    if(cd_fail){
        printf("Error, no directory named %s found\n", dir);
        return;
    }

}

void do_quit(int sockfd) {
    // sendCommand(sockfd, "QUIT", "");
    char temp2[] = "QUIT\r\n";
    if (send(sockfd, temp2, strlen(temp2), 0) < 0){
        printf("Error\n");
        printf("Exiting..\n");
        return;
    }
    // if (getReplyCode(sockfd) == CONTROL_CLOSE)
        // printf("Logout.\n");
}



void do_ls_server(int sockfd) {
    int data_sock, bytes;
    char buf[BUFFER_SIZE] = {0};
    // sendCommand(sockfd, "PASV", "");
    char temp2[] = "PASV\r\n";
    if (send(sockfd, temp2, strlen(temp2), 0) < 0){
        printf("Error\n");
        printf("Exiting..\n");
        return;
    }
    if (getReplyCode(sockfd) != PASSIVEMODE) {
        printf("Error!");
        return;
    }
    server.sin_port = htons(data_port);
    if ((data_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0){
        printf("Error\n");
        exit(-1);
    }
    if (connect(data_sock, (struct sockaddr*)&server, sizeof(server)) < 0){
        printf("Error\n");
        exit(-1);
    }
    // printf("Data connection successfully: %s:%d\n", inet_ntoa(server.sin_addr), ntohs(server.sin_port));

    sendCommand(sockfd, "LIST ", "-al");
    getReplyCode(sockfd);
    printf("\n");

    while ((bytes = read(data_sock, buf, BUFFER_SIZE - 2)) > 0) {
        buf[bytes] = '\0';
        printf("%s", buf);
    }
    printf("\n");
    close(data_sock);
    getReplyCode(sockfd);
}

void do_server_cd(int sockfd, char* cmd) {
    int i = 0;
    char buf[BUFFER_SIZE];
    i = find_i(cmd);
    strncpy(buf, cmd+i, strlen(cmd+i)+1);
    sendCommand(sockfd, "CWD ", buf);
    getReplyCode(sockfd);
}


void do_ls_client()
{
	char buf[5000];

	int temp, n;
    system("ls >.temp");
    temp = open("./.temp", O_RDONLY);
    if (temp < 0)
        puts ("OPEN .temp ERROR");
    else
        n = read(temp,buf,5000);
    buf[n] = '\0';
    printf("%s",buf);
    system("rm -f ./.temp");
}

/* Create a local socket given the server ip and port and bind to it */ 
int start(char* serverip, int serverport) {
    // struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(serverport);
    
    if ((server.sin_addr.s_addr = inet_addr(serverip)) < 0) {
        printf("The host is not valid, exiting...");
        exit(-1);
    }
    
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(sockfd < 0){
        printf("Socket creation encountered an error\n");
        printf("Exiting...\n");
        exit(-1);
    }

    int connect_var = connect(sockfd, (struct sockaddr*)&server, sizeof(server));
    if(connect_var < 0){
        printf("Error connecting to server\n");
        printf("Exiting...\n");
        exit(-1);
    }
    else{
        printf("Connected!\n");
    }

    // if (getReplyCode(sockfd) != SERVICE_READY)
    //     errorReport("Service Connect Error!");
    int flag = 1;
    char buff[BUFFER_SIZE] = {0};
    int bytes_read = read(sockfd, buff, BUFFER_SIZE - 2); 
    

    char cmd[BUFFER_SIZE];
    char dir_addr[BUFFER_SIZE];

    while (1) {
        printf("myftp> ");
        fgets(cmd, sizeof(cmd), stdin);
        cmd[strlen(cmd)-1] = '\0';
        if (strncmp(cmd, "fget", 3) == 0){
            get(sockfd, cmd);
        }
        else if (strncmp(cmd, "fput", 3) == 0){
            put(sockfd, cmd);
        }
        else if (strcmp(cmd, "servpwd") == 0){
            pwd(sockfd);
        }
        else if (strcmp(cmd, "servls") == 0){
            do_ls_server(sockfd);
        }
        else if (strncmp(cmd, "servcd",5) == 0){
            do_server_cd(sockfd, cmd);
        }
        else if (strcmp(cmd, "clipwd") == 0){
            getcwd(dir_addr, sizeof(dir_addr));
            printf("%s\n", dir_addr);
        }
        else if (strcmp(cmd, "clils") == 0){
            do_ls_client();
        }
        else if (strncmp(cmd, "clicd", 4) == 0){
            do_cd(cmd);
        }
        else if (strcmp(cmd, "clear") == 0){
            system("clear");
        }
        else if (strcmp(cmd, "quit") == 0){
            do_quit(sockfd);
            flag = 0;
        }
        else{
            printf("Invalid Command!\n");
        }

        if(flag==0)
            break;
    }
    close(sockfd);
    return 0;
}

char* get_serverip(char* argv[]){
    char* serverip = argv[1];
    return serverip;
}

int get_serverport(char* argv[]){
    int serverport = atoi(argv[2]);
    return serverport;
}

int main(int argc, char* argv[]) {
    if (argc < 3 || argc > 3) {
        printf("Please use as following:");
        printf(" %s <host> <port>\n", argv[0]);
        exit(-1);
    }
    char* serverip = get_serverip(argv);
    int serverport = get_serverport(argv);
    int valid_prog = start(serverip, serverport);
    if(valid_prog!=0){
        printf("Error encountered\n");
        exit(-1);
    }
}
