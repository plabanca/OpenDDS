/*
 *
 *
 * Distributed under the OpenDDS License.
 * See: http://www.opendds.org/license.html
 */

#include "DCPS/DdsDcps_pch.h" //Only the _pch include should start with DCPS/

#include "ConfigStoreImpl.h"

#include "LogAddr.h"
#include "Qos_Helper.h"
#include "debug.h"

#include <ace/OS_NS_ctype.h>

OPENDDS_BEGIN_VERSIONED_NAMESPACE_DECL

namespace OpenDDS {
namespace DCPS {

String ConfigPair::canonicalize(const String& key)
{
  String retval;
  size_t idx = 0;

  // Skip leading punctuation.
  while (idx < key.size() && !ACE_OS::ace_isalnum(key[idx])) {
    ++idx;
  }

  while (idx < key.size()) {
    const char x = key[idx];

    if (idx + 1 < key.size()) {
      // Deal with camelcase;
      const char y = key[idx + 1];

      if (ACE_OS::ace_isupper(x) && ACE_OS::ace_islower(y) && !retval.empty() && retval[retval.size() - 1] != '_') {
        retval += '_';
      }
    }

    // Deal with non-punctuation.
    if (ACE_OS::ace_isalnum(x)) {
      retval += ACE_OS::ace_toupper(x);
      ++idx;
      continue;
    }

    while (idx < key.size() && !ACE_OS::ace_isalnum(key[idx])) {
      ++idx;
    }

    if (idx < key.size() && !retval.empty() && retval[retval.size() - 1] != '_') {
      retval += '_';
    }
  }

  return retval;
}

ConfigStoreImpl::ConfigStoreImpl(ConfigTopic_rch config_topic)
  : config_topic_(config_topic)
  , config_writer_(make_rch<InternalDataWriter<ConfigPair> >(datawriter_qos()))
  , config_reader_(make_rch<InternalDataReader<ConfigPair> >(datareader_qos()))
{
  config_topic_->connect(config_writer_);
  config_topic_->connect(config_reader_);
}

ConfigStoreImpl::~ConfigStoreImpl()
{
  config_topic_->disconnect(config_reader_);
  config_topic_->disconnect(config_writer_);
}

DDS::Boolean
ConfigStoreImpl::has(const char* key)
{
  const ConfigPair cp(key, "");
  DCPS::InternalDataReader<ConfigPair>::SampleSequence samples;
  DCPS::InternalSampleInfoSequence infos;
  config_reader_->read_instance(samples, infos, DDS::LENGTH_UNLIMITED, cp,
                                DDS::ANY_SAMPLE_STATE, DDS::ANY_VIEW_STATE, DDS::ALIVE_INSTANCE_STATE);
  for (size_t idx = 0; idx != samples.size(); ++idx) {
    const DDS::SampleInfo& info = infos[idx];
    if (info.valid_data) {
      return true;
    }
  }

  return false;
}

void
ConfigStoreImpl::set_boolean(const char* key,
                             DDS::Boolean value)
{
  set_string(key, value ? "true" : "false");
}

DDS::Boolean
ConfigStoreImpl::get_boolean(const char* key,
                             DDS::Boolean value)
{
  const ConfigPair cp(key, "");
  DDS::Boolean retval = value;
  DCPS::InternalDataReader<ConfigPair>::SampleSequence samples;
  DCPS::InternalSampleInfoSequence infos;
  config_reader_->read_instance(samples, infos, DDS::LENGTH_UNLIMITED, cp,
                                DDS::ANY_SAMPLE_STATE, DDS::ANY_VIEW_STATE, DDS::ALIVE_INSTANCE_STATE);
  for (size_t idx = 0; idx != samples.size(); ++idx) {
    const ConfigPair& sample = samples[idx];
    const DDS::SampleInfo& info = infos[idx];
    if (info.valid_data) {
      DDS::Boolean x = 0;
      if (sample.value() == "true") {
        retval = true;
      } else if (sample.value() == "false") {
        retval = false;
      } else if (DCPS::convertToInteger(sample.value(), x)) {
        retval = x;
      } else {
        retval = value;
        if (log_level >= LogLevel::Warning) {
          ACE_ERROR((LM_WARNING,
                     ACE_TEXT("(%P|%t) WARNING: ConfigStoreImpl::parse_boolean: ")
                     ACE_TEXT("failed to parse boolean for %C=%C\n"),
                     sample.key().c_str(), sample.value().c_str()));
        }
      }
    }
  }

  if (debug_logging) {
    ACE_DEBUG((LM_DEBUG, "(%P|%t) %C: ConfigStoreImpl::get_boolean: %C=%C\n",
               OPENDDS_CONFIG_DEBUG_LOGGING,
               cp.key().c_str(),
               retval ? "true" : "false"));
  }

  return retval;
}

void
ConfigStoreImpl::set_int32(const char* key,
                           DDS::Int32 value)
{
  set(key, to_dds_string(value));
}

DDS::Int32
ConfigStoreImpl::get_int32(const char* key,
                           DDS::Int32 value)
{
  const ConfigPair cp(key, "");
  DDS::Int32 retval = value;
  DCPS::InternalDataReader<ConfigPair>::SampleSequence samples;
  DCPS::InternalSampleInfoSequence infos;
  config_reader_->read_instance(samples, infos, DDS::LENGTH_UNLIMITED, cp,
                                DDS::ANY_SAMPLE_STATE, DDS::ANY_VIEW_STATE, DDS::ALIVE_INSTANCE_STATE);
  for (size_t idx = 0; idx != samples.size(); ++idx) {
    const ConfigPair& sample = samples[idx];
    const DDS::SampleInfo& info = infos[idx];
    if (info.valid_data) {
      DDS::Int32 x = 0;
      if (DCPS::convertToInteger(sample.value(), x)) {
        retval = x;
      } else {
        retval = value;
        if (log_level >= LogLevel::Warning) {
          ACE_ERROR((LM_WARNING,
                     ACE_TEXT("(%P|%t) WARNING: ConfigStoreImpl::get_int32: ")
                     ACE_TEXT("failed to parse int32 for %C=%C\n"),
                     sample.key().c_str(), sample.value().c_str()));
        }
      }
    }
  }

  if (debug_logging) {
    ACE_DEBUG((LM_DEBUG, "(%P|%t) %C: ConfigStoreImpl::get_int32: %C=%d\n",
               OPENDDS_CONFIG_DEBUG_LOGGING,
               cp.key().c_str(),
               retval));

  }

  return retval;
}

void
ConfigStoreImpl::set_uint32(const char* key,
                            DDS::UInt32 value)
{
  set(key, to_dds_string(value));
}

DDS::UInt32
ConfigStoreImpl::get_uint32(const char* key,
                            DDS::UInt32 value)
{
  const ConfigPair cp(key, "");
  DDS::UInt32 retval = value;
  DCPS::InternalDataReader<ConfigPair>::SampleSequence samples;
  DCPS::InternalSampleInfoSequence infos;
  config_reader_->read_instance(samples, infos, DDS::LENGTH_UNLIMITED, cp,
                                DDS::ANY_SAMPLE_STATE, DDS::ANY_VIEW_STATE, DDS::ALIVE_INSTANCE_STATE);
  for (size_t idx = 0; idx != samples.size(); ++idx) {
    const ConfigPair& sample = samples[idx];
    const DDS::SampleInfo& info = infos[idx];
    if (info.valid_data) {
      DDS::UInt32 x = 0;
      if (DCPS::convertToInteger(sample.value(), x)) {
        retval = x;
      } else {
        retval = value;
        if (log_level >= LogLevel::Warning) {
          ACE_ERROR((LM_WARNING,
                     ACE_TEXT("(%P|%t) WARNING: ConfigStoreImpl::get_uint32: ")
                     ACE_TEXT("failed to parse uint32 for %C=%C\n"),
                     sample.key().c_str(), sample.value().c_str()));
        }
      }
    }
  }

  if (debug_logging) {
    ACE_DEBUG((LM_DEBUG, "(%P|%t) %C: ConfigStoreImpl::get_int32: %C=%u\n",
               OPENDDS_CONFIG_DEBUG_LOGGING,
               cp.key().c_str(),
               retval));

  }

  return retval;
}

void
ConfigStoreImpl::set_float64(const char* key,
                             DDS::Float64 value)
{
  set(key, to_dds_string(value));
}

DDS::Float64
ConfigStoreImpl::get_float64(const char* key,
                             DDS::Float64 value)
{
  const ConfigPair cp(key, "");
  DDS::Float64 retval = value;
  DCPS::InternalDataReader<ConfigPair>::SampleSequence samples;
  DCPS::InternalSampleInfoSequence infos;
  config_reader_->read_instance(samples, infos, DDS::LENGTH_UNLIMITED, cp,
                                DDS::ANY_SAMPLE_STATE, DDS::ANY_VIEW_STATE, DDS::ALIVE_INSTANCE_STATE);
  for (size_t idx = 0; idx != samples.size(); ++idx) {
    const ConfigPair& sample = samples[idx];
    const DDS::SampleInfo& info = infos[idx];
    if (info.valid_data) {
      DDS::Float64 x = 0;
      if (DCPS::convertToDouble(sample.value(), x)) {
        retval = x;
      } else {
        retval = value;
        if (log_level >= LogLevel::Warning) {
          ACE_ERROR((LM_WARNING,
                     ACE_TEXT("(%P|%t) WARNING: ConfigStoreImpl::get_float64: ")
                     ACE_TEXT("failed to parse float64 for %C=%C\n"),
                     sample.key().c_str(), sample.value().c_str()));
        }
      }
    }
  }

  if (debug_logging) {
    ACE_DEBUG((LM_DEBUG, "(%P|%t) %C: ConfigStoreImpl::get_float64: %C=%g\n",
               OPENDDS_CONFIG_DEBUG_LOGGING,
               cp.key().c_str(),
               retval));

  }

  return retval;
}

void
ConfigStoreImpl::set_string(const char* key,
                            const char* value)
{
  const ConfigPair cp(key, value);
  if (log_level >= LogLevel::Info || debug_logging) {
    ACE_DEBUG((LM_INFO, "(%P|%t) INFO: ConfigStoreImpl::set_string: %C=%C\n",
               cp.key().c_str(),
               cp.value().c_str()));
  }
  config_writer_->write(cp);
}

char*
ConfigStoreImpl::get_string(const char* key,
                            const char* value)
{
  const ConfigPair cp(key, "");
  CORBA::String_var retval(value);

  DCPS::InternalDataReader<ConfigPair>::SampleSequence samples;
  DCPS::InternalSampleInfoSequence infos;
  config_reader_->read_instance(samples, infos, DDS::LENGTH_UNLIMITED, cp,
                                DDS::ANY_SAMPLE_STATE, DDS::ANY_VIEW_STATE, DDS::ALIVE_INSTANCE_STATE);
  for (size_t idx = 0; idx != samples.size(); ++idx) {
    const ConfigPair& sample = samples[idx];
    const DDS::SampleInfo& info = infos[idx];
    if (info.valid_data) {
      retval = sample.value().c_str();
    }
  }

  if (debug_logging) {
    ACE_DEBUG((LM_DEBUG, "(%P|%t) %C: ConfigStoreImpl::get_string: %C=%C\n",
               OPENDDS_CONFIG_DEBUG_LOGGING,
               cp.key().c_str(),
               retval.in()));

  }

  return retval._retn();
}

void
ConfigStoreImpl::set_duration(const char* key,
                              const DDS::Duration_t& value)
{
  set(key, to_dds_string(value));
}

DDS::Duration_t
ConfigStoreImpl::get_duration(const char* key,
                              const DDS::Duration_t& value)
{
  const ConfigPair cp(key, "");
  DDS::Duration_t retval = value;

  DCPS::InternalDataReader<ConfigPair>::SampleSequence samples;
  DCPS::InternalSampleInfoSequence infos;
  config_reader_->read_instance(samples, infos, DDS::LENGTH_UNLIMITED, cp,
                                DDS::ANY_SAMPLE_STATE, DDS::ANY_VIEW_STATE, DDS::ALIVE_INSTANCE_STATE);
  for (size_t idx = 0; idx != samples.size(); ++idx) {
    const ConfigPair& sample = samples[idx];
    const DDS::SampleInfo& info = infos[idx];
    if (info.valid_data) {
      if (from_dds_string(sample.value(), retval)) {
        // Okay.
      } else {
        retval = value;
        if (log_level >= LogLevel::Warning) {
          ACE_ERROR((LM_WARNING,
                     ACE_TEXT("(%P|%t) WARNING: ConfigStoreImpl::get_duration: ")
                     ACE_TEXT("failed to parse DDS::Duration_t for %C=%C\n"),
                     sample.key().c_str(), sample.value().c_str()));
        }
      }
    }
  }

  if (debug_logging) {
    ACE_DEBUG((LM_DEBUG, "(%P|%t) %C: ConfigStoreImpl::get_duration: %C=%C\n",
               OPENDDS_CONFIG_DEBUG_LOGGING,
               cp.key().c_str(),
               to_dds_string(retval).c_str()));

  }

  return retval;
}

void
ConfigStoreImpl::unset(const char* key)
{
  const ConfigPair cp(key, "");
  config_writer_->unregister_instance(cp);
}

void
ConfigStoreImpl::set(const char* key,
                     const String& value)
{
  ConfigPair cp(key, value);

  if (log_level >= LogLevel::Info || debug_logging) {
    ACE_DEBUG((LM_INFO, "(%P|%t) INFO: ConfigStoreImpl::set: %C=%C\n",
               cp.key().c_str(),
               cp.value().c_str()));
  }
  config_writer_->write(cp);
}

String
ConfigStoreImpl::get(const char* key,
                     const String& value) const
{
  const ConfigPair cp(key, "");
  String retval = value;

  DCPS::InternalDataReader<ConfigPair>::SampleSequence samples;
  DCPS::InternalSampleInfoSequence infos;
  config_reader_->read_instance(samples, infos, DDS::LENGTH_UNLIMITED, cp,
                                DDS::ANY_SAMPLE_STATE, DDS::ANY_VIEW_STATE, DDS::ALIVE_INSTANCE_STATE);
  for (size_t idx = 0; idx != samples.size(); ++idx) {
    const ConfigPair& sample = samples[idx];
    const DDS::SampleInfo& info = infos[idx];
    if (info.valid_data) {
      retval = sample.value();
    }
  }

  if (debug_logging) {
    ACE_DEBUG((LM_DEBUG, "(%P|%t) %C: ConfigStoreImpl::get: %C=%C\n",
               OPENDDS_CONFIG_DEBUG_LOGGING,
               cp.key().c_str(),
               retval.c_str()));

  }

  return retval;
}

namespace {
  DDS::Int32 time_duration_to_integer(const TimeDuration& value,
                                      ConfigStoreImpl::IntegerTimeFormat format)
  {
    switch (format) {
    case ConfigStoreImpl::Format_IntegerMilliseconds:
      return value.value().msec();
    case ConfigStoreImpl::Format_IntegerSeconds:
      return static_cast<DDS::Int32>(value.value().sec());
    }
    return 0;
  }
}

void
ConfigStoreImpl::set(const char* key,
                     const TimeDuration& value,
                     IntegerTimeFormat format)
{
  set_int32(key, time_duration_to_integer(value, format));
}

TimeDuration
ConfigStoreImpl::get(const char* key,
                     const TimeDuration& value,
                     IntegerTimeFormat format) const
{
  const ConfigPair cp(key, "");
  TimeDuration retval = value;

  DCPS::InternalDataReader<ConfigPair>::SampleSequence samples;
  DCPS::InternalSampleInfoSequence infos;
  config_reader_->read_instance(samples, infos, DDS::LENGTH_UNLIMITED, cp,
                                DDS::ANY_SAMPLE_STATE, DDS::ANY_VIEW_STATE, DDS::ALIVE_INSTANCE_STATE);
  for (size_t idx = 0; idx != samples.size(); ++idx) {
    const ConfigPair& sample = samples[idx];
    const DDS::SampleInfo& info = infos[idx];
    if (info.valid_data) {
      int x = 0;
      if (DCPS::convertToInteger(sample.value(), x)) {
        switch (format) {
        case Format_IntegerMilliseconds:
          retval = TimeDuration::from_msec(x);
          break;
        case Format_IntegerSeconds:
          retval = TimeDuration(x);
          break;
        }
      } else {
        retval = value;
        if (log_level >= LogLevel::Warning) {
          ACE_ERROR((LM_WARNING,
                     ACE_TEXT("(%P|%t) WARNING: ConfigStoreImpl::get: ")
                     ACE_TEXT("failed to parse TimeDuration for %C=%C\n"),
                     sample.key().c_str(), sample.value().c_str()));
        }
      }
    }
  }

  if (debug_logging) {
    ACE_DEBUG((LM_DEBUG, "(%P|%t) %C: ConfigStoreImpl::get: %C=%d\n",
               OPENDDS_CONFIG_DEBUG_LOGGING,
               cp.key().c_str(),
               time_duration_to_integer(retval, format)));
  }

  return retval;
}

namespace {

  bool expected_kind(const NetworkAddress& value,
                     ConfigStoreImpl::NetworkAddressKind kind)
  {
    switch (kind) {
    case ConfigStoreImpl::Kind_IPV4:
      return value.get_type() == AF_INET;
#ifdef ACE_HAS_IPV6
    case ConfigStoreImpl::Kind_IPV6:
      return value.get_type() == AF_INET6;
#endif
    }

    return false;
  }

}

void
ConfigStoreImpl::set(const char* key,
                     const NetworkAddress& value,
                     NetworkAddressFormat format,
                     NetworkAddressKind kind)
{
  String addr_str;

  switch (format) {
  case Format_No_Port:
    addr_str = LogAddr(value, LogAddr::Ip).str();
    break;
  case Format_Required_Port:
    addr_str = LogAddr(value, LogAddr::IpPort).str();
    break;
  case Format_Optional_Port:
    if (value.get_port_number() == 0) {
      addr_str = LogAddr(value, LogAddr::Ip).str();
    } else {
      addr_str = LogAddr(value, LogAddr::IpPort).str();
    }
    break;
  }

  if (!expected_kind(value, kind)) {
    if (log_level >= LogLevel::Warning) {
      ACE_ERROR((LM_WARNING,
                 ACE_TEXT("(%P|%t) WARNING: ConfigStoreImpl::set: ")
                 ACE_TEXT("NetworkAddress kind mismatch for %C=%C\n"),
                 key, addr_str.c_str()));
    }
    return;
  }

  set(key, addr_str);
}

namespace {
  void parse_no_port(const ConfigPair& sample, NetworkAddress& retval, const NetworkAddress& value)
  {
    ACE_INET_Addr addr;
    if (addr.set(u_short(0), sample.value().c_str()) == 0) {
      retval = NetworkAddress(addr);
    } else {
      retval = value;
      if (log_level >= LogLevel::Warning) {
        ACE_ERROR((LM_WARNING,
                   ACE_TEXT("(%P|%t) WARNING: ConfigStoreImpl::get: ")
                   ACE_TEXT("failed to parse NetworkAddress for %C=%C\n"),
                   sample.key().c_str(), sample.value().c_str()));
      }
    }
  }

  void parse_port(const ConfigPair& sample, NetworkAddress& retval, const NetworkAddress& value)
  {
    ACE_INET_Addr addr;
    if (addr.set(sample.value().c_str()) == 0) {
      retval = NetworkAddress(addr);
    } else {
      retval = value;
      if (log_level >= LogLevel::Warning) {
        ACE_ERROR((LM_WARNING,
                   ACE_TEXT("(%P|%t) WARNING: ConfigStoreImpl::get: ")
                   ACE_TEXT("failed to parse NetworkAddress for %C=%C\n"),
                   sample.key().c_str(), sample.value().c_str()));
      }
    }
  }

  void parse_optional_port(const ConfigPair& sample, NetworkAddress& retval, const NetworkAddress& value)
  {
    ACE_INET_Addr addr;
    if (addr.set(sample.value().c_str()) == 0) {
      retval = NetworkAddress(addr);
    } else if (addr.set(u_short(0), sample.value().c_str()) == 0) {
      retval = NetworkAddress(addr);
    } else {
      retval = value;
      if (log_level >= LogLevel::Warning) {
        ACE_ERROR((LM_WARNING,
                   ACE_TEXT("(%P|%t) WARNING: ConfigStoreImpl::get: ")
                   ACE_TEXT("failed to parse NetworkAddress for %C=%C\n"),
                   sample.key().c_str(), sample.value().c_str()));
      }
    }
  }
}

NetworkAddress
ConfigStoreImpl::get(const char* key,
                     const NetworkAddress& value,
                     NetworkAddressFormat format,
                     NetworkAddressKind kind) const
{
  OPENDDS_ASSERT(expected_kind(value, kind));

  const ConfigPair cp(key, "");
  NetworkAddress retval = value;

  DCPS::InternalDataReader<ConfigPair>::SampleSequence samples;
  DCPS::InternalSampleInfoSequence infos;
  config_reader_->read_instance(samples, infos, DDS::LENGTH_UNLIMITED, cp,
                                DDS::ANY_SAMPLE_STATE, DDS::ANY_VIEW_STATE, DDS::ALIVE_INSTANCE_STATE);
  for (size_t idx = 0; idx != samples.size(); ++idx) {
    const ConfigPair& sample = samples[idx];
    const DDS::SampleInfo& info = infos[idx];
    if (info.valid_data) {
      if (!sample.value().empty()) {
        switch (format) {
        case Format_No_Port:
          parse_no_port(sample, retval, value);
          break;
        case Format_Required_Port:
          parse_port(sample, retval, value);
          break;
        case Format_Optional_Port:
          parse_optional_port(sample, retval, value);
          break;
        }
      }
    }
  }

  if (!expected_kind(retval, kind)) {
    if (log_level >= LogLevel::Warning) {
      ACE_ERROR((LM_WARNING,
                 ACE_TEXT("(%P|%t) WARNING: ConfigStoreImpl::get: ")
                 ACE_TEXT("NetworkAddress kind mismatch for %C\n"),
                 cp.key().c_str()));
    }
    retval = value;
  }

  if (retval.get_port_number() == 0) {
    retval.set_port_number(value.get_port_number());
  }

  if (debug_logging) {
    ACE_DEBUG((LM_DEBUG, "(%P|%t) %C: ConfigStoreImpl::get: %C=%C\n",
               OPENDDS_CONFIG_DEBUG_LOGGING,
               cp.key().c_str(),
               LogAddr(retval.to_addr()).c_str()));
  }

  return retval;
}

DDS::DataWriterQos ConfigStoreImpl::datawriter_qos()
{
  return DataWriterQosBuilder().durability_transient_local();
}

DDS::DataReaderQos ConfigStoreImpl::datareader_qos()
{
  return DataReaderQosBuilder()
    .reliability_reliable()
    .durability_transient_local()
    .reader_data_lifecycle_autopurge_nowriter_samples_delay(make_duration_t(0, 0))
    .reader_data_lifecycle_autopurge_disposed_samples_delay(make_duration_t(0, 0));
}

bool
take_has_prefix(ConfigReader_rch reader,
                const String& prefix)
{
  DCPS::InternalDataReader<ConfigPair>::SampleSequence samples;
  DCPS::InternalSampleInfoSequence infos;
  reader->take(samples, infos, DDS::LENGTH_UNLIMITED,
               DDS::ANY_SAMPLE_STATE, DDS::ANY_VIEW_STATE, DDS::ALIVE_INSTANCE_STATE);
  for (size_t idx = 0; idx != samples.size(); ++idx) {
    if (samples[idx].key_has_prefix(prefix)) {
      return true;
    }
  }

  return false;
}

bool ConfigStoreImpl::debug_logging = OPENDDS_CONFIG_DEBUG_LOGGING_default;

void
process_section(ConfigStoreImpl& config_store,
                ConfigReader_rch reader,
                ConfigReaderListener_rch listener,
                const String& key_prefix,
                ACE_Configuration_Heap& config,
                const ACE_Configuration_Section_Key& base,
                const String& filename,
                bool allow_overwrite)
{
  // Process the values.
  int status = 0;
  for (int idx = 0; status == 0; ++idx) {
    ACE_TString key;
    ACE_Configuration_Heap::VALUETYPE value_type;
    status = config.enumerate_values(base, idx, key, value_type);
    if (status == 0) {
      switch (value_type) {
      case ACE_Configuration_Heap::STRING:
        {
          ACE_TString value;
          if (config.get_string_value(base, key.c_str(), value) == 0) {
            const String key_name = key_prefix + "_" + ACE_TEXT_ALWAYS_CHAR(key.c_str());
            String value_str = ACE_TEXT_ALWAYS_CHAR(value.c_str());
            if (value_str == "$file") {
              value_str = filename;
            }
            if (allow_overwrite || !config_store.has(key_name.c_str())) {
              config_store.set(key_name.c_str(), value_str);
              if (listener && reader) {
                listener->on_data_available(reader);
              }
            } else if (log_level >= LogLevel::Notice) {
              ACE_DEBUG((LM_NOTICE,
                         "(%P|%t) NOTICE: process_section: "
                         "value from commandline or user for %s overrides value in config file\n",
                         key.c_str()));
            }
          } else {
            if (log_level >= LogLevel::Error) {
              ACE_ERROR((LM_ERROR,
                         "(%P|%t) ERROR: process_section: "
                         "get_string_value() failed for key \"%s\"\n",
                         key.c_str()));
            }
          }
        }
        break;
      case ACE_Configuration_Heap::INTEGER:
      case ACE_Configuration_Heap::BINARY:
      case ACE_Configuration_Heap::INVALID:
        if (log_level >= LogLevel::Error) {
          ACE_ERROR((LM_ERROR,
                     "(%P|%t) ERROR: process_section: "
                     "unsupported value type for key \"%s\"\n",
                     key.c_str()));
        }
        break;
      }
    }
  }

  // Recur on the subsections.
  status = 0;
  for (int idx = 0; status == 0; ++idx) {
    ACE_TString section_name;
    status = config.enumerate_sections(base, idx, section_name);
    if (status == 0) {
      ACE_Configuration_Section_Key key;
      if (config.open_section(base, section_name.c_str(), 0, key) == 0) {
        process_section(config_store, reader, listener, key_prefix + "_" + ACE_TEXT_ALWAYS_CHAR(section_name.c_str()), config, key, filename, allow_overwrite);
      } else {
        if (log_level >= LogLevel::Error) {
          ACE_ERROR((LM_ERROR,
                     "(%P|%t) ERROR: process_section: "
                     "open_section() failed for name \"%s\"\n",
                     section_name.c_str()));
        }
      }
    }
  }
}

}
}

OPENDDS_END_VERSIONED_NAMESPACE_DECL
