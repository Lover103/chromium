// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SYNC_SESSIONS_DEBUG_INFO_GETTER_H_
#define SYNC_SESSIONS_DEBUG_INFO_GETTER_H_

#include "sync/base/sync_export.h"
#include "sync/protocol/sync.pb.h"

namespace syncer {
namespace sessions {

// This is the interface that needs to be implemented by the event listener
// to communicate the debug info data to the syncer.
class SYNC_EXPORT_PRIVATE DebugInfoGetter {
 public:
  // Gets the client debug info and clears the state so the same data is not
  // sent again.
  virtual void GetAndClearDebugInfo(sync_pb::DebugInfo* debug_info) = 0;
  virtual ~DebugInfoGetter() {}
};

}  // namespace sessions
}  // namespace syncer

#endif  // SYNC_SESSIONS_DEBUG_INFO_GETTER_H_

