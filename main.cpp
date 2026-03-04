#include "packet/Packet.h"
#include <cstdint>
#include <cstdlib>
#include <string>
#include <uv.h>
#include <vector>

static uv_udp_t server_socket = {};
static uint32_t server_port = 8888;
static uv_loop_t *loop = nullptr;

struct ipClient {
  std::string ip;
  uint16_t port;
  uint64_t timestamp;
  uint64_t heartbeat;
};

static std::vector<ipClient> ips;

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

  ips.push_back(ipClient{
      .ip = std::string(sender_ip),
      .port = sender_port,
      .timestamp = packet->timestamp,
      .heartbeat = packet->timestamp,
  });

  printf("Received %ld bytes from %s:%d: %.*s\n", nread, sender_ip, sender_port,
         (int)nread, buf->base);

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