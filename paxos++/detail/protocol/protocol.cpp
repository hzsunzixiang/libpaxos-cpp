
#include <vector>

#include <boost/ref.hpp>
#include <boost/bind.hpp>
#include <boost/foreach.hpp>
#include <boost/detail/endian.hpp>
#include <boost/asio/read.hpp>

#include "../../exception/exception.hpp"
#include "../../configuration.hpp"
#include "../util/debug.hpp"
#include "../util/conversion.hpp"
#include "../quorum.hpp"

#include "command.hpp"

#include "protocol.hpp"

namespace paxos { namespace detail { namespace protocol {

protocol::protocol (
   boost::asio::io_service &            io_service,
   detail::quorum &                     quorum)
   : io_service_ (io_service),
     heartbeat_timer_ (io_service_),
     quorum_ (quorum),
     handshake_ (*this),
     elect_leader_ (*this),
     announce_leadership_ (*this)
{
}

boost::asio::io_service &
protocol::io_service ()
{
   return io_service_;
}


detail::quorum &
protocol::quorum ()
{
   return quorum_;
}

void
protocol::bootstrap ()
{
   /*!
     Bootstrapping is as simple as starting a new health check, the system
     should automatically recover.
    */
   heartbeat ();
}

void
protocol::heartbeat ()
{
   /*!
     Perform handshake to see who's dead and who's alive.
    */
   handshake_.start ();


   if (quorum_.needs_new_leader () == true)
   {
      quorum_.reset_state ();
      elect_leader_.start ();
   }

   if (quorum_.we_are_the_leader () == true)
   {
      PAXOS_DEBUG ("we are the leader, ensure we have connections to all nodes in quorum!");
      announce_leadership_.start ();
   }


   /*!
     And perform a new heartbeat in N seconds
    */
   heartbeat_timer_.expires_from_now (
      boost::posix_time::milliseconds (paxos::configuration::heartbeat_interval));
   heartbeat_timer_.async_wait (
      boost::bind (&protocol::heartbeat, this));
}



void
protocol::new_connection (
   tcp_connection::pointer      connection)
{
   /*!
     Note that this keeps a copy of connection, so it does not die.
    */
   read_command (connection,
                 boost::bind (&protocol::handle_command,
                              this,
                              connection,
                              _1));
}


void
protocol::initiate_request (
   tcp_connection::pointer   connection,
   std::string const &       byte_array)
{
   command command;
   command.set_type (command::type_request_initiate);
   command.set_workload (byte_array);
   write_command (command,
                  connection);
}



void
protocol::handle_command (
   tcp_connection::pointer      connection,
   command const &              command)
{
   read_command_callback_type callback = 
      boost::bind (&protocol::handle_command,
                   this,
                   connection,
                   _1);

   switch (command.type ())
   {
         case command::type_handshake_start:
            handshake_.receive_handshake_start (connection, 
                                                command);
            break;

         case command::type_leader_claim:
            elect_leader_.receive_leader_claim (connection, 
                                                command);
            break;

         case command::type_leader_announce:
            announce_leadership_.receive_leader (connection, 
                                                 command);
            break;

         default:
            PAXOS_THROW (exception::protocol_error ());
            break;
   }

   read_command (connection, callback);
}


void
protocol::write_command (
   command const &                      command,
   detail::tcp_connection::pointer      destination)
{
   std::string binary_string = command::to_string (command);
   uint32_t size             = binary_string.size ();

   std::string buffer        = util::conversion::to_byte_array (size) + binary_string;

   destination->write (buffer);
}

void
protocol::read_command (
   tcp_connection::pointer                      connection,
   protocol::read_command_callback_type const & callback)
{
   boost::shared_array <char> buffer (new char[4]);

   boost::asio::async_read (
      connection->socket (), 
      boost::asio::buffer (buffer.get (), 4), 
      boost::bind (&protocol::read_command_parse_size,
                   this,

                   /*!
                     \warning this assumes connection still is valid when parse_command
                              is called. This can be dangerous! Make sure it does not
                              race with cleaning up the connection_pool!
                   */
                   connection,

                   _1,
                   _2,

                   buffer,

                   /*! 
                     This is EXTREMELY ugly, but since we completely lose our
                     stack at this point, we can't rely on output to remain valid
                     when our parse_command () function is called back.
                     
                     Our solution? Wrap the callback function in a shared_ptr.
                     It's ugly, and needs a more elegant solution.
                     
                     \todo Fix more elegantly. The real problem lies in the fact
                           that our callback is an indirection from parse_command. Perhaps
                           find a way to copy our output callback by value?
                   */
                   boost::shared_ptr <read_command_callback_type> (new read_command_callback_type (callback))));
}

void
protocol::read_command_parse_size (
   tcp_connection::pointer                              connection,
   boost::system::error_code const &                    error,
   size_t                                               bytes_transferred,
   boost::shared_array <char>                           bytes_buffer,
   boost::shared_ptr <read_command_callback_type>       callback)
{
   if (error)
   {
      PAXOS_WARN ("An error has occured while reading a command: " << error.message ());
      return;
   }

   PAXOS_ASSERT (bytes_transferred == 4);

   std::string bytes_raw (bytes_buffer.get (), 4);
   uint32_t    bytes = util::conversion::from_byte_array <uint32_t> (bytes_raw);

   boost::shared_array <char> command_buffer (new char[bytes]);   

   //! Now send a request for the amount of bytes we just parsed
   boost::asio::async_read (
      connection->socket (), 
      boost::asio::buffer (command_buffer.get (), bytes), 
      boost::bind (&protocol::read_command_parse_command,
                   this,
                   
                   connection,
                   _1,
                   _2,
                   command_buffer,
                   callback));
}

void
protocol::read_command_parse_command (
   tcp_connection::pointer                              connection,
   boost::system::error_code const &                    error,
   size_t                                               bytes_transferred,
   boost::shared_array <char>                           buffer,
   boost::shared_ptr <read_command_callback_type>       callback)
{
   /*!
     At this point, we have a command, so we can cancel any timeouts running on the connection.
    */
   connection->cancel_timeout ();

   if (error)
   {
      PAXOS_WARN ("An error has occured while reading a command: " << error.message ());
      return;
   }

   std::string byte_array (buffer.get (),
                           bytes_transferred);

   (*callback) (command::from_string (byte_array));
}


}; }; };
