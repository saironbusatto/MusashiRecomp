/* test_cue_multifile — regression guard for multi-FILE .cue parsing.
 *
 * Bug this pins down: the cue parser let every FILE line overwrite the
 * mounted image, so a "one .bin per track" rip (data track + CD-DA tracks,
 * the common rip layout) mounted the LAST audio track as the data disc.
 * The BIOS then could not Load() the boot EXE and panicked with
 * SystemHalt(906) — observed on Brave Fencer Musashi (SLUS-00726).
 *
 * Also pins the absolute-LBA rule: in a multi-FILE cue every INDEX time is
 * relative to its own file, so track starts must accumulate the sector
 * counts of the preceding files. Single-FILE cues (INDEX already absolute)
 * must keep behaving exactly as before.
 *
 * Build/run standalone:
 *   g++ -std=c++17 -I runtime/include runtime/tests/test_cue_multifile.cpp \
 *       runtime/src/iso_reader.cpp -o /tmp/test_cue && /tmp/test_cue
 */
#include "iso_reader.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static constexpr size_t RAW_SECTOR = 2352;

/* Write `sectors` raw sectors of filler so file_size()/2352 is exact. */
static void write_bin(const fs::path& p, size_t sectors)
{
    std::ofstream f(p, std::ios::binary);
    std::vector<char> sector(RAW_SECTOR, 0);
    for (size_t i = 0; i < sectors; i++)
        f.write(sector.data(), RAW_SECTOR);
}

static void write_text(const fs::path& p, const std::string& s)
{
    std::ofstream f(p);
    f << s;
}

int main()
{
    fs::path dir = fs::temp_directory_path() / "psxrecomp_cue_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    /* ---- multi-FILE cue: data track + two audio tracks, own .bin each ---- */
    const size_t t1_sectors = 100;
    const size_t t2_sectors = 50;
    write_bin(dir / "game (Track 1).bin", t1_sectors);
    write_bin(dir / "game (Track 2).bin", t2_sectors);
    write_bin(dir / "game (Track 3).bin", 30);

    fs::path multi_cue = dir / "game.cue";
    write_text(multi_cue,
        "FILE \"game (Track 1).bin\" BINARY\n"
        "  TRACK 01 MODE2/2352\n"
        "    INDEX 01 00:00:00\n"
        "FILE \"game (Track 2).bin\" BINARY\n"
        "  TRACK 02 AUDIO\n"
        "    INDEX 00 00:00:00\n"
        "    INDEX 01 00:02:00\n"   /* 2s = 150 sectors, file-relative */
        "FILE \"game (Track 3).bin\" BINARY\n"
        "  TRACK 03 AUDIO\n"
        "    INDEX 01 00:00:00\n");

    PS1::ISOReader multi;
    assert(multi.Open(multi_cue.string()) && "multi-FILE cue must mount");

    /* The mounted image is the FIRST file (the data track), never a later
     * audio track — this is the actual SystemHalt(906) bug. */
    assert(fs::path(multi.GetBinPath()).filename().string() == "game (Track 1).bin");

    assert(multi.TrackCount() == 3);
    /* Track 1 starts the disc. */
    assert(multi.TrackStartLBA(1) == 0);
    /* Track 2: file 1's sectors + its own 150-sector INDEX offset. */
    assert(multi.TrackStartLBA(2) == t1_sectors + 150);
    /* Track 3: files 1+2 accumulated, INDEX 0 into its own file. */
    assert(multi.TrackStartLBA(3) == t1_sectors + t2_sectors);

    /* ---- single-FILE cue: INDEX times are already absolute ---- */
    write_bin(dir / "single.bin", 200);
    fs::path single_cue = dir / "single.cue";
    write_text(single_cue,
        "FILE \"single.bin\" BINARY\n"
        "  TRACK 01 MODE2/2352\n"
        "    INDEX 01 00:00:00\n"
        "  TRACK 02 AUDIO\n"
        "    INDEX 01 00:02:00\n");

    PS1::ISOReader single;
    assert(single.Open(single_cue.string()) && "single-FILE cue must mount");
    assert(fs::path(single.GetBinPath()).filename().string() == "single.bin");
    assert(single.TrackCount() == 2);
    assert(single.TrackStartLBA(1) == 0);
    /* No accumulation: one file, so the absolute INDEX passes through. */
    assert(single.TrackStartLBA(2) == 150);

    fs::remove_all(dir);
    std::printf("test_cue_multifile: OK\n");
    return 0;
}
