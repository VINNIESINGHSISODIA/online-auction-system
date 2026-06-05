#ifndef ADMIN_H
#define ADMIN_H
#include "../common/models.h"
#include "server.h"
void handle_add_user    (int client_fd, const char *buf);
void handle_list_users  (int client_fd);
void handle_change_role (int client_fd, const char *buf);
void handle_delete_user (int client_fd, const char *buf);
void handle_modify_user (int client_fd, const char *buf);
#endif