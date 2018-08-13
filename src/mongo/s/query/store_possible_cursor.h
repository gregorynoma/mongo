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

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/tailable_mode.h"
#include "mongo/s/client/shard.h"

namespace mongo {

class BSONObj;
class ClusterCursorManager;
class RemoteCursor;
template <typename T>
class StatusWith;
struct HostAndPort;

namespace executor {
class TaskExecutor;
}  // namespace executor

/**
 * Response object used for storing cursors created based off exsting cursors on remote instances.
 * See storePossibleCursor below for more details.
 */
struct RemoteCursorResponse {
    boost::optional<CursorResponse> cursor;
    BSONObj response;
};

/**
 * Utility function to create a cursor based on existing cursor on a remote instance.  'cmdResult'
 * must be the response object generated upon creation of the cursor. The newly created cursor will
 * use 'executor' to retrieve batches of results from the shards and is stored with 'cursorManager'.
 *
 * 'requestedNss' is used to store the ClusterClientCursor for future lookup. It is also the
 * namespace represented in the cursor response for the returned BSONObj. For views 'requestedNss'
 * may be different then the underlying collection namespace.
 *
 * If 'cmdResult' does not describe a command cursor response document or no cursor is specified,
 * returns a RemoteCursorResponse with 'cmdResult' as 'response' and 'cursor' as none. If a parsing
 * error occurs, returns an error Status. Otherwise, returns a RemoteCursorResponse with 'cursor'
 * as the contents of the newly-created cursor and 'response' as 'cmdResult'.
 *
 * @ shardId the name of the shard on which the cursor-establishing command was run
 * @ server the exact host in the shard on which the cursor-establishing command was run
 * @ cmdResult the result of running the cursor-establishing command
 * @ requestedNss the namespace on which the client issued the cursor-establishing command (can
 * differ from the execution namespace if the command was issued on a view)
 * @ executor the TaskExecutor to store in the resulting ClusterClientCursor
 * @ cursorManager the ClusterCursorManager on which to register the resulting ClusterClientCursor
*/
StatusWith<RemoteCursorResponse> storePossibleCursor(
    OperationContext* opCtx,
    const ShardId& shardId,
    const HostAndPort& server,
    const BSONObj& cmdResult,
    const NamespaceString& requestedNss,
    executor::TaskExecutor* executor,
    ClusterCursorManager* cursorManager,
    TailableModeEnum tailableMode = TailableModeEnum::kNormal);

/**
 * Convenience function which extracts all necessary information from the passed RemoteCursor, and
 * stores a ClusterClientCursor based on it.
 */
StatusWith<RemoteCursorResponse> storePossibleCursor(OperationContext* opCtx,
                                                     const NamespaceString& requestedNss,
                                                     const RemoteCursor& remoteCursor,
                                                     TailableModeEnum tailableMode);

/**
 * Convenience function which extracts all necessary information from the passed CommandResponse,
 * and stores a ClusterClientCursor based on it.
 */
StatusWith<RemoteCursorResponse> storePossibleCursor(OperationContext* opCtx,
                                                     const NamespaceString& requestedNss,
                                                     const ShardId& shardId,
                                                     const Shard::CommandResponse& commandResponse,
                                                     TailableModeEnum tailableMode);
}  // namespace mongo