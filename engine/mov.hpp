// Finds the ZRAW video track in a Z CAM .MOV and lists where each frame lives.
// Same track selection as zraw-parser's process_zraw_old_raw.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <TinyMovFileLibrary.hpp>

struct ZrawTrack
{
    int track_index = -1;
    uint32_t version = 0;               // 0x12EA78D2 = true RAW (CFA), 0x45A32DEF = encrypted HEVC
    uint32_t scheme = 0;                // HEVC only: the 'zraw' box's third word, 1 on the clips we can unscramble
    std::vector<uint64_t> offsets, sizes;
    uint32_t timescale = 0, frame_duration = 0;   // fps = timescale / frame_duration (30000/1001 on the E2)

    double fps() const { return frame_duration ? (double)timescale / frame_duration : 0; }
};

// Copies what it needs, so `mov` need not outlive the returned track.
inline ZrawTrack FindZrawTrack(TinyMovFile& mov)
{
    ZrawTrack z;
    for (int i = 0; i < (int)mov.Tracks().size(); ++i)
    {
        auto& track = mov.Tracks()[i];
        if (track.Media().Type() != TinyMovTrackMedia::Type_t::Video) continue;
        auto& descs = track.Media().Info().DescriptionTable().VideoDescriptionTable();
        if (descs.size() != 1 || descs[0].DataFormat() != MKTAG('z', 'r', 'a', 'w')) continue;
        auto ext = descs[0].Ext_ZRAW();
        if (!ext.exists) continue;
        z.track_index = i;
        z.version = ext.version;
        z.scheme = ext.unk1;
        z.offsets = track.Media().Info().ChunkOffsets();
        z.sizes = track.Media().Info().SampleSizes();
        z.timescale = track.Media().TimeScale();
        auto& stts = track.Media().Info().TimeToSample().Entries();
        if (!stts.empty()) z.frame_duration = stts[0].sample_duration;
        break;
    }
    return z;
}
