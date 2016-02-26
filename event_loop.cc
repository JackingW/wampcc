#include "event_loop.h"
#include "rpc_man.h"
#include "SessionMan.h"
#include "WampTypes.h"
#include "Logger.h"

namespace XXX {

/* Constructor */
event_loop::event_loop(Logger *logptr)
  : __logptr(logptr),
    m_continue(true),
    m_thread(&event_loop::eventmain, this),
    m_rpcman(nullptr),
    m_sesman(nullptr),
    m_handlers( WAMP_MSGID_MAX ) /* initial handles are empty */
{
}

/* Destructor */
event_loop::~event_loop()
{
  this->push( 0 );
  m_thread.join();
}

void event_loop::set_rpc_man(rpc_man* r)
{
  m_rpcman = r;
}

void event_loop::set_session_man(SessionMan* sm)
{
  m_sesman = sm;
}

void event_loop::set_handler(unsigned int eventid, event_cb handler)
{
  if (eventid > m_handlers.size() )
  {
    _ERROR_("resizing handler vector for eventid " << eventid);
    m_handlers.resize( eventid+1 );
  }
  m_handlers[ eventid ] = handler;

}

// TODO: general threading concner here.  How do I enqure that any users of this
// EVL dont make a call into here once self has started into the destructor????
void event_loop::push(event* ev)
{
  if (ev == 0) m_continue = false;
  auto sp = std::shared_ptr<event>(ev);

  std::unique_lock<std::mutex> guard(m_mutex);
  m_queue.push_back( std::move(sp) );
  m_condvar.notify_one();
}


void event_loop::eventmain()
{

  /* A note on memory management of the event objects.  Once they are pushed,
   * they are stored as shared pointers.  This allows other parts of the code to
   * take ownership of the resource, if they so wish.
   */
  _INFO_( "event loop started" );
  while (m_continue)
  {
    std::vector< std::shared_ptr<event> > to_process;
    {
      std::unique_lock<std::mutex> guard(m_mutex);
      m_condvar.wait(guard,
                     [this](){ return !m_queue.empty() && m_queue.size()>0; } );
      to_process.swap( m_queue );
    }

    for (auto & ev : to_process)
    {
      if (ev == 0) return; // TODO: use a proper sentinel event

      bool error_caught = true;
      event_error wamperror("unknown");

      try
      {
        process_event( ev.get() );
        error_caught = false;
      }
      catch ( const event_error & er)
      {
        _ERROR_( "caught event_error error, uri: "<< er.error_uri<< ", what:" << er.what());
        /* only basic code in here, to remove risk of a throw while an exception
         * is already active. */
        wamperror = er;
      }
      catch ( const std::exception& ex)
      {
        _ERROR_( "caught exception during process_event: " << ex.what() ); // DJS
        /* only basic code in here, to remove risk of a throw while an exception
         * is already active. */
      }
      catch ( ... )
      {
        // TODO: cannot do much in here, because might throw
        _ERROR_( "caught unknown error" );
      }

      if (error_caught)
      {

        try
        {
          // TODO: do I want all errors to result in a reply message being sent?
          process_event_error( ev.get(), wamperror );
        }
        catch (std::exception& e)
        {
          _ERROR_( "failure while handing event error: " << e.what() );
        }
        catch ( ... )
        {

          // TODO: cannot do much in here, because might throw
        }
      }



    } // loop end

    // exception safe cleanup of events
    try
    {
      to_process.clear();
    }
    catch ( ... )
    {
      _ERROR_("caught error during cleanup of expired event objects");
    }
  }
}

//----------------------------------------------------------------------

void event_loop::process_event(event * ev)
{
  bool event_handled= true;
  switch ( ev->type )
  {
    case event::eNone :
      /* old style event */
      event_handled = false;
      break;
    case event::outbound_call_event :
    {
      // TODO: create a template for this, which will throw etc.
      outbound_call_event* ev2 = dynamic_cast<outbound_call_event*>(ev);
      process_outbound_call(ev2);
      break;
    }
    case event::outbound_response_event :
    {
      // TODO: create a template for this, which will throw etc. Will be a
      // series error if the cast failes.
      outbound_response_event * ev2 = dynamic_cast<outbound_response_event *>(ev);
      process_outbound_response( ev2 );
      break;
    }
    case event::outbound_message :
    {
      // TODO: create a template for this, which will throw etc. Will be a
      // series error if the cast failes.
      outbound_message * ev2 = dynamic_cast<outbound_message *>(ev);
      process_outbound_message( ev2 );
      break;
    }
    case event::session_state_event :
    {
      session_state_event * ev2 = dynamic_cast<session_state_event *>(ev);
      if (m_sesman)
        m_sesman->handle_event( ev2 );
      else
        throw std::runtime_error("no handler for session state event");  // TODO: should be a event exception?
      break;
    }
    case event::house_keeping :
    {
      if (m_sesman)
        m_sesman->handle_housekeeping_event( );
      break;
    }
    case event::tcp_connect_event :
    {
      tcp_connect_event * tcpev = (tcp_connect_event*) ev;
      if (tcpev->user_cb)
        tcpev->user_cb(tcpev->src.unique_id(), tcpev->status, tcpev->user_data);
      return;
    }
    default:
    {
      _ERROR_( "unsupported event type " << ev->type );

    }
  }

  if (event_handled) return;

  if (ev->mode == event::eInbound)
  {
    switch ( ev->msg_type )
    {
      case YIELD :
      {
        /* We have received a YIELD off a socket; needs to be routed to the
         * originator. */
        process_inbound_yield( ev );
        break;
      }
      case ERROR :
      {
        process_inbound_error( ev );
        break;
      }
      case CALL :
      {
        _ERROR_( "THIS CODE IS DEPCRECATED -- NOT SURE IF IT WAS USED" );
        // //process_event_InboundCall( e );
        // // TODO: put this back in
        // if (!m_rpcman) throw event_error(WAMP_URI_NO_SUCH_PROCEDURE);
        // m_rpcman->invoke_rpc(ev->ja);
        break;
      }
      case REGISTER :
      {
        if (!m_rpcman) throw event_error(WAMP_URI_NO_SUCH_PROCEDURE);

        // Register the RPC. Once this function has been called, we should
        // expect that requests can be sent immediately, so its important that
        // we immediately sent the registration ID to the peer, before requests
        // arrive.
        int registration_id = m_rpcman->handle_register_event(ev->src, ev->ja);

        jalson::json_array msg;
        msg.push_back( REGISTERED );
        msg.push_back( ev->ja[1] );
        msg.push_back( registration_id );
        m_sesman->send_to_session( ev->src, msg );

        break;
      }
      case HEARTBEAT :
        // TODO: handle ... should it even come here?
        break;
      case HELLO :
      case REGISTERED :
      case INVOCATION :
      case CHALLENGE :
      case AUTHENTICATE :
      {
        event_cb& cb = m_handlers[ ev->msg_type ];
        if (cb)
        {
          cb( ev );
        }
        else
        {
          _ERROR_( "no handler for message type " << ev->msg_type);
        }
        break;
      }
      default:
      {
        // TODO: probably should reply here
        std::ostringstream os;
        os << "msg type " << ev->msg_type << " not supported"; // DJS
        _ERROR_( os.str() );
        throw std::runtime_error(os.str());
      }
    }
  }
  else if (ev->mode == event::eOutbound)
  {
    _ERROR_( "not yet handling outbound events" );
  }
  else
  {
    _ERROR_("unhandled event");
  }

}

void event_loop::process_event_InboundCall(event* )
{
}


void event_loop::process_event_error(event* ev, event_error& er)
{

  if (er.msg_type != UNDEF)
  {
    /* new style error */
    jalson::json_array msg;
    msg.push_back( ERROR );
    msg.push_back( er.msg_type );
    msg.push_back( er.request_id );
    msg.push_back( jalson::json_object() );
    msg.push_back( er.error_uri );
    msg.push_back( jalson::json_array() );
    msg.push_back( jalson::json_object() );
    m_sesman->send_to_session( ev->src, msg );
    return;
  }

/*
    [
      ERROR,
      CALL,
      CALL.Request|id,
      Details|dict,
      Error|uri,
      Arguments|list,
      ArgumentsKw|dict
    ]
*/
  switch ( ev->msg_type )
  {


    case CALL :
    {
      jalson::json_array msg;
      msg.push_back( ERROR );
      msg.push_back( CALL );
//      msg.push_back( ev->ja[1]);   // TODO: put this back only when we check for its exsitence
      msg.push_back( jalson::json_object() );
      msg.push_back( er.error_uri );
      msg.push_back( jalson::json_array() );
      msg.push_back( jalson::json_object() );

      m_sesman->send_to_session( ev->src, msg );
      break;
    }
    case REGISTER :
    {
      jalson::json_array msg;
      msg.push_back( ERROR );
      msg.push_back( REGISTER );
      msg.push_back( ev->ja[1]);
      msg.push_back( jalson::json_object() );
      msg.push_back( er.error_uri );
      msg.push_back( jalson::json_array() );
      msg.push_back( jalson::json_object() );
      msg.push_back( "qazwsx" );

      m_sesman->send_to_session( ev->src, msg );
      break;
    }
    default:
    {
      THROW(std::runtime_error,
            "unsupported event type " << ev->msg_type );
    }
  }

}


struct Request_INVOCATION_CB_Data : public Request_CB_Data
{
  Request_INVOCATION_CB_Data()
    : cb_data( nullptr )
  {
  }
  std::string procedure;
  void * cb_data;  // TODO: just change to a outbound_request_type
};

//----------------------------------------------------------------------

void event_loop::process_outbound_call(outbound_call_event* ev)
{

  // TODO: use wamp error here ... say I AM NOT BROKER
  if (!m_rpcman) throw event_error(WAMP_URI_NO_SUCH_PROCEDURE);

  /* find the RPC we need to invoke */
  rpc_details rpcinfo = m_rpcman->get_rpc_details( ev->rpc_name );
  if (rpcinfo.registration_id == 0)
  {
    throw event_error(WAMP_URI_NO_SUCH_PROCEDURE);
  }

  // not good... we need a to a copy of the event for the later arrival of the
  // YIELD/ERROR respons.  Eventually I need to try to just steal the source
  // event.
  //outbound_call_event * copy = new outbound_call_event( *ev );

  // also not good ... need to create the request content data.  Is there way to
  // just use the source event object directly?
  //Request_INVOCATION_CB_Data* cb_data = new Request_INVOCATION_CB_Data(); // TODO: memleak?
  //cb_data->cb_data = copy;

  build_message_cb_v2 msg_builder2 = [&](int request_id)
    {
       /* WAMP spec.
          [
            INVOCATION,
            Request|id,
            REGISTERED.Registration|id,
            Details|dict
            CALL.Arguments|list,
            CALL.ArgumentsKw|dict
          ]
       */

      jalson::json_array msg;
      msg.push_back( INVOCATION );
      msg.push_back( request_id );
      msg.push_back( rpcinfo.registration_id );
      msg.push_back( jalson::json_object() );
      if (ev->args.args != nullptr)   // TODO: how the hell does this compile? Fix Jalson, and remove check.
      {
        msg.push_back( ev->args.args );
      }

      return std::pair< jalson::json_array, Request_CB_Data*> ( msg,
                                                                nullptr );

    };

  m_sesman->send_request( rpcinfo.sid, INVOCATION, ev->internal_req_id, msg_builder2);
}

//----------------------------------------------------------------------

void event_loop::process_inbound_error(event* e)
{

  Request_INVOCATION_CB_Data* request_cb_data
    = dynamic_cast<Request_INVOCATION_CB_Data*>( e->cb_data );

  if (request_cb_data != nullptr)
  {
    outbound_call_event* origev = ( outbound_call_event*)request_cb_data->cb_data;
    if (origev && origev->cb)
    {

      // TODO: create a generic callback function, which does all the exception
      // catch/log etc
      try
      {

        call_info info; // TODO: dfill in
        // TODO: should use an error callback
        rpc_args args;
        origev->cb(info, args, origev->cb_user_data);  /* TODO: take from network message */
      }
      catch(...)
      {
        // TODO: log exceptions here
      }
    }
  }
  else
  {
    _ERROR_( "error, no request_cb_data found\n" );
  }
}
//----------------------------------------------------------------------
void event_loop::process_inbound_yield(event* e)
{
  /* This handles a YIELD message received off a socket.  There are two possible
    options next.  Either route to the session which originated the CALL.  Or,
    if we can find a local callback function, invoke that.
   */


  // new .... see if we have an external handler
  event_cb& cb = m_handlers[ e->msg_type ];
  if (cb)
  {
    cb( e );
    return;
  }



  /*  NOTICE!!!

      This was the original approach for having a YIELD and translating to
      callback into user code. I.e., the callback was invoked from the event
      loop.  In the new approach, the callback is invoked from the
      client_service.

   */

  // Request_INVOCATION_CB_Data* request_cb_data
  //   = dynamic_cast<Request_INVOCATION_CB_Data*>( e->cb_data );

  // if (request_cb_data != nullptr)
  // {
  //   outbound_call_event* origev = ( outbound_call_event*)request_cb_data->cb_data;
  //   if (origev && origev->cb)
  //   {

  //     // TODO: create a generic callback function, which does all the exception
  //     // catch/log etc
  //     try
  //     {
  //       call_info info; // TODO: dfill in
  //       info.reqid = e->ja[1].as_uint();
  //       info.procedure = origev->rpc_name;

  //       rpc_args args;
  //       args.args    = e->ja[3]; // dont care about the type
  //       args.options = e->ja[2].as_object();  // TODO: need to pre-verify the message

  //       origev->cb(info, args, origev->cb_user_data); /* TODO: take from network message */
  //     }
  //     catch(...)
  //     {
  //       // TODO: log exceptions here
  //     }
  //   }
  //   else
  //   {
  //     _ERROR_( "cannot find any orig event for a received YIELD\n" );
  //   }
  // }
  // else
  // {
  //   _ERROR_( "error, no request_cb_data found" );
  // }
}

//----------------------------------------------------------------------

void event_loop::process_outbound_response(outbound_response_event* ev)
{
  /* Handle outbound response events.  Example flows coming through here are

     - YIELD
     - REGISTERED
     - ERROR

    Outbound means these these event are destined to end up sessions, and
    trigger an output IO event.

    TODO: add support here for REGISTERED, and see if we can remove the legacy
    implementation of REGISTERED.
   */

  build_message_cb_v4 msgbuilder;

  if (ev->response_type == YIELD)
  {
    msgbuilder = [ev](){
      jalson::json_array msg;
      msg.push_back(YIELD);
      msg.push_back(ev->reqid);
      msg.push_back(ev->options);
      if (ev->args.args.is_null() == false)
      {
        msg.push_back(ev->args.args);
      }
      return msg;
    };
  }
  else if (ev->response_type == ERROR)
  {
    msgbuilder = [ev](){
      jalson::json_array msg;
      msg.push_back(ERROR);
      msg.push_back(ev->request_type);
      msg.push_back(ev->reqid);
      msg.push_back(ev->options);
      msg.push_back(ev->error_uri);
      return msg;
    };
  }
  else
  {
    throw std::runtime_error("unknown response_type");
  }

  m_sesman->send_to_session(ev->destination, msgbuilder);

}

//----------------------------------------------------------------------

void event_loop::process_outbound_message(outbound_message* ev)
{
  m_sesman->send_to_session(ev->destination, ev->ja);
}

//----------------------------------------------------------------------

} // namespace XXX
