#include "include/network_session.h"

network_session::connection::stream_messenger::stream_messenger() :
	_session(nullptr),
	_connection(nullptr),
	_local_low_n_sent(0),
	_local_low_n_received(0),
	_remote_low_n_received(0),
	_last_ack_time(0),
	_last_resend_time(0) { }

void network_session::connection::stream_messenger::create(network_session* session, network_session::connection* connection, size_t packet_queue_buffer_size)
{
	_session = session;
	_connection = connection;

	_local_low_n_sent = 0;
	_local_low_n_received = 0;

	_remote_low_n_received = 0;

	uint64_t current_time = session->_timer.get_microseconds();
	_last_ack_time = current_time;
	_last_resend_time = current_time;

	_allocator.create(packet_queue_buffer_size);

	for (uint32_t i = 0; i < window_size; ++i)
	{
		_window[i].buffer = nullptr;
		_window[i].buffer_length = 0;
	}

	while (!_queue.empty())
	{
		_queue.pop();
	}
}

void network_session::connection::stream_messenger::receive_ack(uint8_t new_rnd, uint64_t current_time)
{
	// ensure the new_rnd has either remained the same or acknowledged some packets

	uint32_t dist_old_rnd = modulus_distance(_local_low_n_sent, _remote_low_n_received);
	uint32_t dist_new_rnd = modulus_distance(_local_low_n_sent, new_rnd);

	if (dist_new_rnd <= dist_old_rnd)
	{
		_last_ack_time = current_time;

		if (new_rnd < _remote_low_n_received)
		{
			for (uint32_t i = _remote_low_n_received; i <= stream_messenger::maximum_sequence_number; ++i)
			{
				uint32_t message_index = i % stream_messenger::window_size;

				_allocator.pop_front();
				_window[message_index] = packet();
			}
			for (uint32_t i = 0; i < new_rnd; ++i)
			{
				uint32_t message_index = i % stream_messenger::window_size;

				_allocator.pop_front();
				_window[message_index] = packet();
			}
		}
		else
		{
			for (uint32_t i = _remote_low_n_received; i < new_rnd; ++i)
			{
				uint32_t message_index = i % stream_messenger::window_size;

				_allocator.pop_front();
				_window[message_index] = packet();
			}
		}

		_remote_low_n_received = new_rnd;
	}
}
void network_session::connection::stream_messenger::receive_message(bit_stream& stream, uint64_t current_time)
{
	if (stream.size() >= 3)
	{
		uint8_t message_id = stream.fast_read<uint8_t>();

		receive_ack(stream.fast_read<uint8_t>(), current_time);

		if (message_id == _local_low_n_received)
		{
			++_local_low_n_received;
			_session->_handler->on_message_received(
				bit_stream(stream.seek(), stream.size() - stream.tell()),
				_connection->_remote_uuid
				);

			char reliable_ack[2];
			stream.attach(reliable_ack, 2);
			stream.fast_write<uint8_t>(message_type::stream_ack);
			stream.fast_write<uint8_t>(_local_low_n_received);

			_session->_socket.send(reliable_ack, 2, _connection->_remote_address);
		}
	}
}

void network_session::connection::stream_messenger::send(const char* buffer, const uint32_t length)
{
	packet p;
	p.buffer_length = length + 3;
	p.buffer = _allocator.push_back(p.buffer_length);

	memcpy(p.buffer + 3, buffer, length);

	_queue.push(p);
}

void network_session::connection::stream_messenger::update(uint64_t current_time)
{
	bit_stream stream;
	while (!_queue.empty() && modulus_distance(_local_low_n_sent, _remote_low_n_received) < stream_messenger::window_size)
	{
		// reset the resend time on the connection because we are sending a message

		_last_resend_time = current_time;

		// move the queued message to the window

		uint32_t message_index = _local_low_n_sent % stream_messenger::window_size;

		_window[message_index] = _queue.front();
		_queue.pop();

		// write the packet header and send it

		stream.attach(
			_window[message_index].buffer,
			_window[message_index].buffer_length
			);

		stream.fast_write<uint8_t>(message_type::stream);
		stream.fast_write<uint8_t>(_local_low_n_sent);
		stream.fast_write<uint8_t>(_local_low_n_received);

		_session->_socket.send(
			_window[message_index].buffer,
			_window[message_index].buffer_length,
			_connection->_remote_address
			);

		// advance the window forward, let it wrap around

		++_local_low_n_sent;
	}

	uint64_t time_since_last_resend = current_time - _last_resend_time;

	// resend messages if we have unacknowledged messages and haven't sent a reliable message in a while

	if (
		time_since_last_resend > network_session::resend_time &&
		modulus_distance(_local_low_n_sent, _remote_low_n_received) > 0
		)
	{
		_last_resend_time = current_time;

		if (_local_low_n_sent < _remote_low_n_received)
		{
			for (uint32_t i = _remote_low_n_received; i <= stream_messenger::maximum_sequence_number; ++i)
			{
				resend_message(i);
			}
			for (uint32_t i = 0; i < _local_low_n_sent; ++i)
			{
				resend_message(i);
			}
		}
		else
		{
			for (uint32_t i = _remote_low_n_received; i < _local_low_n_sent; ++i)
			{
				resend_message(i);
			}
		}
	}
}

void network_session::connection::stream_messenger::resend_message(uint32_t seq)
{
	uint32_t message_index = seq % stream_messenger::window_size;

	// we need to update the next desired field of the header, it may have changed

	bit_stream stream = _window[message_index].get_stream();
	stream.skip(2);
	stream.fast_write<uint8_t>(_local_low_n_received);

	_session->_socket.send(
		_window[message_index].buffer,
		_window[message_index].buffer_length,
		_connection->_remote_address
		);
}