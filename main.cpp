#include "MushRoom.h"
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <flat_map>
#include <ranges>
#include <string>
#include <utility>
#include <uv.h>
#include <vector>

static uv_udp_t server_socket = {};
static uint32_t server_port = 8888;
static uv_loop_t *loop = nullptr;

static const auto heartbeat_interval = 3;

static uv_timer_t fresh_client_timer;
// key:ip,port
static std::flat_map<std::pair<uint32_t, uint16_t>, NetworkClient> clients;

static bool sendUDP(uint8_t *data, size_t len, uint32_t ip, uint16_t port) {
  uv_udp_send_t *send_req = (uv_udp_send_t *)malloc(sizeof(uv_udp_send_t));
  sockaddr_in send_addr;
  send_addr.sin_family = AF_INET;
  send_addr.sin_port = htons(port);
  send_addr.sin_addr.s_addr = ip;

  uv_buf_t buf = uv_buf_init((char *)data, len);

  // 使用在 main 中初始化好的目标地址 send_addr
  auto r = uv_udp_send(
      send_req, &server_socket, &buf, 1, (const sockaddr *)&send_addr,
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

static void dispatch(const NetworkPacket *packet) {
  auto type = packet->type;
  auto sub_type = packet->sub_type;
  std::flat_set<std::pair<uint32_t, uint16_t>> cast_clients;
  // 先找到所有订阅该事件的人
  for (auto [key, client] : clients) {
    if (client.registers.contains(type)) {
      if (client.registers[type].contains(sub_type)) {
        cast_clients.insert(key);
      }
    }
  }
  if (packet->cast_type == CAST_UNICAST) {
    auto key = std::make_pair(packet->cast_ip, packet->cast_port);
    if (cast_clients.contains(key)) {
      cast_clients = {key};
    }
  }
  // 发送
  for (const auto &[ip, port] : cast_clients) {
    sendUDP((uint8_t *)(packet), sizeof(NetworkPacket) + packet->data_len, ip,
            port);
  }
}

static void clientFresh(uv_timer_t *handle) {
  std::vector<std::pair<uint32_t, uint16_t>> r;
  uint64_t now = static_cast<uint64_t>(time(nullptr));
  for (auto [k, v] : clients) {
    auto duration = now - v.heartbeat;
    if (duration >= heartbeat_interval * 3) {
      r.push_back(k);
    }
  }
  for (auto key : r) {
    auto client = clients[key];
    clients.erase(key);
    // 触发bye事件，广播
    NetworkByeResponse r = {.ip = key.first, .port = key.second};
    auto *packet = (NetworkPacket *)malloc(sizeof(NetworkPacket) + sizeof(r));
    packet->magic = 0x1234;
    packet->timestamp = static_cast<uint64_t>(time(nullptr));
    packet->type = PACKET_BYE_RESPONSE;
    packet->sub_type = clients[key].scene;
    packet->data_len = sizeof(r);
    memcpy(packet->data, &r, packet->data_len);
    dispatch(packet);
    free(packet);
  }
}

// 接收回调：当收到数据时被调用
static void on_recv(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
                    const sockaddr *addr, unsigned flags) {
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

  auto addr_in = (const struct sockaddr_in *)addr;

  NetworkPacket p;
  auto packet = (const NetworkPacket *)(buf->base);

  NetworkClient client = {
      .ip = (uint32_t)(addr_in->sin_addr.s_addr),
      .port = ntohs(addr_in->sin_port),
      .timestamp = packet->timestamp,
  };

  // dispatch
  switch (packet->type) {
  case PACKET_HELLO_REQUEST: {
    NetworkHelloResponse r = {.heartbeat_interval = heartbeat_interval};
    auto data_len = sizeof(NetworkHelloResponse);
    auto packet = (NetworkPacket *)malloc(sizeof(NetworkPacket) + data_len);
    packet->magic = 0x1234;
    packet->timestamp = static_cast<uint64_t>(time(nullptr));
    packet->type = PACKET_HELLO_RESPONSE;
    packet->data_len = data_len;
    memcpy(packet->data, &r, data_len);
    sendUDP((uint8_t *)(packet), sizeof(NetworkPacket) + packet->data_len,
            client.ip, client.port);
    auto key = std::make_pair(client.ip, client.port);
    clients.insert({key, client});
    free(packet);
    break;
  }
  case PACKET_HEARTBEAT_REQUEST: {
    auto key = std::make_pair(client.ip, client.port);
    clients[key].heartbeat = static_cast<uint64_t>(time(nullptr));
    break;
  }
  case PACKET_BYE_REQUEST: {
    auto key = std::make_pair(client.ip, client.port);
    clients.erase(key);
    break;
  }
  case PACKET_REGISTER_REQUEST: {
    auto r = (const NetworkRegisterRequest *)packet->data;
    auto key = std::make_pair(client.ip, client.port);
    clients[key].registers[r->type].insert(r->sub_type);
    break;
  }
  case PACKET_UNREGISTER_REQUEST: {
    auto r = (const NetworkRegisterRequest *)packet->data;
    auto key = std::make_pair(client.ip, client.port);
    clients[key].registers[r->type].erase(r->sub_type);
    break;
  }
  case PACKET_HOST_REQUEST: {
    auto r = (const NetworkHostRequest *)packet->data;
    for (auto [k, v] : clients) {
      r->scene;
    }
    break;
  }
  default: {
    break;
  }
  }

  dispatch(packet);

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
  // 创建定时器
  uv_timer_init(loop, &fresh_client_timer);
  auto interval = heartbeat_interval * 1000;
  uv_timer_start(&fresh_client_timer, clientFresh, 0, interval);

  uv_run(loop, UV_RUN_DEFAULT);
  return 0;
}