/*****************************************************************************
 * Copyright [2017] [taurus.ai]
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *****************************************************************************/

/**
 * Socket Handler for page engine.
 * @Author cjiang (changhao.jiang@taurus.ai)
 * @since   March, 2017
 */

#include "PageSocketHandler.h"
#include "Timer.h"
#include "Journal.h"

#include <spdlog/spdlog.h>
#include <boost/thread/mutex.hpp>
#include <boost/filesystem.hpp>
#include <nanomsg/nn.h>
#include <nanomsg/reqrep.h>

USING_YJJ_NAMESPACE

boost::shared_ptr<PageSocketHandler> PageSocketHandler::m_ptr = boost::shared_ptr<PageSocketHandler>(nullptr);

PageSocketHandler::PageSocketHandler(): io_running(false)
{}

PageSocketHandler* PageSocketHandler::getInstance()
{
    if (m_ptr.get() == nullptr)
    {
        m_ptr = boost::shared_ptr<PageSocketHandler>(new PageSocketHandler());
    }
    return m_ptr.get();
}

void PageSocketHandler::run(IPageSocketUtil* _util)
{
    util = _util;
    boost::filesystem::path socket_path = PAGED_SOCKET_FILE;
    boost::filesystem::path socket_folder_path = socket_path.parent_path();
    if(!boost::filesystem::exists(socket_folder_path))
    {
        boost::filesystem::create_directories(socket_folder_path);
    }

    string server_url = "ipc://" + socket_path.string();
    server_response_socket = nn_socket(AF_SP, NN_REP);
    if (server_response_socket < 0)
    {
        SPDLOG_ERROR("Fail to create nanomsg socket");
    }
    int rv = nn_bind(server_response_socket, server_url.c_str());
    if (rv < 0)
    {
        SPDLOG_ERROR("Fail to bind nanomsg socket at {}", server_url);
    }
    SPDLOG_INFO("Start serve nanomsg at {}", server_url);
    
    io_running = true;

    while(io_running)
    {
        int bytes = nn_recv(server_response_socket, data_request_.data(), SOCKET_MESSAGE_MAX_LENGTH, 0);
        if (bytes < 0)
        {
            SPDLOG_ERROR("nn_recv");
        }
        else
        {
            util->acquire_mutex();
            process_msg();
            util->release_mutex();
        }
    }
}

bool PageSocketHandler::is_running()
{
    return io_running;
}

void PageSocketHandler::stop()
{
    io_running = false;
    nn_shutdown(server_response_socket, 0);
}

void PageSocketHandler::process_msg()
{
    string req_str = string(data_request_.data());
    json req_json = json::parse(req_str);
    PagedSocketRequest req;
    req_json.at("type").get_to(req.type);
    req_json.at("name").get_to(req.name);
    req_json.at("pid").get_to(req.pid);
    req_json.at("hash_code").get_to(req.hash_code);
    req_json.at("source").get_to(req.source);

    switch (req.type)
    {
        case TIMER_SEC_DIFF_REQUEST:
        {
            json timer;
            timer["secDiff"] = getSecDiff();
            timer["nano"] = getNanoTime();
            strcpy(&data_response_[0], timer.dump().c_str());
            break;
        }
        case PAGED_SOCKET_JOURNAL_REGISTER:
        {
            int idx = util->reg_journal(req.name);
            PagedSocketRspJournal rsp = {};
            rsp.type = req.type;
            rsp.success = idx >= 0;
            rsp.comm_idx = idx;
            memcpy(&data_response_[0], &rsp, sizeof(rsp));
            break;
        }
        case PAGED_SOCKET_READER_REGISTER:
        case PAGED_SOCKET_WRITER_REGISTER:
        {
            string comm_file;
            int file_size;
            int has_code;
            bool ret = util->reg_client(comm_file, file_size, has_code, req.name, req.pid, req.type==PAGED_SOCKET_WRITER_REGISTER);
            PagedSocketRspClient rsp = {};
            rsp.type = req.type;
            rsp.success = ret;
            rsp.file_size = file_size;
            rsp.hash_code = has_code;
            memcpy(rsp.comm_file, comm_file.c_str(), comm_file.length() + 1);
            memcpy(&data_response_[0], &rsp, sizeof(rsp));
            break;
        }
        case PAGED_SOCKET_CLIENT_EXIT:
        {
            util->exit_client(req.name, req.hash_code, true);
            PagedSocketResponse rsp = {};
            rsp.type = req.type;
            rsp.success = true;
            memcpy(&data_response_[0], &rsp, sizeof(rsp));
            break;
        }
    }

    int bytes = nn_send(server_response_socket, data_response_.data(), data_response_.size(), 0);
    if (bytes < 0)
    {
        SPDLOG_ERROR("nn_send failed {}", bytes);
    }
}