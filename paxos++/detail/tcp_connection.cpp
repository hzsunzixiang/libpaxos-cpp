#include <iostream>

#include <assert.h>
#include <boost/bind.hpp>

#include "util/debug.hpp"
#include "tcp_connection.hpp"

namespace paxos { namespace detail {


tcp_connection::tcp_connection (
   boost::asio::io_service & io_service)
   : socket_ (io_service)
{
}

/*! static */ tcp_connection::pointer 
tcp_connection::create (
   boost::asio::io_service &    io_service)
{
   return pointer (
      new tcp_connection (io_service));
}

void
tcp_connection::close ()
{
   socket_.close ();
}

boost::asio::streambuf &
tcp_connection::read_buffer ()
{
   return read_buffer_;
}

boost::asio::ip::tcp::socket &
tcp_connection::socket ()
{
   return socket_;
}

void
tcp_connection::write (
   std::string const &  message)
{
   /*!
     The problem with Boost.Asio is that it assumes that the buffer we send to async_write ()
     must stay alive until the callback to handle_write () has been called. Since there is no
     way we can guarantee that if we use the message object we get as a reference to this function,
     we have no alternative but to copy the data into our local buffer.

     But wait! What happends when write() is called when we are not yet done writing another
     block? In that case, we will just append that data onto the queue. The handle_write () function
     checks whether any data is still pending on the queue, and if so, ensures another async_write ()
     is called.
    */

   if (write_buffer_.empty () == true)
   {
      write_buffer_ = message;

      start_write ();
   }
   else
   {
      write_buffer_ += message;
   }
}

void
tcp_connection::start_write ()
{
   PAXOS_ASSERT (write_buffer_.empty () == false);

   boost::asio::async_write (socket_,
                             boost::asio::buffer (write_buffer_),
                             boost::bind (&tcp_connection::handle_write, 
                                          shared_from_this(),
                                          boost::asio::placeholders::error,
                                          boost::asio::placeholders::bytes_transferred));
}

void
tcp_connection::handle_write (
   boost::system::error_code const &    error,
   size_t                               bytes_transferred)
{
   PAXOS_DEBUG ("written " << bytes_transferred << " bytes to other end");

   write_buffer_ = write_buffer_.substr (bytes_transferred);

   /*!
     As discussed in the write () function, if we still have data on the buffer, ensure
     that data is also written.
    */
   if (write_buffer_.empty () == false)
   {
      start_write ();
   }
}




}; };
