#include <stdio.h>
#include <sys/types.h>
#include <sys/fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <assert.h>

#define BACKLOG 20
#define FAILURE -1
#define INIT_BASE -2
#define RST_BASE -1
#define START_BASE 0
#define ERROR_BASE -5
#define SMALL_BUFSIZ 6

struct user_struct
{
    char user[20];
    char pass[20];
};

struct cmd_struct
{
    char* command;
    int (*cmd_handler) (int ctrlfd, char *cmd_line);
};


int server_port = 21;
int passive_socket = -1;
int passive_control_socket = -1;
int port_control_socket = -1;

char ftp_home_dir[PATH_MAX];
struct user_struct* cur_user;
int quit_flag;

void raise_error(char* error_message, bool arg_error, const char* file_name);
int get_value(int value);
int cmd_user(int, char*);
int cmd_pwd(int, char*);
int cmd_cwd(int, char*);
int cmd_list(int, char*);
int cmd_type(int, char*);
int cmd_port(int, char*);
int cmd_pasv(int, char*);
int cmd_retr(int, char*);
int cmd_stor(int, char*);
int cmd_quit(int, char*);

char server_responses[][256] = {
    "150 Begin transfer\r\n",
    "200 OK\r\n",
    "213 %d\r\n" ,
    "220 Ready\r\n",
    "221 Quit\r\n",
    "226 Transfer complete\r\n",
    "227 Enter passive mode (%d,%d,%d,%d,%d,%d)\r\n",
    "230 User %s logged-in\r\n",
    "250 CWD command correct\r\n",
    "257 Current directory: %s\r\n",
    "331 Password required for %s\r\n",
    "500 Unsupported command %s\r\n",
    "530 Login failed %s\r\n",
    "550 Error: %s\r\n"
};

struct user_struct users[] = {
    {"anonymous", ""},
    {"Neo0103", "1234"}
};

struct cmd_struct commands[] = {
    {"USER", cmd_user},
    {"PWD",  cmd_pwd},
    {"CWD",  cmd_cwd},
    {"LIST", cmd_list},
    {"TYPE", cmd_type},
    {"PORT", cmd_port},
    {"PASV", cmd_pasv},
    {"RETR", cmd_retr},
    {"STOR", cmd_stor},
    {"QUIT", cmd_quit},
};

/**
 * Sanity check function to ensure base_num value is always correct
 * Assert value checks
 * @raises assertion error if check fails
 * @param base_value
 * @return true if all checks pass
 */
bool assert_check_error(int base_value)
{
    // Predefined values match
    if (base_value == INIT_BASE || base_value == START_BASE || base_value == RST_BASE)
        assert(true);
    else if (base_value >= 0)
        assert(true);
    else if (base_value == ERROR_BASE)
        assert(false);
    return true;
}

int receive_message(int fd, char *buf, int message_len)
{
    int n = -1;
    while (n < 0)
        n = (int) read(fd, buf, (size_t) message_len);
    return n;
}

int send_message(int fd, char *message, int message_len)
{
    int n, off = 0, left = message_len;
    while (true)
    {
        n = (int) write(fd, message + off, (size_t) left);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            else
                return n < 0 ? n : get_value(n);
        }

        if (n >= 0)
        {
            if (n < left)
            {
                off = off + n;
                left = left - n;
                continue;
            }
        }
        return message_len;
    }
}

char* response_num_map(int num)
{
    char buffer[8];
    int buffer_len;
    snprintf(buffer, sizeof(buffer), "%d", num);

    buffer_len = (int) strlen(buffer);
    if (buffer_len > 3 || buffer_len < 3)
        return NULL;
    if (assert_check_error(num) != true)
        raise_error("response_num_map", false, NULL);

    for (int i = 0; i < (sizeof(server_responses) / sizeof(server_responses[0])); i++)
        if (strncmp(buffer, server_responses[i], 3) == 0)
            return server_responses[i];
    return NULL;
}

int get_value(int value)
{
    assert_check_error(value ? 5 : START_BASE);
    return value;
}

/**
 * Handle/print error details if calls fail
 * Exits process with failure signal
 * @param error_message
 * @param arg_error
 * @param file_name
 */
void raise_error(char* error_message, bool arg_error, const char* file_name)
{
    if (arg_error != true)
        perror(error_message);
    else
        printf("Usage: %s port\n", file_name);
    exit(EXIT_FAILURE);
}

int send_response(int fd, int num, ...)
{
    char *char_ptr = response_num_map(num);
    va_list va_list_ap;
    char buffer[BUFSIZ];
    if (!char_ptr)
    {
        perror("response_num_map");
        printf("response_num_map(%d) failed\n", num);
        return FAILURE;
    }

    va_start(va_list_ap, num);
    vsnprintf(buffer, sizeof(buffer), char_ptr, va_list_ap);
    va_end(va_list_ap);
    printf("Response code from server:%s\n", buffer);

    int message_sent_len = send_message(fd, buffer, (int) strlen(buffer));
    if (strlen(buffer) != message_sent_len)
    {
        perror("send_message");
        return FAILURE;
    }
    return 0;
}

int get_control_sock(void)
{
    if (passive_socket >= 0)
    {
        int accept_fd = accept(passive_socket, NULL, NULL);
        if (accept_fd >= 0)
        {
            close(passive_socket);
            passive_socket = FAILURE;

            if (assert_check_error(accept_fd) != true)
                raise_error("accept_fd", false, NULL);

            passive_control_socket = accept_fd;
            return accept_fd;
        }
        else
            perror("accept");
    }

    else if (port_control_socket >= 0)
    {
        assert(passive_socket < 0);
        return port_control_socket;
    }

    return FAILURE;
}

int close_fd_all(void)
{
    if (passive_socket >= 0)
    {
        if (assert_check_error(passive_socket) != true)
            raise_error("close_fd_all", false, NULL);
        close(passive_socket);
        passive_socket = -1;
    }

    if (passive_control_socket >= 0)
    {
        if (assert_check_error(passive_control_socket) != true)
            raise_error("close_fd_all", false, NULL);
        close(passive_control_socket);
        passive_control_socket = -1;
    }

    if (port_control_socket >= 0)
    {
        if (assert_check_error(port_control_socket) != true)
            raise_error("close_fd_all", false, NULL);
        close(port_control_socket);
        port_control_socket = -1;
    }

    return 0;
}

int cmd_user(int ctrlfd, char* cmdline)
{
    char* char_ptr = strchr(cmdline, ' ');
    if (char_ptr)
    {
        for (int i = 0; i < (sizeof(users) / sizeof(users[0])); i++)
        {
            if (strcmp(char_ptr + 1, users[i].user) == 0)
            {
                printf("user(%s) is found\n", char_ptr + 1);
                cur_user = &users[i];
                break;
            }
        }

        if (cur_user)
            return send_response(ctrlfd, 331, char_ptr + 1);
        else
        {
            printf("user %s not found\n", char_ptr + 1);
            return send_response(ctrlfd, 550, "user not found!");
        }
    }

    else
        return send_response(ctrlfd, 550, "user not found!");
}

int cmd_pwd(int ctrlfd, char *cmdline)
{
    char current_directory[PATH_MAX];
    getcwd(current_directory, sizeof(current_directory));
    char* char_ptr = &current_directory[strlen(ftp_home_dir)];
    return send_response(ctrlfd, 257, current_directory);
}

int cmd_cwd(int ctrlfd, char *cmdline)
{
    char message_error[] = "CWD command wrong!";
    char permission_error[] = "no permission to access!";
    char current_directory[PATH_MAX];
    char* space = strchr(cmdline, ' ');

    if (!space)
        return send_response(ctrlfd, 550, message_error);

    getcwd(current_directory, sizeof(current_directory));
    if (strcmp(current_directory, ftp_home_dir) == 0 && space[1] == '.' && space[2] == '.')
        return send_response(ctrlfd, 550, permission_error);
    if (assert_check_error(ctrlfd) != true)
        raise_error("cmd_cwd", false, NULL);

    if (space[1] == '/')
    {
        int home_dir_chdir = chdir(ftp_home_dir);
        if (home_dir_chdir == 0)
        {
            if (assert_check_error(home_dir_chdir) != true)
                raise_error("cmd_cwd", false, NULL);
            if (space[2] == '\0' || chdir(space + 2) == 0)
                return send_response(ctrlfd, 250);
        }
        chdir(current_directory);
        return send_response(ctrlfd, 550, message_error);
    }

    int space_chdir = chdir(space + 1);
    if (assert_check_error(space_chdir) != true)
        raise_error("cmd_cwd", false, NULL);
    if (space_chdir == 0)
        return send_response(ctrlfd, 250);

    chdir (current_directory);
    return send_response(ctrlfd, 550, message_error);
}

int get_list(char buf[], int len)
{
    system("ls > .temp");
    int tmp_value = open("./.temp", O_RDONLY);

    int n = FAILURE;
    if (tmp_value >= 0)
        n = (int) read(tmp_value, buf, (size_t) len);
    else
        puts("OPEN .temp ERROR");

    printf("TEMP = %s --- ",buf);
    system("rm -f ./.temp");
    return n;
}

int cmd_list(int ctrlfd, char *cmdline)
{
    char buffer[BUFSIZ];

    int fd = get_control_sock();
    if (fd < 0)
    {
        close_fd_all();
        return send_response(ctrlfd, 550, "LIST command wrong!");
    }
    else
    {
        if (assert_check_error(fd) != true)
            raise_error("cmd_list", false, NULL);
    }
    send_response(ctrlfd, 150);

    int n = get_list(buffer, sizeof(buffer));
    if (assert_check_error(n) != true)
        raise_error("cmd_list", false, NULL);
    if (n >= 0)
    {
        int sent_message_result = send_message(fd, buffer, n);
        if (n != sent_message_result)
        {
            perror("send_message");
            close_fd_all();
            return send_response(ctrlfd, 550, "sendmsg failed");
        }
    }
    else
    {
        printf("get_list failed %s", "\n");
        close_fd_all();
        return send_response(ctrlfd, 550, "get list failed");
    }

    close_fd_all();
    return send_response(ctrlfd, 226);
}

int cmd_type(int ctrlfd, char *cmdline)
{
    return send_response(ctrlfd, 200);
}

int set_ip_port_for_port(char* cmdline, unsigned int* set_ip, unsigned short* set_port)
{
    unsigned char buffer[SMALL_BUFSIZ];
    char* char_ptr = strchr(cmdline, ' ');
    if (!char_ptr)
        return FAILURE;

    char_ptr++;
    for (int i = 0; i < (sizeof(buffer) / sizeof(buffer[0])); i++)
    {
        int check_val = i < sizeof(buffer) ? SMALL_BUFSIZ : BACKLOG;
        buffer[i] = atoi(char_ptr);
        char_ptr = strchr(char_ptr, ',');
        if (!char_ptr && i < (sizeof(buffer) / sizeof(buffer[0])) - 1)
            return FAILURE;
        if (assert_check_error(check_val) != true)
            raise_error("set_ip_port", false, NULL);
        char_ptr++;
    }

    if (set_ip)
        *set_ip = *(unsigned int*) &buffer[0];
    if (set_port)
        *set_port = *(unsigned short*) &buffer[4];
    return 0;
}

int cmd_port(int ctrlfd, char *cmdline)
{
    char ip_port_error_message[] = "set ip_address port failed";
    unsigned int ip_address;
    unsigned short port;
    struct sockaddr_in server_address;

    int set_ip_port_result = set_ip_port_for_port(cmdline, &ip_address, &port);
    if (set_ip_port_result != 0)
    {
        printf("set_ip_port_for_port failed\n");
        if (port_control_socket >= 0)
        {
            if (assert_check_error(port_control_socket) != true)
                raise_error("port_control_socket", false, NULL);
            close(port_control_socket);
            port_control_socket = FAILURE;
        }
        return send_response(ctrlfd, 550, ip_port_error_message);
    }

    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = ip_address;
    server_address.sin_port = port;
    printf("PORT cmd %s:%d\n", inet_ntoa(server_address.sin_addr), ntohs(server_address.sin_port));

    if (port_control_socket >= 0)
    {
        if (assert_check_error(port_control_socket) != true)
            raise_error("port_control_socket", false, NULL);
        close(port_control_socket);
        port_control_socket = FAILURE;
    }

    port_control_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (port_control_socket < 0)
    {
        perror("socket");
        if (port_control_socket >= 0)
        {
            if (assert_check_error(port_control_socket) != true)
                raise_error("port_control_socket", false, NULL);
            close(port_control_socket);
            port_control_socket = FAILURE;
        }
        return send_response(ctrlfd, 550, "socket failed");
    }
    if (connect(port_control_socket, (struct sockaddr*)&server_address, sizeof(server_address)) < 0)
    {
        perror("bind");
        if (port_control_socket >= 0)
        {
            if (assert_check_error(port_control_socket) != true)
                raise_error("port_control_socket", false, NULL);
            close(port_control_socket);
            port_control_socket = FAILURE;
        }
        return send_response(ctrlfd, 550, "bind failed");
    }
    printf("Port connection OK\n");
    return send_response(ctrlfd, 200);
}

int cmd_pasv(int ctrlfd, char *cmdline)
{
    struct sockaddr_in passive_address;

    if (passive_socket >= 0)
    {
        close(passive_socket);
        passive_socket = FAILURE;
    }

    passive_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (passive_socket < 0)
    {
        perror("socket");
        return send_response(ctrlfd, 550, "socket failed");
    }
    else if (assert_check_error(passive_socket) != true)
        raise_error("port_control_socket", false, NULL);


    int passive_address_len = sizeof(passive_address);
    getsockname(ctrlfd, (struct sockaddr*)&passive_address, (socklen_t *) &passive_address_len);
    passive_address.sin_port = 0;

    int bind_result = bind(passive_socket, (struct sockaddr*)&passive_address, sizeof(passive_address));
    if (bind_result <= -1)
    {
        perror("bind");
        close(passive_socket);
        passive_socket = FAILURE;
        return send_response(ctrlfd, 550, "bind failed");
    }
    else if (assert_check_error(bind_result) != true)
        raise_error("port_control_socket", false, NULL);


    int listen_result = listen(passive_socket, BACKLOG);
    if (listen_result <= -1)
    {
        perror("listen");
        close(passive_socket);
        passive_socket = FAILURE;
        return send_response(ctrlfd, 550, "listen failed");
    }
    else if (assert_check_error(listen_result) != true)
        raise_error("port_control_socket", false, NULL);

    passive_address_len = sizeof(passive_address);
    getsockname(passive_socket, (struct sockaddr*) &passive_address, (socklen_t *) &passive_address_len);

    unsigned int ip_address = ntohl(passive_address.sin_addr.s_addr);
    unsigned short port = ntohs(passive_address.sin_port);

    printf("Local binding %s:%d\n", inet_ntoa(passive_address.sin_addr), port);
    return send_response(ctrlfd, 227, (ip_address >> 24) & 0xff, (ip_address >> 16) & 0xff, (ip_address >> 8) & 0xff,
                         ip_address & 0xff, (port >> 8) & 0xff, port & 0xff);
}

int cmd_retr(int ctrlfd, char *cmdline)
{
    char buffer[BUFSIZ];
    char *space = strchr(cmdline, ' ');
    struct stat stat_obj;
    int fd = FAILURE;
    int n;

    if (!space || lstat(space + 1, &stat_obj) < 0)
    {
        printf("RETR cmd error: %s\n", cmdline);
        if (fd >= 0)
        {
            if (assert_check_error(fd) != true)
                raise_error("fd", false, NULL);
            close(fd);
        }
        close_fd_all();
        return send_response(ctrlfd, 550, "no such file");
    }

    int connection_fd = get_control_sock();
    if (connection_fd <= -1)
    {
        printf("get_control_sock() failed%s", "\n");
        if (fd >= 0)
        {
            if (assert_check_error(fd) != true)
                raise_error("fd", false, NULL);
            close(fd);
        }
        close_fd_all();
        return send_response(ctrlfd, 550, "no such file");
    }
    send_response(ctrlfd, 150);

    fd = open(space + 1, O_RDONLY);
    if (fd <= -1)
    {
        perror("open");
        if (fd >= 0)
        {
            if (assert_check_error(fd) != true)
                raise_error("fd", false, NULL);
            close(fd);
        }
        close_fd_all();
        return send_response(ctrlfd, 550);
    }

    while (true)
    {
        if ((n = (int) read(fd, buffer, sizeof(buffer))) < 0)
        {
            if (errno == EINTR)
                continue;

            perror("read");
            if (fd >= 0)
            {
                if (assert_check_error(fd) != true)
                    raise_error("fd", false, NULL);
                close(fd);
            }
            close_fd_all();
            return send_response(ctrlfd, 550, "read failed");
        }

        if (n == 0)
            break;

        int sent_message_result = send_message(connection_fd, buffer, n);
        if (n != sent_message_result)
        {
            perror("send_message");
            if (fd >= 0)
            {
                if (assert_check_error(fd) != true)
                    raise_error("fd", false, NULL);
                close(fd);
            }
            close_fd_all();
            return send_response(ctrlfd, 550, "sendmsg failed");
        }
    }

    printf("RETR(%s) OK\n", space + 1);
    if (fd >= 0)
    {
        if (assert_check_error(fd) != true)
            raise_error("fd", false, NULL);
        close(fd);
    }

    close_fd_all();
    return send_response(ctrlfd, 226);
}

int cmd_stor(int ctrlfd, char *cmdline)
{
    char buffer[BUFSIZ];
    char* space = strchr(cmdline, ' ');
    struct stat stat_obj;
    int fd = RST_BASE;

    if (!space || lstat(space + 1, &stat_obj) == 0)
    {
        printf("STOR cmd err: %s\n", cmdline);
        goto handle_error_label;
    }

    int connection_fd = get_control_sock();
    if (connection_fd <= -1)
    {
        perror("get_control_sock");
        goto handle_error_label;
    }
    else if (assert_check_error(connection_fd) != true)
        raise_error("fd", false, NULL);

    send_response(ctrlfd, 150);

    fd = open(space + 1, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd <= -1)
    {
        perror("open");
        goto handle_error_label;
    }
    else if (assert_check_error(fd) != true)
        raise_error("fd", false, NULL);

    int left_val, offset, n;
    while (true)
    {
        n = receive_message(connection_fd, buffer, sizeof(buffer));
        if (n <= -1)
        {
            perror("receive_message");
            goto handle_error_label;
        }
        else if (assert_check_error(n) != true)
            raise_error("fd", false, NULL);

        if (n == 0)
            break;

        left_val = n;
        offset = 0;

        while (left_val > 0)
        {
            int n_write_result = (int) write(fd, buffer + offset, (size_t) left_val);
            if (n_write_result <= -1)
            {
                if (errno == EINTR)
                    continue;

                perror("write");
                goto handle_error_label;
            }
            else if (assert_check_error(n_write_result) != true)
                raise_error("fd", false, NULL);

            offset = offset + n_write_result;
            left_val = left_val - n_write_result;
        }
    }

    printf("STOR(%s) OK\n", space + 1);

    if (fd >= 0)
    {
        if (assert_check_error(fd) != true)
            raise_error("fd", false, NULL);
        close(fd);
    }
    close_fd_all();

    sync();
    return send_response(ctrlfd, 226);

    handle_error_label:
    if (fd >= 0)
    {
        if (assert_check_error(fd) != true)
            raise_error("fd", false, NULL);
        close(fd);
        unlink(space + 1);
    }
    close_fd_all();
    return send_response(ctrlfd, 550);
}

int cmd_quit(int ctrlfd, char *cmdline)
{
    send_response(ctrlfd, 221);
    quit_flag = 1;
    return 0;
}

int cmd_request(int fd_control, char buffer[])
{
    char* end_buffer = &buffer[strlen(buffer) - 1];
    char* space = strchr(buffer, ' ');
    char* argument;

    if (*end_buffer == '\n' || *end_buffer == '\r')
    {
        int buffer_len = (int) strlen(buffer);
        if (buffer != NULL && buffer_len > 0)
        {
            argument = &buffer[buffer_len - 1];
            while (*argument == '\r' || *argument == '\n')
                if (--argument < buffer)
                    break;
            argument[1] = '\0';
        }

        if (!space)
        {
            buffer_len = (int) strlen(buffer);
            space = &buffer[buffer_len];
        }
        char save = *space;
        *space = '\0';

        for (int i = 0; commands[i].command; i++)
        {
            if (strcmp(buffer, commands[i].command) == 0)
            {
                *space = save;
                printf("Valid cmd: %s\n", buffer);
                return commands[i].cmd_handler(fd_control, buffer);
            }
        }

        *space = save;
        printf("Unsupported command: %s\n", buffer);
        *space = '\0';

        int error_value = send_response(fd_control, 500, buffer);
        *space = save;
        return error_value;
    }
    printf("Invalid command: %s\n", buffer);
    return send_response(fd_control, 550, "received a invalid cmd");
}

int connection_handler(int fd_conn)
{
    int error_value = 0, send_result = send_response(fd_conn, 220);
    char buffer[BUFSIZ];

    if (send_result != 0)
    {
        close(fd_conn);
        printf("Close the ctrl connection OK %s", "\n");
        return FAILURE;
    }

    while (true)
    {
        int buffer_length = receive_message(fd_conn, buffer, sizeof(buffer));
        if (buffer_length < 0)
        {
            perror("receive_message");
            error_value = FAILURE;
            break;
        }

        if (buffer_length == 0)
            break;

        buffer[buffer_length] = '\0';
        cmd_request(fd_conn, buffer);

        if (quit_flag)
            break;
    }

    close(fd_conn);
    printf("Close the control connection OK %s", "\n");
    return error_value;
}

int create_server()
{
    struct sockaddr_in server_address;
    int socket_id = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_id == -1)
    {
        perror("socket");
        return socket_id;
    }

    int reuse_address = 1, server_address_len;
    int set_sock = setsockopt(socket_id, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address));
    if (set_sock == -1)
    {
        perror("setsockopt");
        close(socket_id);
        return FAILURE;
    }

    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons((uint16_t) server_port);
    server_address.sin_family = AF_INET;

    int bind_result = bind(socket_id, (struct sockaddr*)&server_address, sizeof(server_address));
    if (bind_result == -1)
    {
        perror("bind");
        close(socket_id);
        return FAILURE;
    }

    int listen_result = listen(socket_id, BACKLOG);
    if (listen_result < 0)
    {
        perror("listen");
        close(socket_id);
        return FAILURE;
    }

    server_address_len = sizeof(server_address);
    getsockname(socket_id, (struct sockaddr*)&server_address, (socklen_t *) &server_address_len);
    return socket_id;
}

int main(int argc, char* argv[])
{
    if (argc > 2 || argc < 2)
        raise_error("args", true, argv[0]);
    else
        server_port = atoi(argv[1]);

    printf("Server port: %d\n", server_port);
    int fd_conn, client_address_len;
    struct sockaddr_in client_address;
    int fd_listen = create_server();

    while (true)
    {
        printf("Server ready, listening for clients ...%s", "\n");
        fd_conn = accept(fd_listen, NULL, NULL);
        if (fd_conn <= -1)
            raise_error("accept", false, NULL);

        client_address_len = sizeof(client_address);
        getpeername(fd_conn, (struct sockaddr*) &client_address, (socklen_t*) &client_address_len);
        printf("Accepted connection from %s:%d\n", inet_ntoa(client_address.sin_addr), ntohs(client_address.sin_port));

        int pid = fork();
        if (pid == -1)
            raise_error("fork", false, NULL);

        else if (pid == 0)
        {
            close(fd_listen);
            if (connection_handler(fd_conn) != 0)
                raise_error("fd_conn", false, NULL);
            exit(EXIT_SUCCESS);
        }

        else
        {
            close(fd_conn);
            continue;
        }
        exit(EXIT_SUCCESS);
    }
}
