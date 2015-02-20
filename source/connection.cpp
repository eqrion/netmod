#include "include/network_session.h"

network_session::connection::connection() :
	_session(nullptr),
	_last_ping_time(0),
	_disconnected(false)
{
}
network_session::connection::connection(const connection& rhs) :
	_session(rhs._session),
	_remote_address(rhs._remote_address),
	_remote_uuid(rhs._remote_uuid),
	_last_ping_time(rhs._last_ping_time),
	_stream_messenger(rhs._stream_messenger),
	_reliable_messenger(rhs._reliable_messenger),
	_disconnected(rhs._disconnected)
{
	_stream_messenger.set_connection(this);
	_reliable_messenger.set_connection(this);
}
network_session::connection::connection(connection&& rhs) :
	_session(rhs._session),
	_remote_address(rhs._remote_address),
	_remote_uuid(rhs._remote_uuid),
	_last_ping_time(rhs._last_ping_time),
	_stream_messenger(std::move(rhs._stream_messenger)),
	_reliable_messenger(std::move(rhs._reliable_messenger)),
	_disconnected(rhs._disconnected)
{
	_stream_messenger.set_connection(this);
	_reliable_messenger.set_connection(this);
}
const network_session::connection& network_session::connection::operator=(connection rhs)
{
	swap(*this, rhs);
	return *this;
}

void network_session::connection::swap(connection& a, connection& b)
{
	std::swap(a._session, b._session);
	std::swap(a._remote_address, b._remote_address);
	std::swap(a._remote_uuid, b._remote_uuid);
	std::swap(a._last_ping_time, b._last_ping_time);
	std::swap(a._stream_messenger, b._stream_messenger);
	std::swap(a._reliable_messenger, b._reliable_messenger);
	std::swap(a._disconnected, b._disconnected);

	a._stream_messenger.set_connection(&a);
	b._stream_messenger.set_connection(&b);

	a._reliable_messenger.set_connection(&a);
	b._reliable_messenger.set_connection(&b);
}

void network_session::connection::create(network_session* session, const ip_address& remote_address, const uuid& remote_uuid, size_t stream_packet_queue_buffer_size, size_t reliable_packet_queue_buffer_size)
{
	_session = session;

	_remote_address = remote_address;
	_remote_uuid = remote_uuid;

	_last_ping_time = session->_timer.get_microseconds();

	_stream_messenger.create(session, this, stream_packet_queue_buffer_size);
	_reliable_messenger.create(session, this, reliable_packet_queue_buffer_size);

	_disconnected = false;
}

void network_session::connection::receive_message(packet* msg, uint64_t current_time)
{
	bit_stream stream = bit_stream(msg->buffer, msg->buffer_length);

	if (stream.size() < sizeof(uint8_t))
	{
		return;
	}
	uint8_t message_header = stream.fast_read<uint8_t>();

	switch (message_header)
	{
	case message_type::disconnecting:
	{
		_disconnected = true;
	}
	break;

	case message_type::ping:
	{
		if (stream.size() == 5)
		{
			char ping_response[5];
			stream.attach(ping_response, 5);

			stream.fast_write<uint8_t>(message_type::ping_response);
			stream.fast_write<uint8_t>(_stream_messenger.local_low_n_received());
			stream.fast_write<uint8_t>(_reliable_messenger.local_low_n_received());
			stream.fast_write<uint16_t>(_reliable_messenger.local_messages_received());

			_session->_socket.send(ping_response, 5, _remote_address);
		}
	}
	break;
	case message_type::ping_response:
	{
		if (stream.size() == 5)
		{
			_stream_messenger.receive_ack(stream.fast_read<uint8_t>(), current_time);

			uint8_t reliable_remote_lnr = stream.fast_read<uint8_t>();
			uint16_t reliable_remote_ms = stream.fast_read<uint16_t>();

			_reliable_messenger.receive_ack(reliable_remote_lnr, reliable_remote_ms, current_time);
		}
	}
	break;

	case message_type::stream:
	{
		_stream_messenger.receive_message(stream, current_time);
	}
	break;

	case message_type::stream_ack:
	{
		if (stream.size() == 2)
		{
			_stream_messenger.receive_ack(stream.fast_read<uint8_t>(), current_time);
		}
	}
	break;

	case message_type::reliable:
	{
		_reliable_messenger.receive_message(stream, current_time);
	}
	break;

	case message_type::reliable_ack:
	{
		if (stream.size() == 4)
		{
			uint8_t _remote_low_n_received = stream.fast_read<uint8_t>();
			uint16_t _remote_messages_received = stream.fast_read<uint16_t>();

			_reliable_messenger.receive_ack(
				_remote_low_n_received,
				_remote_messages_received,
				current_time
				);
		}
	}
	break;

	case message_type::unreliable:
	{
		_session->_handler->on_message_received(
			bit_stream(stream.seek(), stream.size() - stream.tell()),
			_remote_uuid
			);
	}
	break;
	}

}

void network_session::connection::send_unreliable(const char* buffer, const uint32_t length)
{
	_session->_socket.send(buffer, length, _remote_address);
}
void network_session::connection::send_stream(const char* buffer, const uint32_t length)
{
	_stream_messenger.send(buffer, length);
}
void network_session::connection::send_reliable(const char* buffer, const uint32_t length)
{
	_reliable_messenger.send(buffer, length);
}

void network_session::connection::update(uint64_t current_time)
{
	uint64_t time_since_last_ping = current_time - _last_ping_time;
	uint64_t time_since_last_stream_ack = current_time - _stream_messenger.last_ack_time();
	uint64_t time_since_last_reliable_ack = current_time - _reliable_messenger.last_ack_time();

	// disconnect from the remote if we haven't gotten an acknowledgment in a while

	if (time_since_last_stream_ack > network_session::timeout_time &&
		time_since_last_reliable_ack > network_session::timeout_time)
	{
		_disconnected = true;
		return;
	}

	// update stream messaging

	_stream_messenger.update(current_time);

	// update reliable messaging

	_reliable_messenger.update(current_time);

	// ping the remote if we haven't pinged them in a while

	if (time_since_last_ping > network_session::ping_time)
	{
		_last_ping_time = current_time;

		char ping_message[5];
		bit_stream stream(ping_message, 5);

		stream.fast_write<uint8_t>(message_type::ping);
		stream.fast_write<uint8_t>(_stream_messenger.local_low_n_received());
		stream.fast_write<uint8_t>(_reliable_messenger.local_low_n_received());
		stream.fast_write<uint16_t>(_reliable_messenger.local_messages_received());

		_session->_socket.send(ping_message, 5, _remote_address);
	}
}