// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2023 Intel Corporation. All Rights Reserved.

#include "rs-dds-sensor-proxy.h"
#include "rs-dds-option.h"

#include <realdds/dds-device.h>
#include <realdds/dds-time.h>

#include <realdds/topics/device-info-msg.h>
#include <realdds/topics/image-msg.h>

#include <src/stream.h>

// Processing blocks for DDS SW sensors
#include <proc/decimation-filter.h>
#include <proc/disparity-transform.h>
#include <proc/hdr-merge.h>
#include <proc/hole-filling-filter.h>
#include <proc/sequence-id-filter.h>
#include <proc/spatial-filter.h>
#include <proc/temporal-filter.h>
#include <proc/threshold.h>

#include <nlohmann/json.hpp>


namespace librealsense {


// Constants for Json lookups
static const std::string frame_number_key( "frame-number", 12 );
static const std::string metadata_key( "metadata", 8 );
static const std::string timestamp_key( "timestamp", 9 );
static const std::string timestamp_domain_key( "timestamp-domain", 16 );
static const std::string depth_units_key( "depth-units", 11 );
static const std::string metadata_header_key( "header", 6 );


dds_sensor_proxy::dds_sensor_proxy( std::string const & sensor_name,
                                    software_device * owner,
                                    std::shared_ptr< realdds::dds_device > const & dev )
    : software_sensor( sensor_name, owner )
    , _dev( dev )
    , _name( sensor_name )
    , _md_enabled( dev->supports_metadata() )
{
}


void dds_sensor_proxy::add_dds_stream( sid_index sidx, std::shared_ptr< realdds::dds_stream > const & stream )
{
    auto & s = _streams[sidx];
    if( s )
    {
        LOG_ERROR( "stream at " << sidx.to_string() << " already exists for sensor '" << get_name() << "'" );
        return;
    }
    s = stream;
}


std::shared_ptr< realdds::dds_video_stream_profile >
dds_sensor_proxy::find_profile( sid_index sidx, realdds::dds_video_stream_profile const & profile ) const
{
    auto it = _streams.find( sidx );
    if( it == _streams.end() )
    {
        LOG_ERROR( "Invalid stream index " << sidx.to_string() << " in rs2 profile for sensor '" << get_name() << "'" );
    }
    else
    {
        auto & stream = it->second;
        for( auto & sp : stream->profiles() )
        {
            auto vsp = std::static_pointer_cast< realdds::dds_video_stream_profile >( sp );
            if( profile.width() == vsp->width() && profile.height() == vsp->height()
                && profile.format() == vsp->format() && profile.frequency() == vsp->frequency() )
            {
                return vsp;
            }
        }
    }
    return std::shared_ptr< realdds::dds_video_stream_profile >();
}


std::shared_ptr< realdds::dds_motion_stream_profile >
dds_sensor_proxy::find_profile( sid_index sidx, realdds::dds_motion_stream_profile const & profile ) const
{
    auto it = _streams.find( sidx );
    if( it == _streams.end() )
    {
        LOG_ERROR( "Invalid stream index " << sidx.to_string() << " in rs2 profile for sensor '" << get_name() << "'" );
    }
    else
    {
        auto & stream = it->second;
        for( auto & sp : stream->profiles() )
        {
            auto msp = std::static_pointer_cast< realdds::dds_motion_stream_profile >( sp );
            if( profile.format() == msp->format() && profile.frequency() == msp->frequency() )
            {
                return msp;
            }
        }
    }
    return std::shared_ptr< realdds::dds_motion_stream_profile >();
}


void dds_sensor_proxy::open( const stream_profiles & profiles )
{
    realdds::dds_stream_profiles realdds_profiles;
    for( size_t i = 0; i < profiles.size(); ++i )
    {
        auto & sp = profiles[i];
        sid_index sidx( sp->get_unique_id(), sp->get_stream_index() );
        if( Is< video_stream_profile >( sp ) )
        {
            const auto && vsp = As< video_stream_profile >( profiles[i] );
            auto video_profile = find_profile(
                sidx,
                realdds::dds_video_stream_profile( sp->get_framerate(),
                                                   realdds::dds_stream_format::from_rs2( sp->get_format() ),
                                                   vsp->get_width(),
                                                   vsp->get_height() ) );
            if( video_profile )
                realdds_profiles.push_back( video_profile );
            else
                LOG_ERROR( "no profile found in stream for rs2 profile " << vsp );
        }
        else if( Is< motion_stream_profile >( sp ) )
        {
            auto motion_profile = find_profile(
                sidx,
                realdds::dds_motion_stream_profile( profiles[i]->get_framerate(),
                                                    realdds::dds_stream_format::from_rs2( sp->get_format() ) ) );
            if( motion_profile )
                realdds_profiles.push_back( motion_profile );
            else
                LOG_ERROR( "no profile found in stream for rs2 profile " << sp );
        }
        else
        {
            LOG_ERROR( "unknown stream profile type for rs2 profile " << sp );
        }
    }

    if( profiles.size() > 0 )
    {
        _dev->open( realdds_profiles );
    }

    software_sensor::open( profiles );
}


void dds_sensor_proxy::handle_video_data( realdds::topics::image_msg && dds_frame,
                                          const std::shared_ptr< stream_profile_interface > & profile,
                                          streaming_impl & streaming )
{
    frame_additional_data data;  // with NO metadata by default!
    data.timestamp               // in ms
        = static_cast< rs2_time_t >( realdds::time_to_double( dds_frame.timestamp ) * 1e3 );
    data.timestamp_domain;  // from metadata, or leave default (hardware domain)
    data.depth_units;       // from metadata
    data.frame_number;      // filled in only once metadata is known

    auto vid_profile = dynamic_cast< video_stream_profile_interface * >( profile.get() );
    if( ! vid_profile )
        throw invalid_value_exception( "non-video profile provided to on_video_frame" );

    auto stride = static_cast< int >( dds_frame.height > 0 ? dds_frame.raw_data.size() / dds_frame.height
                                                           : dds_frame.raw_data.size() );
    auto bpp = dds_frame.width > 0 ? stride / dds_frame.width : stride;
    auto new_frame_interface = allocate_new_video_frame( vid_profile, stride, bpp, std::move( data ) );
    if( ! new_frame_interface )
        return;

    auto new_frame = static_cast< frame * >( new_frame_interface );
    new_frame->data = std::move( dds_frame.raw_data );

    if( _md_enabled )
    {
        streaming.syncer.enqueue_frame( dds_frame.timestamp.to_ns(), streaming.syncer.hold( new_frame ) );
    }
    else
    {
        invoke_new_frame( new_frame,
                          nullptr,    // pixels are already inside new_frame->data
                          nullptr );  // so no deleter is necessary
    }
}


void dds_sensor_proxy::handle_motion_data( realdds::topics::image_msg && dds_frame,
                                           const std::shared_ptr< stream_profile_interface > & profile,
                                           streaming_impl & streaming )
{
    frame_additional_data data;  // with NO metadata by default!
    data.timestamp
        = static_cast< rs2_time_t >( realdds::time_to_double( dds_frame.timestamp ) * 1e-3 );  // milliseconds
    data.timestamp_domain;  // from metadata, or leave default (hardware domain)
    data.depth_units;       // from metadata
    data.frame_number;      // filled in only once metadata is known

    auto new_frame_interface = allocate_new_frame( RS2_EXTENSION_MOTION_FRAME, profile.get(), std::move( data ) );
    if( ! new_frame_interface )
        return;

    auto new_frame = static_cast< frame * >( new_frame_interface );
    new_frame->data = std::move( dds_frame.raw_data );

    if( _md_enabled )
    {
        streaming.syncer.enqueue_frame( dds_frame.timestamp.to_ns(), streaming.syncer.hold( new_frame ) );
    }
    else
    {
        invoke_new_frame( new_frame,
                          nullptr,    // pixels are already inside new_frame->data
                          nullptr );  // so no deleter is necessary
    }
}


void dds_sensor_proxy::handle_new_metadata( std::string const & stream_name, nlohmann::json && dds_md )
{
    if( ! _md_enabled )
        return;

    auto it = _streaming_by_name.find( stream_name );
    if( it != _streaming_by_name.end() )
        it->second.syncer.enqueue_metadata(
            rsutils::json::get< realdds::dds_nsec >( dds_md[metadata_header_key], timestamp_key ),
            std::move( dds_md ) );
    else
        throw std::runtime_error( "Stream '" + stream_name + "' received metadata but does not enable metadata" );
}


void dds_sensor_proxy::add_frame_metadata( frame * const f, nlohmann::json && dds_md, streaming_impl & streaming )
{
    if( dds_md.empty() )
    {
        // Without MD, we have no way of knowing the frame-number - we assume it's one higher than
        // the last
        f->additional_data.last_frame_number = streaming.last_frame_number.fetch_add( 1 );
        f->additional_data.frame_number = f->additional_data.last_frame_number + 1;
        // the frame should already have empty metadata, so no need to do anything else
        return;
    }

    nlohmann::json const & md_header = dds_md[metadata_header_key];
    nlohmann::json const & md = dds_md[metadata_key];

    // A frame number is "optional". If the server supplies it, we try to use it for the simple fact that,
    // otherwise, we have no way of detecting drops without some advanced heuristic tracking the FPS and
    // timestamps. If not supplied, we use an increasing counter.
    // Note that if we have no metadata, we have no frame-numbers! So we need a way of generating them
    if( rsutils::json::get_ex( md_header, frame_number_key, &f->additional_data.frame_number ) )
    {
        f->additional_data.last_frame_number = streaming.last_frame_number.exchange( f->additional_data.frame_number );
        if( f->additional_data.frame_number != f->additional_data.last_frame_number + 1
            && f->additional_data.last_frame_number )
        {
            LOG_DEBUG( "frame drop? expecting " << f->additional_data.last_frame_number + 1 << "; got "
                                                << f->additional_data.frame_number );
        }
    }
    else
    {
        f->additional_data.last_frame_number = streaming.last_frame_number.fetch_add( 1 );
        f->additional_data.frame_number = f->additional_data.last_frame_number + 1;
    }

    // Timestamp is already set in the frame - must be communicated in the metadata, but only for syncing
    // purposes, so we ignore here. The domain is optional, and really only rs-dds-server communicates it
    // because the source is librealsense...
    f->additional_data.timestamp;
    rsutils::json::get_ex( md_header, timestamp_domain_key, &f->additional_data.timestamp_domain );

    // Expected metadata for all depth images
    rsutils::json::get_ex( md_header, depth_units_key, &f->additional_data.depth_units );

    // Other metadata fields. Metadata fields that are present but unknown by librealsense will be ignored.
    auto & metadata = reinterpret_cast< metadata_array & >( f->additional_data.metadata_blob );
    for( size_t i = 0; i < static_cast< size_t >( RS2_FRAME_METADATA_COUNT ); ++i )
    {
        auto key = static_cast< rs2_frame_metadata_value >( i );
        std::string const & keystr = librealsense::get_string( key );
        try
        {
            metadata[key] = { true, rsutils::json::get< rs2_metadata_type >( md, keystr ) };
        }
        catch( std::runtime_error const & )
        {
            // The metadata key doesn't exist or the value isn't the right type... we ignore it!
            // (all metadata is not there when we create the frame, so no need to erase)
        }
    }
}


void dds_sensor_proxy::start( frame_callback_ptr callback )
{
    for( auto & profile : sensor_base::get_active_streams() )
    {
        auto & dds_stream = _streams[sid_index( profile->get_unique_id(), profile->get_stream_index() )];
        // Opening it will start streaming on the server side automatically
        dds_stream->open( "rt/" + _dev->device_info().topic_root + '_' + dds_stream->name(), _dev->subscriber() );
        auto & streaming = _streaming_by_name[dds_stream->name()];
        streaming.syncer.on_frame_release( frame_releaser );
        streaming.syncer.on_frame_ready(
            [this, &streaming]( syncer_type::frame_holder && fh, nlohmann::json && md )
            {
                add_frame_metadata( static_cast< frame * >( fh.get() ), std::move( md ), streaming );
                invoke_new_frame( static_cast< frame * >( fh.release() ), nullptr, nullptr );
            } );

        if( auto dds_video_stream = std::dynamic_pointer_cast< realdds::dds_video_stream >( dds_stream ) )
        {
            dds_video_stream->on_data_available(
                [profile, this, &streaming]( realdds::topics::image_msg && dds_frame )
                {
                    if( _is_streaming )
                        handle_video_data( std::move( dds_frame ), profile, streaming );
                } );
        }
        else if( auto dds_motion_stream = std::dynamic_pointer_cast< realdds::dds_motion_stream >( dds_stream ) )
        {
            dds_motion_stream->on_data_available(
                [profile, this, &streaming]( realdds::topics::image_msg && dds_frame )
                {
                    if( _is_streaming )
                        handle_motion_data( std::move( dds_frame ), profile, streaming );
                } );
        }
        else
            throw std::runtime_error( "Unsupported stream type" );

        dds_stream->start_streaming();
    }

    software_sensor::start( callback );
}


void dds_sensor_proxy::stop()
{
    for( auto & profile : sensor_base::get_active_streams() )
    {
        auto & dds_stream = _streams[sid_index( profile->get_unique_id(), profile->get_stream_index() )];

        if( auto dds_video_stream = std::dynamic_pointer_cast< realdds::dds_video_stream >( dds_stream ) )
        {
            dds_video_stream->on_data_available( nullptr );
        }
        else if( auto dds_motion_stream = std::dynamic_pointer_cast< realdds::dds_motion_stream >( dds_stream ) )
        {
            dds_motion_stream->on_data_available( nullptr );
        }
        else
            throw std::runtime_error( "Unsupported stream type" );

        dds_stream->stop_streaming();
        dds_stream->close();

        _streaming_by_name[dds_stream->name()].syncer.on_frame_ready( nullptr );
    }

    // Must be done after dds_stream->stop_streaming or we will need to add validity checks to on_data_available
    _streaming_by_name.clear();

    // Resets frame source. Nullify streams on_data_available before calling stop.
    software_sensor::stop();
}


void dds_sensor_proxy::add_option( std::shared_ptr< realdds::dds_option > option )
{
    // Convert name to rs2_option type
    rs2_option option_id = RS2_OPTION_COUNT;
    for( size_t i = 0; i < static_cast< size_t >( RS2_OPTION_COUNT ); i++ )
    {
        if( option->get_name().compare( get_string( static_cast< rs2_option >( i ) ) ) == 0 )
        {
            option_id = static_cast< rs2_option >( i );
            break;
        }
    }

    if( option_id == RS2_OPTION_COUNT )
        throw librealsense::invalid_value_exception( "Option " + option->get_name() + " type not found" );

    auto opt = std::make_shared< rs_dds_option >(
        option,
        [&]( const std::string & name, float value ) { set_option( name, value ); },
        [&]( const std::string & name ) -> float { return query_option( name ); } );
    register_option( option_id, opt );
}


void dds_sensor_proxy::set_option( const std::string & name, float value ) const
{
    // Sensor is setting the option for all supporting streams (with same value)
    for( auto & stream : _streams )
    {
        for( auto & dds_opt : stream.second->options() )
        {
            if( dds_opt->get_name().compare( name ) == 0 )
            {
                _dev->set_option_value( dds_opt, value );
                break;
            }
        }
    }
}


float dds_sensor_proxy::query_option( const std::string & name ) const
{
    for( auto & stream : _streams )
    {
        for( auto & dds_opt : stream.second->options() )
        {
            if( dds_opt->get_name().compare( name ) == 0 )
            {
                // Assumes value is same for all relevant streams in the sensor, values are always set together
                return _dev->query_option_value( dds_opt );
            }
        }
    }

    throw std::runtime_error( "Could not find a stream that supports option " + name );
}


void dds_sensor_proxy::add_processing_block( std::string filter_name )
{
    auto & current_filters = get_software_recommended_proccesing_blocks();

    if( processing_block_exists( current_filters.get_recommended_processing_blocks(), filter_name ) )
        return;  // Already created by another stream of this sensor

    create_processing_block( filter_name );
}


bool dds_sensor_proxy::processing_block_exists( processing_blocks const & blocks, std::string const & block_name ) const
{
    for( auto & block : blocks )
        if( block_name.compare( block->get_info( RS2_CAMERA_INFO_NAME ) ) == 0 )
            return true;

    return false;
}


void dds_sensor_proxy::create_processing_block( std::string & filter_name )
{
    auto & current_filters = get_software_recommended_proccesing_blocks();

    if( filter_name.compare( "Decimation Filter" ) == 0 )
        // sensor.cpp sets format option based on sensor type, but the filter does not use it and selects the
        // appropriate decimation algorithm based on processed frame profile format.
        current_filters.add_processing_block( std::make_shared< decimation_filter >() );
    else if( filter_name.compare( "HDR Merge" ) == 0 )
        current_filters.add_processing_block( std::make_shared< hdr_merge >() );
    else if( filter_name.compare( "Filter By Sequence id" ) == 0 )
        current_filters.add_processing_block( std::make_shared< sequence_id_filter >() );
    else if( filter_name.compare( "Threshold Filter" ) == 0 )
        current_filters.add_processing_block( std::make_shared< threshold >() );
    else if( filter_name.compare( "Depth to Disparity" ) == 0 )
        current_filters.add_processing_block( std::make_shared< disparity_transform >( true ) );
    else if( filter_name.compare( "Disparity to Depth" ) == 0 )
        current_filters.add_processing_block( std::make_shared< disparity_transform >( false ) );
    else if( filter_name.compare( "Spatial Filter" ) == 0 )
        current_filters.add_processing_block( std::make_shared< spatial_filter >() );
    else if( filter_name.compare( "Temporal Filter" ) == 0 )
        current_filters.add_processing_block( std::make_shared< temporal_filter >() );
    else if( filter_name.compare( "Hole Filling Filter" ) == 0 )
        current_filters.add_processing_block( std::make_shared< hole_filling_filter >() );
    else
        throw std::runtime_error( "Unsupported processing block '" + filter_name + "' received" );
}


}  // namespace librealsense
