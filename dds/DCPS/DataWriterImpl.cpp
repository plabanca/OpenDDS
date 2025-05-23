/*
 * Distributed under the OpenDDS License.
 * See: http://www.opendds.org/license.html
 */

#include <DCPS/DdsDcps_pch.h> // Only the _pch include should start with DCPS/

#include "DataWriterImpl.h"

#include "DCPS_Utils.h"
#include "DataDurabilityCache.h"
#include "DataSampleElement.h"
#include "DomainParticipantImpl.h"
#include "FeatureDisabledQosCheck.h"
#include "GuidConverter.h"
#include "MonitorFactory.h"
#include "PublicationInstance.h"
#include "PublisherImpl.h"
#include "SendStateDataSampleList.h"
#include "Serializer.h"
#include "Service_Participant.h"
#include "TopicImpl.h"
#include "Transient_Kludge.h"
#include "TypeSupportImpl.h"
#include "Util.h"

#include "XTypes/TypeObject.h"

#include <dds/OpenDDSConfigWrapper.h>
#ifndef OPENDDS_NO_OBJECT_MODEL_PROFILE
#  include "CoherentChangeControl.h"
#endif
#include "AssociationData.h"
#include "transport/framework/EntryExit.h"
#include "transport/framework/TransportExceptions.h"
#include "transport/framework/TransportRegistry.h"
#ifndef DDS_HAS_MINIMUM_BIT
#  include "BuiltInTopicUtils.h"
#endif

#ifndef DDS_HAS_MINIMUM_BIT
#  include <dds/DdsDcpsCoreTypeSupportC.h>
#endif // !defined (DDS_HAS_MINIMUM_BIT)
#include <dds/DdsDcpsCoreC.h>
#include <dds/DdsDcpsGuidTypeSupportImpl.h>

#include <ace/Reactor.h>

#include <stdexcept>

OPENDDS_BEGIN_VERSIONED_NAMESPACE_DECL

namespace OpenDDS {
namespace DCPS {

//TBD - add check for enabled in most methods.
//      currently this is not needed because auto_enable_created_entities
//      cannot be false.

DataWriterImpl::DataWriterImpl()
  : data_dropped_count_(0)
  , data_delivered_count_(0)
  , controlTracker("DataWriterImpl")
  , publisher_content_filter_(TheServiceParticipant->publisher_content_filter())
  , n_chunks_(TheServiceParticipant->n_chunks())
  , association_chunk_multiplier_(TheServiceParticipant->association_chunk_multiplier())
  , qos_(TheServiceParticipant->initial_DataWriterQos())
  , skip_serialize_(false)
  , db_lock_pool_(new DataBlockLockPool((unsigned long)TheServiceParticipant->n_chunks()))
  , topic_id_(GUID_UNKNOWN)
  , topic_servant_(0)
  , type_support_(0)
  , listener_mask_(DEFAULT_STATUS_MASK)
  , domain_id_(0)
  , publication_id_(GUID_UNKNOWN)
  , sequence_number_(SequenceNumber::SEQUENCENUMBER_UNKNOWN())
  , coherent_(false)
  , coherent_samples_(0)
  , last_deadline_missed_total_count_(0)
  , is_bit_(false)
  , min_suspended_transaction_id_(0)
  , max_suspended_transaction_id_(0)
  , liveliness_send_task_(make_rch<DWISporadicTask>(TheServiceParticipant->time_source(), TheServiceParticipant->reactor_task(), rchandle_from(this), &DataWriterImpl::liveliness_send_task))
  , liveliness_lost_task_(make_rch<DWISporadicTask>(TheServiceParticipant->time_source(), TheServiceParticipant->reactor_task(), rchandle_from(this), &DataWriterImpl::liveliness_lost_task))
  , liveliness_send_interval_(TimeDuration::max_value)
  , liveliness_lost_interval_(TimeDuration::max_value)
  , liveliness_lost_(false)
{
  liveliness_lost_status_.total_count = 0;
  liveliness_lost_status_.total_count_change = 0;

  offered_deadline_missed_status_.total_count = 0;
  offered_deadline_missed_status_.total_count_change = 0;
  offered_deadline_missed_status_.last_instance_handle = DDS::HANDLE_NIL;

  offered_incompatible_qos_status_.total_count = 0;
  offered_incompatible_qos_status_.total_count_change = 0;
  offered_incompatible_qos_status_.last_policy_id = 0;
  offered_incompatible_qos_status_.policies.length(0);

  publication_match_status_.total_count = 0;
  publication_match_status_.total_count_change = 0;
  publication_match_status_.current_count = 0;
  publication_match_status_.current_count_change = 0;
  publication_match_status_.last_subscription_handle = DDS::HANDLE_NIL;

  monitor_.reset(TheServiceParticipant->monitor_factory_->create_data_writer_monitor(this));
  periodic_monitor_.reset(TheServiceParticipant->monitor_factory_->create_data_writer_periodic_monitor(this));
}

// This method is called when there are no longer any reference to the
// the servant.
DataWriterImpl::~DataWriterImpl()
{
  DBG_ENTRY_LVL("DataWriterImpl", "~DataWriterImpl", 6);

  liveliness_send_task_->cancel();
  liveliness_lost_task_->cancel();

#ifndef OPENDDS_SAFETY_PROFILE
  RcHandle<DomainParticipantImpl> participant = participant_servant_.lock();
  if (participant) {
    XTypes::TypeLookupService_rch type_lookup_service = participant->get_type_lookup_service();
    if (type_lookup_service) {
      type_lookup_service->remove_guid_from_dynamic_map(publication_id_);
    }
  }
#endif
}

// this method is called when delete_datawriter is called.
void
DataWriterImpl::cleanup()
{
  // As first step set our listener to nill which will prevent us from calling
  // back onto the listener at the moment the related DDS entity has been
  // deleted
  set_listener(0, NO_STATUS_MASK);
  topic_servant_ = 0;
  type_support_ = 0;
}

void
DataWriterImpl::init(
  TopicImpl* topic_servant,
  const DDS::DataWriterQos& qos,
  DDS::DataWriterListener_ptr a_listener,
  const DDS::StatusMask& mask,
  WeakRcHandle<DomainParticipantImpl> participant_servant,
  PublisherImpl* publisher_servant)
{
  DBG_ENTRY_LVL("DataWriterImpl", "init", 6);
  topic_servant_ = topic_servant;
  type_support_ = dynamic_cast<TypeSupportImpl*>(topic_servant->get_type_support());
  topic_name_ = topic_servant_->get_name();
  topic_id_ = topic_servant_->get_id();
  type_name_ = topic_servant_->get_type_name();

#if !defined (DDS_HAS_MINIMUM_BIT)
  is_bit_ = topicIsBIT(topic_name_.in(), type_name_.in());
#endif // !defined (DDS_HAS_MINIMUM_BIT)

  qos_ = qos;
  passed_qos_ = qos;

  set_listener(a_listener, mask);

  // Only store the participant pointer, since it is our "grand"
  // parent, we will exist as long as it does.
  participant_servant_ = participant_servant;

  RcHandle<DomainParticipantImpl> participant = participant_servant.lock();
  domain_id_ = participant->get_domain_id();

  // Only store the publisher pointer, since it is our parent, we will
  // exist as long as it does.
  publisher_servant_ = *publisher_servant;
}

DDS::InstanceHandle_t
DataWriterImpl::get_instance_handle()
{
  const RcHandle<DomainParticipantImpl> participant = participant_servant_.lock();
  return get_entity_instance_handle(publication_id_, participant);
}

DDS::InstanceHandle_t
DataWriterImpl::get_next_handle()
{
  RcHandle<DomainParticipantImpl> participant = this->participant_servant_.lock();
  if (participant) {
    return participant->assign_handle();
  }
  return DDS::HANDLE_NIL;
}

void DataWriterImpl::return_handle(DDS::InstanceHandle_t handle)
{
  const RcHandle<DomainParticipantImpl> participant = participant_servant_.lock();
  if (participant) {
    participant->return_handle(handle);
  }
}

RcHandle<BitSubscriber>
DataWriterImpl::get_builtin_subscriber_proxy() const
{
  RcHandle<DomainParticipantImpl> participant_servant = participant_servant_.lock();
  if (participant_servant) {
    return participant_servant->get_builtin_subscriber_proxy();
  }

  return RcHandle<BitSubscriber>();
}

void
DataWriterImpl::set_publication_id(const GUID_t& guid)
{
  OPENDDS_ASSERT(publication_id_ == GUID_UNKNOWN);
  OPENDDS_ASSERT(guid != GUID_UNKNOWN);
  publication_id_ = guid;
  TransportClient::set_guid(guid);
}

void
DataWriterImpl::add_association(const ReaderAssociation& reader,
                                bool active)
{
  DBG_ENTRY_LVL("DataWriterImpl", "add_association", 6);

  if (DCPS_debug_level) {
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("(%P|%t) DataWriterImpl::add_association - ")
               ACE_TEXT("bit %d local %C remote %C\n"), is_bit_,
               LogGuid(publication_id_).c_str(),
               LogGuid(reader.readerId).c_str()));
  }

  if (get_deleted()) {
    if (DCPS_debug_level)
      ACE_DEBUG((LM_DEBUG, ACE_TEXT("(%P|%t) DataWriterImpl::add_association")
                 ACE_TEXT(" This is a deleted datawriter, ignoring add.\n")));

    return;
  }

  {
    ACE_GUARD(ACE_Thread_Mutex, reader_info_guard, this->reader_info_lock_);
    reader_info_.insert(std::make_pair(reader.readerId,
                                       ReaderInfo(reader.filterClassName,
                                                  publisher_content_filter_ ? reader.filterExpression.in() : "",
                                                  reader.exprParams, participant_servant_,
                                                  reader.readerQos.durability.kind > DDS::VOLATILE_DURABILITY_QOS)));
  }

  if (DCPS_debug_level > 4) {
    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("(%P|%t) DataWriterImpl::add_association(): ")
               ACE_TEXT("adding subscription to publication %C with priority %d.\n"),
               LogGuid(get_guid()).c_str(),
               qos_.transport_priority.value));
  }

  AssociationData data;
  data.remote_id_ = reader.readerId;
  data.remote_data_ = reader.readerTransInfo;
  data.discovery_locator_ = reader.readerDiscInfo;
  data.participant_discovered_at_ = reader.participantDiscoveredAt;
  data.remote_transport_context_ = reader.transportContext;
  data.remote_reliable_ =
    (reader.readerQos.reliability.kind == DDS::RELIABLE_RELIABILITY_QOS);
  data.remote_durable_ =
    (reader.readerQos.durability.kind > DDS::VOLATILE_DURABILITY_QOS);

  if (associate(data, active)) {
    const Observer_rch observer = get_observer(Observer::e_ASSOCIATED);
    if (observer) {
      observer->on_associated(this, data.remote_id_);
    }
  } else {
    //FUTURE: inform inforepo and try again as passive peer
    if (DCPS_debug_level) {
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("(%P|%t) DataWriterImpl::add_association: ")
                 ACE_TEXT("ERROR: transport layer failed to associate.\n")));
    }
  }
}

void
DataWriterImpl::transport_assoc_done(int flags, const GUID_t& remote_id)
{
  DBG_ENTRY_LVL("DataWriterImpl", "transport_assoc_done", 6);

  if (!(flags & ASSOC_OK)) {
    if (DCPS_debug_level) {
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("(%P|%t) DataWriterImpl::transport_assoc_done: ")
                 ACE_TEXT("ERROR: transport layer failed to associate %C\n"),
                 LogGuid(remote_id).c_str()));
    }

    return;
  }

  ACE_Guard<ACE_Recursive_Thread_Mutex> guard(lock_);

  if (DCPS_debug_level) {
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("(%P|%t) DataWriterImpl::transport_assoc_done: ")
               ACE_TEXT("writer %C succeeded in associating with reader %C\n"),
               LogGuid(publication_id_).c_str(),
               LogGuid(remote_id).c_str()));
  }

  if (flags & ASSOC_ACTIVE) {

    // Have we already received an association_complete() callback?
    if (DCPS_debug_level) {
      ACE_DEBUG((LM_DEBUG,
                 ACE_TEXT("(%P|%t) DataWriterImpl::transport_assoc_done: ")
                 ACE_TEXT("writer %C reader %C calling association_complete_i\n"),
                 LogGuid(publication_id_).c_str(),
                 LogGuid(remote_id).c_str()));
    }
    association_complete_i(remote_id);

  } else {
    // In the current implementation, DataWriter is always active, so this
    // code will not be applicable.
    if (DCPS_debug_level) {
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("(%P|%t) DataWriterImpl::transport_assoc_done: ")
                 ACE_TEXT("ERROR: DataWriter (%C) should always be active in current implementation\n"),
                 LogGuid(publication_id_).c_str()));
    }
  }
}

DataWriterImpl::ReaderInfo::ReaderInfo(const char* filterClassName,
                                       const char* filter,
                                       const DDS::StringSeq& params,
                                       WeakRcHandle<DomainParticipantImpl> participant,
                                       bool durable)
#ifndef OPENDDS_NO_CONTENT_FILTERED_TOPIC
  : participant_(participant)
  , filter_class_name_(filterClassName)
  , filter_(filter)
  , expression_params_(params)
  , expected_sequence_(SequenceNumber::SEQUENCENUMBER_UNKNOWN())
  , durable_(durable)
{
  RcHandle<DomainParticipantImpl> part = participant_.lock();
  if (part && *filter) {
    eval_ = part->get_filter_eval(filter);
  }
}
#else
  : expected_sequence_(SequenceNumber::SEQUENCENUMBER_UNKNOWN())
  , durable_(durable)
{
  ACE_UNUSED_ARG(filterClassName);
  ACE_UNUSED_ARG(filter);
  ACE_UNUSED_ARG(params);
  ACE_UNUSED_ARG(participant);
}
#endif // OPENDDS_NO_CONTENT_FILTERED_TOPIC

DataWriterImpl::ReaderInfo::~ReaderInfo()
{
#ifndef OPENDDS_NO_CONTENT_FILTERED_TOPIC
  eval_ = RcHandle<FilterEvaluator>();
  RcHandle<DomainParticipantImpl> participant = participant_.lock();
  if (participant && !filter_.empty()) {
    participant->deref_filter_eval(filter_.c_str());
  }

#endif // OPENDDS_NO_CONTENT_FILTERED_TOPIC
}

void
DataWriterImpl::association_complete_i(const GUID_t& remote_id)
{
  DBG_ENTRY_LVL("DataWriterImpl", "association_complete_i", 6);

  bool reader_durable = false;
#ifndef OPENDDS_NO_CONTENT_FILTERED_TOPIC
  OPENDDS_STRING filterClassName;
  RcHandle<FilterEvaluator> eval;
  DDS::StringSeq expression_params;
#endif
  {
    ACE_GUARD(ACE_Recursive_Thread_Mutex, guard, this->lock_);

    if (DCPS_debug_level >= 1) {
      ACE_DEBUG((LM_DEBUG,
                 ACE_TEXT("(%P|%t) DataWriterImpl::association_complete_i - ")
                 ACE_TEXT("bit %d local %C remote %C\n"),
                 is_bit_,
                 LogGuid(this->publication_id_).c_str(),
                 LogGuid(remote_id).c_str()));
    }

    if (insert(readers_, remote_id) == -1) {
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("(%P|%t) ERROR: DataWriterImpl::association_complete_i: ")
                 ACE_TEXT("insert %C from pending failed.\n"),
                 LogGuid(remote_id).c_str()));
    }
  }
  {
    ACE_GUARD(ACE_Thread_Mutex, reader_info_guard, this->reader_info_lock_);
    RepoIdToReaderInfoMap::const_iterator it = reader_info_.find(remote_id);

    if (it != reader_info_.end()) {
      reader_durable = it->second.durable_;
#ifndef OPENDDS_NO_CONTENT_FILTERED_TOPIC
      filterClassName = it->second.filter_class_name_;
      eval = it->second.eval_;
      expression_params = it->second.expression_params_;
#endif
    }
  }

  if (this->monitor_) {
    this->monitor_->report();
  }

  if (!is_bit_) {

    RcHandle<DomainParticipantImpl> participant = this->participant_servant_.lock();

    if (!participant)
      return;

    data_container_->add_reader_acks(remote_id, get_max_sn());

    const DDS::InstanceHandle_t handle = participant->assign_handle(remote_id);

    {
      // protect publication_match_status_ and status changed flags.
      ACE_GUARD(ACE_Recursive_Thread_Mutex, guard, this->lock_);

      if (DCPS::bind(id_to_handle_map_, remote_id, handle) != 0) {
        ACE_DEBUG((LM_WARNING,
                   ACE_TEXT("(%P|%t) WARNING: DataWriterImpl::association_complete_i: ")
                   ACE_TEXT("id_to_handle_map_%C = 0x%x failed.\n"),
                   LogGuid(remote_id).c_str(),
                   handle));
        return;

      } else if (DCPS_debug_level > 4) {
        ACE_DEBUG((LM_DEBUG,
                   ACE_TEXT("(%P|%t) DataWriterImpl::association_complete_i: ")
                   ACE_TEXT("id_to_handle_map_%C = 0x%x.\n"),
                   LogGuid(remote_id).c_str(),
                   handle));
      }

      ++publication_match_status_.total_count;
      ++publication_match_status_.total_count_change;
      ++publication_match_status_.current_count;
      ++publication_match_status_.current_count_change;
      publication_match_status_.last_subscription_handle = handle;
      set_status_changed_flag(DDS::PUBLICATION_MATCHED_STATUS, true);
    }

    DDS::DataWriterListener_var listener =
      listener_for(DDS::PUBLICATION_MATCHED_STATUS);

    if (!CORBA::is_nil(listener.in())) {

      listener->on_publication_matched(this, publication_match_status_);

      // TBD - why does the spec say to change this but not
      // change the ChangeFlagStatus after a listener call?
      publication_match_status_.total_count_change = 0;
      publication_match_status_.current_count_change = 0;
    }

    notify_status_condition();
  } else {
    data_container_->add_reader_acks(remote_id, get_max_sn());
  }

  // Support DURABILITY QoS
  if (reader_durable) {
    // Tell the WriteDataContainer to resend all sending/sent
    // samples.
    this->data_container_->reenqueue_all(remote_id, this->qos_.lifespan
#ifndef OPENDDS_NO_CONTENT_FILTERED_TOPIC
                                         , filterClassName, eval.in(), expression_params
#endif
                                        );

    // Acquire the data writer container lock to avoid deadlock. The
    // thread calling association_complete() has to acquire lock in the
    // same order as the write()/register() operation.

    // Since the thread calling association_complete() is the ORB
    // thread, it may have some performance penalty. If the
    // performance is an issue, we may need a new thread to handle the
    // data_available() calls.
    ACE_GUARD(ACE_Recursive_Thread_Mutex,
              guard,
              this->get_lock());

    SendStateDataSampleList list = this->get_resend_data();
    {
      ACE_GUARD(ACE_Thread_Mutex, reader_info_guard, this->reader_info_lock_);
      // Update the reader's expected sequence
      SequenceNumber& seq =
        reader_info_.find(remote_id)->second.expected_sequence_;

      for (SendStateDataSampleList::iterator list_el = list.begin();
           list_el != list.end(); ++list_el) {
        list_el->get_header().historic_sample_ = true;

        if (list_el->get_header().sequence_ > seq) {
          seq = list_el->get_header().sequence_;
        }
      }
    }

    RcHandle<PublisherImpl> publisher = this->publisher_servant_.lock();
    if (!publisher || publisher->is_suspended()) {
      this->available_data_list_.enqueue_tail(list);

    } else {
      if (DCPS_debug_level >= 4) {
        ACE_DEBUG((LM_INFO, "(%P|%t) Sending historic samples\n"));
      }

      const Encoding encoding(Encoding::KIND_UNALIGNED_CDR);
      size_t size = 0;
      serialized_size(encoding, size, remote_id);
      Message_Block_Ptr data(
        new ACE_Message_Block(size, ACE_Message_Block::MB_DATA, 0, 0, 0,
                              get_db_lock()));
      Serializer ser(data.get(), encoding);
      ser << remote_id;

      DataSampleHeader header;
      Message_Block_Ptr end_historic_samples(
        create_control_message(END_HISTORIC_SAMPLES, header, OPENDDS_MOVE_NS::move(data),
          SystemTimePoint::now().to_idl_struct()));

      this->controlTracker.message_sent();
      guard.release();
      ACE_Reverse_Lock<ACE_Recursive_Thread_Mutex> rev_lock(lock_);
      ACE_Guard<ACE_Reverse_Lock<ACE_Recursive_Thread_Mutex> > rev_guard(rev_lock);
      SendControlStatus ret = send_w_control(list, header, OPENDDS_MOVE_NS::move(end_historic_samples), remote_id);
      if (ret == SEND_CONTROL_ERROR) {
        ACE_ERROR((LM_WARNING, ACE_TEXT("(%P|%t) WARNING: ")
                             ACE_TEXT("DataWriterImpl::association_complete_i: ")
                             ACE_TEXT("send_w_control failed.\n")));
        this->controlTracker.message_dropped();
      }
    }
  }
}

void
DataWriterImpl::remove_associations(const ReaderIdSeq & readers,
                                    CORBA::Boolean notify_lost)
{
  if (readers.length() == 0) {
    return;
  }

  const Observer_rch observer = get_observer(Observer::e_DISASSOCIATED);
  if (observer) {
    for (CORBA::ULong i = 0; i < readers.length(); ++i) {
      observer->on_disassociated(this, readers[i]);
    }
  }

  if (DCPS_debug_level >= 1) {
    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("(%P|%t) DataWriterImpl::remove_associations: ")
               ACE_TEXT("bit %d local %C remote %C num remotes %d\n"),
               is_bit_,
               LogGuid(publication_id_).c_str(),
               LogGuid(readers[0]).c_str(),
               readers.length()));
  }

  // stop pending associations for these reader ids
  this->stop_associating(readers.get_buffer(), readers.length());

  ReaderIdSeq fully_associated_readers;
  CORBA::ULong fully_associated_len = 0;
  ReaderIdSeq rds;
  CORBA::ULong rds_len = 0;
  DDS::InstanceHandleSeq handles;

  ACE_GUARD(ACE_Thread_Mutex, wait_guard, sync_unreg_rem_assocs_lock_);
  {
    // Ensure the same acquisition order as in wait_for_acknowledgments().
    ACE_GUARD(ACE_Recursive_Thread_Mutex, guard, this->lock_);
    //Remove the readers from fully associated reader list.
    //If the supplied reader is not in the cached reader list then it is
    //already removed. We just need remove the readers in the list that have
    //not been removed.

    CORBA::ULong len = readers.length();

    for (CORBA::ULong i = 0; i < len; ++i) {
      //Remove the readers from fully associated reader list. If it's not
      //in there, the association_complete() is not called yet and remove it
      //from pending list.

      if (remove(readers_, readers[i]) == 0) {
        ++ fully_associated_len;
        fully_associated_readers.length(fully_associated_len);
        fully_associated_readers [fully_associated_len - 1] = readers[i];

        ++ rds_len;
        rds.length(rds_len);
        rds [rds_len - 1] = readers[i];
      }

      data_container_->remove_reader_acks(readers[i]);

      ACE_GUARD(ACE_Thread_Mutex, reader_info_guard, this->reader_info_lock_);
      reader_info_.erase(readers[i]);
      //else reader is already removed which indicates remove_association()
      //is called multiple times.
    }

    if (fully_associated_len > 0 && !is_bit_) {
      // The reader should be in the id_to_handle map at this time
      this->lookup_instance_handles(fully_associated_readers, handles);

      for (CORBA::ULong i = 0; i < fully_associated_len; ++i) {
        id_to_handle_map_.erase(fully_associated_readers[i]);
      }
    }

    // Mirror the PUBLICATION_MATCHED_STATUS processing from
    // association_complete() here.
    if (!this->is_bit_) {

      // Derive the change in the number of subscriptions reading this writer.
      int matchedSubscriptions =
        static_cast<int>(this->id_to_handle_map_.size());
      this->publication_match_status_.current_count_change =
        matchedSubscriptions - this->publication_match_status_.current_count;

      // Only process status if the number of subscriptions has changed.
      if (this->publication_match_status_.current_count_change != 0) {
        this->publication_match_status_.current_count = matchedSubscriptions;

        /// Section 7.1.4.1: total_count will not decrement.

        /// @TODO: Reconcile this with the verbiage in section 7.1.4.1
        this->publication_match_status_.last_subscription_handle =
          handles[fully_associated_len - 1];

        set_status_changed_flag(DDS::PUBLICATION_MATCHED_STATUS, true);

        DDS::DataWriterListener_var listener =
          this->listener_for(DDS::PUBLICATION_MATCHED_STATUS);

        if (!CORBA::is_nil(listener.in())) {
          listener->on_publication_matched(this, this->publication_match_status_);

          // Listener consumes the change.
          this->publication_match_status_.total_count_change = 0;
          this->publication_match_status_.current_count_change = 0;
        }

        this->notify_status_condition();
      }
    }
  }

  for (CORBA::ULong i = 0; i < rds.length(); ++i) {
    this->disassociate(rds[i]);
  }

  // If this remove_association is invoked when the InfoRepo
  // detects a lost reader then make a callback to notify
  // subscription lost.
  if (notify_lost && handles.length() > 0) {
    this->notify_publication_lost(handles);
  }

  const RcHandle<DomainParticipantImpl> participant = participant_servant_.lock();
  for (unsigned int i = 0; i < handles.length(); ++i) {
    participant->return_handle(handles[i]);
  }
}

void DataWriterImpl::replay_durable_data_for(const GUID_t& remote_id)
{
  DBG_ENTRY_LVL("DataWriterImpl", "replay_durable_data_for", 6);

  bool reader_durable = false;
#ifndef OPENDDS_NO_CONTENT_FILTERED_TOPIC
  OPENDDS_STRING filterClassName;
  RcHandle<FilterEvaluator> eval;
  DDS::StringSeq expression_params;
#endif

  {
    ACE_GUARD(ACE_Thread_Mutex, reader_info_guard, this->reader_info_lock_);
    RepoIdToReaderInfoMap::const_iterator it = reader_info_.find(remote_id);

    if (it != reader_info_.end()) {
      reader_durable = it->second.durable_;
#ifndef OPENDDS_NO_CONTENT_FILTERED_TOPIC
      filterClassName = it->second.filter_class_name_;
      eval = it->second.eval_;
      expression_params = it->second.expression_params_;
#endif
    }
  }

  // Support DURABILITY QoS
  if (reader_durable) {
    // Tell the WriteDataContainer to resend all sending/sent
    // samples.
    this->data_container_->reenqueue_all(remote_id, this->qos_.lifespan
#ifndef OPENDDS_NO_CONTENT_FILTERED_TOPIC
                                         , filterClassName, eval.in(), expression_params
#endif
                                         );

    // Acquire the data writer container lock to avoid deadlock. The
    // thread calling association_complete() has to acquire lock in the
    // same order as the write()/register() operation.

    // Since the thread calling association_complete() is the ORB
    // thread, it may have some performance penalty. If the
    // performance is an issue, we may need a new thread to handle the
    // data_available() calls.
    ACE_GUARD(ACE_Recursive_Thread_Mutex,
              guard,
              this->get_lock());

    SendStateDataSampleList list = this->get_resend_data();
    {
      ACE_GUARD(ACE_Thread_Mutex, reader_info_guard, this->reader_info_lock_);
      // Update the reader's expected sequence
      SequenceNumber& seq =
        reader_info_.find(remote_id)->second.expected_sequence_;

      for (SendStateDataSampleList::iterator list_el = list.begin();
           list_el != list.end(); ++list_el) {
        list_el->get_header().historic_sample_ = true;

        if (list_el->get_header().sequence_ > seq) {
          seq = list_el->get_header().sequence_;
        }
      }
    }

    RcHandle<PublisherImpl> publisher = this->publisher_servant_.lock();
    if (!publisher || publisher->is_suspended()) {
      this->available_data_list_.enqueue_tail(list);

    } else {
      if (DCPS_debug_level >= 4) {
        ACE_DEBUG((LM_INFO, ACE_TEXT("(%P|%t) DataWriterImpl::replay_durable_data_for: Sending historic samples\n")));
      }

      const Encoding encoding(Encoding::KIND_UNALIGNED_CDR);
      size_t size = 0;
      serialized_size(encoding, size, remote_id);
      Message_Block_Ptr data(
        new ACE_Message_Block(size, ACE_Message_Block::MB_DATA, 0, 0, 0,
                              get_db_lock()));
      Serializer ser(data.get(), encoding);
      ser << remote_id;

      DataSampleHeader header;
      Message_Block_Ptr end_historic_samples(create_control_message(END_HISTORIC_SAMPLES, header, OPENDDS_MOVE_NS::move(data),
                                                                    SystemTimePoint::now().to_idl_struct()));

      this->controlTracker.message_sent();
      guard.release();
      const SendControlStatus ret = send_w_control(list, header, OPENDDS_MOVE_NS::move(end_historic_samples), remote_id);
      if (ret == SEND_CONTROL_ERROR) {
        ACE_ERROR((LM_ERROR, ACE_TEXT("(%P|%t) ERROR: ")
                   ACE_TEXT("DataWriterImpl::replay_durable_data_for: ")
                   ACE_TEXT("send_w_control failed.\n")));
        this->controlTracker.message_dropped();
      }
    }
  }
}

void DataWriterImpl::remove_all_associations()
{
  DBG_ENTRY_LVL("DataWriterImpl", "remove_all_associations", 6);
  // stop pending associations
  this->stop_associating();

  ReaderIdSeq readers;
  CORBA::ULong size;
  {
    ACE_GUARD(ACE_Recursive_Thread_Mutex, guard, lock_);

    size = static_cast<CORBA::ULong>(readers_.size());
    readers.length(size);

    const RepoIdSet::iterator itEnd = readers_.end();
    DDS::UInt32 i = 0;
    for (RepoIdSet::iterator it = readers_.begin(); it != itEnd; ++it, ++i) {
      readers[i] = *it;
    }
  }

  try {
    if (0 < size) {
      CORBA::Boolean dont_notify_lost = false;

      this->remove_associations(readers, dont_notify_lost);
    }

  } catch (const CORBA::Exception&) {
      ACE_DEBUG((LM_WARNING,
                 ACE_TEXT("(%P|%t) WARNING: DataWriterImpl::remove_all_associations() - ")
                 ACE_TEXT("caught exception from remove_associations.\n")));
  }

  transport_stop();
}

void
DataWriterImpl::register_for_reader(const GUID_t& participant,
                                    const GUID_t& writerid,
                                    const GUID_t& readerid,
                                    const TransportLocatorSeq& locators,
                                    DiscoveryListener* listener)
{
  TransportClient::register_for_reader(participant, writerid, readerid, locators, listener);
}

void
DataWriterImpl::unregister_for_reader(const GUID_t& participant,
                                      const GUID_t& writerid,
                                      const GUID_t& readerid)
{
  TransportClient::unregister_for_reader(participant, writerid, readerid);
}

void
DataWriterImpl::update_locators(const GUID_t& readerId,
                                const TransportLocatorSeq& locators)
{
  {
    ACE_GUARD(ACE_Thread_Mutex, reader_info_guard, reader_info_lock_);
    RepoIdToReaderInfoMap::const_iterator iter = reader_info_.find(readerId);
    if (iter == reader_info_.end()) {
      return;
    }
  }
  TransportClient::update_locators(readerId, locators);
}

void
DataWriterImpl::update_incompatible_qos(const IncompatibleQosStatus& status)
{
  DDS::DataWriterListener_var listener =
    listener_for(DDS::OFFERED_INCOMPATIBLE_QOS_STATUS);

  ACE_GUARD(ACE_Recursive_Thread_Mutex, guard, this->lock_);

#if 0

  if (this->offered_incompatible_qos_status_.total_count == status.total_count) {
    // This test should make the method idempotent.
    return;
  }

#endif

  set_status_changed_flag(DDS::OFFERED_INCOMPATIBLE_QOS_STATUS, true);

  // copy status and increment change
  offered_incompatible_qos_status_.total_count = status.total_count;
  offered_incompatible_qos_status_.total_count_change +=
    status.count_since_last_send;
  offered_incompatible_qos_status_.last_policy_id = status.last_policy_id;
  offered_incompatible_qos_status_.policies = status.policies;

  if (!CORBA::is_nil(listener.in())) {
    listener->on_offered_incompatible_qos(this, offered_incompatible_qos_status_);

    // TBD - Why does the spec say to change this but not change the
    //       ChangeFlagStatus after a listener call?
    offered_incompatible_qos_status_.total_count_change = 0;
  }

  notify_status_condition();
}

void
DataWriterImpl::update_subscription_params(const GUID_t& readerId,
                                           const DDS::StringSeq& params)
{
#ifdef OPENDDS_NO_CONTENT_FILTERED_TOPIC
  ACE_UNUSED_ARG(readerId);
  ACE_UNUSED_ARG(params);
#else
  ACE_GUARD(ACE_Recursive_Thread_Mutex, guard, this->lock_);
  ACE_GUARD(ACE_Thread_Mutex, reader_info_guard, this->reader_info_lock_);
  RepoIdToReaderInfoMap::iterator iter = reader_info_.find(readerId);

  if (iter != reader_info_.end()) {
    iter->second.expression_params_ = params;

  } else if (DCPS_debug_level > 4 &&
             publisher_content_filter_) {
    ACE_DEBUG((LM_WARNING,
               ACE_TEXT("(%P|%t) WARNING: DataWriterImpl::update_subscription_params()")
               ACE_TEXT(" - writer: %C has no info about reader: %C\n"),
               LogGuid(this->publication_id_).c_str(), LogGuid(readerId).c_str()));
  }

#endif
}

DDS::ReturnCode_t DataWriterImpl::set_qos(const DDS::DataWriterQos& qos)
{
  OPENDDS_NO_OWNERSHIP_KIND_EXCLUSIVE_COMPATIBILITY_CHECK(qos, DDS::RETCODE_UNSUPPORTED);
  OPENDDS_NO_OWNERSHIP_STRENGTH_COMPATIBILITY_CHECK(qos, DDS::RETCODE_UNSUPPORTED);
  OPENDDS_NO_OWNERSHIP_PROFILE_COMPATIBILITY_CHECK(qos, DDS::RETCODE_UNSUPPORTED);
  OPENDDS_NO_DURABILITY_SERVICE_COMPATIBILITY_CHECK(qos, DDS::RETCODE_UNSUPPORTED);
  OPENDDS_NO_DURABILITY_KIND_TRANSIENT_PERSISTENT_COMPATIBILITY_CHECK(qos, DDS::RETCODE_UNSUPPORTED);

  DDS::DataWriterQos new_qos = qos;
  new_qos.representation.value = qos_.representation.value;
  if (Qos_Helper::valid(new_qos) && Qos_Helper::consistent(new_qos)) {
    if (qos_ == new_qos)
      return DDS::RETCODE_OK;

    if (enabled_) {
      if (!Qos_Helper::changeable(qos_, new_qos)) {
        return DDS::RETCODE_IMMUTABLE_POLICY;
      }

      Discovery_rch disco = TheServiceParticipant->get_discovery(domain_id_);
      DDS::PublisherQos publisherQos;
      RcHandle<PublisherImpl> publisher = this->publisher_servant_.lock();

      bool status = false;
      if (publisher) {
        publisher->get_qos(publisherQos);
        status
          = disco->update_publication_qos(domain_id_,
                                          dp_id_,
                                          this->publication_id_,
                                          new_qos,
                                          publisherQos);
      }
      if (!status) {
        ACE_ERROR_RETURN((LM_ERROR,
                          ACE_TEXT("(%P|%t) DataWriterImpl::set_qos, ")
                          ACE_TEXT("qos not updated.\n")),
                         DDS::RETCODE_ERROR);
      }

      if (!(qos_ == new_qos)) {
        data_container_->set_deadline_period(TimeDuration(qos.deadline.period));
        qos_ = new_qos;
      }
    }

    qos_ = new_qos;
    passed_qos_ = qos;

    const Observer_rch observer = get_observer(Observer::e_QOS_CHANGED);
    if (observer) {
      observer->on_qos_changed(this);
    }

    return DDS::RETCODE_OK;

  } else {
    return DDS::RETCODE_INCONSISTENT_POLICY;
  }
}

DDS::ReturnCode_t
DataWriterImpl::get_qos(DDS::DataWriterQos & qos)
{
  qos = passed_qos_;
  return DDS::RETCODE_OK;
}

DDS::ReturnCode_t
DataWriterImpl::set_listener(DDS::DataWriterListener_ptr a_listener,
                             DDS::StatusMask mask)
{
  ACE_Guard<ACE_Thread_Mutex> g(listener_mutex_);
  listener_mask_ = mask;
  //note: OK to duplicate  a nil object ref
  listener_ = DDS::DataWriterListener::_duplicate(a_listener);
  return DDS::RETCODE_OK;
}

DDS::DataWriterListener_ptr
DataWriterImpl::get_listener()
{
  ACE_Guard<ACE_Thread_Mutex> g(listener_mutex_);
  return DDS::DataWriterListener::_duplicate(listener_.in());
}

DataWriterListener_ptr
DataWriterImpl::get_ext_listener()
{
  ACE_Guard<ACE_Thread_Mutex> g(listener_mutex_);
  return DataWriterListener::_narrow(listener_.in());
}

DDS::Topic_ptr
DataWriterImpl::get_topic()
{
  return DDS::Topic::_duplicate(topic_servant_.get());
}

bool
DataWriterImpl::should_ack() const
{
  // N.B. It may be worthwhile to investigate a more efficient
  // heuristic for determining if a writer should send SAMPLE_ACK
  // control samples. Perhaps based on a sequence number delta?
  return this->readers_.size() != 0;
}

DataWriterImpl::AckToken
DataWriterImpl::create_ack_token(DDS::Duration_t max_wait) const
{
  const SequenceNumber sn = get_max_sn();
  if (DCPS_debug_level > 0) {
    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("(%P|%t) DataWriterImpl::create_ack_token() - ")
               ACE_TEXT("for sequence %q\n"),
               sn.getValue()));
  }
  return AckToken(max_wait, sn);
}



DDS::ReturnCode_t
DataWriterImpl::send_request_ack()
{
  ACE_GUARD_RETURN(ACE_Recursive_Thread_Mutex,
                   guard,
                   get_lock(),
                   DDS::RETCODE_ERROR);


  DataSampleElement* element = 0;
  DDS::ReturnCode_t ret = this->data_container_->obtain_buffer_for_control(element);

  if (ret != DDS::RETCODE_OK) {
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: ")
                      ACE_TEXT("DataWriterImpl::send_request_ack: ")
                      ACE_TEXT("obtain_buffer_for_control returned %d.\n"),
                      ret),
                     ret);
  }

  Message_Block_Ptr blk;
  // Add header with the registration sample data.
  Message_Block_Ptr sample(
    create_control_message(
      REQUEST_ACK,
      element->get_header(),
      OPENDDS_MOVE_NS::move(blk),
      SystemTimePoint::now().to_idl_struct()));

  element->set_sample(OPENDDS_MOVE_NS::move(sample));

  ret = this->data_container_->enqueue_control(element);

  if (ret != DDS::RETCODE_OK) {
    data_container_->release_buffer(element);
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: ")
                      ACE_TEXT("DataWriterImpl::send_request_ack: ")
                      ACE_TEXT("enqueue_control failed.\n")),
                     ret);
  }


  send_all_to_flush_control(guard);

  return DDS::RETCODE_OK;
}

DDS::ReturnCode_t
DataWriterImpl::wait_for_acknowledgments(const DDS::Duration_t& max_wait)
{
  if (this->qos_.reliability.kind != DDS::RELIABLE_RELIABILITY_QOS)
    return DDS::RETCODE_OK;

  DDS::ReturnCode_t ret = send_request_ack();

  if (ret != DDS::RETCODE_OK)
    return ret;

  DataWriterImpl::AckToken token = create_ack_token(max_wait);
  if (DCPS_debug_level) {
    ACE_DEBUG ((LM_DEBUG, ACE_TEXT("(%P|%t) DataWriterImpl::wait_for_acknowledgments")
                          ACE_TEXT(" waiting for acknowledgment of sequence %q at %T\n"),
                          token.sequence_.getValue()));
  }
  return wait_for_specific_ack(token);
}

DDS::ReturnCode_t
DataWriterImpl::wait_for_specific_ack(const AckToken& token)
{
  return this->data_container_->wait_ack_of_seq(token.deadline(), token.deadline_is_infinite(), token.sequence_);
}

DDS::Publisher_ptr
DataWriterImpl::get_publisher()
{
  return publisher_servant_.lock()._retn();
}

DDS::ReturnCode_t
DataWriterImpl::get_liveliness_lost_status(
  DDS::LivelinessLostStatus & status)
{
  ACE_GUARD_RETURN(ACE_Recursive_Thread_Mutex,
                   guard,
                   this->lock_,
                   DDS::RETCODE_ERROR);
  set_status_changed_flag(DDS::LIVELINESS_LOST_STATUS, false);
  status = liveliness_lost_status_;
  liveliness_lost_status_.total_count_change = 0;
  return DDS::RETCODE_OK;
}

DDS::ReturnCode_t
DataWriterImpl::get_offered_deadline_missed_status(
  DDS::OfferedDeadlineMissedStatus & status)
{
  ACE_GUARD_RETURN(ACE_Recursive_Thread_Mutex,
                   guard,
                   this->lock_,
                   DDS::RETCODE_ERROR);

  set_status_changed_flag(DDS::OFFERED_DEADLINE_MISSED_STATUS, false);

  this->offered_deadline_missed_status_.total_count_change =
    this->offered_deadline_missed_status_.total_count
    - this->last_deadline_missed_total_count_;

  // Update for next status check.
  this->last_deadline_missed_total_count_ =
    this->offered_deadline_missed_status_.total_count;

  status = offered_deadline_missed_status_;

  this->offered_deadline_missed_status_.total_count_change = 0;

  return DDS::RETCODE_OK;
}

DDS::ReturnCode_t
DataWriterImpl::get_offered_incompatible_qos_status(
  DDS::OfferedIncompatibleQosStatus & status)
{
  ACE_GUARD_RETURN(ACE_Recursive_Thread_Mutex,
                   guard,
                   this->lock_,
                   DDS::RETCODE_ERROR);
  set_status_changed_flag(DDS::OFFERED_INCOMPATIBLE_QOS_STATUS, false);
  status = offered_incompatible_qos_status_;
  offered_incompatible_qos_status_.total_count_change = 0;
  return DDS::RETCODE_OK;
}

DDS::ReturnCode_t
DataWriterImpl::get_publication_matched_status(
  DDS::PublicationMatchedStatus & status)
{
  ACE_GUARD_RETURN(ACE_Recursive_Thread_Mutex,
                   guard,
                   this->lock_,
                   DDS::RETCODE_ERROR);
  set_status_changed_flag(DDS::PUBLICATION_MATCHED_STATUS, false);
  status = publication_match_status_;
  publication_match_status_.total_count_change = 0;
  publication_match_status_.current_count_change = 0;
  return DDS::RETCODE_OK;
}

DDS::ReturnCode_t
DataWriterImpl::assert_liveliness()
{
  ACE_Guard<ACE_Recursive_Thread_Mutex> guard(lock_);
  switch (this->qos_.liveliness.kind) {
  case DDS::AUTOMATIC_LIVELINESS_QOS:
    // Do nothing.
    break;
  case DDS::MANUAL_BY_PARTICIPANT_LIVELINESS_QOS:
    {
      RcHandle<DomainParticipantImpl> participant = this->participant_servant_.lock();
      if (participant) {
        return participant->assert_liveliness();
      }
    }
    break;
  case DDS::MANUAL_BY_TOPIC_LIVELINESS_QOS:
    if (!send_liveliness(MonotonicTimePoint::now())) {
      return DDS::RETCODE_ERROR;
    }
    break;
  }

  return DDS::RETCODE_OK;
}

DDS::ReturnCode_t
DataWriterImpl::assert_liveliness_by_participant()
{
  ACE_Guard<ACE_Recursive_Thread_Mutex> guard(lock_);
  // This operation is called by participant.
  if (this->qos_.liveliness.kind == DDS::MANUAL_BY_PARTICIPANT_LIVELINESS_QOS &&
      !send_liveliness(MonotonicTimePoint::now())) {
    return DDS::RETCODE_ERROR;
  }

  return DDS::RETCODE_OK;
}

TimeDuration
DataWriterImpl::liveliness_check_interval(DDS::LivelinessQosPolicyKind kind)
{
  if (this->qos_.liveliness.kind == kind) {
    return liveliness_send_interval_;
  } else {
    return TimeDuration::max_value;
  }
}

bool
DataWriterImpl::participant_liveliness_activity_after(const MonotonicTimePoint& tv)
{
  ACE_Guard<ACE_Recursive_Thread_Mutex> guard(lock_);
  if (this->qos_.liveliness.kind == DDS::MANUAL_BY_PARTICIPANT_LIVELINESS_QOS) {
    return last_liveliness_activity_time_ > tv;
  } else {
    return false;
  }
}

DDS::ReturnCode_t
DataWriterImpl::get_matched_subscriptions(
  DDS::InstanceHandleSeq & subscription_handles)
{
  if (!enabled_) {
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: ")
                      ACE_TEXT("DataWriterImpl::get_matched_subscriptions: ")
                      ACE_TEXT(" Entity is not enabled.\n")),
                     DDS::RETCODE_NOT_ENABLED);
  }

  ACE_GUARD_RETURN(ACE_Recursive_Thread_Mutex,
                   guard,
                   this->lock_,
                   DDS::RETCODE_ERROR);

  // Copy out the handles for the current set of subscriptions.
  subscription_handles.length(
    static_cast<CORBA::ULong>(this->id_to_handle_map_.size()));

  DDS::UInt32 index = 0;
  for (RepoIdToHandleMap::iterator current = this->id_to_handle_map_.begin();
       current != this->id_to_handle_map_.end();
       ++current, ++index) {
    subscription_handles[index] = current->second;
  }

  return DDS::RETCODE_OK;
}

#if !defined (DDS_HAS_MINIMUM_BIT)
DDS::ReturnCode_t
DataWriterImpl::get_matched_subscription_data(
  DDS::SubscriptionBuiltinTopicData & subscription_data,
  DDS::InstanceHandle_t subscription_handle)
{
  if (!enabled_) {
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: DataWriterImpl::")
                      ACE_TEXT("get_matched_subscription_data: ")
                      ACE_TEXT("Entity is not enabled.\n")),
                     DDS::RETCODE_NOT_ENABLED);
  }
  RcHandle<DomainParticipantImpl> participant = this->participant_servant_.lock();

  DDS::ReturnCode_t ret = DDS::RETCODE_ERROR;
  DDS::SubscriptionBuiltinTopicDataSeq data;

  if (participant) {
    ret = instance_handle_to_bit_data<DDS::SubscriptionBuiltinTopicDataDataReader_var>(
            participant.in(),
            BUILT_IN_SUBSCRIPTION_TOPIC,
            subscription_handle,
            data);
  }

  if (ret == DDS::RETCODE_OK) {
    subscription_data = data[0];
  }

  return ret;
}
#endif // !defined (DDS_HAS_MINIMUM_BIT)

DDS::ReturnCode_t
DataWriterImpl::enable()
{
  //According spec:
  // - Calling enable on an already enabled Entity returns OK and has no
  // effect.
  // - Calling enable on an Entity whose factory is not enabled will fail
  // and return PRECONDITION_NOT_MET.

  if (this->is_enabled()) {
    return DDS::RETCODE_OK;
  }

  RcHandle<PublisherImpl> publisher = this->publisher_servant_.lock();
  if (!publisher || !publisher->is_enabled()) {
    return DDS::RETCODE_PRECONDITION_NOT_MET;
  }

  if (!topic_servant_->is_enabled()) {
    return DDS::RETCODE_PRECONDITION_NOT_MET;
  }

  RcHandle<DomainParticipantImpl> participant = participant_servant_.lock();
  if (participant) {
    dp_id_ = participant->get_id();
  }

  // Note: do configuration based on QoS in enable() because
  //       before enable is called the QoS can be changed -- even
  //       for Changeable=NO

  // Configure WriteDataContainer constructor parameters from qos.

  const bool reliable = qos_.reliability.kind == DDS::RELIABLE_RELIABILITY_QOS;

  CORBA::Long const max_samples_per_instance =
    (qos_.resource_limits.max_samples_per_instance == DDS::LENGTH_UNLIMITED)
    ? 0x7fffffff : qos_.resource_limits.max_samples_per_instance;

  CORBA::Long max_instances = 0, max_total_samples = 0;

  if (qos_.resource_limits.max_samples != DDS::LENGTH_UNLIMITED) {
    n_chunks_ = static_cast<size_t>(qos_.resource_limits.max_samples);

    if (qos_.resource_limits.max_instances == DDS::LENGTH_UNLIMITED ||
        (qos_.resource_limits.max_samples < qos_.resource_limits.max_instances)
        || (qos_.resource_limits.max_samples <
            (qos_.resource_limits.max_instances * max_samples_per_instance))) {
      max_total_samples = reliable ? qos_.resource_limits.max_samples : 0;
    }
  }

  if (reliable && qos_.resource_limits.max_instances != DDS::LENGTH_UNLIMITED)
    max_instances = qos_.resource_limits.max_instances;

  const CORBA::Long history_depth =
    (qos_.history.kind == DDS::KEEP_ALL_HISTORY_QOS ||
     qos_.history.depth == DDS::LENGTH_UNLIMITED) ? 0x7fffffff : qos_.history.depth;

  const CORBA::Long max_durable_per_instance =
    qos_.durability.kind == DDS::VOLATILE_DURABILITY_QOS ? 0 : history_depth;

#ifndef OPENDDS_NO_PERSISTENCE_PROFILE
  // Get data durability cache if DataWriter QoS requires durable
  // samples.  Publisher servant retains ownership of the cache.
  DataDurabilityCache* const durability_cache =
    TheServiceParticipant->get_data_durability_cache(qos_.durability);
#endif

  //Note: the QoS used to set n_chunks_ is Changeable=No so
  // it is OK that we cannot change the size of our allocators.
  data_container_ = RcHandle<WriteDataContainer>(
    new WriteDataContainer(
      this,
      max_samples_per_instance,
      history_depth,
      max_durable_per_instance,
      qos_.reliability.max_blocking_time,
      n_chunks_,
      domain_id_,
      topic_name_,
      get_type_name(),
#ifndef OPENDDS_NO_PERSISTENCE_PROFILE
      durability_cache,
      qos_.durability_service,
#endif
      max_instances,
      max_total_samples,
      lock_,
      offered_deadline_missed_status_,
      last_deadline_missed_total_count_),
     keep_count());

  // +1 because we might allocate one before releasing another
  // TBD - see if this +1 can be removed.
  mb_allocator_.reset(new MessageBlockAllocator(n_chunks_ * association_chunk_multiplier_));
  db_allocator_.reset(new DataBlockAllocator(n_chunks_+1));
  header_allocator_.reset(new DataSampleHeaderAllocator(n_chunks_+1));

  if (DCPS_debug_level >= 2) {
    ACE_DEBUG((LM_DEBUG,
               "(%P|%t) DataWriterImpl::enable-mb"
               " Cached_Allocator_With_Overflow %x with %B chunks\n",
               mb_allocator_.get(),
               n_chunks_));

    ACE_DEBUG((LM_DEBUG,
               "(%P|%t) DataWriterImpl::enable-db"
               " Cached_Allocator_With_Overflow %x with %B chunks\n",
               db_allocator_.get(),
               n_chunks_));

    ACE_DEBUG((LM_DEBUG,
               "(%P|%t) DataWriterImpl::enable-header"
               " Cached_Allocator_With_Overflow %x with %B chunks\n",
               header_allocator_.get(),
               n_chunks_));
  }

  if (qos_.liveliness.lease_duration.sec != DDS::DURATION_INFINITE_SEC &&
      qos_.liveliness.lease_duration.nanosec != DDS::DURATION_INFINITE_NSEC) {
    // Must be at least 1 micro second.
    liveliness_send_interval_ = std::max(
      TimeDuration(qos_.liveliness.lease_duration) * (TheServiceParticipant->liveliness_factor() / 100.0),
      TimeDuration(0, 1));
    liveliness_lost_interval_ = TimeDuration(qos_.liveliness.lease_duration);
  }

  if (!participant) {
    return DDS::RETCODE_ERROR;
  }

  participant->add_adjust_liveliness_timers(this);

  data_container_->set_deadline_period(TimeDuration(qos_.deadline.period));

  Discovery_rch disco = TheServiceParticipant->get_discovery(this->domain_id_);
  disco->pre_writer(this);

  this->set_enabled();

  try {
    this->enable_transport(reliable,
                           this->qos_.durability.kind > DDS::VOLATILE_DURABILITY_QOS, participant.get());

  } catch (const Transport::Exception&) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("(%P|%t) ERROR: DataWriterImpl::enable, ")
               ACE_TEXT("Transport Exception.\n")));
    data_container_->shutdown_ = true;
    return DDS::RETCODE_ERROR;
  }

  // Must be done after transport enabled.
  set_writer_effective_data_rep_qos(qos_.representation.value, cdr_encapsulation());
  if (!topic_servant_->check_data_representation(qos_.representation.value, true)) {
    data_container_->shutdown_ = true;
    return DDS::RETCODE_ERROR;
  }

  // Done after enable_transport so we know its swap_bytes.
  const DDS::ReturnCode_t setup_serialization_result = setup_serialization();
  if (setup_serialization_result != DDS::RETCODE_OK) {
    data_container_->shutdown_ = true;
    return setup_serialization_result;
  }

  const TransportLocatorSeq& trans_conf_info = connection_info();
  DDS::PublisherQos pub_qos;
  publisher->get_qos(pub_qos);

  TypeInformation type_info;
  type_support_->to_type_info(type_info);

  XTypes::TypeLookupService_rch type_lookup_service = participant->get_type_lookup_service();
  type_support_->add_types(type_lookup_service);

  const bool success =
    disco->add_publication(this->domain_id_,
                           this->dp_id_,
                           this->topic_servant_->get_id(),
                           rchandle_from(this),
                           this->qos_,
                           trans_conf_info,
                           pub_qos,
                           type_info);

  {
    ACE_Guard<ACE_Recursive_Thread_Mutex> guard(lock_);

    if (!success || publication_id_ == GUID_UNKNOWN) {
      if (DCPS_debug_level >= 1) {
        ACE_DEBUG((LM_WARNING, "(%P|%t) WARNING: DataWriterImpl::enable: "
                   "add_publication failed\n"));
      }
      data_container_->shutdown_ = true;
      return DDS::RETCODE_ERROR;
    }

#if OPENDDS_CONFIG_SECURITY
    security_config_ = participant->get_security_config();
    participant_permissions_handle_ = participant->permissions_handle();
    dynamic_type_ = type_support_->get_type();
#endif

    if (DCPS_debug_level >= 2) {
      ACE_DEBUG((LM_DEBUG, "(%P|%t) DataWriterImpl::enable: "
                 "got GUID %C, publishing to topic name \"%C\" type \"%C\"\n",
                 LogGuid(publication_id_).c_str(),
                 topic_servant_->topic_name(), topic_servant_->type_name()));
    }

    this->data_container_->publication_id_ = this->publication_id_;
  }

  if (qos_.liveliness.lease_duration.sec != DDS::DURATION_INFINITE_SEC &&
      qos_.liveliness.lease_duration.nanosec != DDS::DURATION_INFINITE_NSEC) {
    if (qos_.liveliness.kind == DDS::AUTOMATIC_LIVELINESS_QOS) {
      liveliness_send_task_->schedule(liveliness_send_interval_);
    }
    liveliness_lost_task_->schedule(liveliness_lost_interval_);
  }

  const DDS::ReturnCode_t writer_enabled_result =
    publisher->writer_enabled(topic_name_.in(), this);

  if (this->monitor_) {
    this->monitor_->report();
  }

#ifndef OPENDDS_NO_PERSISTENCE_PROFILE

  // Move cached data from the durability cache to the unsent data
  // queue.
  if (durability_cache != 0) {

    if (!durability_cache->get_data(this->domain_id_,
                                    this->topic_name_,
                                    get_type_name(),
                                    this,
                                    this->mb_allocator_.get(),
                                    this->db_allocator_.get(),
                                    this->qos_.lifespan)) {
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("(%P|%t) ERROR: DataWriterImpl::enable: ")
                 ACE_TEXT("unable to retrieve durable data\n")));
    }
  }

#endif

  if (writer_enabled_result == DDS::RETCODE_OK) {
    const Observer_rch observer = get_observer(Observer::e_ENABLED);
    if (observer) {
      observer->on_enabled(this);
    }
  }

  return writer_enabled_result;
}

void
DataWriterImpl::send_all_to_flush_control(ACE_Guard<ACE_Recursive_Thread_Mutex>& guard)
{
  DBG_ENTRY_LVL("DataWriterImpl","send_all_to_flush_control",6);

  SendStateDataSampleList list;

  ACE_UINT64 transaction_id = this->get_unsent_data(list);

  controlTracker.message_sent();

  //need to release guard to call down to transport
  guard.release();

  this->send(list, transaction_id);
}

DDS::ReturnCode_t
DataWriterImpl::register_instance_i(DDS::InstanceHandle_t& handle,
                                    Message_Block_Ptr data,
                                    const DDS::Time_t& source_timestamp)
{
  DBG_ENTRY_LVL("DataWriterImpl","register_instance_i",6);

  if (!enabled_) {
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: ")
                      ACE_TEXT("DataWriterImpl::register_instance_i: ")
                      ACE_TEXT("Entity is not enabled.\n")),
                     DDS::RETCODE_NOT_ENABLED);
  }

  DDS::ReturnCode_t ret = data_container_->register_instance(handle, data);
  if (ret != DDS::RETCODE_OK) {
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: DataWriterImpl::register_instance_i: ")
                      ACE_TEXT("register instance with container failed, returned <%C>.\n"),
                      retcode_to_string(ret)),
                     ret);
  }

  if (this->monitor_) {
    this->monitor_->report();
  }

  DataSampleElement* element = 0;
  ret = this->data_container_->obtain_buffer_for_control(element);
  if (ret != DDS::RETCODE_OK) {
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: ")
                      ACE_TEXT("DataWriterImpl::register_instance_i: ")
                      ACE_TEXT("obtain_buffer_for_control failed, returned <%C>.\n"),
                      retcode_to_string(ret)),
                     ret);
  }

  // Add header with the registration sample data.
  Message_Block_Ptr sample(
    create_control_message(
     INSTANCE_REGISTRATION,
     element->get_header(),
     OPENDDS_MOVE_NS::move(data),
     source_timestamp));

  element->set_sample(OPENDDS_MOVE_NS::move(sample));

  ret = this->data_container_->enqueue_control(element);

  if (ret != DDS::RETCODE_OK) {
    data_container_->release_buffer(element);
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: ")
                      ACE_TEXT("DataWriterImpl::register_instance_i: ")
                      ACE_TEXT("enqueue_control failed, returned <%C>\n"),
                      retcode_to_string(ret)),
                     ret);
  }

  return ret;
}

DDS::ReturnCode_t
DataWriterImpl::register_instance_from_durable_data(
  DDS::InstanceHandle_t& handle,
  Message_Block_Ptr data,
  const DDS::Time_t& source_timestamp)
{
  DBG_ENTRY_LVL("DataWriterImpl","register_instance_from_durable_data",6);

  ACE_GUARD_RETURN(ACE_Recursive_Thread_Mutex,
                   guard,
                   get_lock(),
                   DDS::RETCODE_ERROR);

  const DDS::ReturnCode_t ret = register_instance_i(handle, OPENDDS_MOVE_NS::move(data), source_timestamp);
  if (ret != DDS::RETCODE_OK) {
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: DataWriterImpl::register_instance_from_durable_data: ")
                      ACE_TEXT("register instance with container failed, returned <%C>.\n"),
                      retcode_to_string(ret)),
                     ret);
  }

  send_all_to_flush_control(guard);

  return ret;
}

DDS::ReturnCode_t
DataWriterImpl::unregister_instance_i(DDS::InstanceHandle_t handle,
                                      const Sample* samp,
                                      const DDS::Time_t& source_timestamp)
{
  DBG_ENTRY_LVL("DataWriterImpl","unregister_instance_i",6);

  if (!enabled_) {
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: DataWriterImpl::unregister_instance_i: ")
                      ACE_TEXT("Entity is not enabled.\n")),
                     DDS::RETCODE_NOT_ENABLED);
  }

  // According to spec 1.2, autodispose_unregistered_instances true causes
  // dispose on the instance prior to calling unregister operation.
  if (this->qos_.writer_data_lifecycle.autodispose_unregistered_instances) {
    return this->dispose_and_unregister(handle, samp, source_timestamp);
  }

  DDS::ReturnCode_t ret = DDS::RETCODE_ERROR;
  ACE_GUARD_RETURN(ACE_Recursive_Thread_Mutex, guard, get_lock(), ret);
  Message_Block_Ptr unregistered_sample_data;
  ret = this->data_container_->unregister(handle, unregistered_sample_data);

  if (ret != DDS::RETCODE_OK) {
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: ")
                      ACE_TEXT("DataWriterImpl::unregister_instance_i: ")
                      ACE_TEXT("unregister with container failed.\n")),
                     ret);
  }

  DataSampleElement* element = 0;
  ret = this->data_container_->obtain_buffer_for_control(element);

  if (ret != DDS::RETCODE_OK) {
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: ")
                      ACE_TEXT("DataWriterImpl::unregister_instance_i: ")
                      ACE_TEXT("obtain_buffer_for_control returned %d.\n"),
                      ret),
                     ret);
  }

  Message_Block_Ptr sample(create_control_message(UNREGISTER_INSTANCE,
                                                  element->get_header(),
                                                  OPENDDS_MOVE_NS::move(unregistered_sample_data),
                                                  source_timestamp));
  element->set_sample(OPENDDS_MOVE_NS::move(sample));

  ret = this->data_container_->enqueue_control(element);

  if (ret != DDS::RETCODE_OK) {
    data_container_->release_buffer(element);
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: ")
                      ACE_TEXT("DataWriterImpl::unregister_instance_i: ")
                      ACE_TEXT("enqueue_control failed.\n")),
                     ret);
  }

  send_all_to_flush_control(guard);

  const ValueDispatcher* vd = get_value_dispatcher();
  const Observer_rch observer = get_observer(Observer::e_UNREGISTERED);
  if (observer && samp && samp->native_data() && vd) {
    Observer::Sample s(handle, element->get_header().instance_state(), source_timestamp, element->get_header().sequence_, samp->native_data(), *vd);
    observer->on_unregistered(this, s);
  }

  return DDS::RETCODE_OK;
}

DDS::ReturnCode_t
DataWriterImpl::dispose_and_unregister(DDS::InstanceHandle_t handle,
                                       const Sample* samp,
                                       const DDS::Time_t& source_timestamp)
{
  DBG_ENTRY_LVL("DataWriterImpl", "dispose_and_unregister", 6);

  DDS::ReturnCode_t ret = DDS::RETCODE_ERROR;
  ACE_GUARD_RETURN(ACE_Recursive_Thread_Mutex, guard, get_lock(), ret);

  Message_Block_Ptr data_sample;
  ret = this->data_container_->dispose(handle, data_sample);

  if (ret != DDS::RETCODE_OK) {
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: ")
                      ACE_TEXT("DataWriterImpl::dispose_and_unregister: ")
                      ACE_TEXT("dispose on container failed.\n")),
                     ret);
  }

  ret = this->data_container_->unregister(handle, data_sample, false);

  if (ret != DDS::RETCODE_OK) {
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: ")
                      ACE_TEXT("DataWriterImpl::dispose_and_unregister: ")
                      ACE_TEXT("unregister with container failed.\n")),
                     ret);
  }

  DataSampleElement* element = 0;
  ret = this->data_container_->obtain_buffer_for_control(element);

  if (ret != DDS::RETCODE_OK) {
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: ")
                      ACE_TEXT("DataWriterImpl::dispose_and_unregister: ")
                      ACE_TEXT("obtain_buffer_for_control returned %d.\n"),
                      ret),
                     ret);
  }

  Message_Block_Ptr sample(create_control_message(DISPOSE_UNREGISTER_INSTANCE,
                                                  element->get_header(),
                                                  OPENDDS_MOVE_NS::move(data_sample),
                                                  source_timestamp));
  element->set_sample(OPENDDS_MOVE_NS::move(sample));

  ret = this->data_container_->enqueue_control(element);

  if (ret != DDS::RETCODE_OK) {
    data_container_->release_buffer(element);
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: ")
                      ACE_TEXT("DataWriterImpl::dispose_and_unregister: ")
                      ACE_TEXT("enqueue_control failed.\n")),
                     ret);
  }

  send_all_to_flush_control(guard);

  const ValueDispatcher* vd = get_value_dispatcher();
  {
    const Observer_rch observer = get_observer(Observer::e_DISPOSED);
    if (observer && samp && samp->native_data() && vd) {
      Observer::Sample s(handle, element->get_header().instance_state(), source_timestamp, element->get_header().sequence_, samp->native_data(), *vd);
      observer->on_disposed(this, s);
    }
  }
  {
    const Observer_rch observer = get_observer(Observer::e_UNREGISTERED);
    if (observer && samp && samp->native_data() && vd) {
      Observer::Sample s(handle, element->get_header().instance_state(), source_timestamp, element->get_header().sequence_, samp->native_data(), *vd);
      observer->on_unregistered(this, s);
    }
  }

  return DDS::RETCODE_OK;
}

void
DataWriterImpl::unregister_instances(const DDS::Time_t& source_timestamp)
{
  ACE_GUARD(ACE_Thread_Mutex, guard, sync_unreg_rem_assocs_lock_);

  while (!this->data_container_->instances_.empty()) {
    const DDS::InstanceHandle_t handle = data_container_->instances_.begin()->first;
    InstanceHandlesToValues::const_iterator pos = instance_handles_to_values_.find(handle);
    if (pos != instance_handles_to_values_.end()) {
      const Sample& s = *pos->second;
      unregister_instance_i(handle, &s, source_timestamp);
    } else {
      unregister_instance_i(handle, 0, source_timestamp);
    }
  }
}

DDS::ReturnCode_t
DataWriterImpl::write(Message_Block_Ptr data,
                      DDS::InstanceHandle_t handle,
                      const DDS::Time_t& source_timestamp,
                      GUIDSeq* filter_out,
                      const void* real_data)
{
  DBG_ENTRY_LVL("DataWriterImpl","write",6);

  ACE_Guard<ACE_Recursive_Thread_Mutex> guard(lock_);

  // take ownership of sequence allocated in FooDWImpl::write_w_timestamp()
  GUIDSeq_var filter_out_var(filter_out);

  if (!enabled_) {
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: DataWriterImpl::write: ")
                      ACE_TEXT("Entity is not enabled.\n")),
                     DDS::RETCODE_NOT_ENABLED);
  }

  ACE_GUARD_RETURN (ACE_Recursive_Thread_Mutex,
                    dc_guard,
                    get_lock(),
                    DDS::RETCODE_ERROR);

  DataSampleElement* element = 0;
  DDS::ReturnCode_t ret = this->data_container_->obtain_buffer(element, handle);

  if (ret == DDS::RETCODE_TIMEOUT) {
    return ret; // silent for timeout

  } else if (ret != DDS::RETCODE_OK) {
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: ")
                      ACE_TEXT("DataWriterImpl::write: ")
                      ACE_TEXT("obtain_buffer returned %d.\n"),
                      ret),
                     ret);
  }

  Message_Block_Ptr temp;
  ret = create_sample_data_message(OPENDDS_MOVE_NS::move(data),
                                   handle,
                                   element->get_header(),
                                   temp,
                                   source_timestamp,
                                   (filter_out != 0));
  element->set_sample(OPENDDS_MOVE_NS::move(temp));

  if (ret != DDS::RETCODE_OK) {
    data_container_->release_buffer(element);
    return ret;
  }

  element->set_filter_out(filter_out_var._retn()); // ownership passed to element

  ret = this->data_container_->enqueue(element, handle);

  if (ret != DDS::RETCODE_OK) {
    data_container_->release_buffer(element);
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: ")
                      ACE_TEXT("DataWriterImpl::write: ")
                      ACE_TEXT("enqueue failed.\n")),
                     ret);
  }
  last_liveliness_activity_time_.set_to_now();
  liveliness_lost_ = false;

  track_sequence_number(filter_out);

  if (this->coherent_) {
    ++this->coherent_samples_;
  }
  SendStateDataSampleList list;

  ACE_UINT64 transaction_id = this->get_unsent_data(list);

  RcHandle<PublisherImpl> publisher = this->publisher_servant_.lock();
  if (!publisher || publisher->is_suspended()) {
    if (min_suspended_transaction_id_ == 0) {
      //provides transaction id for lower bound of suspended transactions
      //or transaction id for single suspended write transaction
      min_suspended_transaction_id_ = transaction_id;
    } else {
      //when multiple write transactions have suspended, provides the upper bound
      //for suspended transactions.
      max_suspended_transaction_id_ = transaction_id;
    }
    this->available_data_list_.enqueue_tail(list);

  } else {
    dc_guard.release();
    guard.release();
    this->send(list, transaction_id);
  }

  const ValueDispatcher* vd = get_value_dispatcher();
  const Observer_rch observer = get_observer(Observer::e_SAMPLE_SENT);
  if (observer && real_data && vd) {
    Observer::Sample s(handle, element->get_header().instance_state(), source_timestamp, element->get_header().sequence_, real_data, *vd);
    observer->on_sample_sent(this, s);
  }

  return DDS::RETCODE_OK;
}

void DataWriterImpl::get_flexible_types(const char* key, XTypes::TypeInformation& type_info)
{
  type_support_->get_flexible_types(key, type_info);
}

void
DataWriterImpl::track_sequence_number(GUIDSeq* filter_out)
{
  const SequenceNumber sn = get_max_sn();
  ACE_GUARD(ACE_Thread_Mutex, reader_info_guard, this->reader_info_lock_);

#ifndef OPENDDS_NO_CONTENT_FILTERED_TOPIC
  // Track individual expected sequence numbers in ReaderInfo
  RepoIdSet excluded;

  if (filter_out && !reader_info_.empty()) {
    const GUID_t* buf = filter_out->get_buffer();
    excluded.insert(buf, buf + filter_out->length());
  }

  for (RepoIdToReaderInfoMap::iterator iter = reader_info_.begin(),
       end = reader_info_.end(); iter != end; ++iter) {
    // If not excluding this reader, update expected sequence
    if (excluded.count(iter->first) == 0) {
      iter->second.expected_sequence_ = sn;
    }
  }

#else
  ACE_UNUSED_ARG(filter_out);
  for (RepoIdToReaderInfoMap::iterator iter = reader_info_.begin(),
       end = reader_info_.end(); iter != end; ++iter) {
    iter->second.expected_sequence_ = sn;
  }

#endif // OPENDDS_NO_CONTENT_FILTERED_TOPIC

}

void
DataWriterImpl::send_suspended_data()
{
  //this serves to get TransportClient's max_transaction_id_seen_
  //to the correct value for this list of transactions
  if (max_suspended_transaction_id_ != 0) {
    this->send(this->available_data_list_, max_suspended_transaction_id_);
    max_suspended_transaction_id_ = 0;
  }

  //this serves to actually have the send proceed in
  //sending the samples to the datalinks by passing it
  //the min_suspended_transaction_id_ which should be the
  //TransportClient's expected_transaction_id_
  this->send(this->available_data_list_, min_suspended_transaction_id_);
  min_suspended_transaction_id_ = 0;
  this->available_data_list_.reset();
}

DDS::ReturnCode_t
DataWriterImpl::dispose(DDS::InstanceHandle_t handle,
                        const Sample& samp,
                        const DDS::Time_t & source_timestamp)
{
  DBG_ENTRY_LVL("DataWriterImpl","dispose",6);

  if (!enabled_) {
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: DataWriterImpl::dispose: ")
                      ACE_TEXT("Entity is not enabled.\n")),
                     DDS::RETCODE_NOT_ENABLED);
  }

  DDS::ReturnCode_t ret = DDS::RETCODE_ERROR;

  ACE_GUARD_RETURN (ACE_Recursive_Thread_Mutex, guard, get_lock(), ret);

  Message_Block_Ptr registered_sample_data;
  ret = this->data_container_->dispose(handle, registered_sample_data);

  if (ret != DDS::RETCODE_OK) {
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: ")
                      ACE_TEXT("DataWriterImpl::dispose: ")
                      ACE_TEXT("dispose failed.\n")),
                     ret);
  }

  DataSampleElement* element = 0;
  ret = this->data_container_->obtain_buffer_for_control(element);

  if (ret != DDS::RETCODE_OK) {
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: ")
                      ACE_TEXT("DataWriterImpl::dispose: ")
                      ACE_TEXT("obtain_buffer_for_control returned %d.\n"),
                      ret),
                     ret);
  }

  Message_Block_Ptr sample(create_control_message(DISPOSE_INSTANCE,
                                                  element->get_header(),
                                                  OPENDDS_MOVE_NS::move(registered_sample_data),
                                                  source_timestamp));
  element->set_sample(OPENDDS_MOVE_NS::move(sample));

  ret = this->data_container_->enqueue_control(element);

  if (ret != DDS::RETCODE_OK) {
    data_container_->release_buffer(element);
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: ")
                      ACE_TEXT("DataWriterImpl::dispose: ")
                      ACE_TEXT("enqueue_control failed.\n")),
                     ret);
  }

  send_all_to_flush_control(guard);

  const ValueDispatcher* vd = get_value_dispatcher();
  const Observer_rch observer = get_observer(Observer::e_DISPOSED);
  if (observer && samp.native_data() && vd) {
    Observer::Sample s(handle, element->get_header().instance_state(), source_timestamp, element->get_header().sequence_, samp.native_data(), *vd);
    observer->on_disposed(this, s);
  }

  return DDS::RETCODE_OK;
}

DDS::ReturnCode_t
DataWriterImpl::num_samples(DDS::InstanceHandle_t handle,
                            size_t&                 size)
{
  return data_container_->num_samples(handle, size);
}

void
DataWriterImpl::unregister_all()
{
  data_container_->unregister_all();
}

GUID_t
DataWriterImpl::get_dp_id()
{
  return dp_id_;
}

char const *
DataWriterImpl::get_type_name() const
{
  return type_name_.in();
}

ACE_Message_Block*
DataWriterImpl::create_control_message(MessageId message_id,
                                       DataSampleHeader& header_data,
                                       Message_Block_Ptr data,
                                       const DDS::Time_t& source_timestamp)
{
  header_data.message_id_ = static_cast<char>(message_id);
  header_data.byte_order_ =
    this->swap_bytes() ? !ACE_CDR_BYTE_ORDER : ACE_CDR_BYTE_ORDER;
  header_data.coherent_change_ = false;

  if (data) {
    header_data.message_length_ = static_cast<ACE_UINT32>(data->total_length());
  }

  header_data.sequence_ = SequenceNumber::SEQUENCENUMBER_UNKNOWN();
  header_data.sequence_repair_ = false; // set below
  header_data.source_timestamp_sec_ = source_timestamp.sec;
  header_data.source_timestamp_nanosec_ = source_timestamp.nanosec;
  header_data.publication_id_ = publication_id_;

  RcHandle<PublisherImpl> publisher = this->publisher_servant_.lock();
  if (!publisher) {
    return 0;
  }

  header_data.publisher_id_ = publisher->publisher_id_;

  ACE_Guard<ACE_Thread_Mutex> guard(sn_lock_);
  SequenceNumber sequence = sequence_number_;
  if (message_id == INSTANCE_REGISTRATION
      || message_id == DISPOSE_INSTANCE
      || message_id == UNREGISTER_INSTANCE
      || message_id == DISPOSE_UNREGISTER_INSTANCE
      || message_id == REQUEST_ACK) {

    header_data.sequence_repair_ = need_sequence_repair();
    header_data.sequence_ = get_next_sn_i();
    header_data.key_fields_only_ = true;
    sequence = sequence_number_;
  }
  guard.release();

  ACE_Message_Block* message = 0;
  ACE_NEW_MALLOC_RETURN(message,
                        static_cast<ACE_Message_Block*>(
                          mb_allocator_->malloc(sizeof(ACE_Message_Block))),
                        ACE_Message_Block(
                          DataSampleHeader::get_max_serialized_size(),
                          ACE_Message_Block::MB_DATA,
                          header_data.message_length_ ? data.release() : 0, //cont
                          0, //data
                          0, //allocator_strategy
                          get_db_lock(), //locking_strategy
                          ACE_DEFAULT_MESSAGE_BLOCK_PRIORITY,
                          ACE_Time_Value::zero,
                          ACE_Time_Value::max_time,
                          db_allocator_.get(),
                          mb_allocator_.get()),
                        0);

  *message << header_data;

  // If we incremented sequence number for this control message
  if (header_data.sequence_ != SequenceNumber::SEQUENCENUMBER_UNKNOWN()) {
    ACE_GUARD_RETURN(ACE_Thread_Mutex, reader_info_guard, this->reader_info_lock_, 0);
    // Update the expected sequence number for all readers
    RepoIdToReaderInfoMap::iterator reader;

    for (reader = reader_info_.begin(); reader != reader_info_.end(); ++reader) {
      reader->second.expected_sequence_ = sequence;
    }
  }
  if (DCPS_debug_level >= 4) {
    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("(%P|%t) DataWriterImpl::create_control_message: ")
               ACE_TEXT("from publication %C sending control sample: %C .\n"),
               LogGuid(publication_id_).c_str(),
               to_string(header_data).c_str()));
  }
  return message;
}

DDS::ReturnCode_t
DataWriterImpl::create_sample_data_message(Message_Block_Ptr data,
                                           DDS::InstanceHandle_t instance_handle,
                                           DataSampleHeader& header_data,
                                           Message_Block_Ptr& message,
                                           const DDS::Time_t& source_timestamp,
                                           bool content_filter)
{
  PublicationInstance_rch instance =
    data_container_->get_handle_instance(instance_handle);

  if (0 == instance) {
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) DataWriterImpl::create_sample_data_message ")
                      ACE_TEXT("failed to find instance for handle %d\n"),
                      instance_handle),
                     DDS::RETCODE_ERROR);
  }

  header_data.message_id_ = SAMPLE_DATA;
  header_data.byte_order_ =
    this->swap_bytes() ? !ACE_CDR_BYTE_ORDER : ACE_CDR_BYTE_ORDER;
  header_data.coherent_change_ = this->coherent_;

  RcHandle<PublisherImpl> publisher = this->publisher_servant_.lock();

  if (!publisher) {
    return DDS::RETCODE_ERROR;
  }

#ifndef OPENDDS_NO_OBJECT_MODEL_PROFILE
  header_data.group_coherent_ =
    publisher->qos_.presentation.access_scope
    == DDS::GROUP_PRESENTATION_QOS;
#endif
  header_data.content_filter_ = content_filter;
  header_data.cdr_encapsulation_ = this->cdr_encapsulation();
  header_data.message_length_ = static_cast<ACE_UINT32>(data->total_length());
  {
    ACE_Guard<ACE_Thread_Mutex> guard(sn_lock_);
    header_data.sequence_repair_ = need_sequence_repair();
    header_data.sequence_ = get_next_sn_i();
  }
  header_data.source_timestamp_sec_ = source_timestamp.sec;
  header_data.source_timestamp_nanosec_ = source_timestamp.nanosec;

  if (qos_.lifespan.duration.sec != DDS::DURATION_INFINITE_SEC
      || qos_.lifespan.duration.nanosec != DDS::DURATION_INFINITE_NSEC) {
    header_data.lifespan_duration_ = true;
    header_data.lifespan_duration_sec_ = qos_.lifespan.duration.sec;
    header_data.lifespan_duration_nanosec_ = qos_.lifespan.duration.nanosec;
  }

  header_data.publication_id_ = publication_id_;
  header_data.publisher_id_ = publisher->publisher_id_;

  ACE_Message_Block* tmp_message;
  ACE_NEW_MALLOC_RETURN(tmp_message,
                        static_cast<ACE_Message_Block*>(
                          mb_allocator_->malloc(sizeof(ACE_Message_Block))),
                        ACE_Message_Block(DataSampleHeader::get_max_serialized_size(),
                                          ACE_Message_Block::MB_DATA,
                                          data.release(), //cont
                                          0, //data
                                          header_allocator_.get(), //alloc_strategy
                                          get_db_lock(), //locking_strategy
                                          ACE_DEFAULT_MESSAGE_BLOCK_PRIORITY,
                                          ACE_Time_Value::zero,
                                          ACE_Time_Value::max_time,
                                          db_allocator_.get(),
                                          mb_allocator_.get()),
                        DDS::RETCODE_ERROR);
  message.reset(tmp_message);
  *message << header_data;
  if (DCPS_debug_level >= 4) {
    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("(%P|%t) DataWriterImpl::create_sample_data_message: ")
               ACE_TEXT("from publication %C sending data sample: %C .\n"),
               LogGuid(publication_id_).c_str(),
               to_string(header_data).c_str()));
  }
  return DDS::RETCODE_OK;
}

void
DataWriterImpl::data_delivered(const DataSampleElement* sample)
{
  DBG_ENTRY_LVL("DataWriterImpl","data_delivered",6);

  if (!(sample->get_pub_id() == this->publication_id_)) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("(%P|%t) ERROR: DataWriterImpl::data_delivered: ")
               ACE_TEXT("The publication id %C from delivered element ")
               ACE_TEXT("does not match the datawriter's id %C\n"),
               LogGuid(sample->get_pub_id()).c_str(),
               LogGuid(publication_id_).c_str()));
    return;
  }
  //provided for statistics tracking in tests
  ++data_delivered_count_;

  this->data_container_->data_delivered(sample);
}

void
DataWriterImpl::control_delivered(const Message_Block_Ptr&)
{
  DBG_ENTRY_LVL("DataWriterImpl","control_delivered",6);
  controlTracker.message_delivered();
}

RcHandle<EntityImpl>
DataWriterImpl::parent() const
{
  return this->publisher_servant_.lock();
}

#ifndef OPENDDS_NO_CONTENT_FILTERED_TOPIC
bool
DataWriterImpl::filter_out(const DataSampleElement& elt,
                           const OPENDDS_STRING& filterClassName,
                           const FilterEvaluator& evaluator,
                           const DDS::StringSeq& expression_params) const
{
  if (!type_support_) {
    if (log_level >= LogLevel::Error) {
      ACE_ERROR((LM_ERROR, "(%P|%t) ERROR: DataWriterImpl::filter_out: Could not cast type support, not filtering\n"));
    }
    return false;
  }

  if (filterClassName == "DDSSQL" ||
      filterClassName == "OPENDDSSQL") {
    if (!elt.get_header().valid_data() && evaluator.has_non_key_fields(*type_support_)) {
      return true;
    }
    try {
      return !evaluator.eval(elt.get_sample()->cont(), encoding_mode_.encoding(),
                             *type_support_, expression_params);
    } catch (const std::runtime_error&) {
      // if the eval fails, the throws will do the logging
      // return false here so that the sample is not filtered
      return false;
    }
  } else {
    return false;
  }
}
#endif

bool
DataWriterImpl::check_transport_qos(const TransportInst&)
{
  // DataWriter does not impose any constraints on which transports
  // may be used based on QoS.
  return true;
}

#ifndef OPENDDS_NO_OBJECT_MODEL_PROFILE

bool
DataWriterImpl::coherent_changes_pending()
{
  ACE_GUARD_RETURN(ACE_Recursive_Thread_Mutex,
                   guard,
                   get_lock(),
                   false);

  return this->coherent_;
}

void
DataWriterImpl::begin_coherent_changes()
{
  ACE_GUARD(ACE_Recursive_Thread_Mutex,
            guard,
            get_lock());

  this->coherent_ = true;
}

void
DataWriterImpl::end_coherent_changes(const GroupCoherentSamples& group_samples)
{
  // PublisherImpl::pi_lock_ should be held.
  ACE_GUARD(ACE_Recursive_Thread_Mutex,
            guard,
            get_lock());

  CoherentChangeControl end_msg;
  end_msg.coherent_samples_.num_samples_ = this->coherent_samples_;
  end_msg.coherent_samples_.last_sample_ = get_max_sn();

  RcHandle<PublisherImpl> publisher = this->publisher_servant_.lock();

  if (publisher) {
    end_msg.group_coherent_
      = publisher->qos_.presentation.access_scope == DDS::GROUP_PRESENTATION_QOS;
  }

  if (publisher && end_msg.group_coherent_) {
    end_msg.publisher_id_ = publisher->publisher_id_;
    end_msg.group_coherent_samples_ = group_samples;
  }

  Message_Block_Ptr data(
    new ACE_Message_Block(
      end_msg.get_max_serialized_size(),
      ACE_Message_Block::MB_DATA,
      0, // cont
      0, // data
      0, // alloc_strategy
      get_db_lock()));

  Serializer serializer(data.get(), Encoding::KIND_UNALIGNED_CDR,
    this->swap_bytes());

  serializer << end_msg;

  DataSampleHeader header;
  Message_Block_Ptr control(
    create_control_message(END_COHERENT_CHANGES, header, OPENDDS_MOVE_NS::move(data),
      SystemTimePoint::now().to_idl_struct()));

  this->coherent_ = false;
  this->coherent_samples_ = 0;

  guard.release();
  if (this->send_control(header, OPENDDS_MOVE_NS::move(control)) == SEND_CONTROL_ERROR) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("(%P|%t) ERROR: DataWriterImpl::end_coherent_changes:")
               ACE_TEXT(" unable to send END_COHERENT_CHANGES control message!\n")));
  }
}

#endif // OPENDDS_NO_OBJECT_MODEL_PROFILE

void
DataWriterImpl::data_dropped(const DataSampleElement* element,
                             bool dropped_by_transport)
{
  DBG_ENTRY_LVL("DataWriterImpl","data_dropped",6);

  //provided for statistics tracking in tests
  ++data_dropped_count_;

  this->data_container_->data_dropped(element, dropped_by_transport);
}

void
DataWriterImpl::control_dropped(const Message_Block_Ptr&,
                                bool /* dropped_by_transport */)
{
  DBG_ENTRY_LVL("DataWriterImpl","control_dropped",6);
  controlTracker.message_dropped();
}

DDS::DataWriterListener_ptr
DataWriterImpl::listener_for(DDS::StatusKind kind)
{
  // per 2.1.4.3.1 Listener Access to Plain Communication Status
  // use this entities factory if listener is mask not enabled
  // for this kind.
  RcHandle<PublisherImpl> publisher = publisher_servant_.lock();
  if (!publisher)
    return 0;

  ACE_Guard<ACE_Thread_Mutex> g(listener_mutex_);
  if (CORBA::is_nil(listener_.in()) || (listener_mask_ & kind) == 0) {
    g.release();
    return publisher->listener_for(kind);

  } else {
    return DDS::DataWriterListener::_duplicate(listener_.in());
  }
}

void
DataWriterImpl::liveliness_send_task(const MonotonicTimePoint& now)
{
  ThreadStatusManager::Event ev(TheServiceParticipant->get_thread_status_manager());

  ACE_Guard<ACE_Recursive_Thread_Mutex> guard(lock_);
  OPENDDS_ASSERT(qos_.liveliness.kind == DDS::AUTOMATIC_LIVELINESS_QOS);

  const TimeDuration elapsed = now - last_liveliness_activity_time_;

  if (elapsed < liveliness_send_interval_) {
    // Reschedule.
    liveliness_send_task_->schedule(liveliness_send_interval_ - elapsed);
    return;
  }

  send_liveliness(now);
  liveliness_send_task_->schedule(liveliness_send_interval_);
}

void
DataWriterImpl::liveliness_lost_task(const MonotonicTimePoint& now)
{
  ThreadStatusManager::Event ev(TheServiceParticipant->get_thread_status_manager());

  ACE_Guard<ACE_Recursive_Thread_Mutex> guard(lock_);

  const TimeDuration elapsed = now - last_liveliness_activity_time_;

  if (elapsed < liveliness_lost_interval_) {
    // Reschedule.
    liveliness_lost_task_->schedule(liveliness_lost_interval_ - elapsed);
    return;
  }

  const bool notify = !liveliness_lost_;
  liveliness_lost_task_->schedule(liveliness_lost_interval_);
  liveliness_lost_ = true;

  if (notify) {
    ++liveliness_lost_status_.total_count;
    ++liveliness_lost_status_.total_count_change;

    set_status_changed_flag(DDS::LIVELINESS_LOST_STATUS, true);
    notify_status_condition();

    DDS::DataWriterListener_var listener = listener_for(DDS::LIVELINESS_LOST_STATUS);

    if (!CORBA::is_nil(listener.in())) {
      {
        ACE_Reverse_Lock<ACE_Recursive_Thread_Mutex> rev_lock(lock_);
        ACE_Guard<ACE_Reverse_Lock<ACE_Recursive_Thread_Mutex> > rev_guard(rev_lock);
        listener->on_liveliness_lost(this, liveliness_lost_status_);
      }
      liveliness_lost_status_.total_count_change = 0;
    }
  }
}

bool
DataWriterImpl::send_liveliness(const MonotonicTimePoint& now)
{
  if (this->qos_.liveliness.kind == DDS::MANUAL_BY_TOPIC_LIVELINESS_QOS ||
      !TheServiceParticipant->get_discovery(domain_id_)->supports_liveliness()) {
    DataSampleHeader header;
    Message_Block_Ptr empty;
    Message_Block_Ptr liveliness_msg(
      create_control_message(DATAWRITER_LIVELINESS, header, OPENDDS_MOVE_NS::move(empty),
        SystemTimePoint::now().to_idl_struct()));

    if (this->send_control(header, OPENDDS_MOVE_NS::move(liveliness_msg)) == SEND_CONTROL_ERROR) {
      ACE_ERROR_RETURN((LM_ERROR,
                        ACE_TEXT("(%P|%t) ERROR: DataWriterImpl::send_liveliness: ")
                        ACE_TEXT("send_control failed.\n")),
                       false);
    }
  }
  last_liveliness_activity_time_ = now;
  liveliness_lost_ = false;
  return true;
}

void
DataWriterImpl::prepare_to_delete()
{
  this->set_deleted(true);
  this->stop_associating();
  this->terminate_send_if_suspended();

#ifndef OPENDDS_NO_PERSISTENCE_PROFILE
  // Trigger data to be persisted, i.e. made durable, if so
  // configured. This needs be called before unregister_instances
  // because unregister_instances may cause instance dispose.
  if (!persist_data() && DCPS_debug_level >= 2) {
    ACE_ERROR((LM_ERROR, ACE_TEXT("(%P|%t) ERROR: DataWriterImpl::prepare_to_delete: ")
      ACE_TEXT("failed to make data durable.\n")));
  }
#endif

  // Unregister all registered instances prior to deletion.
  unregister_instances(SystemTimePoint::now().to_idl_struct());

  const Observer_rch observer = get_observer(Observer::e_DELETED);
  if (observer) {
    observer->on_deleted(this);
  }
}

PublicationInstance_rch
DataWriterImpl::get_handle_instance(DDS::InstanceHandle_t handle)
{

  if (0 != data_container_) {
    return data_container_->get_handle_instance(handle);
  }

  return PublicationInstance_rch();
}

void
DataWriterImpl::notify_publication_disconnected(const ReaderIdSeq& subids)
{
  DBG_ENTRY_LVL("DataWriterImpl","notify_publication_disconnected",6);

  if (!is_bit_) {
    // Narrow to DDS::DCPS::DataWriterListener. If a DDS::DataWriterListener
    // is given to this DataWriter then narrow() fails.
    DataWriterListener_var the_listener = get_ext_listener();

    if (!CORBA::is_nil(the_listener.in())) {
      PublicationDisconnectedStatus status;
      // Since this callback may come after remove_association which
      // removes the reader from id_to_handle map, we can ignore this
      // error.
      this->lookup_instance_handles(subids,
                                    status.subscription_handles);
      the_listener->on_publication_disconnected(this, status);
    }
  }
}

void
DataWriterImpl::notify_publication_reconnected(const ReaderIdSeq& subids)
{
  DBG_ENTRY_LVL("DataWriterImpl","notify_publication_reconnected",6);

  if (!is_bit_) {
    // Narrow to DDS::DCPS::DataWriterListener. If a
    // DDS::DataWriterListener is given to this DataWriter then
    // narrow() fails.
    DataWriterListener_var the_listener = get_ext_listener();

    if (!CORBA::is_nil(the_listener.in())) {
      PublicationDisconnectedStatus status;

      // If it's reconnected then the reader should be in id_to_handle
      this->lookup_instance_handles(subids, status.subscription_handles);

      the_listener->on_publication_reconnected(this, status);
    }
  }
}

void
DataWriterImpl::notify_publication_lost(const ReaderIdSeq& subids)
{
  DBG_ENTRY_LVL("DataWriterImpl","notify_publication_lost",6);

  if (!is_bit_) {
    // Narrow to DDS::DCPS::DataWriterListener. If a
    // DDS::DataWriterListener is given to this DataWriter then
    // narrow() fails.
    DataWriterListener_var the_listener = get_ext_listener();

    if (!CORBA::is_nil(the_listener.in())) {
      PublicationLostStatus status;

      // Since this callback may come after remove_association which removes
      // the reader from id_to_handle map, we can ignore this error.
      this->lookup_instance_handles(subids,
                                    status.subscription_handles);
      the_listener->on_publication_lost(this, status);
    }
  }
}

void
DataWriterImpl::notify_publication_lost(const DDS::InstanceHandleSeq& handles)
{
  DBG_ENTRY_LVL("DataWriterImpl","notify_publication_lost",6);

  if (!is_bit_) {
    // Narrow to DDS::DCPS::DataWriterListener. If a
    // DDS::DataWriterListener is given to this DataWriter then
    // narrow() fails.
    DataWriterListener_var the_listener = get_ext_listener();

    if (!CORBA::is_nil(the_listener.in())) {
      PublicationLostStatus status;

      CORBA::ULong len = handles.length();
      status.subscription_handles.length(len);

      for (CORBA::ULong i = 0; i < len; ++ i) {
        status.subscription_handles[i] = handles[i];
      }

      the_listener->on_publication_lost(this, status);
    }
  }
}


void
DataWriterImpl::lookup_instance_handles(const ReaderIdSeq& ids,
                                        DDS::InstanceHandleSeq & hdls)
{
  CORBA::ULong const num_rds = ids.length();
  RcHandle<DomainParticipantImpl> participant = this->participant_servant_.lock();

  if (!participant)
    return;

  if (DCPS_debug_level > 9) {
    OPENDDS_STRING separator;
    OPENDDS_STRING buffer;

    for (CORBA::ULong i = 0; i < num_rds; ++i) {
      buffer += separator + LogGuid(ids[i]).conv_;
      separator = ", ";
    }

    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("(%P|%t) DataWriterImpl::lookup_instance_handles: ")
               ACE_TEXT("searching for handles for reader Ids: %C.\n"),
               buffer.c_str()));
  }

  hdls.length(num_rds);

  for (CORBA::ULong i = 0; i < num_rds; ++i) {
    hdls[i] = participant->lookup_handle(ids[i]);
  }
}

#ifndef OPENDDS_NO_PERSISTENCE_PROFILE
bool
DataWriterImpl::persist_data()
{
  return this->data_container_->persist_data();
}
#endif

void DataWriterImpl::wait_pending()
{
  if (!TransportRegistry::instance()->released()) {
    data_container_->wait_pending(wait_pending_deadline_);
    controlTracker.wait_messages_pending("DataWriterImpl::wait_pending", wait_pending_deadline_);
  }
}

void
DataWriterImpl::get_instance_handles(InstanceHandleVec& instance_handles)
{
  this->data_container_->get_instance_handles(instance_handles);
}

void
DataWriterImpl::get_readers(RepoIdSet& readers)
{
  ACE_GUARD(ACE_Recursive_Thread_Mutex, guard, this->lock_);
  readers = this->readers_;
}

void
DataWriterImpl::retrieve_inline_qos_data(TransportSendListener::InlineQosData& qos_data) const
{
  RcHandle<PublisherImpl> publisher = this->publisher_servant_.lock();
  if (publisher) {
    publisher->get_qos(qos_data.pub_qos);
  }
  qos_data.dw_qos = this->qos_;
  qos_data.topic_name = this->topic_name_.in();
}

#if OPENDDS_CONFIG_SECURITY
DDS::Security::ParticipantCryptoHandle DataWriterImpl::get_crypto_handle() const
{
  RcHandle<DomainParticipantImpl> participant = participant_servant_.lock();
  return participant ? participant->crypto_handle() : DDS::HANDLE_NIL;
}
#endif

bool
DataWriterImpl::need_sequence_repair()
{
  ACE_GUARD_RETURN(ACE_Thread_Mutex, reader_info_guard, this->reader_info_lock_, false);
  return need_sequence_repair_i();
}

bool
DataWriterImpl::need_sequence_repair_i() const
{
  for (RepoIdToReaderInfoMap::const_iterator it = reader_info_.begin(),
       end = reader_info_.end(); it != end; ++it) {
    if (it->second.expected_sequence_ != sequence_number_) {
      return true;
    }
  }

  return false;
}

SendControlStatus
DataWriterImpl::send_control(const DataSampleHeader& header,
                             Message_Block_Ptr msg)
{
  controlTracker.message_sent();

  SendControlStatus status = TransportClient::send_control(header, OPENDDS_MOVE_NS::move(msg));

  if (status != SEND_CONTROL_OK) {
    controlTracker.message_dropped();
  }

  return status;
}

WeakRcHandle<ICE::Endpoint>
DataWriterImpl::get_ice_endpoint()
{
  return TransportClient::get_ice_endpoint();
}

void DataWriterImpl::set_wait_pending_deadline(const MonotonicTimePoint& deadline)
{
  wait_pending_deadline_ = deadline;
}

void DataWriterImpl::transport_discovery_change()
{
  RcHandle<DomainParticipantImpl> participant = participant_servant_.lock();
  populate_connection_info(participant.get());
  const TransportLocatorSeq& trans_conf_info = connection_info();

  ACE_Guard<ACE_Recursive_Thread_Mutex> guard(lock_);
  const GUID_t dp_id_copy = dp_id_;
  const GUID_t publication_id_copy = publication_id_;
  const int domain_id = domain_id_;
  guard.release();

  Discovery_rch disco = TheServiceParticipant->get_discovery(domain_id);
  disco->update_publication_locators(domain_id,
                                     dp_id_copy,
                                     publication_id_copy,
                                     trans_conf_info);
}

DDS::ReturnCode_t DataWriterImpl::setup_serialization()
{
  if (qos_.representation.value.length() > 0 &&
      qos_.representation.value[0] != UNALIGNED_CDR_DATA_REPRESENTATION) {
    // If the QoS explicitly sets XCDR, XCDR2, or XML, force encapsulation
    cdr_encapsulation(true);
  }

  if (cdr_encapsulation()) {
    Encoding::Kind encoding_kind;
    // There should only be one data representation in a DataWriter, so
    // simply use qos_.representation.value[0].
    if (repr_to_encoding_kind(qos_.representation.value[0], encoding_kind)) {
      encoding_mode_ = EncodingMode(type_support_, encoding_kind, swap_bytes());
      if (encoding_kind == Encoding::KIND_XCDR1 &&
          type_support_->max_extensibility() == MUTABLE) {
        if (log_level >= LogLevel::Notice) {
          ACE_ERROR((LM_NOTICE, "(%P|%t) NOTICE: DataWriterImpl::setup_serialization: "
            "Encountered unsupported combination of XCDR1 encoding and mutable extensibility "
            "for writer of type %C\n",
            type_support_->name()));
        }
        return DDS::RETCODE_ERROR;
      } else if (encoding_kind == Encoding::KIND_UNALIGNED_CDR) {
        if (log_level >= LogLevel::Notice) {
          ACE_ERROR((LM_NOTICE, "(%P|%t) NOTICE: DataWriterImpl::setup_serialization: "
            "Unaligned CDR is not supported by transport types that require encapsulation\n"));
        }
        return DDS::RETCODE_ERROR;
      }
    } else if (log_level >= LogLevel::Warning) {
      ACE_ERROR((LM_WARNING, "(%P|%t) WARNING: DataWriterImpl::setup_serialization: "
                 "Encountered unsupported or unknown data representation: %C ",
                 "for writer of type %C\n",
                 repr_to_string(qos_.representation.value[0]).c_str(),
                 type_support_->name()));
    }
  } else {
    // Pick unaligned CDR as it is the implicit representation for non-encapsulated
    encoding_mode_ = EncodingMode(type_support_, Encoding::KIND_UNALIGNED_CDR, swap_bytes());
  }
  if (!encoding_mode_.valid()) {
    if (log_level >= LogLevel::Notice) {
      ACE_ERROR((LM_NOTICE, "(%P|%t) NOTICE: DataWriterImpl::setup_serialization: "
                 "Could not find a valid data representation\n"));
    }
    return DDS::RETCODE_ERROR;
  }

  if (DCPS_debug_level >= 2) {
    ACE_DEBUG((LM_DEBUG, "(%P|%t) WriterImpl::setup_serialization: "
      "Setup successfully with %C data representation.\n",
      Encoding::kind_to_string(encoding_mode_.encoding().kind()).c_str()));
  }

  // Set up allocator with reserved space for data if it is bounded
  const SerializedSizeBound buffer_size_bound = encoding_mode_.buffer_size_bound();
  if (buffer_size_bound) {
    const size_t chunk_size = buffer_size_bound.get();
    data_allocator_.reset(new DataAllocator(n_chunks_, chunk_size));
    if (DCPS_debug_level >= 2) {
      ACE_DEBUG((LM_DEBUG, "(%P|%t) DataWriterImpl::setup_serialization: "
        "using data allocator at %x with %B %B byte chunks\n",
        data_allocator_.get(),
        n_chunks_,
        chunk_size));
    }
  } else if (DCPS_debug_level >= 2) {
    ACE_DEBUG((LM_DEBUG, "(%P|%t) DataWriterImpl::setup_serialization: "
      "sample size is unbounded, not using data allocator, "
      "always allocating from heap\n"));
  }
  return DDS::RETCODE_OK;
}

DDS::ReturnCode_t DataWriterImpl::get_key_value(Sample_rch& sample, DDS::InstanceHandle_t handle)
{
  ACE_GUARD_RETURN(ACE_Recursive_Thread_Mutex, guard, get_lock(), DDS::RETCODE_ERROR);
  const InstanceHandlesToValues::iterator it = instance_handles_to_values_.find(handle);
  if (it == instance_handles_to_values_.end()) {
    return DDS::RETCODE_BAD_PARAMETER;
  }
  sample = it->second->copy(Sample::Mutable);
  return DDS::RETCODE_OK;
}

DDS::InstanceHandle_t DataWriterImpl::lookup_instance(const Sample& sample)
{
  ACE_GUARD_RETURN(ACE_Recursive_Thread_Mutex, guard, get_lock(), DDS::RETCODE_ERROR);
  const InstanceValuesToHandles::iterator it = find_instance(sample);
  return it == instance_values_to_handles_.end() ? DDS::HANDLE_NIL : it->second;
}

DDS::InstanceHandle_t DataWriterImpl::register_instance_w_timestamp(
  const Sample& sample, const DDS::Time_t& timestamp)
{
  DDS::InstanceHandle_t registered_handle = DDS::HANDLE_NIL;
  const DDS::ReturnCode_t ret = get_or_create_instance_handle(registered_handle, sample, timestamp);
  if (ret != DDS::RETCODE_OK && log_level >= LogLevel::Notice) {
    ACE_ERROR((LM_NOTICE, ACE_TEXT("(%P|%t) NOTICE: DataWriterImpl::register_instance_w_timestamp: ")
               ACE_TEXT("register failed: %C\n"),
               retcode_to_string(ret)));
  }
  return registered_handle;
}

DDS::ReturnCode_t DataWriterImpl::unregister_instance_w_timestamp(
  const Sample& sample,
  DDS::InstanceHandle_t instance_handle,
  const DDS::Time_t& timestamp)
{
  const DDS::ReturnCode_t rc = instance_must_exist(
    "unregister_instance_w_timestamp", sample, instance_handle, /* remove = */ true);
  if (rc != DDS::RETCODE_OK) {
    return rc;
  }
  return unregister_instance_i(instance_handle, &sample, timestamp);
}

DDS::ReturnCode_t DataWriterImpl::dispose_w_timestamp(
  const Sample& sample,
  DDS::InstanceHandle_t instance_handle,
  const DDS::Time_t& source_timestamp)
{
#if OPENDDS_CONFIG_SECURITY && OPENDDS_HAS_DYNAMIC_DATA_ADAPTER
  DDS::DynamicData_var dynamic_data = sample.get_dynamic_data(dynamic_type_);
  DDS::Security::SecurityException ex;
  if (dynamic_data && security_config_ &&
      participant_permissions_handle_ != DDS::HANDLE_NIL &&
      !security_config_->get_access_control()->check_local_datawriter_dispose_instance(participant_permissions_handle_, this, dynamic_data, ex)) {
    if (log_level >= LogLevel::Notice) {
      ACE_ERROR((LM_NOTICE,
                 "(%P|%t) NOTICE: DataWriterImpl::dispose_w_timestamp: unable to dispose instance SecurityException[%d.%d]: %C\n",
                 ex.code, ex.minor_code, ex.message.in()));
    }
    return DDS::Security::RETCODE_NOT_ALLOWED_BY_SECURITY;
  }
#endif

  const DDS::ReturnCode_t rc = instance_must_exist(
    "dispose_w_timestamp", sample, instance_handle);
  if (rc != DDS::RETCODE_OK) {
    return rc;
  }
  return dispose(instance_handle, sample, source_timestamp);
}

ACE_Message_Block* DataWriterImpl::serialize_sample(const Sample& sample)
{
  const bool encapsulated = cdr_encapsulation();
  const Encoding& encoding = encoding_mode_.encoding();
  Message_Block_Ptr mb;
  ACE_Message_Block* tmp_mb;

  // Don't use the cached allocator for the registered sample message
  // block.
  if (sample.key_only() && !skip_serialize_) {
    ACE_NEW_RETURN(tmp_mb,
      ACE_Message_Block(
        encoding_mode_.buffer_size(sample),
        ACE_Message_Block::MB_DATA,
        0, // cont
        0, // data
        0, // alloc_strategy
        get_db_lock()),
      0);
  } else {
    ACE_NEW_MALLOC_RETURN(tmp_mb,
      static_cast<ACE_Message_Block*>(
        mb_allocator_->malloc(sizeof(ACE_Message_Block))),
      ACE_Message_Block(
        encoding_mode_.buffer_size(sample),
        ACE_Message_Block::MB_DATA,
        0, // cont
        0, // data
        data_allocator_.get(), // allocator_strategy
        get_db_lock(), // data block locking_strategy
        ACE_DEFAULT_MESSAGE_BLOCK_PRIORITY,
        ACE_Time_Value::zero,
        ACE_Time_Value::max_time,
        db_allocator_.get(),
        mb_allocator_.get()),
      0);
  }
  mb.reset(tmp_mb);

  if (skip_serialize_) {
    if (!sample.to_message_block(*mb)) {
      if (log_level >= LogLevel::Error) {
        ACE_ERROR((LM_ERROR, "(%P|%t) ERROR: DataWriterImpl::serialize_sample: "
                   "to_message_block failed\n"));
      }
      return 0;
    }
  } else {
    Serializer serializer(mb.get(), encoding);
    if (encapsulated) {
      EncapsulationHeader encap;
      if (!from_encoding(encap, encoding, type_support_->base_extensibility())) {
        // from_encoding logged the error
        return 0;
      }
      if (!(serializer << encap)) {
        if (log_level >= LogLevel::Error) {
          ACE_ERROR((LM_ERROR, "(%P|%t) ERROR: DataWriterImpl::serialize_sample: "
            "failed to serialize data encapsulation header\n"));
        }
        return 0;
      }
    }
    if (!sample.serialize(serializer)) {
      if (log_level >= LogLevel::Error) {
        ACE_ERROR((LM_ERROR, "(%P|%t) ERROR: DataWriterImpl::serialize_sample: "
          "failed to serialize sample data\n"));
      }
      return 0;
    }
    if (encapsulated && !EncapsulationHeader::set_encapsulation_options(mb)) {
      if (log_level >= LogLevel::Error) {
        ACE_ERROR((LM_ERROR, "(%P|%t) ERROR: DataWriterImpl::serialize_sample: "
          "set_encapsulation_options failed\n"));
      }
      return 0;
    }
  }

  return mb.release();
}

bool DataWriterImpl::insert_instance(DDS::InstanceHandle_t handle, Sample_rch& sample)
{
  OPENDDS_ASSERT(sample->key_only());
  if (!instance_handles_to_values_.insert(
        InstanceHandlesToValues::value_type(handle, sample)).second) {
    return false;
  }
  if (!instance_values_to_handles_.insert(
        InstanceValuesToHandles::value_type(sample, handle)).second) {
    instance_handles_to_values_.erase(handle);
    return false;
  }
  return true;
}

DataWriterImpl::InstanceValuesToHandles::iterator
DataWriterImpl::find_instance(const Sample& sample)
{
  Sample_rch dummy_rch(const_cast<Sample*>(&sample), keep_count());
  InstanceValuesToHandles::iterator pos = instance_values_to_handles_.find(dummy_rch);
  dummy_rch._retn();
  return pos;
}

DDS::ReturnCode_t DataWriterImpl::get_or_create_instance_handle(
  DDS::InstanceHandle_t& handle,
  const Sample& sample,
  const DDS::Time_t& source_timestamp)
{
  ACE_GUARD_RETURN(ACE_Recursive_Thread_Mutex, guard, get_lock(), DDS::RETCODE_ERROR);

  handle = lookup_instance(sample);
  if (handle == DDS::HANDLE_NIL || !get_handle_instance(handle)) {
    Sample_rch copy = sample.copy(Sample::ReadOnly, Sample::KeyOnly);
#if OPENDDS_CONFIG_SECURITY && OPENDDS_HAS_DYNAMIC_DATA_ADAPTER
    DDS::DynamicData_var dynamic_data = copy->get_dynamic_data(dynamic_type_);
    DDS::Security::SecurityException ex;
    if (dynamic_data && security_config_ &&
        participant_permissions_handle_ != DDS::HANDLE_NIL &&
        !security_config_->get_access_control()->check_local_datawriter_register_instance(participant_permissions_handle_, this, dynamic_data, ex)) {
      if (log_level >= LogLevel::Notice) {
        ACE_ERROR((LM_NOTICE,
                   "(%P|%t) NOTICE: DataWriterImpl::get_or_create_instance_handle: unable to register instance SecurityException[%d.%d]: %C\n",
                   ex.code, ex.minor_code, ex.message.in()));
      }
      return DDS::Security::RETCODE_NOT_ALLOWED_BY_SECURITY;
    }
#endif

    // don't use fast allocator for registration.
    const TypeSupportImpl* const ts = get_type_support();
    Message_Block_Ptr serialized(serialize_sample(*copy));
    if (!serialized) {
      if (log_level >= LogLevel::Notice) {
        ACE_ERROR((LM_NOTICE, "(%P|%t) NOTICE: %CDataWriterImpl::get_or_create_instance_handle: "
          "failed to serialize sample\n", ts->name()));
      }
      return DDS::RETCODE_ERROR;
    }

    // tell DataWriterLocal and Publisher about the instance.
    const DDS::ReturnCode_t ret = register_instance_i(handle, OPENDDS_MOVE_NS::move(serialized), source_timestamp);
    // note: the WriteDataContainer/PublicationInstance maintains ownership
    // of the marshalled sample.
    if (ret != DDS::RETCODE_OK) {
      handle = DDS::HANDLE_NIL;
      return ret;
    }

    if (!insert_instance(handle, copy)) {
      handle = DDS::HANDLE_NIL;
      if (log_level >= LogLevel::Notice) {
        ACE_ERROR((LM_NOTICE, "(%P|%t) NOTICE: %CDataWriterImpl::get_or_create_instance_handle: "
           "insert instance failed\n", ts->name()));
      }
      return DDS::RETCODE_ERROR;
    }

    send_all_to_flush_control(guard);
  }

  return DDS::RETCODE_OK;
}

DDS::ReturnCode_t DataWriterImpl::instance_must_exist(
  const char* const method_name,
  const Sample& sample,
  DDS::InstanceHandle_t& instance_handle,
  bool remove)
{
  OPENDDS_ASSERT(sample.key_only());

  ACE_GUARD_RETURN(ACE_Recursive_Thread_Mutex, guard, get_lock(), DDS::RETCODE_ERROR);

  const InstanceValuesToHandles::iterator pos = find_instance(sample);
  if (pos == instance_values_to_handles_.end()) {
    if (log_level >= LogLevel::Notice) {
      ACE_ERROR((LM_NOTICE, "(%P|%t) NOTICE: DataWriterImpl::%C: "
        "The instance sample is not registered\n",
        method_name));
    }
    return DDS::RETCODE_ERROR;
  }

  if (instance_handle != DDS::HANDLE_NIL && instance_handle != pos->second) {
    return DDS::RETCODE_PRECONDITION_NOT_MET;
  }

  instance_handle = pos->second;

  if (remove) {
    instance_values_to_handles_.erase(pos);
    instance_handles_to_values_.erase(instance_handle);
  }

  return DDS::RETCODE_OK;
}

DDS::ReturnCode_t DataWriterImpl::write_w_timestamp(
  const Sample& sample,
  DDS::InstanceHandle_t handle,
  const DDS::Time_t& source_timestamp)
{
  // This operation assumes the provided handle is valid. The handle provided
  // will not be verified.

  if (handle == DDS::HANDLE_NIL) {
    DDS::InstanceHandle_t registered_handle = DDS::HANDLE_NIL;
    const DDS::ReturnCode_t ret =
      get_or_create_instance_handle(registered_handle, sample, source_timestamp);
    if (ret != DDS::RETCODE_OK) {
      if (log_level >= LogLevel::Notice) {
        ACE_ERROR((LM_NOTICE, "(%P|%t) NOTICE: %CDataWriterImpl::write_w_timestamp: "
                   "register failed: %C\n",
                   get_type_support()->name(),
                   retcode_to_string(ret)));
      }
      return ret;
    }

    handle = registered_handle;
  }

  // list of reader GUID_ts that should not get data
  GUIDSeq_var filter_out;
#ifndef OPENDDS_NO_CONTENT_FILTERED_TOPIC
  if (publisher_content_filter_) {
    ACE_GUARD_RETURN(ACE_Thread_Mutex, reader_info_guard, reader_info_lock_, DDS::RETCODE_ERROR);
    for (RepoIdToReaderInfoMap::iterator iter = reader_info_.begin(),
         end = reader_info_.end(); iter != end; ++iter) {
      const ReaderInfo& ri = iter->second;
      if (!ri.eval_.is_nil()) {
        if (!filter_out.ptr()) {
          filter_out = new OpenDDS::DCPS::GUIDSeq;
        }
        if (!sample.eval(*ri.eval_, ri.expression_params_)) {
          push_back(filter_out.inout(), iter->first);
        }
      }
    }
  }
#endif

  return write_sample(sample, handle, source_timestamp, filter_out._retn());
}

DDS::ReturnCode_t DataWriterImpl::write_sample(
  const Sample& sample,
  DDS::InstanceHandle_t handle,
  const DDS::Time_t& source_timestamp,
  GUIDSeq* filter_out)
{
  Message_Block_Ptr serialized(serialize_sample(sample));
  if (!serialized) {
    if (log_level >= LogLevel::Notice) {
      ACE_ERROR((LM_NOTICE, "(%P|%t) NOTICE: DataWriterImpl::write_sample: "
        "failed to serialize sample\n"));
    }
    return DDS::RETCODE_ERROR;
  }

  return write(OPENDDS_MOVE_NS::move(serialized), handle, source_timestamp, filter_out, sample.native_data());
}

} // namespace DCPS
} // namespace OpenDDS

OPENDDS_END_VERSIONED_NAMESPACE_DECL
