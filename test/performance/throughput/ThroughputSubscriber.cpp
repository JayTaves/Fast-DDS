// Copyright 2016 Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file ThroughputSubscriber.cxx
 *
 */

#include "ThroughputSubscriber.hpp"

#include <vector>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/qos/DomainParticipantQos.hpp>
#include <fastdds/dds/log/Colors.hpp>
#include <fastdds/dds/log/Log.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastrtps/utils/TimeConversion.h>
#include <fastrtps/xmlparser/XMLProfileManager.h>

using namespace eprosima::fastdds::dds;
using namespace eprosima::fastrtps::rtps;
using namespace eprosima::fastrtps::types;

// *******************************************************************************************
// ************************************ DATA SUB LISTENER ************************************
// *******************************************************************************************

void ThroughputSubscriber::DataReaderListener::reset()
{
    last_seq_num_ = 0;
    first_ = true;
    lost_samples_ = 0;
    matched_ = 0;
}

/*
 * Our current inplementation of MatchedStatus info:
 * - total_count(_change) holds the actual number of matches
 * - current_count(_change) is a flag to signal match or unmatch.
 *   (TODO: review if fits standard definition)
 * */

void ThroughputSubscriber::DataReaderListener::on_subscription_matched(
        DataReader*,
        const SubscriptionMatchedStatus& match_info)
{
    std::unique_lock<std::mutex> lock(throughput_subscriber_.data_mutex_);

    if (1 == info.current_count)
    {
        std::cout << C_RED << "Sub: DATA Sub Matched" << C_DEF << std::endl;
    }
    else
    {
        std::cout << C_RED << "DATA SUBSCRIBER MATCHING REMOVAL" << C_DEF << std::endl;
    }

    matched_ = info.total_count;
    lock.unlock();
    throughput_subscriber_.data_discovery_cv_.notify_one();
}

void ThroughputSubscriber::DataReaderListener::on_data_available(DataReader* reader)
{
    // TODO: udpate new API

    // In case the TSubscriber is removing entities because a TEST_ENDS msg, it waits
    std::unique_lock<std::mutex> lecture_lock(throughput_subscriber_.data_mutex_);

    if (throughput_subscriber_.dynamic_data_)
    {
        while (subscriber->takeNextData((void*)throughput_subscriber_.dynamic_data_type_, &info_))
        {
            if (info_.sampleKind == ALIVE)
            {
                if ((last_seq_num_ + 1) < throughput_subscriber_.dynamic_data_type_->get_uint32_value(0))
                {
                    lost_samples_ += throughput_subscriber_.dynamic_data_type_->get_uint32_value(0) - last_seq_num_ - 1;
                }
                last_seq_num_ = throughput_subscriber_.dynamic_data_type_->get_uint32_value(0);
            }
            else
            {
                std::cout << "NOT ALIVE DATA RECEIVED" << std::endl;
            }
        }
    }
    else
    {
        if (nullptr != throughput_subscriber_.throughput_type_)
        {
            while (subscriber->takeNextData((void*)throughput_subscriber_.throughput_type_, &info_))
            {
                if (info_.sampleKind == ALIVE)
                {
                    if ((last_seq_num_ + 1) < throughput_subscriber_.throughput_type_->seqnum)
                    {
                        lost_samples_ += throughput_subscriber_.throughput_type_->seqnum - last_seq_num_ - 1;
                    }
                    last_seq_num_ = throughput_subscriber_.throughput_type_->seqnum;
                }
                else
                {
                    std::cout << "NOT ALIVE DATA RECEIVED" << std::endl;
                }
            }
        }
        else
        {
            std::cout << "DATA MESSAGE RECEIVED BEFORE COMMAND READY_TO_START" << std::endl;
        }
    }
}

void ThroughputSubscriber::DataReaderListener::save_numbers()
{
    saved_last_seq_num_ = last_seq_num_;
    saved_lost_samples_ = lost_samples_;
}

// *******************************************************************************************
// *********************************** COMMAND SUB LISTENER **********************************
// *******************************************************************************************

void ThroughputSubscriber::CommandReaderListener::on_subscription_matched(
                DataReader*,
                const SubscriptionMatchedStatus& info)
{
    std::unique_lock<std::mutex> lock(throughput_subscriber_.command_mutex_);

    if (1 == info.current_count)
    {
        std::cout << C_RED << "Sub: COMMAND Sub Matched" << C_DEF << std::endl;
    }
    else
    {
        std::cout << C_RED << "Sub: COMMAND SUBSCRIBER MATCHING REMOVAL" << C_DEF << std::endl;
    }

    matched_ = info.total_count;
    lock.unlock();
    throughput_subscriber_.command_discovery_cv_.notify_one();
}

void ThroughputSubscriber::CommandReaderListener::on_data_available(DataReader* reader) {}

// *******************************************************************************************
// *********************************** COMMAND PUB LISTENER **********************************
// *******************************************************************************************

void ThroughputSubscriber::CommandWriterListener::on_publication_matched(
                DataWriter*,
                const eprosima::fastdds::dds::PublicationMatchedStatus& info)
{
    std::unique_lock<std::mutex> lock(throughput_subscriber_.command_mutex_);

    if ( 1 == info.current_count)
    {
        std::cout << C_RED << "Sub: COMMAND Pub Matched" << C_DEF << std::endl;
    }
    else
    {
        std::cout << C_RED << "Sub: COMMAND PUBLISHER MATCHING REMOVAL" << C_DEF << std::endl;
    }

    matched_ = info.total_count;
    lock.unlock();
    throughput_subscriber_.command_discovery_cv_.notify_one();
}

// *******************************************************************************************
// ********************************** THROUGHPUT SUBSCRIBER **********************************
// *******************************************************************************************

ThroughputSubscriber::ThroughputSubscriber(
    : data_sub_listener_(*this)
    , command_sub_listener_(*this)
    , command_pub_listener_(*this)
{
}

ThroughputSubscriber::~ThroughputSubscriber()
{
    Domain::removeParticipant(participant_);
    std::cout << "Sub: Participant removed" << std::endl;
}

bool ThroughputSubscriber::init(
        bool reliable,
        uint32_t pid,
        bool hostname,
        const eprosima::fastrtps::rtps::PropertyPolicy& part_property_policy,
        const eprosima::fastrtps::rtps::PropertyPolicy& property_policy,
        const std::string& xml_config_file,
        bool dynamic_types,
        int forced_domain)
{
    pid_ = pid;
    hostname_ = hostname;
    dynamic_types_ = dynamic_types;
    reliable_ = reliable;
    forced_domain_ = forced_domain;
    xml_config_file_ = xml_config_file;

    /* Create DomainParticipant*/
    std::string participant_profile_name = "sub_participant_profile";
    DomainParticipantQos pqos;

    // Default domain
    DomainId_t domainId = pid % 230;

    // Default participant name
    pqos.name("throughput_test_subscriber");

    // Load XML configuration
    if (xml_config_file_.length() > 0)
    {
        if ( ReturnCode_t::RETCODE_OK !=
                DomainParticipantFactory::get_instance()->
                        get_participant_qos_from_profile(
                    participant_profile_name,
                    pqos))
        {
            return false;
        }
    }

    // Apply user's force domain
    if (forced_domain_ >= 0)
    {
        domainId = forced_domain_;
    }

    // If the user has specified a participant property policy with command line arguments, it overrides whatever the
    // XML configures.
    if (PropertyPolicyHelper::length(part_property_policy) > 0)
    {
        pqos.properties(part_property_policy);
    }

    // Create the participant
    participant_ =
            DomainParticipantFactory::get_instance()->create_participant(domainId, pqos);

    if (participant_ == nullptr)
    {
        std::cout << "ERROR creating participant" << std::endl;
        ready_ = false;
        return;
    }

    // Create the command data type
    throughput_command_type_.reset(new ThroughputCommandDataType());

    // Register the command data type
    if (ReturnCode_t::RETCODE_OK
            != throughput_command_type_.register_type(participant_))
    {
        logError(THROUGHPUTSUBSCRIBER, "ERROR registering command type");
        return false;
    }

    /* Create Publisher */
    publisher_ = participant_->create_publisher(PUBLISHER_QOS_DEFAULT, nullptr);
    if (publisher_ == nullptr)
    {
        logError(THROUGHPUTSUBSCRIBER, "ERROR creating the Publisher");
        return false;
    }

    /* Create Subscriber */
    subscriber_ = participant_->create_subscriber(SUBSCRIBER_QOS_DEFAULT, nullptr);
    if (subscriber_ == nullptr)
    {
        logError(THROUGHPUTSUBSCRIBER, "ERROR creating the Subscriber");
        return false;
    }

    /* Update DataReaderQoS from xml profile data */
    std::string profile_name = "subscriber_profile";

    if (xml_config_file_.length() > 0
        && ReturnCode_t::RETCODE_OK != subscriber_->get_datareader_qos_from_profile(profile_name, dr_qos_))
    {
        logError(THROUGHPUTSUBSCRIBER, "ERROR unable to retrieve the " << profile_name);
        return false;
    }
    // Load the property policy specified
    dr_qos_.properties(property_policy);

    // Create Command topic
    {
        std::ostringstream topic_name;
        topic_name << "ThroughputTest_Command_";
        if (hostname)
        {
            topic_name << asio::ip::host_name() << "_";
        }
        topic_name << pid << "_PUB2SUB";

        command_sub_topic_ = participant_->create_topic(
                topic_name.str(),
                "ThroughputCommand",
                TOPIC_QOS_DEFAULT);

        if (nullptr == command_sub_topic_)
        {
            logError(THROUGHPUTSUBSCRIBER, "ERROR creating the COMMAND Sub topic");
            return false;
        }

        topic_name.str("");
        topic_name.clear();
        topic_name << "ThroughputTest_Command_";
        if (hostname)
        {
            topic_name << asio::ip::host_name() << "_";
        }
        topic_name << pid << "_SUB2PUB";

        command_pub_topic_ = participant_->create_topic(
                topic_name.str(),
                "ThroughputCommand",
                TOPIC_QOS_DEFAULT);

        if (nullptr == command_pub_topic_)
        {
            logError(THROUGHPUTSUBSCRIBER, "ERROR creating the COMMAND Pub topic");
            return false;
        }
    }

    /* Create Command Reader */
    {
        DataReaderQos cr_qos;
        cr_qos.history().kind = KEEP_ALL_HISTORY_QOS;
        cr_qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
        cr_qos.durability().durabilityKind(TRANSIENT_LOCAL);
        cr_qos.properties(property_policy);

        command_reader_ = subscriber_->create_datareader(
            command_sub_topic_,
            cr_qos,
            &command_reader_listener_);

        if (command_reader_ == nullptr)
        {
            logError(THROUGHPUTSUBSCRIBER, "ERROR creating the COMMAND DataWriter");
            return false;
        }
    }

    /* Create Command Writer */
    {
        DataWriterQos cw_qos;
        cw_qos.history().kind = KEEP_ALL_HISTORY_QOS;
        cw_qos.durability().durabilityKind(TRANSIENT_LOCAL);
        cw_qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
        cw_qos.publish_mode().kind = SYNCHRONOUS_PUBLISH_MODE;
        cw_qos.properties(property_policy);

        command_writer_ = publisher_->create_datawriter(
                command_pub_topic_,
                cw_qos,
                &command_writer_listener_);

        if (command_writer_ == nullptr)
        {
            logError(THROUGHPUTSUBSCRIBER, "ERROR creating the COMMAND DataReader");
            return false;
        }
    }

    // Calculate overhead
    t_start_ = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i)
    {
        t_end_ = std::chrono::steady_clock::now();
    }
    t_overhead_ = std::chrono::duration<double, std::micro>(t_end_ - t_start_) / 1001;
    std::cout << "Subscriber's clock access overhead: " << t_overhead_.count() << " us" << std::endl;

    // Endpoints using dynamic data endpoints span the whole test duration
    // Static types and endpoints are created for each payload iteration
    return dynamic_types_ ? init_dynamic_types() && create_data_endpoints() : true;
}

void ThroughputSubscriber::process_message()
{
    if (command_subscriber_->wait_for_unread_samples({100, 0}))
    {
        if (command_subscriber_->takeNextData((void*)&command_sub_listener_.command_type_,
                &command_sub_listener_.info_))
        {
            switch (command_sub_listener_.command_type_.m_command)
            {
                case (DEFAULT):
                {
                    break;
                }
                case (BEGIN):
                {
                    break;
                }
                case (READY_TO_START):
                {
                    std::cout << "-----------------------------------------------------------------------" << std::endl;
                    std::cout << "Command: READY_TO_START" << std::endl;
                    data_size_ = command_sub_listener_.command_type_.m_size;
                    demand_ = command_sub_listener_.command_type_.m_demand;

                    if (dynamic_data_)
                    {
                        // Create basic builders
                        DynamicTypeBuilder_ptr struct_type_builder(
                            DynamicTypeBuilderFactory::get_instance()->create_struct_builder());

                        // Add members to the struct.
                        struct_type_builder->add_member(0, "seqnum",
                                DynamicTypeBuilderFactory::get_instance()->create_uint32_type());
                        struct_type_builder->add_member(1, "data",
                                DynamicTypeBuilderFactory::get_instance()->create_sequence_builder(
                                    DynamicTypeBuilderFactory::get_instance()->create_byte_type(), data_size_));

                        struct_type_builder->set_name("ThroughputType");
                        dynamic_type_ = struct_type_builder->build();
                        dynamic_pub_sub_type_.CleanDynamicType();
                        dynamic_pub_sub_type_.SetDynamicType(dynamic_type_);

                        Domain::registerType(participant_, &dynamic_pub_sub_type_);

                        dynamic_data_type_ = DynamicDataFactory::get_instance()->create_data(dynamic_type_);
                    }
                    else
                    {
                        delete(throughput_data_type_);
                        delete(throughput_type_);

                        throughput_data_type_ = new ThroughputDataType(data_size_);
                        Domain::registerType(participant_, throughput_data_type_);
                        throughput_type_ = new ThroughputType(data_size_);
                    }

                    data_subscriber_ = Domain::createSubscriber(participant_, sub_attrs_, &data_sub_listener_);

                    ThroughputCommandType command_sample(BEGIN);
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    data_sub_listener_.reset();
                    command_publisher_->write(&command_sample);

                    std::cout << "Waiting for data discovery" << std::endl;
                    std::unique_lock<std::mutex> data_disc_lock(data_mutex_);
                    data_discovery_cv_.wait(data_disc_lock, [&]()
                            {
                                return data_discovery_count_ > 0;
                            });
                    data_disc_lock.unlock();
                    std::cout << "Discovery data complete" << std::endl;
                    break;
                }
                case (TEST_STARTS):
                {
                    std::cout << "Command: TEST_STARTS" << std::endl;
                    t_start_ = std::chrono::steady_clock::now();
                    break;
                }
                case (TEST_ENDS):
                {
                    t_end_ = std::chrono::steady_clock::now();
                    std::cout << "Command: TEST_ENDS" << std::endl;
                    data_sub_listener_.save_numbers();
                    std::unique_lock<std::mutex> lock(command_mutex_);
                    stop_count_ = 1;
                    lock.unlock();

                    // It stops the data listener to avoid seg faults with already removed entities
                    // It waits if the data listener is in the middle of a reading
                    std::unique_lock<std::mutex> lecture_lock(data_mutex_);

                    if (dynamic_data_)
                    {
                        DynamicTypeBuilderFactory::delete_instance();
                        DynamicDataFactory::get_instance()->delete_data(dynamic_data_type_);
                    }
                    else
                    {
                        delete(throughput_type_);
                        throughput_type_ = nullptr;
                    }
                    lecture_lock.unlock();

                    sub_attrs_ = data_subscriber_->getAttributes();
                    break;
                }
                case (ALL_STOPS):
                {
                    std::cout << "-----------------------------------------------------------------------" << std::endl;
                    std::unique_lock<std::mutex> lock(command_mutex_);
                    stop_count_ = 2;
                    lock.unlock();
                    std::cout << "Command: ALL_STOPS" << std::endl;
                    break;
                }
                default:
                {
                    break;
                }
            }
        }
    }
}

bool ThroughputSubscriber::ready()
{
    return ready_;
}

void ThroughputSubscriber::run()
{
    if (!ready_)
    {
        return;
    }
    std::cout << "Sub Waiting for command discovery" << std::endl;
    {
        std::unique_lock<std::mutex> lock(command_mutex_);
        std::cout << "Sub: lock command_mutex_ and wait to " << command_discovery_count_ <<
            " == 2" << std::endl;     // TODO remove if error disappear"
        command_discovery_cv_.wait(lock, [&]()
                {
                    std::cout << "Sub: wait to " << command_discovery_count_ << " == 2" <<
                        std::endl;     // TODO remove if error disappear"
                    return command_discovery_count_ >= 2;
                });
    }
    std::cout << "Sub Discovery command complete" << std::endl;

    do
    {
        process_message();

        if (stop_count_ == 1)
        {
            std::cout << "Waiting for data matching removal" << std::endl;
            std::unique_lock<std::mutex> data_disc_lock(data_mutex_);
            data_discovery_cv_.wait(data_disc_lock, [&]()
                    {
                        return data_discovery_count_ == 0;
                    });
            data_disc_lock.unlock();

            std::cout << "Waiting clean state" << std::endl;
            while (!data_subscriber_->isInCleanState())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            std::cout << "Sending results" << std::endl;
            ThroughputCommandType command_sample;
            command_sample.m_command = TEST_RESULTS;
            command_sample.m_demand = demand_;
            command_sample.m_size = data_size_ + 4 + 4;
            command_sample.m_lastrecsample = data_sub_listener_.saved_last_seq_num_;
            command_sample.m_lostsamples = data_sub_listener_.saved_lost_samples_;

            double total_time_count =
                    (std::chrono::duration<double, std::micro>(t_end_ - t_start_) - t_overhead_).count();

            if (total_time_count < std::numeric_limits<uint64_t>::min())
            {
                command_sample.m_totaltime = std::numeric_limits<uint64_t>::min();
            }
            else if (total_time_count > std::numeric_limits<uint64_t>::max())
            {
                command_sample.m_totaltime = std::numeric_limits<uint64_t>::max();
            }
            else
            {
                command_sample.m_totaltime = static_cast<uint64_t>(total_time_count);
            }

            std::cout << "Last Received Sample: " << command_sample.m_lastrecsample << std::endl;
            std::cout << "Lost Samples: " << command_sample.m_lostsamples << std::endl;
            std::cout << "Samples per second: "
                      << (double)(command_sample.m_lastrecsample - command_sample.m_lostsamples) * 1000000 /
                command_sample.m_totaltime
                      << std::endl;
            std::cout << "Test of size " << command_sample.m_size << " and demand " << command_sample.m_demand <<
                " ends." << std::endl;
            command_publisher_->write(&command_sample);

            stop_count_ = 0;

            Domain::removeSubscriber(data_subscriber_);
            std::cout << "Sub: Data subscriber removed" << std::endl;
            data_subscriber_ = nullptr;
            Domain::unregisterType(participant_, "ThroughputType");
            std::cout << "Sub: ThroughputType unregistered" << std::endl;

            if (!dynamic_data_)
            {
                delete throughput_data_type_;
                throughput_data_type_ = nullptr;
            }
            std::cout << "-----------------------------------------------------------------------" << std::endl;
        }
    } while (stop_count_ != 2);

    // ThroughputPublisher is waiting for all ThroughputSubscriber publishers and subscribers to unmatch. Leaving the
    // destruction of the entities to ~ThroughputSubscriber() is not enough for the intraprocess case, because
    // main_ThroughputTests first joins the publisher run thread and only then it joins this thread. This means that
    // ~ThroughputSubscriber() is only called when all the ThroughputSubscriber publishers and subscribers are disposed.
    Domain::removePublisher(command_publisher_);
    std::cout << "Sub: Command publisher removed" << std::endl;
    Domain::removeSubscriber(command_subscriber_);
    std::cout << "Sub: Command subscriber removed" << std::endl;

    return;
}

bool ThroughputSubscriber::init_dynamic_types()
{
    assert(participant_ != nullptr);

    // Check if it has been initialized before
    if (dynamic_pub_sub_type_)
    {
        logError(THROUGHPUTSUBSCRIBER, "ERROR DYNAMIC DATA type already initialized");
        return false;
    }
    else if(participant_->find_type(ThroughputDataType::type_name_))
    {
        logError(THROUGHPUTSUBSCRIBER, "ERROR DYNAMIC DATA type already registered");
        return false;
    }

    // Dummy type registration
    // Create basic builders
    DynamicTypeBuilder_ptr struct_type_builder(DynamicTypeBuilderFactory::get_instance()->create_struct_builder());

    // Add members to the struct.
    struct_type_builder->add_member(0, "seqnum", DynamicTypeBuilderFactory::get_instance()->create_uint32_type());
    struct_type_builder->add_member(1, "data", DynamicTypeBuilderFactory::get_instance()->create_sequence_builder(
                DynamicTypeBuilderFactory::get_instance()->create_byte_type(), BOUND_UNLIMITED));
    struct_type_builder->set_name(ThroughputDataType::type_name_);
    dynamic_pub_sub_type_.reset(new DynamicPubSubType(struct_type_builder->build()));

    // Register the data type
    if (ReturnCode_t::RETCODE_OK
            != dynamic_pub_sub_type_.register_type(participant_))
    {
        logError(THROUGHPUTSUBSCRIBER, "ERROR registering the DYNAMIC DATA topic");
        return false;
    }

    return true;
}

bool ThroughputSubscriber::init_static_types(uint32_t payload)
{
    assert(participant_ != nullptr);

    // Check if it has been initialized before
    if (throughput_data_type_)
    {
        logError(THROUGHPUTSUBSCRIBER, "ERROR STATIC DATA type already initialized");
        return false;
    }
    else if(participant_->find_type(ThroughputDataType::type_name_))
    {
        logError(THROUGHPUTSUBSCRIBER, "ERROR STATIC DATA type already registered");
        return false;
    }

    // Create the static type
    throughput_data_type_.reset(new ThroughputDataType(payload));
    // Register the static type
    if (ReturnCode_t::RETCODE_OK
            != throughput_data_type_.register_type(participant_))
    {
        return false;
    }

    return true;
}

bool ThroughputSubscriber::create_data_endpoints()
{
    if (nullptr != data_pub_topic_)
    {
        logError(THROUGHPUTSUBSCRIBER, "ERROR topic already initialized");
        return false;
    }

    if (nullptr != data_reader_)
    {
        logError(THROUGHPUTSUBSCRIBER, "ERROR data_writer_ already initialized");
        return false;
    }

    // Create the topic
    std::ostringstream topic_name;
    topic_name << "ThroughputTest_";
    if (hostname_)
    {
        topic_name << asio::ip::host_name() << "_";
    }
    topic_name << pid_ << "_UP";

    data_sub_topic_ = participant_->create_topic(
            topic_name.str(),
            ThroughputDataType::type_name_,
            TOPIC_QOS_DEFAULT);

    if (nullptr == data_sub_topic_)
    {
        logError(THROUGHPUTSUBSCRIBER, "ERROR creating the DATA topic");
        return false;
    }

    // Create the DataReader
    // Reliability
    ReliabilityQosPolicy rp;
    rp.kind = reliable_ ? eprosima::fastrtps::RELIABLE_RELIABILITY_QOS: eprosima::fastrtps::BEST_EFFORT_RELIABILITY_QOS;
    dr_qos_.reliability(rp);

    // Create the endpoint
    if (nullptr !=
            (data_writer_ = publisher_->create_datareader(
                data_sub_topic_,
                dw_qos_,
                &data_reader_listener_)))
    {
        return false;
    }

    return true;
}

bool ThroughputSubscriber::destroy_data_endpoints()
{
    assert(nullptr != participant_);
    assert(nullptr != subscriber_);

    // Delete the endpoint
    if (nullptr == data_reader_
            || ReturnCode_t::RETCODE_OK != subscriber_->delete_datareader(data_reader_))
    {
        logError(THROUGHPUTSUBSCRIBER, "ERROR destroying the DataWriter");
        return false;
    }
    data_reader_ = nullptr;
    data_reader_listener_.reset();

    // Delete the Topic
    if (nullptr == data_sub_topic_
            || ReturnCode_t::RETCODE_OK != participant_->delete_topic(data_sub_topic_))
    {
        logError(THROUGHPUTSUBSCRIBER, "ERROR destroying the DATA topic");
        return false;
    }
    data_sub_topic_ = nullptr;

    // Delete the Type
    if (ReturnCode_t::RETCODE_OK
            !=participant_->unregister_type(ThroughputDataType::type_name_))
    {
        logError(THROUGHPUTSUBSCRIBER, "ERROR unregistering the DATA type");
        return false;
    }

    throughput_data_type_.reset();

    return true;
}

int ThroughputSubscriber::total_matches() const
{
    // no need to lock because is used always within a
    // condition variable wait predicate

    int count = data_reader_listener_.get_matches()
            + command_writer_listener_.get_matches()
            + command_reader_listener_.get_matches();

    // Each endpoint has a mirror counterpart in the ThroughputPublisher
    // thus, the maximun number of matches is 3 
    assert(count >= 0 && count <= 3 );
    return count;
}
