#pragma once

#include <string>
#include <vector>
#include <stack>
#include <fstream>
#include <iostream>
#include <ostream>
#include <sstream>
#include <functional>
#include <memory.h>

#ifdef _MSC_VER

#include <stdlib.h>
#define bswap_16(x) _byteswap_ushort(x)
#define bswap_32(x) _byteswap_ulong(x)
#define bswap_64(x) _byteswap_uint64(x)

#elif defined(__APPLE__)

#include <libkern/OSByteOrder.h>
#define bswap_16(x) OSSwapInt16(x)
#define bswap_32(x) OSSwapInt32(x)
#define bswap_64(x) OSSwapInt64(x)

#else

#include <byteswap.h>

#endif

#ifndef __TINY_MOV_FILE_BASE_IMPL__
#define __TINY_MOV_FILE_BASE_IMPL__

#define MKTAG(a, b, c, d) (a | (b << 8) | (c << 16) | (d << 24))
#define __TINY_MOV_FILE_OUT_INITIAL_MDAT_ATOM_OFFSET 0x8000

class TinyMovMetadataKey
{
public:
    void Data(std::vector<uint8_t>& data)
    {
        _data = data;
    }
    std::vector<uint8_t>& Data()
    {
        return _data;
    }

    void Namespace(uint32_t value)
    {
        _namespace = value;
    }
    uint32_t Namespace()
    {
        return _namespace;
    }

protected:
    uint32_t _namespace;
    std::vector<uint8_t> _data;
};

class TinyMovMetadataValue
{
public:
    void Data(std::vector<uint8_t>& data)
    {
        _data = data;
    }
    std::vector<uint8_t>& Data()
    {
        return _data;
    }

    void Type(uint32_t type)
    {
        _type = type;
    }
    uint32_t Type()
    {
        return _type;
    }

    void Locale(uint32_t locale)
    {
        _locale = locale;
    }
    uint32_t Locale()
    {
        return _locale;
    }

protected:
    uint32_t _type;
    uint32_t _locale;
    std::vector<uint8_t> _data;
};

class TinyMovHandlerInfo
{
public:
    TinyMovHandlerInfo() : _enabled(false) {}

    void Enabled(bool enabled)
    {
        _enabled = enabled;
    }
    bool Enabled()
    {
        return _enabled;
    }

    void ComponentType(uint32_t component_type)
    {
        _component_type = component_type;
    }
    uint32_t ComponentType()
    {
        return _component_type;
    }

    void ComponentSubType(uint32_t component_subtype)
    {
        _component_subtype = component_subtype;
    }
    uint32_t ComponentSubType()
    {
        return _component_subtype;
    }

    void ComponentManufacturer(uint32_t component_manufacturer)
    {
        _component_manufacturer = component_manufacturer;
    }
    uint32_t ComponentManufacturer()
    {
        return _component_manufacturer;
    }

    void ComponentFlags(uint32_t component_flags)
    {
        _component_flags = component_flags;
    }
    uint32_t ComponentFlags()
    {
        return _component_flags;
    }

    void ComponentFlagsMask(uint32_t component_flags_mask)
    {
        _component_flags_mask = component_flags_mask;
    }
    uint32_t ComponentFlagsMask()
    {
        return _component_flags_mask;
    }

    void ComponentName(std::string component_name)
    {
        _component_name = component_name;
    }
    std::string ComponentName()
    {
        return _component_name;
    }

protected:
    uint32_t _component_type;
    uint32_t _component_subtype;
    uint32_t _component_manufacturer;
    uint32_t _component_flags;
    uint32_t _component_flags_mask;
    std::string _component_name;

    bool _enabled;
};

class TinyMovMetadata
{
public:
    std::vector<TinyMovMetadataKey>& Keys()
    {
        return _keys;
    }
    std::vector<TinyMovMetadataValue>& Values()
    {
        return _values;
    }

    TinyMovHandlerInfo& Handler()
    {
        return _hdlr;
    }

    bool IsOk()
    {
        if (Keys().size() != Values().size())
            return false;
        
        return true;
    }

protected:
    std::vector<TinyMovMetadataKey> _keys;
    std::vector<TinyMovMetadataValue> _values;
    TinyMovHandlerInfo _hdlr;
};

class TinyMovTrackMediaVideoDescription
{
public:
    TinyMovTrackMediaVideoDescription() {}

    uint32_t DataFormat()
    {
        return _data_format;
    }
    void DataFormat(uint32_t data_format)
    {
        _data_format = data_format;
    }

    uint16_t DataReferenceIndex()
    {
        return _data_reference_index;
    }
    void DataReferenceIndex(uint16_t data_reference_index)
    {
        _data_reference_index = data_reference_index;
    }

    uint16_t Version()
    {
        return _version;
    }
    void Version(uint16_t version)
    {
        _version = version;
    }

    uint16_t RevisionLevel()
    {
        return _revision_level;
    }
    void RevisionLevel(uint16_t revision_level)
    {
        _revision_level = revision_level;
    }

    uint32_t Vendor()
    {
        return _vendor;
    }
    void Vendor(uint32_t vendor)
    {
        _vendor = vendor;
    }

    uint32_t TemporalQuality()
    {
        return _temporal_quality;
    }
    void TemporalQuality(uint32_t temporal_quality)
    {
        _temporal_quality = temporal_quality;
    }

    uint32_t SpatialQuality()
    {
        return _spatial_quality;
    }
    void SpatialQuality(uint32_t spatial_quality)
    {
        _spatial_quality = spatial_quality;
    }

    uint16_t Width()
    {
        return _width;
    }
    void Width(uint16_t width)
    {
        _width = width;
    }

    uint16_t Height()
    {
        return _height;
    }
    void Height(uint16_t height)
    {
        _height = height;
    }

    uint32_t HorizontalResolution()
    {
        return _horizontal_resolution;
    }
    void HorizontalResolution(uint32_t horizontal_resolution)
    {
        _horizontal_resolution = horizontal_resolution;
    }

    uint32_t VerticalResolution()
    {
        return _vertical_resolution;
    }
    void VerticalResolution(uint32_t vertical_resolution)
    {
        _vertical_resolution = vertical_resolution;
    }

    uint16_t FrameCount()
    {
        return _frame_count;
    }
    void FrameCount(uint16_t frame_count)
    {
        _frame_count = frame_count;
    }

    std::string CompressorName()
    {
        return _compressor_name;
    }
    void CompressorName(std::string compressor_name)
    {
        _compressor_name = compressor_name;
    }

    uint16_t Depth()
    {
        return _depth;
    }
    void Depth(uint16_t depth)
    {
        _depth = depth;
    }

    uint16_t ColorTableId()
    {
        return _color_table_id;
    }
    void ColorTableId(uint16_t color_table_id)
    {
        _color_table_id = color_table_id;
    }

    struct Extension_t
    {
        uint32_t tag;
        std::vector<uint8_t> data;
    };
    std::vector<Extension_t>& Extensions()
    {
        return _extensions;
    }

    struct Extension_ZRAW_t
    {
        bool exists;
        uint32_t version;
        uint32_t unk0;
        uint32_t unk1;
    };
    Extension_ZRAW_t Ext_ZRAW()
    {
        Extension_ZRAW_t ext;
        ext.exists = false;
        ext.version = 0;
        ext.unk0 = 0;
        ext.unk1 = 0;

        for (auto it = Extensions().begin(); it != Extensions().end(); ++it)
        {
            if (it->tag == MKTAG('z', 'r', 'a', 'w'))
            {
                ext.exists = true;

                if (it->data.size() >= 4)
                    ext.version = bswap_32(((uint32_t*)it->data.data())[0]);
                
                if (it->data.size() >= 8)
                    ext.unk0 = bswap_32(((uint32_t*)it->data.data())[1]);

                if (it->data.size() >= 12)
                    ext.unk1 = bswap_32(((uint32_t*)it->data.data())[2]);

                break;
            }
        }

        return ext;
    }

protected:
    uint32_t _data_format;
    uint16_t _data_reference_index;
    uint16_t _version;
    uint16_t _revision_level;
    uint32_t _vendor;
    uint32_t _temporal_quality;
    uint32_t _spatial_quality;
    uint16_t _width;
    uint16_t _height;
    uint32_t _horizontal_resolution;
    uint32_t _vertical_resolution;
    uint16_t _frame_count;
    std::string _compressor_name;
    uint16_t _depth;
    uint16_t _color_table_id;

    std::vector<Extension_t> _extensions;
};

class TinyMovTrackMediaAudioDescription
{
public:
    TinyMovTrackMediaAudioDescription() {}

    uint32_t DataFormat()
    {
        return _data_format;
    }
    void DataFormat(uint32_t data_format)
    {
        _data_format = data_format;
    }

    uint16_t DataReferenceIndex()
    {
        return _data_reference_index;
    }
    void DataReferenceIndex(uint16_t data_reference_index)
    {
        _data_reference_index = data_reference_index;
    }

    uint16_t Version()
    {
        return _version;
    }
    void Version(uint16_t version)
    {
        _version = version;
    }

    uint16_t RevisionLevel()
    {
        return _revision_level;
    }
    void RevisionLevel(uint16_t revision_level)
    {
        _revision_level = revision_level;
    }

    uint32_t Vendor()
    {
        return _vendor;
    }
    void Vendor(uint32_t vendor)
    {
        _vendor = vendor;
    }

    uint16_t NumberOfChannels()
    {
        return _number_of_channels;
    }
    void NumberOfChannels(uint16_t number_of_channels)
    {
        _number_of_channels = number_of_channels;
    }

    uint16_t SampleSize()
    {
        return _sample_size;
    }
    void SampleSize(uint16_t sample_size)
    {
        _sample_size = sample_size;
    }

    uint16_t CompressionId()
    {
        return _compression_id;
    }
    void CompressionId(uint16_t compression_id)
    {
        _compression_id = compression_id;
    }

    uint16_t PacketSize()
    {
        return _packet_size;
    }
    void PacketSize(uint16_t packet_size)
    {
        _packet_size = packet_size;
    }

    uint32_t SampleRate()
    {
        return _sample_rate;
    }
    void SampleRate(uint32_t sample_rate)
    {
        _sample_rate = sample_rate;
    }

    std::vector<uint8_t>& ExtensionsData()
    {
        return _extensions_data;
    }

protected:
    uint32_t _data_format;
    uint16_t _data_reference_index;
    uint16_t _version;
    uint16_t _revision_level;
    uint32_t _vendor;
    uint16_t _number_of_channels;
    uint16_t _sample_size;
    uint16_t _compression_id;
    uint16_t _packet_size;
    uint32_t _sample_rate;

    std::vector<uint8_t> _extensions_data;
};

class TinyMovTrackMediaTimecodeDescription
{
public:
    TinyMovTrackMediaTimecodeDescription() {}

    uint32_t DataFormat()
    {
        return _data_format;
    }
    void DataFormat(uint32_t data_format)
    {
        _data_format = data_format;
    }

    uint16_t DataReferenceIndex()
    {
        return _data_reference_index;
    }
    void DataReferenceIndex(uint16_t data_reference_index)
    {
        _data_reference_index = data_reference_index;
    }

    uint32_t Flags()
    {
        return _flags;
    }
    void Flags(uint32_t flags)
    {
        _flags = flags;
    }

    uint32_t TimeScale()
    {
        return _time_scale;
    }
    void TimeScale(uint32_t time_scale)
    {
        _time_scale = time_scale;
    }

    uint32_t FrameDuration()
    {
        return _frame_duration;
    }
    void FrameDuration(uint32_t frame_duration)
    {
        _frame_duration = frame_duration;
    }

    uint8_t NumberOfFrames()
    {
        return _number_of_frames;
    }
    void NumberOfFrames(uint8_t number_of_frames)
    {
        _number_of_frames = number_of_frames;
    }

    std::vector<uint8_t>& ExtensionsData()
    {
        return _extensions_data;
    }

protected:
    uint32_t _data_format;
    uint16_t _data_reference_index;
    uint32_t _flags;
    uint32_t _time_scale;
    uint32_t _frame_duration;
    uint8_t _number_of_frames;

    std::vector<uint8_t> _extensions_data;
};

class TinyMovTrackMediaSampleDescriptionTable
{
public:
    std::vector<TinyMovTrackMediaVideoDescription>& VideoDescriptionTable()
    {
        return _video_description_table;
    }

    std::vector<TinyMovTrackMediaAudioDescription>& AudioDescriptionTable()
    {
        return _audio_description_table;
    }

    std::vector<TinyMovTrackMediaTimecodeDescription>& TimecodeDescriptionTable()
    {
        return _timecode_description_table;
    }

protected:
    std::vector<TinyMovTrackMediaVideoDescription> _video_description_table;
    std::vector<TinyMovTrackMediaAudioDescription> _audio_description_table;
    std::vector<TinyMovTrackMediaTimecodeDescription> _timecode_description_table;
};

class TinyMovTrackTimeToSampleInfo
{
public:
    struct Info_t
    {
        uint32_t sample_count;
        uint32_t sample_duration;
    };
    std::vector<Info_t>& Entries()
    {
        return _entries;
    }

protected:
    std::vector<Info_t> _entries;
};

class TinyMovTrackSampleToChunkInfo
{
public:
    struct Info_t
    {
        uint32_t first_chunk;
        uint32_t samples_per_chunk;
        uint32_t sample_description_id;
    };
    std::vector<Info_t>& Entries()
    {
        return _entries;
    }

protected:
    std::vector<Info_t> _entries;
};

class TinyMovTrackDataInfo
{
public:
    struct DataReferenceEntry_t
    {
        uint32_t type;
        uint32_t version_and_flags;
        std::vector<uint8_t> data;
    };
    std::vector<DataReferenceEntry_t>& Entries()
    {
        return _entries;
    }

protected:
    std::vector<DataReferenceEntry_t> _entries;
};

class TinyMovTrackMedia;
class TinyMovTrackMediaInfo
{
public:
    TinyMovTrackMediaInfo(TinyMovTrackMedia& parent)
        : _parent(&parent), _forced_sample_size(0) {}

    uint32_t ForcedSampleSize()
    {
        return _forced_sample_size;
    }
    void ForcedSampleSize(uint32_t forced_sample_size)
    {
        _forced_sample_size = forced_sample_size;
    }

    std::vector<uint64_t>& SampleSizes()
    {
        return _sample_sizes;
    }
    std::vector<uint64_t>& ChunkOffsets()
    {
        return _chunk_offsets;
    }

    TinyMovTrackMediaSampleDescriptionTable& DescriptionTable()
    {
        return _description_table;
    }

    TinyMovTrackTimeToSampleInfo& TimeToSample()
    {
        return _time_to_sample_info;
    }

    TinyMovTrackDataInfo& DataInfo()
    {
        return _data_info;
    }

    TinyMovTrackSampleToChunkInfo& SampleToChunk()
    {
        return _sample_to_chunk_info;
    }

    TinyMovHandlerInfo& Handler()
    {
        return _hdlr;
    }

    TinyMovTrackMedia& Parent()
    {
        return *_parent;
    }

protected:
    std::vector<uint64_t> _sample_sizes;
    std::vector<uint64_t> _chunk_offsets;
    uint32_t _forced_sample_size;

    TinyMovTrackMediaSampleDescriptionTable _description_table;
    TinyMovTrackTimeToSampleInfo _time_to_sample_info;
    TinyMovTrackDataInfo _data_info;
    TinyMovTrackSampleToChunkInfo _sample_to_chunk_info;

    TinyMovHandlerInfo _hdlr;

    TinyMovTrackMedia* _parent;
};

class TinyMovTrack;
class TinyMovTrackMedia
{
public:
    TinyMovTrackMedia(TinyMovTrack& parent) : _parent(&parent), _info(*this), _version(0) {}

    struct Flags_t
    {
        uint8_t values[3];
    };

	void Version(uint8_t value)
	{
		_version = value;
	}
	uint8_t Version()
	{
		return _version;
	}
    void Flags(uint8_t flags[3])
    {
        memcpy(_flags.values, flags, sizeof(_flags.values));
    }
    Flags_t Flags()
    {
        return _flags;
    }
    void CreationTime(uint64_t creation_time)
    {
        _creation_time = creation_time;
    }
    uint64_t CreationTime()
    {
        return _creation_time;
    }
    void ModificationTime(uint64_t modification_time)
    {
        _modification_time = modification_time;
    }
    uint64_t ModificationTime()
    {
        return _modification_time;
    }
    void TimeScale(uint32_t time_scale)
    {
        _time_scale = time_scale;
    }
    uint32_t TimeScale()
    {
        return _time_scale;
    }
    void Duration(uint64_t duration)
    {
        _duration = duration;
    }
    uint64_t Duration()
    {
        return _duration;
    }
    void Language(uint16_t language)
    {
        _language = language;
    }
    uint16_t Language()
    {
        return _language;
    }
    void Quality(uint16_t quality)
    {
        _quality = quality;
    }
    uint16_t Quality()
    {
        return _quality;
    }

    TinyMovHandlerInfo& Handler()
    {
        return _hdlr;
    }
    TinyMovTrackMediaInfo& Info()
    {
        return _info;
    }

    enum Type_t {Video, Audio, Timecode, Unknown};
    Type_t Type()
    {
        switch (Handler().ComponentSubType())
        {
        case MKTAG('v', 'i', 'd', 'e'):
            return Video;
        case MKTAG('s', 'o', 'u', 'n'):
            return Audio;
        case MKTAG('t', 'm', 'c', 'd'):
            return Timecode;
        }
        return Unknown;
    }

    TinyMovTrack& Parent()
    {
        return *_parent;
    }

protected:
    // Media Header
	uint8_t _version;
    Flags_t _flags;
    uint64_t _creation_time;
    uint64_t _modification_time;
    uint32_t _time_scale;
    uint64_t _duration;
    uint16_t _language;
    uint16_t _quality;

    TinyMovHandlerInfo _hdlr;
    TinyMovTrackMediaInfo _info;

    TinyMovTrack* _parent;
};

class TinyMovFile;
class TinyMovTrack
{
public:
    TinyMovTrack(TinyMovFile& parent) : _parent(&parent), _media(*this), _version(0) {}

    struct DisplayMatrix_t
    {
        uint32_t values[3][3];
    };
    struct Flags_t
    {
        uint8_t values[3];
    };

	void Version(uint8_t value)
	{
		_version = value;
	}
	uint8_t Version()
	{
		return _version;
	}
    void Flags(uint8_t flags[3])
    {
        memcpy(_flags.values, flags, sizeof(_flags.values));
    }
    Flags_t Flags()
    {
        return _flags;
    }
    void CreationTime(uint64_t creation_time)
    {
        _creation_time = creation_time;
    }
    uint64_t CreationTime()
    {
        return _creation_time;
    }
    void ModificationTime(uint64_t modification_time)
    {
        _modification_time = modification_time;
    }
    uint64_t ModificationTime()
    {
        return _modification_time;
    }
    void TrackId(uint32_t track_id)
    {
        _track_id = track_id;
    }
    uint32_t TrackId()
    {
        return _track_id;
    }
    void Duration(uint64_t duration)
    {
        _duration = duration;
    }
    uint64_t Duration()
    {
        return _duration;
    }
    void Layer(uint16_t layer)
    {
        _layer = layer;
    }
    uint16_t Layer()
    {
        return _layer;
    }
    void AlternativeGroup(uint16_t alternative_group)
    {
        _alternative_group = alternative_group;
    }
    uint16_t AlternativeGroup()
    {
        return _alternative_group;
    }
    void Volume(uint16_t volume)
    {
        _volume = volume;
    }
    uint16_t Volume()
    {
        return _volume;
    }
    void DisplayMatrix(uint32_t display_matrix[3][3])
    {
        memcpy(_display_matrix.values, display_matrix, sizeof(_display_matrix.values));
    }
    DisplayMatrix_t DisplayMatrix()
    {
        return _display_matrix;
    }
    void TrackWidth(uint32_t track_width)
    {
        _track_width = track_width;
    }
    uint32_t TrackWidth()
    {
        return _track_width;
    }
    void TrackHeight(uint32_t track_height)
    {
        _track_height = track_height;
    }
    uint32_t TrackHeight()
    {
        return _track_height;
    }

    TinyMovTrackMedia& Media()
    {
        return _media;
    }
    TinyMovFile& Parent()
    {
        return *_parent;
    }
	void Parent(TinyMovFile& parent)
	{
		_parent = &parent;
	}
protected:
    // Track Header
	uint8_t _version;
    Flags_t _flags;
    uint64_t _creation_time;
    uint64_t _modification_time;
    uint32_t _track_id;
    uint64_t _duration;
    uint16_t _layer;
    uint16_t _alternative_group;
    uint16_t _volume;
    DisplayMatrix_t _display_matrix;
    uint32_t _track_width;
    uint32_t _track_height;

    // Track Media
    TinyMovTrackMedia _media;

    // Parent
    TinyMovFile* _parent;
};

class TinyMovFileCompatibilityInfo
{
public:
    uint32_t MajorBrand()
    {
        return _major_brand;
    }
    void MajorBrand(uint32_t major_brand)
    {
        _major_brand = major_brand;
    }

    uint32_t MinorBrand()
    {
        return _minor_brand;
    }
    void MinorBrand(uint32_t minor_brand)
    {
        _minor_brand = minor_brand;
    }

    std::vector<uint32_t>& CompatibleBrands()
    {
        return _compatible_brands;
    }

protected:
    uint32_t _major_brand;
    uint32_t _minor_brand;
    std::vector<uint32_t> _compatible_brands;
};

class TinyMovFile
{
public:
	TinyMovFile() {}

	TinyMovFile(const TinyMovFile& in)
	{
		_tracks = in._tracks;
		_metadata = in._metadata;
		_compatibility_info = in._compatibility_info;
		_path = in._path;

		_flags = in._flags;
		_creation_time = in._creation_time;
		_modification_time = in._modification_time;
		_time_scale = in._time_scale;
		_duration = in._duration;
		_preferred_rate = in._preferred_rate;
		_preferred_volume = in._preferred_volume;
		_display_matrix = in._display_matrix;
		_preview_time = in._preview_time;
		_preview_duration = in._preview_duration;
		_poster_time = in._poster_time;
		_selection_time = in._selection_time;
		_selection_duration = in._selection_duration;
		_current_time = in._current_time;
		_next_track_id = in._next_track_id;

		for (auto it = _tracks.begin(); it != _tracks.end(); ++it)
			it->Parent(*this);
	}

    std::vector<TinyMovTrack>& Tracks()
    {
        return _tracks;
    }
    TinyMovMetadata& Metadata()
    {
        return _metadata;
    }

    TinyMovFileCompatibilityInfo& CompatibilityInfo()
    {
        return _compatibility_info;
    }

    struct DisplayMatrix_t
    {
        uint32_t values[3][3];
    };
    struct Flags_t
    {
        uint8_t values[3];
    };

    void Flags(uint8_t flags[3])
    {
        memcpy(_flags.values, flags, sizeof(_flags.values));
    }
    Flags_t Flags()
    {
        return _flags;
    }
    void CreationTime(uint64_t creation_time)
    {
        _creation_time = creation_time;
    }
    uint64_t CreationTime()
    {
        return _creation_time;
    }
    void ModificationTime(uint64_t modification_time)
    {
        _modification_time = modification_time;
    }
    uint64_t ModificationTime()
    {
        return _modification_time;
    }
    void TimeScale(uint32_t time_scale)
    {
        _time_scale = time_scale;
    }
    uint32_t TimeScale()
    {
        return _time_scale;
    }
    void Duration(uint64_t duration)
    {
        _duration = duration;
    }
    uint64_t Duration()
    {
        return _duration;
    }
    void PreferredRate(uint32_t preferred_rate)
    {
        _preferred_rate = preferred_rate;
    }
    uint32_t PreferredRate()
    {
        return _preferred_rate;
    }
    void PreferredVolume(uint16_t preferred_volume)
    {
        _preferred_volume = preferred_volume;
    }
    uint16_t PreferredVolume()
    {
        return _preferred_volume;
    }
    void DisplayMatrix(uint32_t display_matrix[3][3])
    {
        memcpy(_display_matrix.values, display_matrix, sizeof(_display_matrix.values));
    }
    DisplayMatrix_t DisplayMatrix()
    {
        return _display_matrix;
    }
    void PreviewTime(uint32_t preview_time)
    {
        _preview_time = preview_time;
    }
    uint32_t PreviewTime()
    {
        return _preview_time;
    }
    void PreviewDuration(uint32_t preview_duration)
    {
        _preview_duration = preview_duration;
    }
    uint32_t PreviewDuration()
    {
        return _preview_duration;
    }
    void PosterTime(uint32_t poster_time)
    {
        _poster_time = poster_time;
    }
    uint32_t PosterTime()
    {
        return _poster_time;
    }
    void SelectionTime(uint32_t selection_time)
    {
        _selection_time = selection_time;
    }
    uint32_t SelectionTime()
    {
        return _selection_time;
    }
    void SelectionDuration(uint32_t selection_duration)
    {
        _selection_duration = selection_duration;
    }
    uint32_t SelectionDuration()
    {
        return _selection_duration;
    }
    void CurrentTime(uint32_t current_time)
    {
        _current_time = current_time;
    }
    uint32_t CurrentTime()
    {
        return _current_time;
    }
    void NextTrackId(uint32_t next_track_id)
    {
        _next_track_id = next_track_id;
    }
    uint32_t NextTrackId()
    {
        return _next_track_id;
    }

    void Path(std::string path)
    {
        _path = path;
    }
    std::string Path()
    {
        return _path;
    }
protected:
    std::vector<TinyMovTrack> _tracks;
    TinyMovMetadata _metadata;
    std::string _path;
    TinyMovFileCompatibilityInfo _compatibility_info;

    // Movie Header
    Flags_t _flags;
    uint64_t _creation_time;
    uint64_t _modification_time;
    uint32_t _time_scale;
    uint64_t _duration;
    uint32_t _preferred_rate;
    uint16_t _preferred_volume;
    DisplayMatrix_t _display_matrix;
    uint32_t _preview_time;
    uint32_t _preview_duration;
    uint32_t _poster_time;
    uint32_t _selection_time;
    uint32_t _selection_duration;
    uint32_t _current_time;
    uint32_t _next_track_id;
};

struct TinyMovFileWriterParameters
{
	TinyMovFileWriterParameters()
	{
		chunk_handler = [](TinyMovTrack& track, uint64_t chunk_offset, std::vector<uint8_t>& chunk_data)
		{
			return chunk_data;
		};

		video_description_handler = [](TinyMovTrackMediaVideoDescription video_description)
		{
			return video_description;
		};
	}

	std::function<std::vector<uint8_t>(TinyMovTrack&, uint64_t, std::vector<uint8_t>&)> chunk_handler;
	std::function<TinyMovTrackMediaVideoDescription(TinyMovTrackMediaVideoDescription)> video_description_handler;
};

class fstream_in_wrapper
{
public:
    fstream_in_wrapper(std::fstream &stream, int64_t initial_size) : _stream(stream)
    {
        _atom_offsets_stack.push(0);
        _atom_sizes_stack.push(initial_size);
        _atom_ids_stack.push(MKTAG('R', 'O', 'O', 'T'));
    }

    uint8_t get_byte()
    {
        uint8_t value;
        if (atom_left() < sizeof(value))
            throw std::exception();

        _stream.read((char *)&value, sizeof(value));
        _atom_offsets_stack.top() += sizeof(value);
        return value;
    }
    uint16_t get_le16()
    {
        uint16_t value;
        if (atom_left() < sizeof(value))
            throw std::exception();

        _stream.read((char *)&value, sizeof(value));
        _atom_offsets_stack.top() += sizeof(value);
        return value;
    }
    uint16_t get_be16()
    {
        return bswap_16(get_le16());
    }
    uint32_t get_le32()
    {
        uint32_t value;
        if (atom_left() < sizeof(value))
            throw std::exception();

        _stream.read((char *)&value, sizeof(value));
        _atom_offsets_stack.top() += sizeof(value);
        return value;
    }
    uint32_t get_be32()
    {
        return bswap_32(get_le32());
    }
    uint64_t get_le64()
    {
        uint64_t value;
        if (atom_left() < sizeof(value))
            throw std::exception();

        _stream.read((char *)&value, sizeof(value));
        _atom_offsets_stack.top() += sizeof(value);
        return value;
    }
    uint64_t get_be64()
    {
        return bswap_64(get_le64());
    }
    void read(void* dst, uint32_t size)
    {
        uint8_t* dest8 = (uint8_t*)dst;
        for (uint32_t i = 0; i < size; ++i)
            dest8[i] = get_byte();
    }
    void seekg(int64_t offset)
    {
        if (atom_left() < offset)
            throw std::exception();
        _atom_offsets_stack.top() += offset;

        _stream.seekg(offset, _stream.cur);
    }
    void seekp(int64_t offset)
    {
        _stream.seekp(offset, _stream.cur);
    }
    int64_t tellg()
    {
        return _stream.tellg();
    }
    int64_t tellp()
    {
        return _stream.tellp();
    }
    void push_atom(int64_t size, uint32_t parent_id)
    {
        if (atom_left() < size)
            throw std::exception();

        _atom_offsets_stack.push(0);
        _atom_sizes_stack.push(size);
        _atom_ids_stack.push(parent_id);
    }
    void pop_atom()
    {
        if (atom_level() == 0)
            throw std::exception();

        auto top_size = _atom_sizes_stack.top();
        _atom_offsets_stack.pop();
        _atom_sizes_stack.pop();
        _atom_ids_stack.pop();

        _atom_offsets_stack.top() += top_size;
    }
    int64_t atom_left()
    {
        return _atom_sizes_stack.top() - _atom_offsets_stack.top();
    }
    int64_t atom_level()
    {
        return (int64_t)_atom_offsets_stack.size() - 1;
    }
    uint32_t atom_id()
    {
        return _atom_ids_stack.top();
    }
    uint32_t atom_parent_id()
    {
        auto id = atom_id();

        _atom_ids_stack.pop();
        auto parent_id = _atom_ids_stack.top();
        _atom_ids_stack.push(id);

        return parent_id;
    }

    void set_current_track(TinyMovTrack* track)
    {
        _current_track = track;
    }
    TinyMovTrack& get_current_track()
    {
        if (_current_track == nullptr)
            throw std::exception();
        return *_current_track;
    }

protected:
    std::fstream &_stream;
    std::stack<uint32_t> _atom_ids_stack;
    std::stack<int64_t> _atom_offsets_stack;
    std::stack<int64_t> _atom_sizes_stack;

    TinyMovTrack* _current_track;
};

#endif

class TinyMovFileWriter;
class fstream_out_wrapper
{
public:
    fstream_out_wrapper(std::fstream &stream, TinyMovFileWriterParameters parameters) :
        _file_stream(stream), _parameters(parameters)
    {
        uint32_t valA = bswap_32(0x00000001);           // Use 64-bit size after 'mdat' tag
        _file_stream.write((char *)&valA, sizeof(valA));

        uint32_t valB = MKTAG('m', 'd', 'a', 't');
        _file_stream.write((char *)&valB, sizeof(valB));

        uint64_t valC = bswap_64(0x0000000000000000);   // This field will be filled in finalize_file()
        _file_stream.write((char *)&valC, sizeof(valC));
    }

    void put_byte(uint8_t value)
    {
        _post_atoms_stream.write((char *)&value, sizeof(value));
        _atom_offsets_stack.top() += sizeof(value);
    }
    void put_le16(uint16_t value)
    {
        _post_atoms_stream.write((char *)&value, sizeof(value));
        _atom_offsets_stack.top() += sizeof(value);
    }
    void put_be16(uint16_t value)
    {
        return put_le16(bswap_16(value));
    }
    void put_le32(uint32_t value)
    {
        _post_atoms_stream.write((char *)&value, sizeof(value));
        _atom_offsets_stack.top() += sizeof(value);
    }
    void put_be32(uint32_t value)
    {
        return put_le32(bswap_32(value));
    }
    void put_le64(uint64_t value)
    {
        _post_atoms_stream.write((char *)&value, sizeof(value));
        _atom_offsets_stack.top() += sizeof(value);
    }
    void put_be64(uint64_t value)
    {
        return put_le64(bswap_64(value));
    }
    void write(void* src, uint32_t size)
    {
        _post_atoms_stream.write((char *)src, size);
        _atom_offsets_stack.top() += size;
    }
    int64_t tellp()
    {
        return _post_atoms_stream.tellp();
    }

    void push_atom(uint32_t atom_id)
    {
        _atom_offsets_stack.push(0);
        put_be32(0x00000000);
        put_le32(atom_id);
    }
    void pop_atom()
    {
        auto atom_size = _atom_offsets_stack.top();
        uint32_t atom_size_be = bswap_32(atom_size);

        // Seek back to atom size and write calculated atom size
        _post_atoms_stream.seekp(-atom_size, _post_atoms_stream.cur);

        // Rewrite atom size field
        _post_atoms_stream.write((char *)&atom_size_be, sizeof(atom_size_be));

        // Seek back to end of the written atom
        _post_atoms_stream.seekp(atom_size - sizeof(atom_size_be), _post_atoms_stream.cur);

        // Update parent atom info
        _atom_offsets_stack.pop();

        if (_atom_offsets_stack.size() > 0)
            _atom_offsets_stack.top() += atom_size;
    }

    void write_mdat_sample(void* src, uint32_t size)
    {
        _file_stream.write((char *)src, size);
    }
    int64_t tellp_mdat()
    {
        return _file_stream.tellp();
    }

    void finalize_file()
    {
        // Finalize 'mdat' atom
        auto mdat_atom_end = _file_stream.tellp();
        uint64_t mdat_atom_size_be = bswap_64(mdat_atom_end);

        // Seek back to atom size and write calculated atom size
        _file_stream.seekp(8, _file_stream.beg);
        _file_stream.write((char *)&mdat_atom_size_be, sizeof(mdat_atom_size_be));

        // Seek back to end of the written atom
        _file_stream.seekp(mdat_atom_end, _file_stream.beg);

        // Do other stuff
        _atom_offsets_stack = std::stack<int64_t>();
        _file_stream << _post_atoms_stream.rdbuf();
        _post_atoms_stream.clear();
    }

    TinyMovFileWriterParameters& parameters()
    {
        return _parameters;
    }

protected:
    std::stringstream _post_atoms_stream;
    std::fstream &_file_stream;
    std::stack<int64_t> _atom_offsets_stack;

    TinyMovFileWriterParameters _parameters;
};

class ITinyMovFileAtomProcessor
{
public:
    virtual ~ITinyMovFileAtomProcessor() {}

    virtual uint32_t Type() = 0;
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov) = 0;
};

std::vector<ITinyMovFileAtomProcessor*> TinyMovFileAtomProcessors;

class TinyMovFileAtomProcessorBase : public ITinyMovFileAtomProcessor
{
public:
    TinyMovFileAtomProcessorBase()
    {
        TinyMovFileAtomProcessors.push_back(this);
    }
};

class : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return MKTAG('f', 't', 'y', 'p');
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        if (stream.atom_parent_id() != MKTAG('R', 'O', 'O', 'T'))
        {
            std::cout << "Warning! Parent is not ROOT!" << std::endl;
            return false;
        }

        mov.CompatibilityInfo().MajorBrand(stream.get_le32());
        mov.CompatibilityInfo().MinorBrand(stream.get_le32());
        mov.CompatibilityInfo().CompatibleBrands().clear();

        auto count = stream.atom_left() / 4;
        for (int i = 0; i < count; ++i)
        {
            auto brand = stream.get_le32();
            if (brand != 0)
                mov.CompatibilityInfo().CompatibleBrands().push_back(brand);
        }

        // Skip garbage
        stream.seekg(stream.atom_left());

        return true;
    }
    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovFile& mov)
    {
        stream.push_atom(Type());

        // major
        stream.put_le32(mov.CompatibilityInfo().MajorBrand());

        // minor
        stream.put_le32(mov.CompatibilityInfo().MinorBrand());

        // compatible brands
        for (auto it = mov.CompatibilityInfo().CompatibleBrands().begin(); it != mov.CompatibilityInfo().CompatibleBrands().end(); ++it)
            stream.put_le32(*it);

        stream.pop_atom();
        return true;
    }
} TinyMovFileAtomProcessor_ftyp;

class TinyMovFileAtomProcessor_default_t : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return 0x00000000;
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        while (stream.atom_left() > 0)
        {
            int64_t size = stream.get_be32();
            uint32_t type = stream.get_le32();

            if (size == 1)
            {
                // "length may error!"
                size = stream.get_be64() - 8;
            }

            stream.push_atom(size - 8, type);
            
            int i = 0;
            for (; i < TinyMovFileAtomProcessors.size(); ++i)
                if (TinyMovFileAtomProcessors[i]->Type() == type)
                    break;

            char name[5];
            *(uint32_t*)name = type;
            name[4] = '\0';
            std::cout << "Atom: ";
            for (int i = 0; i < stream.atom_level() - 1; ++i)
                std::cout << "---";

            if (i < TinyMovFileAtomProcessors.size())
            {
                std::cout << "<" << name << ">" << std::endl;

                auto res = TinyMovFileAtomProcessors[i]->ProcessRead(stream, mov);
                if (!res)
                    return false;

                if (stream.atom_left() < 0)
                {
                    std::cout << "Atom left: " << stream.atom_left() << std::endl;
                    throw std::exception();
                }

                if (stream.atom_left() != 0)
                {
                    std::cout << "Warning: " << name << " atom left " << stream.atom_left() << std::endl;
                }
            }
            else
                std::cout << name << std::endl;

            stream.seekg(stream.atom_left());   // Skip atom garbage

            stream.pop_atom();
        }

        return true;
    }
    
    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovFile& mov) {return false;}
} TinyMovFileAtomProcessor_default;

class : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return MKTAG('m', 'v', 'h', 'd');
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        if (stream.atom_parent_id() != MKTAG('m', 'o', 'o', 'v'))
        {
            std::cout << "Warning! Parent is not moov!" << std::endl;
            return false;
        }

        std::cout << "Atom left: " << stream.atom_left() << std::endl;

        uint8_t version = stream.get_byte();

        uint8_t flags[3];
        flags[0] = stream.get_byte();
        flags[1] = stream.get_byte();
        flags[2] = stream.get_byte();
        mov.Flags(flags);

        mov.CreationTime((version == 1) ? stream.get_be64() : stream.get_be32());
        mov.ModificationTime((version == 1) ? stream.get_be64() : stream.get_be32());
        mov.TimeScale(stream.get_be32());
        mov.Duration((version == 1) ? stream.get_be64() : stream.get_be32());
        mov.PreferredRate(stream.get_be32());
        mov.PreferredVolume(stream.get_be16());

        stream.seekg(10);               // reserved

        uint32_t display_matrix[3][3];
        for (int i = 0; i < 3; ++i)     // display matrix
        {
            display_matrix[i][0] = stream.get_be32();
            display_matrix[i][1] = stream.get_be32();
            display_matrix[i][2] = stream.get_be32();
        }
        mov.DisplayMatrix(display_matrix);

        mov.PreviewTime(stream.get_be32());
        mov.PreviewDuration(stream.get_be32());
        mov.PosterTime(stream.get_be32());
        mov.SelectionTime(stream.get_be32());
        mov.SelectionDuration(stream.get_be32());
        mov.CurrentTime(stream.get_be32());
        mov.NextTrackId(stream.get_be32());

        return true;
    }
    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovFile& mov)
    {
        stream.push_atom(Type());
        
        uint8_t version = 1;
        stream.put_byte(version);

        auto flags = mov.Flags();
        stream.put_byte(flags.values[0]);
        stream.put_byte(flags.values[1]);
        stream.put_byte(flags.values[2]);

        (version == 1) ? stream.put_be64(mov.CreationTime()) : stream.put_be32(mov.CreationTime());
        (version == 1) ? stream.put_be64(mov.ModificationTime()) : stream.put_be32(mov.ModificationTime());
        stream.put_be32(mov.TimeScale());
        (version == 1) ? stream.put_be64(mov.Duration()) : stream.put_be32(mov.Duration());
        stream.put_be32(mov.PreferredRate());
        stream.put_be16(mov.PreferredVolume());

        // reserved
        stream.put_le64(0);
        stream.put_le16(0);
        // ===

        auto display_matrix = mov.DisplayMatrix();
        for (int i = 0; i < 3; ++i)     // display matrix
        {
            stream.put_be32(display_matrix.values[i][0]);
            stream.put_be32(display_matrix.values[i][1]);
            stream.put_be32(display_matrix.values[i][2]);
        }
        
        stream.put_be32(mov.PreviewTime());
        stream.put_be32(mov.PreviewDuration());
        stream.put_be32(mov.PosterTime());
        stream.put_be32(mov.SelectionTime());
        stream.put_be32(mov.SelectionDuration());
        stream.put_be32(mov.CurrentTime());
        stream.put_be32(mov.NextTrackId());

        stream.pop_atom();
        return true;
    }
} TinyMovFileAtomProcessor_mvhd;

class : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return MKTAG('t', 'k', 'h', 'd');
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        if (stream.atom_parent_id() != MKTAG('t', 'r', 'a', 'k'))
        {
            std::cout << "Warning! Parent is not trak!" << std::endl;
            return false;
        }

        uint8_t version = stream.get_byte();
		stream.get_current_track().Version(version);

        uint8_t flags[3];
        flags[0] = stream.get_byte();
        flags[1] = stream.get_byte();
        flags[2] = stream.get_byte();
        stream.get_current_track().Flags(flags);

        stream.get_current_track().CreationTime(
            (version == 1) ?
            stream.get_be64() :
            stream.get_be32());
        
        stream.get_current_track().ModificationTime(
            (version == 1) ?
            stream.get_be64() :
            stream.get_be32());

        stream.get_current_track().TrackId(stream.get_be32());

        stream.get_be32();      // reserved

        stream.get_current_track().Duration(
            (version == 1) ?
            stream.get_be64() :
            stream.get_be32());

        stream.get_be64();      // reserved

        stream.get_current_track().Layer(stream.get_be16());
        stream.get_current_track().AlternativeGroup(stream.get_be16());
        stream.get_current_track().Volume(stream.get_be16());

        stream.get_be16();      // reserved

        uint32_t display_matrix[3][3];
        for (int i = 0; i < 3; ++i)     // display matrix
        {
            display_matrix[i][0] = stream.get_be32();
            display_matrix[i][1] = stream.get_be32();
            display_matrix[i][2] = stream.get_be32();
        }
        stream.get_current_track().DisplayMatrix(display_matrix);

        stream.get_current_track().TrackWidth(stream.get_be32());
        stream.get_current_track().TrackHeight(stream.get_be32());

        return true;
    }
    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovTrack& track)
    {
        stream.push_atom(Type());

        stream.put_byte(track.Version());

        auto flags = track.Flags();
        stream.put_byte(flags.values[0]);
        stream.put_byte(flags.values[1]);
        stream.put_byte(flags.values[2]);

        (track.Version() == 1) ? stream.put_be64(track.CreationTime()) : stream.put_be32(track.CreationTime());
        (track.Version() == 1) ? stream.put_be64(track.ModificationTime()) : stream.put_be32(track.ModificationTime());
        stream.put_be32(track.TrackId());
        stream.put_be32(0);      // reserved
        (track.Version() == 1) ? stream.put_be64(track.Duration()) : stream.put_be32(track.Duration());
        stream.put_be64(0);      // reserved
        stream.put_be16(track.Layer());
        stream.put_be16(track.AlternativeGroup());
        stream.put_be16(track.Volume());
        stream.put_be16(0);      // reserved

        auto display_matrix = track.DisplayMatrix();
        for (int i = 0; i < 3; ++i)     // display matrix
        {
            stream.put_be32(display_matrix.values[i][0]);
            stream.put_be32(display_matrix.values[i][1]);
            stream.put_be32(display_matrix.values[i][2]);
        }

        stream.put_be32(track.TrackWidth());
        stream.put_be32(track.TrackWidth());

        stream.pop_atom();
        return true;
    }
} TinyMovFileAtomProcessor_tkhd;

class : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return MKTAG('m', 'd', 'h', 'd');
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        if (stream.atom_parent_id() != MKTAG('m', 'd', 'i', 'a'))
        {
            std::cout << "Warning! Parent is not mdia!" << std::endl;
            return false;
        }

        uint8_t version = stream.get_byte();
		stream.get_current_track().Media().Version(version);

        uint8_t flags[3];
        flags[0] = stream.get_byte();
        flags[1] = stream.get_byte();
        flags[2] = stream.get_byte();
        stream.get_current_track().Media().Flags(flags);

        stream.get_current_track().Media().CreationTime(
            (version == 1) ?
            stream.get_be64() :
            stream.get_be32());
        
        stream.get_current_track().Media().ModificationTime(
            (version == 1) ?
            stream.get_be64() :
            stream.get_be32());
        
        stream.get_current_track().Media().TimeScale(stream.get_be32());

        stream.get_current_track().Media().Duration(
            (version == 1) ?
            stream.get_be64() :
            stream.get_be32());
        
        stream.get_current_track().Media().Language(stream.get_be16());
        stream.get_current_track().Media().Quality(stream.get_be16());

        return true;
    }

    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovTrack& track)
    {
        stream.push_atom(Type());
        
        stream.put_byte(track.Media().Version());

        auto flags = track.Media().Flags();
        stream.put_byte(flags.values[0]);
        stream.put_byte(flags.values[1]);
        stream.put_byte(flags.values[2]);

        (track.Media().Version() == 1) ?
            stream.put_be64(track.Media().CreationTime()) :
            stream.put_be32(track.Media().CreationTime());

        (track.Media().Version() == 1) ?
            stream.put_be64(track.Media().ModificationTime()) :
            stream.put_be32(track.Media().ModificationTime());
        
        stream.put_be32(track.Media().TimeScale());

        (track.Media().Version() == 1) ?
            stream.put_be64(track.Media().Duration()) :
            stream.put_be32(track.Media().Duration());

        stream.put_be16(track.Media().Language());
        stream.put_be16(track.Media().Quality());

        stream.pop_atom();
        return true;
    }
} TinyMovFileAtomProcessor_mdhd;

class
{
public:
    bool ProcessRead(fstream_in_wrapper& stream, TinyMovTrackMediaVideoDescription& vdesc)
    {
        // reserved
        for (int i = 0; i < 6; ++i)
            stream.get_byte();

        // data reference index
        vdesc.DataReferenceIndex(stream.get_be16());

        // version
        vdesc.Version(stream.get_be16());

        // revision level
        vdesc.RevisionLevel(stream.get_be16());

        // vendor
        vdesc.Vendor(stream.get_be32());

        // temporal quality
        vdesc.TemporalQuality(stream.get_be32());

        // spatial quality
        vdesc.SpatialQuality(stream.get_be32());

        // width
        vdesc.Width(stream.get_be16());

        // height
        vdesc.Height(stream.get_be16());

        // horizontal resolution (fixed-point)
        vdesc.HorizontalResolution(stream.get_be32());

        // vertical resolution (fixed-point)
        vdesc.VerticalResolution(stream.get_be32());

        // data size
        auto data_size = stream.get_be32();
        if (data_size != 0)
            return false;

        // frame count
        vdesc.FrameCount(stream.get_be16());

        // str_len
        auto str_len = stream.get_byte();
        if (str_len > 31)
            str_len = 31;

        // compressor name
        for (int i = 0; i < 31; ++i)
            stream.get_byte();

        // depth
        vdesc.Depth(stream.get_be16());

        // color table id
        vdesc.ColorTableId(stream.get_be16());

        return true;
    }

    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovTrackMediaVideoDescription& vdesc)
    {
        // reserved
        for (int i = 0; i < 6; ++i)
            stream.put_byte(0);

        // data reference index
        stream.put_be16(vdesc.DataReferenceIndex());

        // version
        stream.put_be16(vdesc.Version());

        // revision level
        stream.put_be16(vdesc.RevisionLevel());

        // vendor
        stream.put_be32(vdesc.Vendor());

        // temporal quality
        stream.put_be32(vdesc.TemporalQuality());

        // spatial quality
        stream.put_be32(vdesc.SpatialQuality());

        // width
        stream.put_be16(vdesc.Width());

        // height
        stream.put_be16(vdesc.Height());

        // horizontal resolution (fixed-point)
        stream.put_be32(vdesc.HorizontalResolution());

        // vertical resolution (fixed-point)
        stream.put_be32(vdesc.VerticalResolution());

        // data size
        stream.put_be32(0);

        // frame count
        stream.put_be16(vdesc.FrameCount());

        // str_len
        auto str_len = vdesc.CompressorName().size();
        if (str_len > 31)
            str_len = 31;
        stream.put_byte(str_len);
        stream.write((char*)vdesc.CompressorName().c_str(), str_len);
        for (int i = str_len; i < 31; ++i)
            stream.put_byte(0);

        // depth
        stream.put_be16(vdesc.Depth());

        // color table id
        stream.put_be16(vdesc.ColorTableId());

        return true;
    }
} TinyMovVideoDescriptionTableProcessor;

class
{
public:
    bool ProcessRead(fstream_in_wrapper& stream, TinyMovTrackMediaAudioDescription& adesc)
    {
        // reserved
        for (int i = 0; i < 6; ++i)
            stream.get_byte();

        // data reference index
        adesc.DataReferenceIndex(stream.get_be16());

        // version
        adesc.Version(stream.get_be16());

        // revision level
        adesc.RevisionLevel(stream.get_be16());

        // vendor
        adesc.Vendor(stream.get_be32());

        // number of channels
        adesc.NumberOfChannels(stream.get_be16());

        // sample size
        adesc.SampleSize(stream.get_be16());

        // compression id
        adesc.CompressionId(stream.get_be16());

        // packet size
        adesc.PacketSize(stream.get_be16());

        // sample rate (fixed-point)
        adesc.SampleRate(stream.get_be32());

        return true;
    }

    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovTrackMediaAudioDescription& adesc)
    {
        // reserved
        for (int i = 0; i < 6; ++i)
            stream.put_byte(0);

        // data reference index
        stream.put_be16(adesc.DataReferenceIndex());

        // version
        stream.put_be16(adesc.Version());

        // revision level
        stream.put_be16(adesc.RevisionLevel());

        // vendor
        stream.put_be32(adesc.Vendor());

        // number of channels
        stream.put_be16(adesc.NumberOfChannels());

        // sample size
        stream.put_be16(adesc.SampleSize());

        // compression id
        stream.put_be16(adesc.CompressionId());

        // packet size
        stream.put_be16(adesc.PacketSize());

        // sample rate (fixed-point)
        stream.put_be32(adesc.SampleRate());

        return true;
    }
} TinyMovAudioDescriptionTableProcessor;

class
{
public:
    bool ProcessRead(fstream_in_wrapper& stream, TinyMovTrackMediaTimecodeDescription& tdesc)
    {
        // reserved
        for (int i = 0; i < 6; ++i)
            stream.get_byte();

        // data reference index
        tdesc.DataReferenceIndex(stream.get_be16());

        // reserved
        stream.get_be32();

        // flags
        tdesc.Flags(stream.get_be32());

        // time scale
        tdesc.TimeScale(stream.get_be32());

        // frame duration
        tdesc.FrameDuration(stream.get_be32());

        // number of frames
        tdesc.NumberOfFrames(stream.get_byte());

        // reserved
        stream.get_byte();

        return true;
    }

    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovTrackMediaTimecodeDescription& tdesc)
    {
        // reserved
        for (int i = 0; i < 6; ++i)
            stream.put_byte(0);

        // data reference index
        stream.put_be16(tdesc.DataReferenceIndex());

        // reserved
        stream.put_be32(0);

        // flags
        stream.put_be32(tdesc.Flags());

        // time scale
        stream.put_be32(tdesc.TimeScale());

        // frame duration
        stream.put_be32(tdesc.FrameDuration());

        // number of frames
        stream.put_byte(tdesc.NumberOfFrames());

        // reserved
        stream.put_byte(0);

        return true;
    }
} TinyMovTimecodeDescriptionTableProcessor;

class : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return MKTAG('s', 't', 's', 'd');
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        if (stream.atom_parent_id() != MKTAG('s', 't', 'b', 'l'))
        {
            std::cout << "Warning! Parent is not stbl!" << std::endl;
            return false;
        }

        uint8_t version = stream.get_byte();

        uint8_t flags[3];
        flags[0] = stream.get_byte();
        flags[1] = stream.get_byte();
        flags[2] = stream.get_byte();

        uint32_t entries = stream.get_be32();

        auto& table = stream.get_current_track().Media().Info().DescriptionTable();

        for (int i = 0; i < entries; ++i)
        {
            uint32_t sz = stream.get_be32();
            uint32_t data_format = stream.get_le32();

            stream.push_atom(sz - 8, data_format);

            switch (stream.get_current_track().Media().Type())
            {
            case TinyMovTrackMedia::Type_t::Video:
                // Parse Video Description Table
                {
                    TinyMovTrackMediaVideoDescription vdesc;
                    vdesc.DataFormat(data_format);
                    TinyMovVideoDescriptionTableProcessor.ProcessRead(stream, vdesc);
                    
                    // Save extensions
                    while (stream.atom_left() > 0)
                    {
                        uint32_t ext_size = stream.get_be32();
                        uint32_t ext_tag = stream.get_le32();

                        stream.push_atom(ext_size - 8, ext_tag);

                        auto ext_sz = stream.atom_left();

                        TinyMovTrackMediaVideoDescription::Extension_t ext;
                        ext.tag = ext_tag;
                        ext.data.resize(ext_sz);
                        stream.read(ext.data.data(), ext_sz);

                        vdesc.Extensions().push_back(ext);

                        stream.pop_atom();
                    }

                    table.VideoDescriptionTable().push_back(vdesc);
                }
                break;

            case TinyMovTrackMedia::Type_t::Audio:
                // Parse Video Description Table
                {
                    TinyMovTrackMediaAudioDescription adesc;
                    adesc.DataFormat(data_format);
                    TinyMovAudioDescriptionTableProcessor.ProcessRead(stream, adesc);
                    
                    // Save extensions
                    auto ext_sz = stream.atom_left();
                    adesc.ExtensionsData().resize(ext_sz);
                    stream.read(adesc.ExtensionsData().data(), ext_sz);

                    table.AudioDescriptionTable().push_back(adesc);
                }
                break;

            case TinyMovTrackMedia::Type_t::Timecode:
                // Parse Timecode Description Table
                {
                    TinyMovTrackMediaTimecodeDescription tdesc;
                    tdesc.DataFormat(data_format);
                    TinyMovTimecodeDescriptionTableProcessor.ProcessRead(stream, tdesc);
                    
                    // Save extensions
                    auto ext_sz = stream.atom_left();
                    tdesc.ExtensionsData().resize(ext_sz);
                    stream.read(tdesc.ExtensionsData().data(), ext_sz);

                    table.TimecodeDescriptionTable().push_back(tdesc);
                }
                break;

            default:
                // Skip data
                stream.seekg(stream.atom_left());
                break;
            }

            stream.pop_atom();
        }

        return true;
    }

    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovTrack& track)
    {
        stream.push_atom(Type());

        // version
        stream.put_byte(0);

        // flags
        stream.put_byte(0);
        stream.put_byte(0);
        stream.put_byte(0);

        auto& table = track.Media().Info().DescriptionTable();

        switch (track.Media().Type())
        {
        case TinyMovTrackMedia::Type_t::Video:
            {
                // entries (count)
                stream.put_be32(table.VideoDescriptionTable().size());

                for (auto it = table.VideoDescriptionTable().begin(); it != table.VideoDescriptionTable().end(); ++it)
                {
                    auto vdesc = stream.parameters().video_description_handler(*it);
                    
                    stream.push_atom(vdesc.DataFormat());

                    // Write table
                    TinyMovVideoDescriptionTableProcessor.ProcessWrite(stream, vdesc);

                    // Write extensions
                    for (auto e_it = vdesc.Extensions().begin(); e_it != vdesc.Extensions().end(); ++e_it)
                    {
                        stream.push_atom(e_it->tag);
                        stream.write(e_it->data.data(), e_it->data.size());
                        stream.pop_atom();
                    }

                    stream.pop_atom();
                }
                break;
            }
        case TinyMovTrackMedia::Type_t::Audio:
            {
                // entries (count)
                stream.put_be32(table.AudioDescriptionTable().size());

                for (auto it = table.AudioDescriptionTable().begin(); it != table.AudioDescriptionTable().end(); ++it)
                {
                    stream.push_atom(it->DataFormat());

                    // Write table
                    TinyMovAudioDescriptionTableProcessor.ProcessWrite(stream, *it);

                    // Write extensions
                    stream.write(it->ExtensionsData().data(), it->ExtensionsData().size());

                    stream.pop_atom();
                }
                break;
            }
        case TinyMovTrackMedia::Type_t::Timecode:
            {
                // entries (count)
                stream.put_be32(table.TimecodeDescriptionTable().size());

                for (auto it = table.TimecodeDescriptionTable().begin(); it != table.TimecodeDescriptionTable().end(); ++it)
                {
                    stream.push_atom(it->DataFormat());

                    // Write table
                    TinyMovTimecodeDescriptionTableProcessor.ProcessWrite(stream, *it);

                    // Write extensions
                    stream.write(it->ExtensionsData().data(), it->ExtensionsData().size());

                    stream.pop_atom();
                }
                break;
            }
        }

        stream.pop_atom();
        return true;
    }
} TinyMovFileAtomProcessor_stsd;

class : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return MKTAG('s', 't', 't', 's');
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        if (stream.atom_parent_id() != MKTAG('s', 't', 'b', 'l'))
        {
            std::cout << "Warning! Parent is not stbl!" << std::endl;
            return false;
        }

        uint8_t version = stream.get_byte();

        uint8_t flags[3];
        flags[0] = stream.get_byte();
        flags[1] = stream.get_byte();
        flags[2] = stream.get_byte();

        uint32_t entries = stream.get_be32();
        
        for (int i = 0; i < entries; ++i)
        {
            TinyMovTrackTimeToSampleInfo::Info_t tts_info;
            tts_info.sample_count = stream.get_be32();
            tts_info.sample_duration = stream.get_be32();

            stream.get_current_track().Media().Info().TimeToSample().Entries().push_back(tts_info);
        }

        return true;
    }

    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovTrack& track)
    {
        stream.push_atom(Type());

        // version
        stream.put_byte(0);

        // flags
        stream.put_byte(0);
        stream.put_byte(0);
        stream.put_byte(0);

        // entries (count)
        stream.put_be32(track.Media().Info().TimeToSample().Entries().size());

        for (auto it = track.Media().Info().TimeToSample().Entries().begin(); it != track.Media().Info().TimeToSample().Entries().end(); ++it)
        {
            stream.put_be32(it->sample_count);
            stream.put_be32(it->sample_duration);
        }

        stream.pop_atom();
        return true;
    }
} TinyMovFileAtomProcessor_stts;

class : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return MKTAG('s', 't', 's', 'c');
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        if (stream.atom_parent_id() != MKTAG('s', 't', 'b', 'l'))
        {
            std::cout << "Warning! Parent is not stbl!" << std::endl;
            return false;
        }

        uint8_t version = stream.get_byte();

        uint8_t flags[3];
        flags[0] = stream.get_byte();
        flags[1] = stream.get_byte();
        flags[2] = stream.get_byte();

        uint32_t entries = stream.get_be32();
        
        for (int i = 0; i < entries; ++i)
        {
            TinyMovTrackSampleToChunkInfo::Info_t tts_info;
            tts_info.first_chunk = stream.get_be32();
            tts_info.samples_per_chunk = stream.get_be32();
            tts_info.sample_description_id = stream.get_be32();

            stream.get_current_track().Media().Info().SampleToChunk().Entries().push_back(tts_info);
        }

        return true;
    }

    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovTrack& track)
    {
        stream.push_atom(Type());

        // version
        stream.put_byte(0);

        // flags
        stream.put_byte(0);
        stream.put_byte(0);
        stream.put_byte(0);

        // entries (count)
        stream.put_be32(track.Media().Info().SampleToChunk().Entries().size());

        for (auto it = track.Media().Info().SampleToChunk().Entries().begin(); it != track.Media().Info().SampleToChunk().Entries().end(); ++it)
        {
            stream.put_be32(it->first_chunk);
            stream.put_be32(it->samples_per_chunk);
            stream.put_be32(it->sample_description_id);
        }

        stream.pop_atom();
        return true;
    }
} TinyMovFileAtomProcessor_stsc;

class : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return MKTAG('s', 't', 's', 'z');
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        if (stream.atom_parent_id() != MKTAG('s', 't', 'b', 'l'))
        {
            std::cout << "Warning! Parent is not stbl!" << std::endl;
            return false;
        }

        uint8_t version = stream.get_byte();

        uint8_t flags[3];
        flags[0] = stream.get_byte();
        flags[1] = stream.get_byte();
        flags[2] = stream.get_byte();

        uint32_t sample_size = stream.get_be32();
        stream.get_current_track().Media().Info().ForcedSampleSize(sample_size);

        uint32_t entries = stream.get_be32();
        if (!entries)
            return true;

        // If sample size is forced, we do not read a list
        if (sample_size)
            return true;

        for (int i = 0; i < entries; ++i)
            stream.get_current_track().Media().Info().SampleSizes().push_back(stream.get_be32());

        return true;
    }

    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovTrack& track)
    {
        stream.push_atom(Type());

        // version
        stream.put_byte(0);

        // flags
        stream.put_byte(0);
        stream.put_byte(0);
        stream.put_byte(0);

        // sample size
        stream.put_be32(track.Media().Info().ForcedSampleSize());

        // entries (count)
        stream.put_be32(track.Media().Info().SampleSizes().size());

        // We write list only if sample size is not forced before
        if (track.Media().Info().ForcedSampleSize() == 0)
            for (auto it = track.Media().Info().SampleSizes().begin(); it != track.Media().Info().SampleSizes().end(); ++it)
                stream.put_be32(*it);

        stream.pop_atom();
        return true;
    }
} TinyMovFileAtomProcessor_stsz;

class : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return MKTAG('s', 't', 'c', 'o');
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        if (stream.atom_parent_id() != MKTAG('s', 't', 'b', 'l'))
        {
            std::cout << "Warning! Parent is not stbl!" << std::endl;
            return false;
        }

        uint32_t version_and_flags = stream.get_be32();

        // Check field according to zraw extension
        uint32_t zraw_xor_base = 0x0;
        if (version_and_flags != 0)
        {
            auto& track = stream.get_current_track();
            if (track.Media().Type() == TinyMovTrackMedia::Type_t::Video)
            {
                auto& desc_table = track.Media().Info().DescriptionTable().VideoDescriptionTable();
                if (desc_table.size() == 1)
                {
                    auto ext_zraw = desc_table[0].Ext_ZRAW();
                    if (ext_zraw.exists)
                    {
                        if (ext_zraw.version == 0x45A32DEF)
                            zraw_xor_base = version_and_flags;
                    }
                }
            }
        }

        uint32_t entries = stream.get_be32() ^ zraw_xor_base;
        if (!entries)
            return true;

        for (int i = 0; i < entries; ++i)
            stream.get_current_track().Media().Info().ChunkOffsets().push_back(stream.get_be32() ^ zraw_xor_base);

        return true;
    }

    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovTrack& track)
    {
        stream.push_atom(Type());

        // version
        stream.put_byte(0);

        // flags
        stream.put_byte(0);
        stream.put_byte(0);
        stream.put_byte(0);

        // entries (count)
        stream.put_be32(track.Media().Info().ChunkOffsets().size());

        for (int i = 0; i < track.Media().Info().ChunkOffsets().size(); ++i)
        {
            uint32_t begin_sample_index = i;
            uint32_t end_sample_index = i + 1;

            if (track.Media().Info().SampleToChunk().Entries().size() > 0)
            {
                struct tmp_sample_to_chunk_info_t
                {
                    uint32_t sample_size_sum_before;
                    uint32_t begin_sample;
                    uint32_t end_sample;
                    uint32_t begin_chunk;
                    uint32_t end_chunk;
                    uint32_t samples_per_chunk;
                };
                std::vector<tmp_sample_to_chunk_info_t> tmp_stci;

                for (int g = 0; g < track.Media().Info().SampleToChunk().Entries().size(); ++g)
                {
                    auto& entry = track.Media().Info().SampleToChunk().Entries()[g];

                    tmp_sample_to_chunk_info_t tmp_info;

                    tmp_info.samples_per_chunk = entry.samples_per_chunk;

                    tmp_info.begin_chunk = entry.first_chunk - 1;
                    tmp_info.end_chunk = track.Media().Info().ChunkOffsets().size();

                    // Fix previous info
                    if (g != 0)
                    {
                        tmp_stci[g - 1].end_chunk = tmp_info.begin_chunk;
                        tmp_stci[g - 1].end_sample = tmp_stci[g - 1].begin_sample + tmp_stci[g - 1].samples_per_chunk * (tmp_stci[g - 1].end_chunk - tmp_stci[g - 1].begin_chunk);
                    }

                    tmp_info.begin_sample = (g == 0) ? 0 : tmp_stci[g - 1].end_sample;
                    tmp_info.end_sample = tmp_info.begin_sample + tmp_info.samples_per_chunk * (tmp_info.end_chunk - tmp_info.begin_chunk);

                    tmp_info.sample_size_sum_before = 0;
                    if (g != 0)
                    {
                        tmp_info.sample_size_sum_before = tmp_stci[g - 1].sample_size_sum_before;
                        for (int l = tmp_stci[g - 1].begin_sample; l < tmp_stci[g - 1].end_sample; ++l)
                        {
                            auto sample_size = (track.Media().Info().ForcedSampleSize() != 0) ?
                                track.Media().Info().ForcedSampleSize() :
                                track.Media().Info().SampleSizes()[l];

                            tmp_info.sample_size_sum_before += sample_size;
                        }
                            
                    }
                    
                    tmp_stci.push_back(tmp_info);
                }

                for (int g = 0; g < tmp_stci.size(); ++g)
                {
                    auto& tmp_stci_item = tmp_stci[g];
                    if (tmp_stci_item.begin_chunk <= i && i < tmp_stci_item.end_chunk)
                    {
                        begin_sample_index = tmp_stci_item.begin_sample + (i - tmp_stci_item.begin_chunk) * tmp_stci_item.samples_per_chunk;
                        end_sample_index = begin_sample_index + tmp_stci_item.samples_per_chunk;

                        break;
                    }
                }
            }

            // Now we need to calculate chunk size (it equals to sum of all sample sizes in this chunk)
            uint64_t chunk_size = 0;
            for (int p = begin_sample_index; p < end_sample_index; ++p)
            {
                auto sample_size = (track.Media().Info().ForcedSampleSize() != 0) ?
                    track.Media().Info().ForcedSampleSize() :
                    track.Media().Info().SampleSizes()[p];
                
                chunk_size += sample_size;
            }
            
            // Read data from the original file
            std::fstream f_in(track.Parent().Path(), std::ios::in | std::ios::binary);
            if (!f_in.is_open())
                return false;

            std::vector<uint8_t> tmp_sample_data(chunk_size);

            auto chunk_offset = track.Media().Info().ChunkOffsets()[i];
            f_in.seekg(chunk_offset);
            f_in.read((char*)tmp_sample_data.data(), chunk_size);

            f_in.close();

            // Get offset of new sample
            uint64_t offset = stream.tellp_mdat();

            // Pre-process chunk
            tmp_sample_data = stream.parameters().chunk_handler(track, chunk_offset, tmp_sample_data);

            // Write sample
            stream.write_mdat_sample(tmp_sample_data.data(), tmp_sample_data.size());

            stream.put_be32(offset);
        }

        stream.pop_atom();
        return true;
    }
} TinyMovFileAtomProcessor_stco;

class : public TinyMovFileAtomProcessorBase
{
public:
	virtual uint32_t Type()
	{
		return MKTAG('c', 'o', '6', '4');
	}
	virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
	{
		if (stream.atom_parent_id() != MKTAG('s', 't', 'b', 'l'))
		{
			std::cout << "Warning! Parent is not stbl!" << std::endl;
			return false;
		}

		uint32_t version_and_flags = stream.get_be32();

		// Check field according to zraw extension
		uint32_t zraw_xor_base = 0x0;
		if (version_and_flags != 0)
		{
			auto& track = stream.get_current_track();
			if (track.Media().Type() == TinyMovTrackMedia::Type_t::Video)
			{
				auto& desc_table = track.Media().Info().DescriptionTable().VideoDescriptionTable();
				if (desc_table.size() == 1)
				{
					auto ext_zraw = desc_table[0].Ext_ZRAW();
					if (ext_zraw.exists)
					{
						if (ext_zraw.version == 0x45A32DEF)
							zraw_xor_base = version_and_flags;
					}
				}
			}
		}

		uint32_t entries = stream.get_be32() ^ zraw_xor_base;
		if (!entries)
			return true;

		for (int i = 0; i < entries; ++i)
		{
			if (zraw_xor_base == 0)
				stream.get_current_track().Media().Info().ChunkOffsets().push_back(stream.get_be64());
			else
				stream.get_current_track().Media().Info().ChunkOffsets().push_back(stream.get_be64() ^ (0xFFFFFFFF00000000 | zraw_xor_base));
		}

		return true;
	}

	bool ProcessWrite(fstream_out_wrapper& stream, TinyMovTrack& track)
	{
		// NOT IMPLEMENTED YET!
		return false;
	}
} TinyMovFileAtomProcessor_co64;

class : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return MKTAG('s', 't', 'b', 'l');
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        if (stream.atom_parent_id() != MKTAG('m', 'i', 'n', 'f'))
        {
            std::cout << "Warning! Parent is not minf!" << std::endl;
            return false;
        }

        return TinyMovFileAtomProcessor_default.ProcessRead(stream, mov);
    }
    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovTrack& track)
    {
        stream.push_atom(Type());

        // Add 'stsd' atom
        if (!TinyMovFileAtomProcessor_stsd.ProcessWrite(stream, track))
            return false;

        // Add 'stts' atom
        if (!TinyMovFileAtomProcessor_stts.ProcessWrite(stream, track))
            return false;

        // Add 'stsc' atom
        if (!TinyMovFileAtomProcessor_stsc.ProcessWrite(stream, track))
            return false;
        
        // Add 'stsz' atom
        if (!TinyMovFileAtomProcessor_stsz.ProcessWrite(stream, track))
            return false;
        
        // Add 'stco' atom
        if (!TinyMovFileAtomProcessor_stco.ProcessWrite(stream, track))
            return false;

        stream.pop_atom();
        return true;
    }
} TinyMovFileAtomProcessor_stbl;

class : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return MKTAG('h', 'd', 'l', 'r');
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        if (stream.atom_parent_id() != MKTAG('m', 'e', 't', 'a') &&
            stream.atom_parent_id() != MKTAG('m', 'd', 'i', 'a') &&
            stream.atom_parent_id() != MKTAG('m', 'i', 'n', 'f'))
        {
            std::cout << "Warning! Parent is not meta or mdia!" << std::endl;
            return false;
        }

        TinyMovHandlerInfo* hdlr = nullptr;
        switch (stream.atom_parent_id())
        {
        case MKTAG('m', 'e', 't', 'a'):
            hdlr = &mov.Metadata().Handler();
            break;

        case MKTAG('m', 'd', 'i', 'a'):
            hdlr = &stream.get_current_track().Media().Handler();
            break;

        case MKTAG('m', 'i', 'n', 'f'):
            hdlr = &stream.get_current_track().Media().Info().Handler();
            break;

        default:
            std::cout << "Warning! Parent is not meta or mdia or minf!" << std::endl;
            return false;
        }

        // version
        stream.get_byte();

        // flags
        stream.get_byte();
        stream.get_byte();
        stream.get_byte();

        // component type
        hdlr->ComponentType(stream.get_be32());

        // component sub-type
        hdlr->ComponentSubType(stream.get_le32());

        // component manufacturer
        hdlr->ComponentManufacturer(stream.get_be32());

        // component flags
        hdlr->ComponentFlags(stream.get_be32());

        // component flags mask
        hdlr->ComponentFlagsMask(stream.get_be32());

        // component name
        auto name_len = stream.get_byte();
        auto ptr = new char[name_len + 1];
        stream.read(ptr, name_len);
        ptr[name_len] = '\0';
        hdlr->ComponentName(std::string(ptr));
        delete[] ptr;

        hdlr->Enabled(true);

        return true;
    }
    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovHandlerInfo& hdlr)
    {
        stream.push_atom(Type());
        
        // version
        stream.put_byte(0);

        // flags
        stream.put_byte(0);
        stream.put_byte(0);
        stream.put_byte(0);

        // component type
        stream.put_be32(hdlr.ComponentType());

        // component sub-type
        stream.put_le32(hdlr.ComponentSubType());

        // component manufacturer
        stream.put_be32(hdlr.ComponentManufacturer());

        // component flags
        stream.put_be32(hdlr.ComponentFlags());

        // component flags mask
        stream.put_be32(hdlr.ComponentFlagsMask());

        // component name
        stream.put_byte(hdlr.ComponentName().size());
        stream.write((char*)hdlr.ComponentName().c_str(), hdlr.ComponentName().size());

        stream.pop_atom();
        return true;
    }
} TinyMovFileAtomProcessor_hdlr;

class : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return MKTAG('d', 'r', 'e', 'f');
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        TinyMovTrackDataInfo& dinf = stream.get_current_track().Media().Info().DataInfo();

        // version
        stream.get_byte();

        // flags
        stream.get_byte();
        stream.get_byte();
        stream.get_byte();

        // entries
        auto entries = stream.get_be32();

        for (int i = 0; i < entries; ++i)
        {
            int64_t size = stream.get_be32();
            uint32_t type = stream.get_le32();

            stream.push_atom(size - 8, type);

            TinyMovTrackDataInfo::DataReferenceEntry_t entry;
            entry.type = type;
            entry.version_and_flags = stream.get_be32();

            auto sz = stream.atom_left();
            entry.data.resize(sz);
            stream.read(entry.data.data(), sz);

            dinf.Entries().push_back(entry);

            stream.pop_atom();
        }

        return true;
    }
    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovTrackDataInfo& dinf)
    {
        stream.push_atom(Type());

        // version
        stream.put_byte(0);

        // flags
        stream.put_byte(0);
        stream.put_byte(0);
        stream.put_byte(0);

        // entries
        stream.put_be32(dinf.Entries().size());

        for (auto it = dinf.Entries().begin(); it != dinf.Entries().end(); ++it)
        {
            stream.push_atom(it->type);

            // version and flags
            stream.put_be32(it->version_and_flags);

            // data
            stream.write(it->data.data(), it->data.size());

            stream.pop_atom();
        }

        stream.pop_atom();
        return true;
    }
} TinyMovFileAtomProcessor_dref;

class : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return MKTAG('d', 'i', 'n', 'f');
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        return TinyMovFileAtomProcessor_default.ProcessRead(stream, mov);
    }
    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovTrackDataInfo& dinf)
    {
        stream.push_atom(Type());
        auto res = TinyMovFileAtomProcessor_dref.ProcessWrite(stream, dinf);
        stream.pop_atom();
        return res;
    }
} TinyMovFileAtomProcessor_dinf;

class : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return MKTAG('v', 'm', 'h', 'd');
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        // version
        stream.get_byte();

        // flags
        stream.get_byte();
        stream.get_byte();
        stream.get_byte();

        // graphics mode
        stream.get_be16();

        // opcolor
        stream.get_be16();
        stream.get_be16();
        stream.get_be16();

        return true;
    }
    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovTrack& track)
    {
        stream.push_atom(Type());
        
        // version
        stream.put_byte(0);

        // flags
        stream.put_byte(0);
        stream.put_byte(0);
        stream.put_byte(0);

        // graphics mode
        stream.put_be16(0);

        // opcolor
        stream.put_be16(0);
        stream.put_be16(0);
        stream.put_be16(0);
        
        stream.pop_atom();
        return true;
    }
} TinyMovFileAtomProcessor_vmhd;

class : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return MKTAG('s', 'm', 'h', 'd');
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        // version
        stream.get_byte();

        // flags
        stream.get_byte();
        stream.get_byte();
        stream.get_byte();

        // balance
        stream.get_be16();

        // reserved
        stream.get_be16();

        return true;
    }
    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovTrack& track)
    {
        stream.push_atom(Type());
        
        // version
        stream.put_byte(0);

        // flags
        stream.put_byte(0);
        stream.put_byte(0);
        stream.put_byte(0);

        // balance
        stream.put_be16(0);

        // reserved
        stream.put_be16(0);
        
        stream.pop_atom();
        return true;
    }
} TinyMovFileAtomProcessor_smhd;

class : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return MKTAG('g', 'm', 'i', 'n');
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        // version
        stream.get_byte();

        // flags
        stream.get_byte();
        stream.get_byte();
        stream.get_byte();

        // graphics mode
        stream.get_be16();

        // opcolor
        stream.get_be16();
        stream.get_be16();
        stream.get_be16();

        // balance
        stream.get_be16();

        // reserved
        stream.get_be16();

        return true;
    }
    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovTrack& track)
    {
        stream.push_atom(Type());
        
        // version
        stream.put_byte(0);

        // flags
        stream.put_byte(0);
        stream.put_byte(0);
        stream.put_byte(0);

        // graphics mode
        stream.put_be16(64);

        // opcolor
        stream.put_be16(32768);
        stream.put_be16(32768);
        stream.put_be16(32768);

        // balance
        stream.put_be16(0);

        // reserved
        stream.put_be16(0);
        
        stream.pop_atom();
        return true;
    }
} TinyMovFileAtomProcessor_gmin;

class : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return MKTAG('t', 'c', 'm', 'i');
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        // version
        stream.get_byte();

        // flags
        stream.get_byte();
        stream.get_byte();
        stream.get_byte();

        // text font
        stream.get_be16();

        // text face
        stream.get_be16();

        // text size
        stream.get_be16();

        // text color
        stream.get_be16();
        stream.get_be16();
        stream.get_be16();

        // background color
        stream.get_be16();
        stream.get_be16();
        stream.get_be16();

        // reserved
        stream.get_be16();

        // font name size
        auto fns = stream.get_byte();
        stream.seekg(fns);

        return true;
    }
    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovTrack& track)
    {
        stream.push_atom(Type());
        
        // version
        stream.put_byte(0);

        // flags
        stream.put_byte(0);
        stream.put_byte(0);
        stream.put_byte(0);

        // text font
        stream.put_be16(0);

        // text face
        stream.put_be16(0);

        // text size
        stream.put_be16(12);

        // text color
        stream.put_be16(0x0000);
        stream.put_be16(0xFFFF);
        stream.put_be16(0xFFFF);

        // background color
        stream.put_be16(0xFFFF);
        stream.put_be16(0x0000);
        stream.put_be16(0x0000);

        // reserved
        stream.put_be16(0);

        std::string font_name = "System";

        // font name size
        stream.put_byte(font_name.size());

        // font name
        stream.write((char*)font_name.c_str(), font_name.size());
        
        stream.pop_atom();
        return true;
    }
} TinyMovFileAtomProcessor_tcmi;

class : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return MKTAG('t', 'm', 'c', 'd');
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        return TinyMovFileAtomProcessor_default.ProcessRead(stream, mov);
    }
    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovTrack& track)
    {
        stream.push_atom(Type());

        if (!TinyMovFileAtomProcessor_tcmi.ProcessWrite(stream, track))
            return false;
        
        stream.pop_atom();
        return true;
    }
} TinyMovFileAtomProcessor_tmcd;

class : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return MKTAG('g', 'm', 'h', 'd');
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        return TinyMovFileAtomProcessor_default.ProcessRead(stream, mov);
    }
    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovTrack& track)
    {
        stream.push_atom(Type());
        
        if (!TinyMovFileAtomProcessor_gmin.ProcessWrite(stream, track))
            return false;

        if (!TinyMovFileAtomProcessor_tmcd.ProcessWrite(stream, track))
            return false;
        
        stream.pop_atom();
        return true;
    }
} TinyMovFileAtomProcessor_gmhd;

class : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return MKTAG('m', 'i', 'n', 'f');
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        if (stream.atom_parent_id() != MKTAG('m', 'd', 'i', 'a'))
        {
            std::cout << "Warning! Parent is not mdia!" << std::endl;
            return false;
        }

        return TinyMovFileAtomProcessor_default.ProcessRead(stream, mov);
    }
    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovTrack& track)
    {
        stream.push_atom(Type());
        
        switch (track.Media().Type())
        {
        case TinyMovTrackMedia::Type_t::Video:
            if (!TinyMovFileAtomProcessor_vmhd.ProcessWrite(stream, track))
                return false;
            break;

        case TinyMovTrackMedia::Type_t::Audio:
            if (!TinyMovFileAtomProcessor_smhd.ProcessWrite(stream, track))
                return false;
            break;

        case TinyMovTrackMedia::Type_t::Timecode:
            if (!TinyMovFileAtomProcessor_gmhd.ProcessWrite(stream, track))
                return false;
            break;

        default:
            break;
        }

        // Add 'hdlr' atom
        if (track.Media().Info().Handler().Enabled())
            if (!TinyMovFileAtomProcessor_hdlr.ProcessWrite(stream, track.Media().Info().Handler()))
                return false;
        
        // Add 'dinf' atom
        if (!TinyMovFileAtomProcessor_dinf.ProcessWrite(stream, track.Media().Info().DataInfo()))
            return false;

        // Add 'stbl' atom
        if (!TinyMovFileAtomProcessor_stbl.ProcessWrite(stream, track))
            return false;
        
        stream.pop_atom();
        return true;
    }
} TinyMovFileAtomProcessor_minf;

class : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return MKTAG('m', 'd', 'i', 'a');
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        if (stream.atom_parent_id() != MKTAG('t', 'r', 'a', 'k'))
        {
            std::cout << "Warning! Parent is not trak!" << std::endl;
            return false;
        }

        return TinyMovFileAtomProcessor_default.ProcessRead(stream, mov);
    }

    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovTrack& track)
    {
        stream.push_atom(Type());

        // Add 'mdhd' atom
        if (!TinyMovFileAtomProcessor_mdhd.ProcessWrite(stream, track))
            return false;

        // Add 'hdlr' atom
        if (track.Media().Handler().Enabled())
            if (!TinyMovFileAtomProcessor_hdlr.ProcessWrite(stream, track.Media().Handler()))
                return false;

        // Add 'minf' atom
        if (!TinyMovFileAtomProcessor_minf.ProcessWrite(stream, track))
            return false;

        stream.pop_atom();
        return true;
    }
} TinyMovFileAtomProcessor_mdia;

class : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return MKTAG('t', 'r', 'a', 'k');
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        if (stream.atom_parent_id() != MKTAG('m', 'o', 'o', 'v'))
        {
            std::cout << "Warning! Parent is not moov!" << std::endl;
            return false;
        }

        TinyMovTrack track(mov);

        stream.set_current_track(&track);
        auto result = TinyMovFileAtomProcessor_default.ProcessRead(stream, mov);
        stream.set_current_track(nullptr);

        mov.Tracks().push_back(track);

        return result;
    }

    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovTrack& track)
    {
        stream.push_atom(Type());

        // Add 'tkhd' atom
        if (!TinyMovFileAtomProcessor_tkhd.ProcessWrite(stream, track))
            return false;

        // Add 'mdia' atom
        if (!TinyMovFileAtomProcessor_mdia.ProcessWrite(stream, track))
            return false;
        
        stream.pop_atom();
        return true;
    }
} TinyMovFileAtomProcessor_trak;

class : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return MKTAG('k', 'e', 'y', 's');
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        if (stream.atom_parent_id() != MKTAG('m', 'e', 't', 'a'))
        {
            std::cout << "Warning! Parent is not meta!" << std::endl;
            return false;
        }

        stream.seekg(4);    // Skip

        uint32_t count = stream.get_be32();

        for (int i = 0; i < count; ++i)
        {
            auto sz = stream.get_be32() - 8;

            auto ns = stream.get_be32();

            auto str_text = new char[sz];
            stream.read(str_text, sz);
            std::vector<uint8_t> text_data(str_text, str_text + sz);

            TinyMovMetadataKey key;
            key.Data(text_data);
            key.Namespace(ns);

            mov.Metadata().Keys().push_back(key);

            delete[] str_text;
        }

        return TinyMovFileAtomProcessor_default.ProcessRead(stream, mov);
    }
    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovFile& mov)
    {
        stream.push_atom(Type());

        // version
        stream.put_byte(0);

        // flags
        stream.put_byte(0);
        stream.put_byte(0);
        stream.put_byte(0);

        // count
        stream.put_be32(mov.Metadata().Keys().size());
        
        // keys
        for (auto it = mov.Metadata().Keys().begin(); it != mov.Metadata().Keys().end(); ++it)
        {
            stream.put_be32(it->Data().size() + 8);
            stream.put_be32(it->Namespace());
            stream.write((char*)it->Data().data(), it->Data().size());
        }

        stream.pop_atom();
        return true;
    }
} TinyMovFileAtomProcessor_keys;

class : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return MKTAG('d', 'a', 't', 'a');
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        if (stream.atom_parent_id() != MKTAG('i', 'l', 's', 't'))
        {
            std::cout << "Warning! Parent is not ilst!" << std::endl;
            return false;
        }
        
        uint32_t type_indicator = stream.get_be32();
        if (type_indicator != 1)
            throw std::exception();     // FIXME: make support for each data type!

        uint32_t locale_indicator = stream.get_be32();
        uint32_t size = stream.atom_left();

        uint8_t* text = new uint8_t[size];
        stream.read((char*)text, size);
        std::vector<uint8_t> text_data(text, text + size);

        TinyMovMetadataValue val;
        val.Type(type_indicator);
        val.Locale(locale_indicator);
        val.Data(text_data);

        delete[] text;

        mov.Metadata().Values().push_back(val);

        return TinyMovFileAtomProcessor_default.ProcessRead(stream, mov);
    }
    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovMetadataValue& value)
    {
        stream.push_atom(Type());
        
        stream.put_be32(value.Type());
        stream.put_be32(value.Locale());
        stream.write((char*)value.Data().data(), value.Data().size());

        stream.pop_atom();
        return true;
    }
} TinyMovFileAtomProcessor_data;

class : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return MKTAG('i', 'l', 's', 't');
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        if (stream.atom_parent_id() != MKTAG('m', 'e', 't', 'a'))
        {
            std::cout << "Warning! Parent is not meta!" << std::endl;
            return false;
        }

        while (stream.atom_left() > 0)
        {
            uint32_t len = stream.get_be32();
            uint32_t idx = stream.get_be32();

            stream.push_atom(len - 8, Type());

            if(!TinyMovFileAtomProcessor_default.ProcessRead(stream, mov))
                return false;

            stream.pop_atom();
        }

        return TinyMovFileAtomProcessor_default.ProcessRead(stream, mov);
    }
    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovFile& mov)
    {
        stream.push_atom(Type());

        auto& values = mov.Metadata().Values();

        for (int i = 0; i < values.size(); ++i)
        {
            stream.push_atom(bswap_32(i + 1));

            if(!TinyMovFileAtomProcessor_data.ProcessWrite(stream, values[i]))
                return false;

            stream.pop_atom();
        }

        stream.pop_atom();
        return true;
    }
} TinyMovFileAtomProcessor_ilst;

class : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return MKTAG('m', 'e', 't', 'a');
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        if (stream.atom_parent_id() != MKTAG('m', 'o', 'o', 'v'))
        {
            std::cout << "Warning! Parent is not moov!" << std::endl;
            return false;
        }
        
        return TinyMovFileAtomProcessor_default.ProcessRead(stream, mov);
    }
    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovFile& mov)
    {
        stream.push_atom(Type());

        // Add 'hdlr' atom
        if (mov.Metadata().Handler().Enabled())
            if (!TinyMovFileAtomProcessor_hdlr.ProcessWrite(stream, mov.Metadata().Handler()))
                return false;
            
        // Add 'keys' atom
        if (!TinyMovFileAtomProcessor_keys.ProcessWrite(stream, mov))
            return false;

        // Add 'ilst' atom
        if (!TinyMovFileAtomProcessor_ilst.ProcessWrite(stream, mov))
            return false;

        stream.pop_atom();
        return true;
    }
} TinyMovFileAtomProcessor_meta;

class : public TinyMovFileAtomProcessorBase
{
public:
    virtual uint32_t Type()
    {
        return MKTAG('m', 'o', 'o', 'v');
    }
    virtual bool ProcessRead(fstream_in_wrapper& stream, TinyMovFile& mov)
    {
        if (stream.atom_parent_id() != MKTAG('R', 'O', 'O', 'T'))
        {
            std::cout << "Warning! Parent is not ROOT!" << std::endl;
            return false;
        }

        return TinyMovFileAtomProcessor_default.ProcessRead(stream, mov);
    }
    bool ProcessWrite(fstream_out_wrapper& stream, TinyMovFile& mov)
    {
        stream.push_atom(Type());

        // Add 'mvhd' atom
        if (!TinyMovFileAtomProcessor_mvhd.ProcessWrite(stream, mov))
            return false;

        auto& tracks = mov.Tracks();
        for (int i = 0; i < tracks.size(); ++i)
        {
            // Add 'trak' atom
            if (!TinyMovFileAtomProcessor_trak.ProcessWrite(stream, tracks[i]))
                return false;
        }

        // Add 'meta' atom
        if (!TinyMovFileAtomProcessor_meta.ProcessWrite(stream, mov))
            return false;

        stream.pop_atom();
        return true;
    }
} TinyMovFileAtomProcessor_moov;

class TinyMovFileReader
{
public:
    TinyMovFile OpenMovFile(std::string path)
    {
        TinyMovFile mov;
        mov.Path(path);

        std::fstream f_in(path, std::ios::in | std::ios::binary);
        
        auto fsize = f_in.tellg();
        f_in.seekg(0, std::ios::end);
        fsize = f_in.tellg() - fsize;
        f_in.seekg(0, std::ios::beg);

        fstream_in_wrapper f_in_wrapper(f_in, fsize);
        TinyMovFileAtomProcessor_default.ProcessRead(f_in_wrapper, mov);

        return mov;
    }
};

class TinyMovFileWriter
{
public:
    bool SaveMovFile(TinyMovFile& mov, std::string path)
    {
        std::fstream f_out(path, std::ios::out | std::ios::binary);
        if (!f_out.is_open())
            return false;

        fstream_out_wrapper f_out_wrapper(f_out, _parameters);

        // Add 'ftyp' atom
        if (!TinyMovFileAtomProcessor_ftyp.ProcessWrite(f_out_wrapper, mov))
        {
            f_out_wrapper.finalize_file();
            return false;
        }

        // Add 'moov' atom
        if (!TinyMovFileAtomProcessor_moov.ProcessWrite(f_out_wrapper, mov))
        {
            f_out_wrapper.finalize_file();
            return false;
        }

        // ('mdat' is written automatically)
        f_out_wrapper.finalize_file();

        return true;
    }

    void ChunkHandler(std::function<std::vector<uint8_t> (TinyMovTrack& track, uint64_t chunk_offset, std::vector<uint8_t>& chunk_data)> handler)
    {
        _parameters.chunk_handler = handler;
    }
    std::function<std::vector<uint8_t> (TinyMovTrack& track, uint64_t chunk_offset, std::vector<uint8_t>& chunk_data)> ChunkHandler()
    {
        return _parameters.chunk_handler;
    }

    void VideoDescriptionHandler(std::function<TinyMovTrackMediaVideoDescription (TinyMovTrackMediaVideoDescription)> handler)
    {
        _parameters.video_description_handler = handler;
    }
    std::function<TinyMovTrackMediaVideoDescription (TinyMovTrackMediaVideoDescription)> VideoDescriptionHandler()
    {
        return _parameters.video_description_handler;
    }

protected:
    TinyMovFileWriterParameters _parameters;
};
