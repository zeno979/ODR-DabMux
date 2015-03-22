/*
   Copyright (C) 2009 Her Majesty the Queen in Right of Canada (Communications
   Research Center Canada)

   Copyright (C) 2014, 2015
   Matthias P. Braendli, matthias.braendli@mpb.li

    http://www.opendigitalradio.org

   A TCP Socket server that serves state information and statistics for
   monitoring purposes, and also serves the internal configuration
   property tree.
   */
/*
   This file is part of ODR-DabMux.

   ODR-DabMux is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as
   published by the Free Software Foundation, either version 3 of the
   License, or (at your option) any later version.

   ODR-DabMux is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with ODR-DabMux.  If not, see <http://www.gnu.org/licenses/>.
   */

#include <errno.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <limits>
#include <sstream>
#include <ctime>
#include <boost/thread.hpp>
#include "ManagementServer.h"
#include "Log.h"

ManagementServer& get_mgmt_server()
{
    static ManagementServer mgmt_server;

    return mgmt_server;

    /* Warning, do not use the mgmt_server in the destructor
     * of another global object: you don't know which one
     * gets destroyed first
     */
}

void ManagementServer::registerInput(InputStat* is)
{
    boost::mutex::scoped_lock lock(m_statsmutex);

    std::string id(is->get_name());

    if (m_inputStats.count(id) == 1) {
        etiLog.level(error) <<
            "Double registration in MGMT Server with id '" <<
            id << "'";
        return;
    }

    m_inputStats[id] = is;
}

void ManagementServer::unregisterInput(std::string id)
{
    boost::mutex::scoped_lock lock(m_statsmutex);

    if (m_inputStats.count(id) == 1) {
        m_inputStats.erase(id);
    }
}


bool ManagementServer::isInputRegistered(std::string& id)
{
    boost::mutex::scoped_lock lock(m_statsmutex);

    if (m_inputStats.count(id) == 0) {
        etiLog.level(error) <<
            "Management Server: id '" <<
            id << "' does was not registered";
        return false;
    }
    return true;
}

std::string ManagementServer::getStatConfigJSON()
{
    boost::mutex::scoped_lock lock(m_statsmutex);

    std::ostringstream ss;
    ss << "{ \"config\" : [\n";

    std::map<std::string,InputStat*>::iterator iter;
    int i = 0;
    for(iter = m_inputStats.begin(); iter != m_inputStats.end();
            ++iter, i++)
    {
        std::string id = iter->first;

        if (i > 0) {
            ss << ", ";
        }

        ss << " \"" << id << "\" ";
    }

    ss << "] }\n";

    return ss.str();
}

std::string ManagementServer::getValuesJSON()
{
    boost::mutex::scoped_lock lock(m_statsmutex);

    std::ostringstream ss;
    ss << "{ \"values\" : {\n";

    std::map<std::string,InputStat*>::iterator iter;
    int i = 0;
    for(iter = m_inputStats.begin(); iter != m_inputStats.end();
            ++iter, i++)
    {
        const std::string& id = iter->first;
        InputStat* stats = iter->second;

        if (i > 0) {
            ss << " ,\n";
        }

        ss << " \"" << id << "\" : ";
        ss << stats->encodeValuesJSON();
        stats->reset();
    }

    ss << "}\n}\n";

    return ss.str();
}

std::string ManagementServer::getStateJSON()
{
    boost::mutex::scoped_lock lock(m_statsmutex);

    std::ostringstream ss;
    ss << "{\n";

    std::map<std::string,InputStat*>::iterator iter;
    int i = 0;
    for(iter = m_inputStats.begin(); iter != m_inputStats.end();
            ++iter, i++)
    {
        const std::string& id = iter->first;
        InputStat* stats = iter->second;

        if (i > 0) {
            ss << " ,\n";
        }

        ss << " \"" << id << "\" : ";
        ss << stats->encodeStateJSON();
        stats->reset();
    }

    ss << "}\n";

    return ss.str();
}

void ManagementServer::restart()
{
    m_restarter_thread = boost::thread(&ManagementServer::restart_thread,
            this, 0);
}

// This runs in a separate thread, because
// it would take too long to be done in the main loop
// thread.
void ManagementServer::restart_thread(long)
{
    m_running = false;

    if (m_listenport) {
        m_thread.interrupt();
        m_thread.join();
    }

    m_thread = boost::thread(&ManagementServer::serverThread, this);
}

void ManagementServer::serverThread()
{
    using boost::asio::ip::tcp;

    m_running = true;
    m_fault = false;

    while (m_running) {
        m_io_service.reset();

        tcp::acceptor acceptor(m_io_service, tcp::endpoint(
                    boost::asio::ip::address::from_string("127.0.0.1"),
                    m_listenport) );


        // Add a job to start accepting connections.
        boost::shared_ptr<tcp::socket> socket(
                new tcp::socket(acceptor.get_io_service()));

        // Add an accept call to the service.  This will prevent io_service::run()
        // from returning.
        etiLog.level(warn) << "MGMT: Waiting on connection";
        acceptor.async_accept(*socket,
                boost::bind(&ManagementServer::handle_accept,
                    this,
                    boost::asio::placeholders::error,
                    socket,
                    boost::ref(acceptor)));

        // Process event loop.
        m_io_service.run();
    }

    m_fault = true;
}

void ManagementServer::handle_accept(
                const boost::system::error_code& boost_error,
                boost::shared_ptr< boost::asio::ip::tcp::socket > socket,
                boost::asio::ip::tcp::acceptor& acceptor)
{
    if (boost_error)
    {
        etiLog.level(error) << "MGMT: Error accepting connection";
        return;
    }

    std::stringstream welcome_msg;

    welcome_msg <<
        "{ \"service\": \"" <<
        PACKAGE_NAME << " " <<
#if defined(GITVERSION)
        GITVERSION <<
#else
        PACKAGE_VERSION <<
#endif
        " MGMT Server\" }\n";

    try {
        etiLog.level(info) << "RC: Accepted";

        boost::system::error_code ignored_error;

        boost::asio::write(*socket, boost::asio::buffer(welcome_msg.str()),
                boost::asio::transfer_all(),
                ignored_error);

        boost::asio::streambuf buffer;
        size_t length =
            boost::asio::read_until(*socket, buffer, "\n", ignored_error);

        std::string in_message;
        std::istream str(&buffer);
        std::getline(str, in_message);

        if (in_message == "config") {
            std::string json = getStatConfigJSON();
            boost::asio::write(*socket, boost::asio::buffer(json),
                    boost::asio::transfer_all(),
                    ignored_error);
        }
        else if (in_message == "values") {
            std::string json = getValuesJSON();
            boost::asio::write(*socket, boost::asio::buffer(json),
                    boost::asio::transfer_all(),
                    ignored_error);
        }
        else if (in_message == "state") {
            std::string json = getStateJSON();

            boost::asio::write(*socket, boost::asio::buffer(json),
                    boost::asio::transfer_all(),
                    ignored_error);
        }
        else if (in_message == "setptree") {
            boost::asio::streambuf jsonbuffer;
            length = boost::asio::read_until(
                    *socket, jsonbuffer, "\n", ignored_error);

            if (length > 0) {
                boost::unique_lock<boost::mutex> lock(m_configmutex);
                m_pt.clear();
                boost::property_tree::json_parser::read_json(jsonbuffer, m_pt);
            }
            else if (length == 0) {
                etiLog.level(warn) <<
                    "MGMT: No JSON data received";
            }
            else {
                etiLog.level(error) <<
                    "MGMT: Error JSON reception";
            }
        }
        else if (in_message == "getptree") {
            boost::unique_lock<boost::mutex> lock(m_configmutex);
            m_pending = true;

            while (m_pending && !m_retrieve_pending) {
                m_condition.wait(lock);
            }
            std::stringstream ss;
            boost::property_tree::json_parser::write_json(ss, m_pt);

            boost::asio::write(*socket, ss,
                    boost::asio::transfer_all(),
                    ignored_error);
        }
        else {
            std::stringstream ss;
            ss << "Invalid command\n";
            boost::asio::write(*socket, ss,
                    boost::asio::transfer_all(),
                    ignored_error);
        }
    }
    catch (std::exception& e) {
        etiLog.level(error) <<
            "MGMT server caught exception: " <<
            e.what();
    }
}

bool ManagementServer::retrieve_new_ptree(boost::property_tree::ptree& pt)
{
    boost::unique_lock<boost::mutex> lock(m_configmutex);

    if (m_retrieve_pending)
    {
        pt = m_pt;

        m_retrieve_pending = false;
        m_condition.notify_one();
        return true;
    }

    return false;
}

void ManagementServer::update_ptree(const boost::property_tree::ptree& pt)
{
    if (m_running) {
        boost::unique_lock<boost::mutex> lock(m_configmutex);
        m_pt = pt;
        m_pending = false;

        m_condition.notify_one();
    }
}

/************************************************/

void InputStat::registerAtServer()
{
    get_mgmt_server().registerInput(this);
}

InputStat::~InputStat()
{
    get_mgmt_server().unregisterInput(m_name);
}

std::string InputStat::encodeValuesJSON()
{
    std::ostringstream ss;

    const int16_t int16_max = std::numeric_limits<int16_t>::max();

    boost::mutex::scoped_lock lock(m_mutex);

    /* convert to dB */
    int dB_l = peak_left  ? round(20*log10((double)peak_left / int16_max))  : -90;
    int dB_r = peak_right ? round(20*log10((double)peak_right / int16_max)) : -90;

    ss <<
    "{ \"inputstat\" : {"
        "\"min_fill\": " << min_fill_buffer << ", "
        "\"max_fill\": " << max_fill_buffer << ", "
        "\"peak_left\": " << dB_l << ", "
        "\"peak_right\": " << dB_r << ", "
        "\"num_underruns\": " << num_underruns << ", "
        "\"num_overruns\": " << num_overruns <<
    " } }";

    return ss.str();
}

std::string InputStat::encodeStateJSON()
{
    std::ostringstream ss;

    ss << "{ \"state\" : ";

    switch (determineState()) {
        case NoData:
            ss << "\"NoData\"";
            break;
        case Unstable:
            ss << "\"Unstable\"";
            break;
        case Silence:
            ss << "\"Silent\"";
            break;
        case Streaming:
            ss << "\"Streaming\"";
            break;
        default:
            ss << "\"Unknown\"";
    }

    ss << " }";

    return ss.str();
}

input_state_t InputStat::determineState(void)
{
    boost::mutex::scoped_lock lock(m_mutex);

    time_t now = time(NULL);
    input_state_t state;

    /* if the last event was more that INPUT_COUNTER_RESET_TIME
     * minutes ago, the timeout has expired. We can reset our
     * glitch counter.
     */
    if (now - m_time_last_event > 60*INPUT_COUNTER_RESET_TIME) {
        m_glitch_counter = 0;
    }

    // STATE CALCULATION

    /* If the buffer has been empty for more than
     * INPUT_NODATA_TIMEOUT, we go to the NoData state.
     */
    if (m_buffer_empty &&
            now - m_time_last_buffer_nonempty > INPUT_NODATA_TIMEOUT) {
        state = NoData;
    }

    /* Otherwise, the state depends on the glitch counter */
    else if (m_glitch_counter >= INPUT_UNSTABLE_THRESHOLD) {
        state = Unstable;
    }
    else {
        /* The input is streaming, check if the audio level is too low */

        if (m_silence_counter > INPUT_AUDIO_LEVEL_SILENCE_COUNT) {
            state = Silence;
        }
        else {
            state = Streaming;
        }
    }

    return state;
}

