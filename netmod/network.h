#ifndef onyx_network_h
#define onyx_network_h

#include <stdint.h>
#include <stdio.h>
#include <WinSock2.h>
#include <Ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")

void print_wsa_error()
{
	printf("last error: %d\n", WSAGetLastError());
}

class ip_address
{
public:
	bool resolve(const char* node_name, const char* port_number)
	{
		struct addrinfo* host_addr = nullptr;
		struct addrinfo hints;
		ZeroMemory(&hints, sizeof(hints));

		hints.ai_family = AF_INET6;
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_protocol = IPPROTO_UDP;
		hints.ai_flags = 0;

		int addr_result = getaddrinfo(node_name, port_number, &hints, &host_addr);

		if (addr_result != 0)
		{
			printf("error resolving the host service ip_address.\n");
			return false;
		}

		wsa_ip_address = *(struct sockaddr_in6*)host_addr->ai_addr;

		freeaddrinfo(host_addr);

		return true;
	}

	sockaddr_in6 wsa_ip_address;
};

static bool operator==(const ip_address& a, const ip_address& b)
{
	return a.wsa_ip_address.sin6_family == b.wsa_ip_address.sin6_family && a.wsa_ip_address.sin6_port == b.wsa_ip_address.sin6_port &&
		a.wsa_ip_address.sin6_addr.u.Word[0] == b.wsa_ip_address.sin6_addr.u.Word[0] &&
		a.wsa_ip_address.sin6_addr.u.Word[1] == b.wsa_ip_address.sin6_addr.u.Word[1] &&
		a.wsa_ip_address.sin6_addr.u.Word[2] == b.wsa_ip_address.sin6_addr.u.Word[2] &&
		a.wsa_ip_address.sin6_addr.u.Word[3] == b.wsa_ip_address.sin6_addr.u.Word[3] &&
		a.wsa_ip_address.sin6_addr.u.Word[4] == b.wsa_ip_address.sin6_addr.u.Word[4] &&
		a.wsa_ip_address.sin6_addr.u.Word[5] == b.wsa_ip_address.sin6_addr.u.Word[5] &&
		a.wsa_ip_address.sin6_addr.u.Word[6] == b.wsa_ip_address.sin6_addr.u.Word[6] &&
		a.wsa_ip_address.sin6_addr.u.Word[7] == b.wsa_ip_address.sin6_addr.u.Word[7];
}
static bool operator!=(const ip_address& a, const ip_address& b)
{
	return !(a == b);
}

class udp_socket
{
public:
	udp_socket() : wsa_socket(INVALID_SOCKET) { }
	~udp_socket()
	{
		destroy();
	}

	static const int recv_buf_size = 1024 * 256;
	static const int send_buf_size = 1024 * 16;

	bool create(const char* port_number)
	{
		destroy();

		struct addrinfo* host_addr = nullptr;
		struct addrinfo hints;
		ZeroMemory(&hints, sizeof(hints));

		hints.ai_family = AF_INET6;
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_protocol = IPPROTO_UDP;
		hints.ai_flags = AI_PASSIVE;

		int addr_result = getaddrinfo(nullptr, port_number, &hints, &host_addr);

		if (addr_result != 0)
		{
			printf("error resolving a port to host the service.\n");
			return false;
		}

		SOCKET sock = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);

		if (sock == INVALID_SOCKET)
		{
			printf("error creating a socket.\n");
			freeaddrinfo(host_addr);
			return false;
		}

		int bind_result = ::bind(sock, host_addr->ai_addr, (int)host_addr->ai_addrlen);

		if (bind_result == SOCKET_ERROR)
		{
			printf("error binding the socket.\n");
			print_wsa_error();
			freeaddrinfo(host_addr);
			closesocket(sock);
			sock = INVALID_SOCKET;
			return false;
		}

		freeaddrinfo(host_addr);

		printf("successfully bound a socket.\n");

		int sock_opt = udp_socket::recv_buf_size;
		setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char *)& sock_opt, sizeof(sock_opt));
		sock_opt = 0;
		setsockopt(sock, SOL_SOCKET, SO_LINGER, (char *)& sock_opt, sizeof(sock_opt));
		sock_opt = udp_socket::send_buf_size;
		setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char *)& sock_opt, sizeof(sock_opt));

#ifdef _WIN32
		unsigned long non_blocking = 1;
		ioctlsocket(sock, FIONBIO, &non_blocking);
#else
		fcntl(rns2Socket, F_SETFL, O_NONBLOCK);
#endif

		wsa_socket = sock;

		return true;
	}
	void destroy()
	{
		if (wsa_socket != INVALID_SOCKET)
		{
			closesocket(wsa_socket);
			wsa_socket = INVALID_SOCKET;
		}
	}

	bool send(const char* buffer, uint32_t length, ip_address to)
	{
#if _DEBUG
		if (rand() % 3 == 0)
		{
			return false;
		}
#endif

		if (sendto(
			wsa_socket,
			buffer,
			length,
			0,
			(sockaddr*)&to.wsa_ip_address,
			sizeof(struct sockaddr_in6)
			) != length)
		{
			print_wsa_error();
			printf("an error occured in sending a udp_packet.\n");

			return false;
		}
		return true;
	}
	bool try_receive(char* buffer, size_t buffer_capacity, size_t* amount_written, ip_address* from)
	{
		int from_len = sizeof(struct sockaddr_in6);

		int result =
			recvfrom(
			wsa_socket,
			buffer,
			buffer_capacity,
			0,
			(sockaddr*)&from->wsa_ip_address,
			&from_len
			);

		if (result == SOCKET_ERROR)
		{
			return false;
		}
		else
		{
			*amount_written = result;
			return true;
		}
	}
	
	SOCKET wsa_socket;
};

#endif