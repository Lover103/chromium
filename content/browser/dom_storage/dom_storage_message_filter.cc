// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/dom_storage/dom_storage_message_filter.h"

#include "base/auto_reset.h"
#include "base/bind.h"
#include "base/strings/nullable_string16.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/sequenced_worker_pool.h"
#include "content/browser/dom_storage/dom_storage_area.h"
#include "content/browser/dom_storage/dom_storage_context_wrapper.h"
#include "content/browser/dom_storage/dom_storage_host.h"
#include "content/browser/dom_storage/dom_storage_namespace.h"
#include "content/browser/dom_storage/dom_storage_task_runner.h"
#include "content/common/dom_storage/dom_storage_messages.h"
#include "base/synchronization/waitable_event.h"
#include "content/public/browser/user_metrics.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/user_metrics.h"
#include "url/gurl.h"

using base::WaitableEvent;
using WebKit::WebStorageArea;

namespace content {

DOMStorageMessageFilter::DOMStorageMessageFilter(
    int render_process_id,
    DOMStorageContextWrapper* context)
    : context_(context->context()),
      connection_dispatching_message_for_(0),
      render_process_id_(render_process_id) {
}

DOMStorageMessageFilter::~DOMStorageMessageFilter() {
  DCHECK(!host_.get());
}

void DOMStorageMessageFilter::InitializeInSequence() {
  DCHECK(!BrowserThread::CurrentlyOn(BrowserThread::IO));
  host_.reset(new DOMStorageHost(context_.get()));
  context_->AddEventObserver(this);
}

void DOMStorageMessageFilter::UninitializeInSequence() {
  // TODO(michaeln): Restore this DCHECK once crbug/166470 and crbug/164403
  // are resolved.
  // DCHECK(!BrowserThread::CurrentlyOn(BrowserThread::IO));
  context_->RemoveEventObserver(this);
  host_.reset();
}

void DOMStorageMessageFilter::OnFilterAdded(IPC::Channel* channel) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  BrowserMessageFilter::OnFilterAdded(channel);
  context_->task_runner()->PostShutdownBlockingTask(
      FROM_HERE,
      DOMStorageTaskRunner::PRIMARY_SEQUENCE,
      base::Bind(&DOMStorageMessageFilter::InitializeInSequence, this));
}

void DOMStorageMessageFilter::OnFilterRemoved() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  BrowserMessageFilter::OnFilterRemoved();
  context_->task_runner()->PostShutdownBlockingTask(
      FROM_HERE,
      DOMStorageTaskRunner::PRIMARY_SEQUENCE,
      base::Bind(&DOMStorageMessageFilter::UninitializeInSequence, this));
}

base::TaskRunner* DOMStorageMessageFilter::OverrideTaskRunnerForMessage(
    const IPC::Message& message) {
  if (IPC_MESSAGE_CLASS(message) == DOMStorageMsgStart)
    return context_->task_runner();
  return NULL;
}

bool DOMStorageMessageFilter::OnMessageReceived(const IPC::Message& message,
                                                bool* message_was_ok) {
  if (IPC_MESSAGE_CLASS(message) != DOMStorageMsgStart)
    return false;
  DCHECK(!BrowserThread::CurrentlyOn(BrowserThread::IO));
  DCHECK(host_.get());
  current_routing_id_ = message.routing_id();

  bool handled = true;
  IPC_BEGIN_MESSAGE_MAP_EX(DOMStorageMessageFilter, message, *message_was_ok)
    IPC_MESSAGE_HANDLER(DOMStorageHostMsg_OpenStorageArea, OnOpenStorageArea)
    IPC_MESSAGE_HANDLER(DOMStorageHostMsg_CloseStorageArea, OnCloseStorageArea)
    IPC_MESSAGE_HANDLER(DOMStorageHostMsg_LoadStorageArea, OnLoadStorageArea)
    IPC_MESSAGE_HANDLER(DOMStorageHostMsg_SetItem, OnSetItem)
    IPC_MESSAGE_HANDLER(DOMStorageHostMsg_RemoveItem, OnRemoveItem)
    IPC_MESSAGE_HANDLER(DOMStorageHostMsg_Clear, OnClear)
    IPC_MESSAGE_HANDLER(DOMStorageHostMsg_FlushMessages, OnFlushMessages)
    IPC_MESSAGE_UNHANDLED(handled = false)
  IPC_END_MESSAGE_MAP()
  return handled;
}

void DOMStorageMessageFilter::HandleOnOpenStorageAreaOnUIThread(
      int routing_id,
      const GURL& origin,
      GURL* override,
      WaitableEvent* done) {
  RenderViewHost* view = RenderViewHost::FromID(render_process_id_, routing_id);
  WebContents* web_contents = NULL;
  web_contents = WebContents::FromRenderViewHost(view);
  if (web_contents) {
    WebContentsDelegate* delegate = web_contents->GetDelegate();
    if (delegate) {
      *override = delegate->OverrideDOMStorageOrigin(origin);
    }
  }
  done->Signal();
}

void DOMStorageMessageFilter::OnOpenStorageArea(
                                                int connection_id,
                                                int64 namespace_id,
                                                const GURL& origin) {
  DCHECK(!BrowserThread::CurrentlyOn(BrowserThread::IO));
  WaitableEvent done(false, false);
  GURL origin_override(origin);

  BrowserThread::PostTask(BrowserThread::UI, FROM_HERE,
                          base::Bind(&DOMStorageMessageFilter::HandleOnOpenStorageAreaOnUIThread,
                                     this, current_routing_id_, origin, &origin_override, &done));

  done.Wait();

  if (!host_->OpenStorageArea(connection_id, namespace_id, origin_override)) {
    RecordAction(UserMetricsAction("BadMessageTerminate_DSMF_1"));
    BadMessageReceived();
  }
}

void DOMStorageMessageFilter::OnCloseStorageArea(int connection_id) {
  DCHECK(!BrowserThread::CurrentlyOn(BrowserThread::IO));
  host_->CloseStorageArea(connection_id);
}

void DOMStorageMessageFilter::OnLoadStorageArea(int connection_id,
                                                DOMStorageValuesMap* map) {
  DCHECK(!BrowserThread::CurrentlyOn(BrowserThread::IO));
  if (!host_->ExtractAreaValues(connection_id, map)) {
    RecordAction(UserMetricsAction("BadMessageTerminate_DSMF_2"));
    BadMessageReceived();
  }
  Send(new DOMStorageMsg_AsyncOperationComplete(true));
}

void DOMStorageMessageFilter::OnSetItem(
    int connection_id, const string16& key,
    const string16& value, const GURL& page_url) {
  DCHECK(!BrowserThread::CurrentlyOn(BrowserThread::IO));
  DCHECK_EQ(0, connection_dispatching_message_for_);
  base::AutoReset<int> auto_reset(&connection_dispatching_message_for_,
                            connection_id);
  base::NullableString16 not_used;
  bool success = host_->SetAreaItem(connection_id, key, value,
                                    page_url, &not_used);
  Send(new DOMStorageMsg_AsyncOperationComplete(success));
}

void DOMStorageMessageFilter::OnRemoveItem(
    int connection_id, const string16& key,
    const GURL& page_url) {
  DCHECK(!BrowserThread::CurrentlyOn(BrowserThread::IO));
  DCHECK_EQ(0, connection_dispatching_message_for_);
  base::AutoReset<int> auto_reset(&connection_dispatching_message_for_,
                            connection_id);
  string16 not_used;
  host_->RemoveAreaItem(connection_id, key, page_url, &not_used);
  Send(new DOMStorageMsg_AsyncOperationComplete(true));
}

void DOMStorageMessageFilter::OnClear(
    int connection_id, const GURL& page_url) {
  DCHECK(!BrowserThread::CurrentlyOn(BrowserThread::IO));
  DCHECK_EQ(0, connection_dispatching_message_for_);
  base::AutoReset<int> auto_reset(&connection_dispatching_message_for_,
                            connection_id);
  host_->ClearArea(connection_id, page_url);
  Send(new DOMStorageMsg_AsyncOperationComplete(true));
}

void DOMStorageMessageFilter::OnFlushMessages() {
  // Intentionally empty method body.
}

void DOMStorageMessageFilter::OnDOMStorageItemSet(
    const DOMStorageArea* area,
    const string16& key,
    const string16& new_value,
    const base::NullableString16& old_value,
    const GURL& page_url) {
  SendDOMStorageEvent(area, page_url,
                      base::NullableString16(key, false),
                      base::NullableString16(new_value, false),
                      old_value);
}

void DOMStorageMessageFilter::OnDOMStorageItemRemoved(
    const DOMStorageArea* area,
    const string16& key,
    const string16& old_value,
    const GURL& page_url) {
  SendDOMStorageEvent(area, page_url,
                      base::NullableString16(key, false),
                      base::NullableString16(),
                      base::NullableString16(old_value, false));
}

void DOMStorageMessageFilter::OnDOMStorageAreaCleared(
    const DOMStorageArea* area,
    const GURL& page_url) {
  SendDOMStorageEvent(area, page_url,
                      base::NullableString16(),
                      base::NullableString16(),
                      base::NullableString16());
}

void DOMStorageMessageFilter::SendDOMStorageEvent(
    const DOMStorageArea* area,
    const GURL& page_url,
    const base::NullableString16& key,
    const base::NullableString16& new_value,
    const base::NullableString16& old_value) {
  DCHECK(!BrowserThread::CurrentlyOn(BrowserThread::IO));
  // Only send mutation events to processes which have the area open.
  bool originated_in_process = connection_dispatching_message_for_ != 0;
  if (originated_in_process ||
      host_->HasAreaOpen(area->namespace_id(), area->origin())) {
    DOMStorageMsg_Event_Params params;
    params.origin = area->origin();
    params.page_url = page_url;
    params.connection_id = connection_dispatching_message_for_;
    params.key = key;
    params.new_value = new_value;
    params.old_value = old_value;
    params.namespace_id = area->namespace_id();
    Send(new DOMStorageMsg_Event(params));
  }
}

}  // namespace content
