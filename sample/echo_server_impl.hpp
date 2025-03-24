/* echo_server_impl.hpp -- v1.0
   Server callback implementations */

#pragma once

#include "fserv/client_session.hpp"
#include <cstdio>
#include <mutex>
#include <ncurses.h>
#include <set>
#include <string>

namespace app::impl {

    //! @class Stats
    /*! Prints stats to ncurses window */
    class Stats {
    public:
        //! Ctor.
        //! @param line
        //!     Line number
        explicit Stats(int line)
            : line_(line)
        {}

        //! Adds to errors count.
        //! @param n
        //!     Number of errors to add
        void add_err(int n)
        {
            std::lock_guard<std::mutex> l(access_lock_);
            err_ += n;
            print();
        }

        //! Adds to received messages count.
        //! @param n
        //!     Number of received messages to add
        void add_rx(int n)
        {
            std::lock_guard<std::mutex> l(access_lock_);
            rx_ += n;
            print();
        }

        //! Adds to sent replies count.
        //! @param n
        //!     Number of sent replies to add
        void add_tx(int n)
        {
            std::lock_guard<std::mutex> l(access_lock_);
            tx_ += n;
            print();
        }

        //! Sets the clients count.
        //! @param n
        //!     Number of clients
        void set_clients(int n)
        {
            std::lock_guard<std::mutex> l(access_lock_);
            clients_ = n;
            print();
        }
    private:
        /* # of clients */
        int clients_ = 0;
        /* # of connection errors */
        int err_ = 0;
        /* # of received messages */
        int rx_ = 0;
        /* # of sent replies */
        int tx_ = 0;
        /* Line number in ncurses window of output line */
        int line_ = 0;
        /* Primary access lock */
        std::mutex access_lock_;

        //! Helper function to print stats to ncurses window.
        void print() const
        {
            move(5, 0);
            clrtoeol();
            mvprintw(line_,
                     0,
                     "Conn: %d, Rx: %d, Tx: %d, Err: %d",
                     clients_,
                     rx_,
                     tx_,
                     err_);
            refresh();
        }
    };

    //! Handles new echo client.
    //! @param session
    //!     Client session
    //! @param active_sessions
    //!     Set of active sessions
    //! @param stats
    //!     Pointer to stats object (optional)
    template <typename ClientType>
    inline void handle_new_echo_client(
        fserv::ClientSession<ClientType>& session,
        std::set<int>& active_sessions,
        Stats* stats = nullptr)
    {
        auto itr = active_sessions.find(session.uuid());
        if (itr != active_sessions.end()) {
            // Internal error
            // Why are we receiving the same client again
            constexpr const char* kErr = "Bad new session invocation";
            throw std::string(kErr);
        }

        active_sessions.insert(session.uuid());

        // Maybe print new stats
        if (stats) {
            stats->set_clients(active_sessions.size());
        }
    }

    //! Handles echo client error.
    //! @param session
    //!     Client session
    //! @param active_sessions
    //!     Set of active sessions
    //! @param stats
    //!     Pointer to stats object (optional)
    template <typename ClientType>
    inline void handle_echo_client_error(
        fserv::ClientSession<ClientType>& session,
        std::set<int>& active_sessions,
        Stats* stats = nullptr)
    {
        // Erase client
        auto itr = active_sessions.find(session.uuid());
        if (itr != active_sessions.end()) {
            active_sessions.erase(itr);
        }

        // Maybe print new stats
        if (stats) {
            stats->add_err(1);
            stats->set_clients(active_sessions.size());
        }
    }

    //! Handles echo client closed.
    //! @param session
    //!     Client session
    //! @param active_sessions
    //!     Set of active sessions
    //! @param stats
    //!     Pointer to stats object (optional)
    template <typename ClientType>
    inline void handle_echo_client_closed(
        fserv::ClientSession<ClientType>& session,
        std::set<int>& active_sessions,
        Stats* stats = nullptr)
    {
        // Erase client
        auto itr = active_sessions.find(session.uuid());
        if (itr != active_sessions.end()) {
            active_sessions.erase(itr);
        }

        // Maybe print new stats
        if (stats) {
            stats->set_clients(active_sessions.size());
        }
    }

    //! Handles echo client data received.
    //! @param session
    //!     Client session
    //! @param data
    //!     Received data
    //! @param size
    //!     Size of received data
    //! @param active_sessions
    //!     Set of active sessions
    //! @param stats
    //!     Pointer to stats object (optional)
    template <typename ClientType>
    inline void handle_echo_client_data_received(
        fserv::ClientSession<ClientType>& session,
        const char* data,
        const int size,
        const std::set<int>& active_sessions,
        Stats* stats = nullptr)
    {
        auto itr = active_sessions.find(session.uuid());
        if (itr == active_sessions.end()) {
            // Sanity check
            // Why are we receiving an unregistered client
            constexpr const char* kErr
                = "Received data from non-existent client";
            throw std::string(kErr);
        }

        // Maybe print new stats
        if (stats) {
            stats->add_rx(1);
        }

        // Our stored client pointer is stale, erase and return
        // Echo back data...
        if (session.write(data, size) == size) {
            // Maybe print new stats
            if (stats) {
                stats->add_tx(1);
            }
        }

        session.rearm();
    }
} // namespace app::impl
