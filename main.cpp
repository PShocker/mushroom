#include "packet/MushRoom.h"
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <string>
#include <uv.h>
#include <vector>

static uv_udp_t server_socket = {};
static uint32_t server_port = 8888;
static uv_loop_t *loop = nullptr;

static std::vector<ipClient> clients;

static bool sendUDP(const uint8_t *data, size_t length, ipClient &client) {
  // 3. 准备目标地址（用于发送）
  uv_udp_send_t *send_req = (uv_udp_send_t *)malloc(sizeof(uv_udp_send_t));
  sockaddr_in send_addr = {};
  uv_ip4_addr(client.ip.c_str(), client.port, &send_addr);

  uv_buf_t buf = uv_buf_init((char *)data, length);

  // 使用在 main 中初始化好的目标地址 send_addr
  auto r = uv_udp_send(
      send_req, &server_socket, &buf, 1, (const struct sockaddr *)&send_addr,
      [](uv_udp_send_t *req, int status) {
        if (status < 0) {
          fprintf(stderr, "Send error: %s\n", uv_strerror(status));
        }
        free(req); // 释放发送请求
      });
  if (r < 0) {
    fprintf(stderr, "uv_udp_send error: %s\n", uv_strerror(r));
    free(send_req);
    return false;
  }
  return true;
}
// 接收回调：当收到数据时被调用
static void on_recv(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
                    const struct sockaddr *addr, unsigned flags) {
  if (nread < 0) {
    fprintf(stderr, "Read error: %s\n", uv_err_name(nread));
    uv_close((uv_handle_t *)handle, NULL);
    free(buf->base);
    return;
  }

  if (nread == 0) {
    // 没有数据或收到空包
    free(buf->base);
    return;
  }

  char sender_ip[17] = {0};
  uint16_t sender_port = 0;
  if (addr) {
    uv_ip4_name((const struct sockaddr_in *)addr, sender_ip, 16);
    sender_port = ntohs(((const struct sockaddr_in *)addr)->sin_port);
  }
  NetworkPacket p;
  auto packet = (const NetworkPacket *)(buf->base);

  ipClient client = {
      .ip = std::string(sender_ip),
      .port = sender_port,
      .timestamp = packet->timestamp,
      .heartbeat = packet->timestamp,
  };

  // dispatch
  switch (packet->type) {
  case PACKET_HELLO_REQUEST: {
    NetworkHelloResponse r = {.heartbeat_interval = 5};
    NetworkPacket pack = {
        .magic = 0x1234,
        .timestamp = static_cast<uint64_t>(time(nullptr)),
        .type = PACKET_HELLO_RESPONSE,
        .data_len = sizeof(r),
    };
    memcpy(pack.data, &r, pack.data_len);
    sendUDP((const uint8_t *)(&pack), sizeof(pack) + pack.data_len, client);
    break;
  }
  case PACKET_ENTER_REQUEST: {
    std::string ips;
    for (uint32_t i = 0; i < clients.size(); i++) {
      auto &c = clients[i];
      ips += c.ip + ":" + std::to_string(c.port);
      if (i < clients.size() - 1) {
        ips += ",";
      }
    }
    // 正确的方式：动态分配足够空间
    size_t total_size = sizeof(NetworkPacket) + ips.length();
    NetworkPacket *pack = (NetworkPacket *)malloc(total_size);
    pack->magic = 0x1234;
    pack->timestamp = static_cast<uint64_t>(time(nullptr));
    pack->type = PACKET_ENTER_RESPONSE;
    pack->data_len = (uint16_t)ips.length();
    memcpy(pack->data, ips.c_str(), pack->data_len);
    sendUDP((const uint8_t *)(pack), total_size, client);
    clients.push_back(client);
    free(pack);
    break;
  }
  default: {
    break;
  }
  }

  free(buf->base); // 释放由 alloc_cb 分配的内存
}

// 分配缓冲区回调
static void alloc_cb(uv_handle_t *handle, size_t suggested_size,
                     uv_buf_t *buf) {
  buf->base = (char *)malloc(suggested_size);
  buf->len = suggested_size;
}

int main(int argc, char *argv[]) {
  loop = uv_default_loop();
  // 1. 创建UDP句柄
  uv_udp_init(loop, &server_socket);

  // 绑定到 IPv4 和 IPv6
  struct sockaddr_in recv_addr;
  uv_ip4_addr("0.0.0.0", server_port, &recv_addr);
  auto r = uv_udp_bind(&server_socket, (const struct sockaddr *)&recv_addr,
                       UV_UDP_REUSEADDR);
  if (r < 0) {
    std::abort();
  }
  // 4. 开始接收数据
  r = uv_udp_recv_start(&server_socket, alloc_cb, on_recv);
  if (r < 0) {
    fprintf(stderr, "Recv start error: %s\n", uv_err_name(r));
    std::abort();
  }
  printf("Started receiving on port %d...\n", server_port);
  uv_run(loop, UV_RUN_DEFAULT);
  return 0;
}