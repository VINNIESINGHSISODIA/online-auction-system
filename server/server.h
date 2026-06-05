#ifndef SERVER_H
#define SERVER_H

#include "../common/models.h"

int send_msg(int fd,
             MessageType type,
             int status,
             const void *payload,
             uint32_t length);

int recv_msg(int fd,
             MessageHeader *hdr_out,
             void *buf,
             size_t buf_size);

#endif