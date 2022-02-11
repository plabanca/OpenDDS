/*
 * Distributed under the OpenDDS License.
 * See: http://www.opendds.org/license.html
 */

#ifndef OPENDDS_DCPS_TRANSPORT_RTPS_UDP_RTPSUDPTRANSPORT_H
#define OPENDDS_DCPS_TRANSPORT_RTPS_UDP_RTPSUDPTRANSPORT_H

#include "RtpsUdpDataLink.h"
#include "RtpsUdpDataLink_rch.h"
#include "Rtps_Udp_Export.h"

#include <dds/DCPS/PoolAllocator.h>
#include <dds/DCPS/ConnectionRecords.h>
#include <dds/DCPS/transport/framework/TransportImpl.h>
#include <dds/DCPS/transport/framework/TransportClient.h>
#include <dds/DCPS/RTPS/ICE/Ice.h>

#include <dds/DCPS/RTPS/RtpsCoreC.h>

OPENDDS_BEGIN_VERSIONED_NAMESPACE_DECL

namespace OpenDDS {
namespace DCPS {

class RtpsUdpInst;

class OpenDDS_Rtps_Udp_Export RtpsUdpTransport : public TransportImpl {
public:
  RtpsUdpTransport(RtpsUdpInst& inst);
  RtpsUdpInst& config() const;
#ifdef OPENDDS_SECURITY
  DCPS::RcHandle<ICE::Agent> get_ice_agent() const;
#endif
  virtual DCPS::WeakRcHandle<ICE::Endpoint> get_ice_endpoint();
  virtual void rtps_relay_only_now(bool flag);
  virtual void use_rtps_relay_now(bool flag);
  virtual void use_ice_now(bool flag);
#ifdef OPENDDS_SECURITY
  ICE::ServerReflexiveStateMachine& relay_srsm() { return relay_srsm_; }
  void process_relay_sra(ICE::ServerReflexiveStateMachine::StateChange);
  void disable_relay_stun_task();
#endif

  virtual void update_locators(const RepoId& /*remote*/,
                               const TransportLocatorSeq& /*locators*/);

  NetworkAddress get_last_recv_addr(const RepoId& local_id, const RepoId& remote_id);

  void rtps_relay_address_change();
  void append_transport_statistics(TransportStatisticsSequence& seq);

private:
  virtual AcceptConnectResult connect_datalink(const RemoteTransport& remote,
                                               const ConnectionAttribs& attribs,
                                               const TransportClient_rch& client);

  virtual AcceptConnectResult accept_datalink(const RemoteTransport& remote,
                                              const ConnectionAttribs& attribs,
                                              const TransportClient_rch& client);

  virtual void stop_accepting_or_connecting(const TransportClient_wrch& client,
                                            const RepoId& remote_id,
                                            bool disassociate,
                                            bool association_failed);

  bool configure_i(RtpsUdpInst& config);

  void client_stop(const RepoId& localId);

  virtual void shutdown_i();

  virtual void register_for_reader(const RepoId& participant,
                                   const RepoId& writerid,
                                   const RepoId& readerid,
                                   const TransportLocatorSeq& locators,
                                   DiscoveryListener* listener);

  virtual void unregister_for_reader(const RepoId& participant,
                                     const RepoId& writerid,
                                     const RepoId& readerid);

  virtual void register_for_writer(const RepoId& /*participant*/,
                                   const RepoId& /*readerid*/,
                                   const RepoId& /*writerid*/,
                                   const TransportLocatorSeq& /*locators*/,
                                   DiscoveryListener* /*listener*/);

  virtual void unregister_for_writer(const RepoId& /*participant*/,
                                     const RepoId& /*readerid*/,
                                     const RepoId& /*writerid*/);

  virtual bool connection_info_i(TransportLocator& info, ConnectionInfoFlags flags) const;

  void get_connection_addrs(const TransportBLOB& data,
                            AddrSet* uc_addrs,
                            AddrSet* mc_addrs = 0,
                            bool* requires_inline_qos = 0,
                            unsigned int* blob_bytes_read = 0) const;

  virtual void release_datalink(DataLink* link);

  virtual OPENDDS_STRING transport_type() const { return "rtps_udp"; }

  RtpsUdpDataLink_rch make_datalink(const GuidPrefix_t& local_prefix);

  bool use_datalink(const RepoId& local_id,
                    const RepoId& remote_id,
                    const TransportBLOB& remote_data,
                    const MonotonicTime_t& participant_discovered_at,
                    ACE_CDR::ULong participant_flags,
                    bool local_reliable, bool remote_reliable,
                    bool local_durable, bool remote_durable,
                    SequenceNumber max_sn,
                    const TransportClient_rch& client);

#if defined(OPENDDS_SECURITY)
  void local_crypto_handle(DDS::Security::ParticipantCryptoHandle pch)
  {
    RtpsUdpDataLink_rch link;
    {
      ACE_Guard<ACE_Thread_Mutex> guard(links_lock_);
      local_crypto_handle_ = pch;
      link = link_;
    }
    if (link) {
      link->local_crypto_handle(pch);
    }
  }
#endif

  //protects access to link_ for duration of make_datalink
  typedef ACE_Thread_Mutex          ThreadLockType;
  typedef ACE_Guard<ThreadLockType> GuardThreadType;
  ThreadLockType links_lock_;

  /// This protects the connections_ data member
  typedef ACE_SYNCH_MUTEX     LockType;
  typedef ACE_Guard<LockType> GuardType;
  LockType connections_lock_;

  DDS::Subscriber_var bit_sub_;
  GuidPrefix_t local_prefix_;

  /// RTPS uses only one link per transport.
  /// This link can be safely reused by any clients that belong to the same
  /// domain participant (same GUID prefix).  Use by a second participant
  /// is not possible because the network location returned by
  /// connection_info_i() can't be shared among participants.
  RtpsUdpDataLink_rch link_;

  ACE_SOCK_Dgram unicast_socket_;
#ifdef ACE_HAS_IPV6
  ACE_SOCK_Dgram ipv6_unicast_socket_;
#endif
  TransportClient_wrch default_listener_;

  JobQueue_rch job_queue_;

#if defined(OPENDDS_SECURITY)
  DDS::Security::ParticipantCryptoHandle local_crypto_handle_;
#endif

#ifdef OPENDDS_SECURITY

#ifndef DDS_HAS_MINIMUM_BIT
  ConnectionRecords deferred_connection_records_;
#endif

  struct IceEndpoint : public virtual ACE_Event_Handler, public virtual ICE::Endpoint {
    RtpsUdpTransport& transport;

    IceEndpoint(RtpsUdpTransport& a_transport)
      : transport(a_transport)
      , network_is_unreachable_(false)
    {}

    const ACE_SOCK_Dgram& choose_recv_socket(ACE_HANDLE fd) const;
    virtual int handle_input(ACE_HANDLE fd);
    virtual ICE::AddressListType host_addresses() const;
    ACE_SOCK_Dgram& choose_send_socket(const ACE_INET_Addr& address) const;
    virtual void send(const ACE_INET_Addr& address, const STUN::Message& message);
    virtual ACE_INET_Addr stun_server_address() const;

    bool network_is_unreachable_;
  };
  RcHandle<IceEndpoint> ice_endpoint_;

  typedef PmfSporadicTask<RtpsUdpTransport> Sporadic;
  void relay_stun_task(const MonotonicTimePoint& now);
  RcHandle<Sporadic> relay_stun_task_;
  DCPS::FibonacciSequence<TimeDuration> relay_stun_task_falloff_;
  ICE::ServerReflexiveStateMachine relay_srsm_;

  mutable ACE_Thread_Mutex relay_stun_mutex_;

  void start_ice();
  void stop_ice();

  RcHandle<ICE::Agent> ice_agent_;
#endif

  InternalTransportStatistics transport_statistics_;
  ACE_Thread_Mutex transport_statistics_mutex_;

  friend class RtpsUdpSendStrategy;
  friend class RtpsUdpReceiveStrategy;
};

} // namespace DCPS
} // namespace OpenDDS

OPENDDS_END_VERSIONED_NAMESPACE_DECL

#endif  /* DCPS_RTPSUDPTRANSPORT_H */
