#ifndef onyx_network_h
#define onyx_network_h

#include <stdint.h>
#include <stdio.h>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <WinSock2.h>
#include <Ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")

#if !defined(NETWORK_USE_IPV6)
#define NETWORK_USE_IPV4
#endif

class network_timer
{
public:
	network_timer()
	{
		QueryPerformanceFrequency(&_frequency);
	}

	uint64_t get_milliseconds()
	{
		LARGE_INTEGER current_time;
		QueryPerformanceCounter(&current_time);

		uint64_t quotient = current_time.QuadPart / _frequency.QuadPart;
		uint64_t remainder = current_time.QuadPart % _frequency.QuadPart;

		return quotient * 1000 + (remainder * 1000) / _frequency.QuadPart;
	}
	uint64_t get_microseconds()
	{
		LARGE_INTEGER current_time;
		QueryPerformanceCounter(&current_time);

		uint64_t quotient = current_time.QuadPart / _frequency.QuadPart;
		uint64_t remainder = current_time.QuadPart % _frequency.QuadPart;

		return quotient * 1000000 + (remainder * 1000000) / _frequency.QuadPart;
	}
	uint64_t get_nanoseconds()
	{
		LARGE_INTEGER current_time;
		QueryPerformanceCounter(&current_time);

		uint64_t quotient = current_time.QuadPart / _frequency.QuadPart;
		uint64_t remainder = current_time.QuadPart % _frequency.QuadPart;

		return quotient * 1000000000 + (remainder * 1000000000) / _frequency.QuadPart;
	}

private:
	LARGE_INTEGER _frequency;
};

static void print_wsa_error()
{
	auto last_error = WSAGetLastError();

	if (last_error == 10047)
	{
		__debugbreak();
	}

	printf("last error: %d\n", last_error);
}

class ip_address
{
public:
	bool resolve(const char* node_name, const char* port_number)
	{
		struct addrinfo* host_addr = nullptr;
		struct addrinfo hints;
		ZeroMemory(&hints, sizeof(hints));

#if defined(NETWORK_USE_IPV6)
		hints.ai_family = AF_INET6;
#else
		hints.ai_family = AF_INET;
#endif
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_protocol = IPPROTO_UDP;
		hints.ai_flags = 0;

		int addr_result = getaddrinfo(node_name, port_number, &hints, &host_addr);

		if (addr_result != 0)
		{
			printf("error resolving the host service ip_address: %s.\n", gai_strerrorA(addr_result));
			return false;
		}

#if defined(NETWORK_USE_IPV6)
		wsa_ip_address = *(struct sockaddr_in6*)host_addr->ai_addr;
#else
		wsa_ip_address = *(struct sockaddr_in*)host_addr->ai_addr;
#endif

		freeaddrinfo(host_addr);

		return true;
	}

#if defined(NETWORK_USE_IPV6)
	sockaddr_in6 wsa_ip_address;
#else
	sockaddr_in wsa_ip_address;
#endif
};

static bool operator==(const ip_address& a, const ip_address& b)
{
#if defined(NETWORK_USE_IPV6)
	return a.wsa_ip_address.sin6_family == b.wsa_ip_address.sin6_family && a.wsa_ip_address.sin6_port == b.wsa_ip_address.sin6_port &&
		a.wsa_ip_address.sin6_addr.u.Word[0] == b.wsa_ip_address.sin6_addr.u.Word[0] &&
		a.wsa_ip_address.sin6_addr.u.Word[1] == b.wsa_ip_address.sin6_addr.u.Word[1] &&
		a.wsa_ip_address.sin6_addr.u.Word[2] == b.wsa_ip_address.sin6_addr.u.Word[2] &&
		a.wsa_ip_address.sin6_addr.u.Word[3] == b.wsa_ip_address.sin6_addr.u.Word[3] &&
		a.wsa_ip_address.sin6_addr.u.Word[4] == b.wsa_ip_address.sin6_addr.u.Word[4] &&
		a.wsa_ip_address.sin6_addr.u.Word[5] == b.wsa_ip_address.sin6_addr.u.Word[5] &&
		a.wsa_ip_address.sin6_addr.u.Word[6] == b.wsa_ip_address.sin6_addr.u.Word[6] &&
		a.wsa_ip_address.sin6_addr.u.Word[7] == b.wsa_ip_address.sin6_addr.u.Word[7];
#else
	return a.wsa_ip_address.sin_family == b.wsa_ip_address.sin_family &&
		a.wsa_ip_address.sin_port == b.wsa_ip_address.sin_port &&
		a.wsa_ip_address.sin_addr.S_un.S_addr == b.wsa_ip_address.sin_addr.S_un.S_addr;
#endif
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
	static const int drop_rate = 4;

	bool create(const char* port_number, bool should_drop_packets = false)
	{
		destroy();

		drop_packets = should_drop_packets;

		struct addrinfo* host_addr = nullptr;
		struct addrinfo hints;
		ZeroMemory(&hints, sizeof(hints));

#if defined(NETWORK_USE_IPV6)
		hints.ai_family = AF_INET6;
#else
		hints.ai_family = AF_INET;
#endif
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_protocol = IPPROTO_UDP;
		hints.ai_flags = AI_PASSIVE;

		int addr_result = getaddrinfo(nullptr, port_number, &hints, &host_addr);

		if (addr_result != 0)
		{
			printf("error resolving a port to host the service.\n");
			return false;
		}

#if defined(NETWORK_USE_IPV6)
		SOCKET sock = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
#else
		SOCKET sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#endif

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
		if (!drop_packets || (rand() % drop_rate) != 0)
		{
			if (sendto(
				wsa_socket,
				buffer,
				length,
				0,
				(sockaddr*)&to.wsa_ip_address,
				sizeof(to.wsa_ip_address)
				) != length)
			{
				print_wsa_error();
				printf("an error occured in sending a udp_packet.\n");

				return false;
			}
		}

		return true;
	}
	bool try_receive(char* buffer, size_t buffer_capacity, size_t* amount_written, ip_address* from)
	{
		int from_len = sizeof(from->wsa_ip_address);

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

private:
	bool drop_packets;
	SOCKET wsa_socket;
};

#endif