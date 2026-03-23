/* echo_server_impl.hpp -- v1.0
   Server callback implementations */

#pragma once

#include "fserv/client_session.hpp"
#include <chrono>
#include <mutex>
#include <ncurses.h>
#include <set>
#include <stdexcept>
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
            std::scoped_lock l(access_lock_);
            err_ += n;
            print();
        }

        //! Adds to received messages count.
        //! @param n
        //!     Number of received messages to add
        void add_rx(int n)
        {
            std::scoped_lock l(access_lock_);
            rx_ += n;
            print();
        }

        //! Adds to sent replies count.
        //! @param n
        //!     Number of sent replies to add
        void add_tx(int n)
        {
            std::scoped_lock l(access_lock_);
            tx_ += n;
            print();
        }

        //! Sets the clients count.
        //! @param n
        //!     Number of clients
        void set_clients(int n)
        {
            std::scoped_lock l(access_lock_);
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
        std::set<std::uint64_t>& active_sessions,
        Stats* stats = nullptr)
    {
        if (auto itr = active_sessions.find(session.uuid());
            itr != active_sessions.end()) {
            // Internal error
            // Why are we receiving the same client again
            constexpr const char* kErr = "Bad new session invocation";
            throw std::runtime_error(kErr);
        }

        active_sessions.insert(session.uuid());

        // Maybe print new stats
        if (stats) {
            stats->set_clients(static_cast<int>(active_sessions.size()));
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
        std::set<std::uint64_t>& active_sessions,
        Stats* stats = nullptr)
    {
        // Erase client
        if (auto itr = active_sessions.find(session.uuid());
            itr != active_sessions.end()) {
            active_sessions.erase(itr);
        }

        // Maybe print new stats
        if (stats) {
            stats->add_err(1);
            stats->set_clients(static_cast<int>(active_sessions.size()));
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
        std::set<std::uint64_t>& active_sessions,
        Stats* stats = nullptr)
    {
        // Erase client
        if (auto itr = active_sessions.find(session.uuid());
            itr != active_sessions.end()) {
            active_sessions.erase(itr);
        }

        // Maybe print new stats
        if (stats) {
            stats->set_clients(static_cast<int>(active_sessions.size()));
        }
    }

    namespace detail {
        /* Helper */
        inline bool should_time_out(
            const unsigned stall_timeout_interval,
            const std::chrono::time_point<std::chrono::steady_clock>& then)
        {
            if (stall_timeout_interval == 0) {
                return false;
            }

            auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - then);
            return delta.count() > stall_timeout_interval;
        }

        template <typename ClientType>
        inline bool echo(fserv::ClientSession<ClientType>& session,
                         const char* data,
                         std::uint64_t size,
                         unsigned timeout_interval)
        {
            // Use a timeout
            auto then = std::chrono::steady_clock::now();

            std::uint64_t nbytes_total = 0;
            while (nbytes_total < size) {
                const char* curr = data + nbytes_total;
                std::uint64_t nbytes_written = 0;
                if (!session.write(
                        curr, size - nbytes_total, &nbytes_written)) {
                    return false;
                }

                if (nbytes_written == 0
                    && detail::should_time_out(timeout_interval, then)) {
                    return false;
                }

                // Keep working until all bytes written to peer
                if (nbytes_written > 0) {
                    // Successful write
                    nbytes_total += nbytes_written;
                    // Reset timer
                    then = std::chrono::steady_clock::now();
                }
            }

            return true;
        }
    } // namespace detail

    //! Handles echo client data received.
    //! @param session
    //!     Client session
    //! @param data
    //!     Received data
    //! @param size
    //!     Size of received data
    //! @param active_sessions
    //!     Set of active sessions
    //! @param timeout_interval
    //!     Timeout before an unsuccessful write attempt quits and terminates
    //!     the session
    //! @param stats
    //!     Pointer to stats object (optional)
    template <typename ClientType>
    inline bool handle_echo_client_data_received(
        fserv::ClientSession<ClientType>& session,
        const char* data,
        std::uint64_t size,
        std::set<std::uint64_t>& active_sessions,
        unsigned timeout_interval,
        Stats* stats = nullptr)
    {
        if (auto itr = active_sessions.find(session.uuid());
            itr == active_sessions.end()) {
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

        // Echo back data...
        if (!detail::echo(session, data, size, timeout_interval)) {
            active_sessions.erase(session.uuid());
            return false;
        }

        if (stats) {
            stats->add_tx(1);
        }

        return true;
    }
} // namespace app::impl
