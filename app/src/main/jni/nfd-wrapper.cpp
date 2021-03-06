/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * Copyright (c) 2015 Regents of the University of California
 *
 * This file is part of NFD (Named Data Networking Forwarding Daemon) Android.
 * See AUTHORS.md for complete list of NFD Android authors and contributors.
 *
 * NFD Android is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * NFD Android is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * NFD Android, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "nfd-wrapper.hpp"

#include "daemon/nfd.hpp"
#include "rib/nrd.hpp"

#include "core/global-io.hpp"
#include "core/config-file.hpp"
#include "core/logger.hpp"
#include "core/privilege-helper.hpp"

#include <stdlib.h>
#include <boost/property_tree/info_parser.hpp>
#include <boost/thread.hpp>
#include <mutex>

NFD_LOG_INIT("NfdWrapper");

namespace nfd {


// A little bit of cheating to make sure NFD can be properly restarted

namespace scheduler {
// defined in scheduler.cpp
void
resetGlobalScheduler();
} // namespace scheduler

void
resetGlobalIoService();


class Runner
{
public:
  Runner()
    : m_io(nullptr)
  {
    std::string initialConfig =
      "general\n"
      "{\n"
      "}\n"
      "\n"
      "log\n"
      "{\n"
      "  default_level ALL\n"
      "  NameTree INFO\n"
      "  BestRouteStrategy2 INFO\n"
      "  InternalFace INFO\n"
      "  Forwarder INFO\n"
      "  ContentStore INFO\n"
      "  DeadNonceList INFO\n"
      "}\n"
      "tables\n"
      "{\n"
      "  cs_max_packets 100\n"
      "\n"
      "  strategy_choice\n"
      "  {\n"
      "    /               /localhost/nfd/strategy/best-route\n"
      "    /localhost      /localhost/nfd/strategy/broadcast\n"
      "    /localhost/nfd  /localhost/nfd/strategy/best-route\n"
      "    /ndn/broadcast  /localhost/nfd/strategy/broadcast\n"
      "  }\n"
      "}\n"
      "\n"
      "face_system\n"
      "{\n"
      "  tcp\n"
      "  {\n"
      "    port 6363\n"
      "  }\n"
      "\n"
      "  udp\n"
      "  {\n"
      "    port 6363\n"
      "    idle_timeout 600\n"
      "    keep_alive_interval 25\n"
      "    mcast no\n"
      "  }\n"
      "}\n"
      "\n"
      "authorizations\n"
      "{\n"
      "  authorize\n"
      "  {\n"
      "    certfile any\n"
      "    privileges\n"
      "    {\n"
      "      faces\n"
      "      fib\n"
      "      strategy-choice\n"
      "    }\n"
      "  }\n"
      "}\n"
      "\n"
      "rib\n"
      "{\n"
      "  localhost_security\n"
      "  {\n"
      "    trust-anchor\n"
      "    {\n"
      "      type any\n"
      "    }\n"
      "  }\n"
      "\n"
      "  remote_register\n"
      "  {\n"
      "    cost 15\n"
      "    timeout 10000\n"
      "    retry 0\n"
      "    refresh_interval 300\n"
      "  }\n"
      "}\n"
      "\n";

    std::istringstream input(initialConfig);
    boost::property_tree::read_info(input, m_config);

    std::unique_lock<std::mutex> lock(m_pointerMutex);
    m_nfd.reset(new Nfd(m_config, m_keyChain));
    m_nrd.reset(new rib::Nrd(m_config, m_keyChain));

    m_nfd->initialize();
    m_nrd->initialize();
  }

  ~Runner()
  {
    stop();
    m_io->reset();
  }

  void
  start()
  {
    {
      std::unique_lock<std::mutex> lock(m_pointerMutex);
      m_io = &getGlobalIoService();
    }
    m_io->run();
    m_io->reset();
  }

  void
  stop()
  {
    std::unique_lock<std::mutex> lock(m_pointerMutex);

    m_io->post([this] {
        m_io->stop();
        this->m_nrd.reset();
        this->m_nfd.reset();
      });
  }

private:
  std::mutex m_pointerMutex;
  boost::asio::io_service* m_io;
  ndn::KeyChain m_keyChain;
  unique_ptr<Nfd> m_nfd; // will use globalIoService
  unique_ptr<rib::Nrd> m_nrd; // will use globalIoService

  nfd::ConfigSection m_config;
};

static unique_ptr<Runner> g_runner;
static boost::thread g_thread;

} // namespace nfd

JNIEXPORT void JNICALL
Java_net_named_1data_nfd_service_NfdService_startNfd(JNIEnv* env, jclass, jstring homePathJ)
{
  if (nfd::g_runner.get() == nullptr) {
    // set/update HOME environment variable
    const char* homePath = env->GetStringUTFChars(homePathJ, nullptr);
    ::setenv("HOME", homePath, true);
    env->ReleaseStringUTFChars(homePathJ, homePath);
    NFD_LOG_INFO("Use [" << homePath << "] as a security storage");

    nfd::g_thread = boost::thread([] {
        NFD_LOG_INFO("Starting NFD...");
        try {
          nfd::g_runner.reset(new nfd::Runner());
          nfd::g_runner->start();
        }
        catch (const std::exception& e) {
          NFD_LOG_FATAL(e.what());
        }
        catch (const nfd::PrivilegeHelper::Error& e) {
          NFD_LOG_FATAL("PrivilegeHelper: " << e.what());
        }
        catch (...) {
          NFD_LOG_FATAL("Unknown fatal error");
        }

        nfd::g_runner.reset();
        nfd::scheduler::resetGlobalScheduler();
        nfd::resetGlobalIoService();
        NFD_LOG_INFO("NFD stopped");
      });
  }
}

JNIEXPORT void JNICALL
Java_net_named_1data_nfd_service_NfdService_stopNfd(JNIEnv*, jclass)
{
  if (nfd::g_runner.get() != nullptr) {
    NFD_LOG_INFO("Stopping NFD...");
    nfd::g_runner->stop();
    // do not block anything
  }
}
