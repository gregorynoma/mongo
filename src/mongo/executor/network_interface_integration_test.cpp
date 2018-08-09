/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include <algorithm>
#include <exception>
#include <unistd.h>

#include "mongo/base/status_with.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_integration_fixture.h"
#include "mongo/executor/test_network_connection_hook.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/message.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace executor {
namespace {

bool pingCommandMissing(const RemoteCommandResponse& result) {
    if (result.isOK()) {
        // On mongos, there is no sleep command, so just check that the command failed with
        // a "Command not found" error code
        ASSERT_EQ(result.data["ok"].Double(), 0.0);
        ASSERT_EQ(result.data["code"].Int(), 59);
        return true;
    }

    return false;
}

// Hook that intentionally never finishes
class HangingHook : public executor::NetworkConnectionHook {
    Status validateHost(const HostAndPort&,
                        const BSONObj& request,
                        const RemoteCommandResponse&) final {
        return Status::OK();
    }

    StatusWith<boost::optional<RemoteCommandRequest>> makeRequest(
        const HostAndPort& remoteHost) final {
        return {boost::make_optional(RemoteCommandRequest(remoteHost,
                                                          "admin",
                                                          BSON("sleep" << 1 << "lock"
                                                                       << "none"
                                                                       << "secs"
                                                                       << 100000000),
                                                          BSONObj(),
                                                          nullptr))};
    }

    Status handleReply(const HostAndPort& remoteHost, RemoteCommandResponse&& response) final {
        if (!pingCommandMissing(response)) {
            ASSERT_EQ(ErrorCodes::CallbackCanceled, response.status);
            return response.status;
        }

        return {ErrorCodes::NetworkInterfaceExceededTimeLimit,
                "No ping command. Simulating timeout"};
    }
};

using ResponseStatus = TaskExecutor::ResponseStatus;

BSONObj objConcat(std::initializer_list<BSONObj> objs) {
    BSONObjBuilder bob;

    for (const auto& obj : objs) {
        bob.appendElements(obj);
    }

    return bob.obj();
}

class NetworkInterfaceTest : public NetworkInterfaceIntegrationFixture {
public:
    void assertNumOps(uint64_t canceled, uint64_t timedOut, uint64_t failed, uint64_t succeeded) {
        auto counters = net().getCounters();
        ASSERT_EQ(canceled, counters.canceled);
        ASSERT_EQ(timedOut, counters.timedOut);
        ASSERT_EQ(failed, counters.failed);
        ASSERT_EQ(succeeded, counters.succeeded);
    }

    void setUp() override {
        setTestCommandsEnabled(true);
        startNet(std::make_unique<WaitForIsMasterHook>(this));
    }

    RemoteCommandRequest makeTestCommand(boost::optional<Milliseconds> timeout = boost::none,
                                         BSONObj cmd = BSON("echo" << 1 << "foo"
                                                                   << "bar")) {
        auto cs = fixture();
        return RemoteCommandRequest(cs.getServers().front(),
                                    "admin",
                                    std::move(cmd),
                                    BSONObj(),
                                    nullptr,
                                    timeout ? *timeout : RemoteCommandRequest::kNoTimeout);
    }

    struct IsMasterData {
        BSONObj request;
        RemoteCommandResponse response;
    };
    IsMasterData waitForIsMaster() {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        _isMasterCond.wait(lk, [this] { return _isMasterResult != boost::none; });

        return std::move(*_isMasterResult);
    }

    bool hasIsMaster() {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _isMasterResult != boost::none;
    }

private:
    class WaitForIsMasterHook : public NetworkConnectionHook {
    public:
        explicit WaitForIsMasterHook(NetworkInterfaceTest* parent) : _parent(parent) {}

        Status validateHost(const HostAndPort& host,
                            const BSONObj& request,
                            const RemoteCommandResponse& isMasterReply) override {
            stdx::lock_guard<stdx::mutex> lk(_parent->_mutex);
            _parent->_isMasterResult = {request, isMasterReply};
            _parent->_isMasterCond.notify_all();
            return Status::OK();
        }

        StatusWith<boost::optional<RemoteCommandRequest>> makeRequest(const HostAndPort&) override {
            return {boost::none};
        }

        Status handleReply(const HostAndPort&, RemoteCommandResponse&&) override {
            return Status::OK();
        }

    private:
        NetworkInterfaceTest* _parent;
    };

    stdx::mutex _mutex;
    stdx::condition_variable _isMasterCond;
    boost::optional<IsMasterData> _isMasterResult;
};

TEST_F(NetworkInterfaceTest, ToppleMongos) {
    BSONObj obj = BSON("find" << "doc");
    auto request1 = RemoteCommandRequest(HostAndPort("172.31.61.190:28003"), "db1", obj, BSONObj(), nullptr);
    auto request2 = RemoteCommandRequest(HostAndPort("172.31.61.190:28003"), "db2", obj, BSONObj(), nullptr);

    while (true) {
        runCommand(makeCallbackHandle(), request1)
            .getAsync([](StatusWith<RemoteCommandResponse> res) {
                //std::cout << res.getValue() << std::endl;
            });
        runCommand(makeCallbackHandle(), request2)
            .getAsync([](StatusWith<RemoteCommandResponse> res) {
                //std::cout << res.getValue() << std::endl;
            });
        usleep(500u);
    }
}

}  // namespace
}  // namespace executor
}  // namespace mongo
