/*
 * Copyright (C) 2011, British Broadcasting Corporation
 * All Rights Reserved.
 *
 * Author: Philip de Nier
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the British Broadcasting Corporation nor the names
 *       of its contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cstdio>

#include <algorithm>

#include <libMXF++/MXF.h>

#include <mxf/mxf_avid.h>

#include <bmx/avid_mxf/AvidClip.h>
#include "AvidRGBColors.h"
#include <bmx/MXFUtils.h>
#include <bmx/Utils.h>
#include <bmx/Version.h>
#include <bmx/BMXException.h>
#include <bmx/Logging.h>

using namespace std;
using namespace bmx;
using namespace mxfpp;


// max locators limited by number of string references in a strong reference vector ((2^16 - 1) / 16)
#define MAX_LOCATORS    4095


static uint32_t DM_TRACK_ID     = 1000;
static uint32_t DM_TRACK_NUMBER = 1;



static bool compare_track(const AvidTrack *left, const AvidTrack *right)
{
    return (left->IsPicture() && !right->IsPicture()) ||
           (left->IsPicture() && left->GetTrackIndex() < right->GetTrackIndex());
}

static bool compare_locator(const AvidLocator &left, const AvidLocator &right)
{
    return left.position < right.position;
}



AvidClip::AvidClip(mxfRational frame_rate, MXFFileFactory *file_factory, bool take_factory_ownership,
                   string filename_prefix)
{
    BMX_CHECK(frame_rate == FRAME_RATE_23976 ||
              frame_rate == FRAME_RATE_24 ||
              frame_rate == FRAME_RATE_25 ||
              frame_rate == FRAME_RATE_2997 ||
              frame_rate == FRAME_RATE_30 ||
              frame_rate == FRAME_RATE_50 ||
              frame_rate == FRAME_RATE_5994 ||
              frame_rate == FRAME_RATE_60);

    mFileFactory = file_factory;
    mOwnFileFactory = take_factory_ownership;
    mClipFrameRate = frame_rate;
    mFilenamePrefix = filename_prefix;
    mStartTimecode = Timecode(frame_rate, false);
    mStartTimecodeSet = false;
    mCompanyName = get_bmx_company_name();
    mProductName = get_bmx_library_name();
    mProductVersion = get_bmx_mxf_product_version();
    mVersionString = get_bmx_mxf_version_string();
    mProductUID = get_bmx_product_uid();
    mMaxLocatorsExceeded = false;
    mxf_get_timestamp_now(&mCreationDate);
    mxf_generate_uuid(&mGenerationUID);
    mxf_generate_aafsdk_umid(&mMaterialPackageUID);
    mMaterialPackageCreationDate = mCreationDate;
    mMaterialPackageCreationDateSet = false;
    mDataModel = 0;
    mHeaderMetadata = 0;
    mContentStorage = 0;
    mMaterialPackage = 0;
    mPhysicalSourcePackage = 0;
    mHavePhysSourceTimecodeTrack = false;
    mMaterialTimecodeComponent = 0;
    mLocatorDescribedTrackId = 0;

    CreateMinimalHeaderMetadata();
}

AvidClip::~AvidClip()
{
    if (mOwnFileFactory)
        delete mFileFactory;

    delete mHeaderMetadata;
    delete mDataModel;

    size_t i;
    for (i = 0; i < mTracks.size(); i++)
        delete mTracks[i];
}

void AvidClip::SetProjectName(string name)
{
    mProjectName = name;
}

void AvidClip::SetClipName(string name)
{
    mClipName = name;
}

void AvidClip::SetStartTimecode(Timecode start_timecode)
{
    mStartTimecode = start_timecode;
    mStartTimecodeSet = true;
}

void AvidClip::SetProductInfo(string company_name, string product_name, mxfProductVersion product_version,
                              string version, mxfUUID product_uid)
{
    mCompanyName = company_name;
    mProductName = product_name;
    mProductVersion = product_version;
    mVersionString = version;
    mProductUID = product_uid;
}

void AvidClip::SetCreationDate(mxfTimestamp creation_date)
{
    mCreationDate = creation_date;
    if (!mMaterialPackageCreationDateSet)
        mMaterialPackageCreationDate = creation_date;
}

void AvidClip::SetGenerationUID(mxfUUID generation_uid)
{
    mGenerationUID = generation_uid;
}

void AvidClip::SetMaterialPackageUID(mxfUMID package_uid)
{
    mMaterialPackageUID = package_uid;
}

void AvidClip::SetMaterialPackageCreationDate(mxfTimestamp creation_date)
{
    mMaterialPackageCreationDate = creation_date;
    mMaterialPackageCreationDateSet = true;
}

void AvidClip::SetUserComment(string name, string value)
{
    mUserComments[name] = value;
}

void AvidClip::AddLocator(AvidLocator locator)
{
    if (!mMaxLocatorsExceeded) {
        if (mLocators.size() == MAX_LOCATORS) {
            log_warn("Maximum locators, %u, exceeded.\n", MAX_LOCATORS);
            mMaxLocatorsExceeded = true;
        } else {
            mLocators.push_back(locator);
        }
    }
}

SourcePackage* AvidClip::CreateDefaultTapeSource(string name, uint32_t num_video_tracks, uint32_t num_audio_tracks)
{
    mxfUMID tape_package_uid;
    mxf_generate_aafsdk_umid(&tape_package_uid);
    int64_t tape_duration = 120 * 60 * 60 * get_rounded_tc_base(mClipFrameRate);

    // Preface - ContentStorage - tape SourcePackage
    SourcePackage *tape_package = new SourcePackage(mHeaderMetadata);
    mContentStorage->appendPackages(tape_package);
    tape_package->setPackageUID(tape_package_uid);
    tape_package->setPackageCreationDate(mCreationDate);
    tape_package->setPackageModifiedDate(mCreationDate);
    if (!name.empty())
        tape_package->setName(name);
    if (!mProjectName.empty())
        tape_package->appendAvidAttribute("_PJ", mProjectName);

    uint32_t track_id = 1;
    uint32_t video_track_number = 1, audio_track_number = 1;
    uint32_t i;
    for (i = 0; i < num_video_tracks + num_audio_tracks; i++) {
        bool is_video = (i < num_video_tracks);

        // Preface - ContentStorage - tape SourcePackage - Timeline Track
        Track *track = new Track(mHeaderMetadata);
        tape_package->appendTracks(track);
        track->setTrackID(track_id);
        track->setTrackName(get_track_name(is_video, (is_video ? video_track_number : audio_track_number)));
        track->setTrackNumber(is_video ? video_track_number : audio_track_number);
        track->setEditRate(mClipFrameRate);
        track->setOrigin(0);

        // Preface - ContentStorage - tape SourcePackage - Timeline Track - Sequence
        Sequence *sequence = new Sequence(mHeaderMetadata);
        track->setSequence(sequence);
        sequence->setDataDefinition(is_video ? MXF_DDEF_L(Picture) : MXF_DDEF_L(Sound));
        sequence->setDuration(tape_duration);

        // Preface - ContentStorage - tape SourcePackage - Timeline Track - Sequence - SourceClip
        SourceClip *source_clip = new SourceClip(mHeaderMetadata);
        sequence->appendStructuralComponents(source_clip);
        source_clip->setDataDefinition(is_video ? MXF_DDEF_L(Picture) : MXF_DDEF_L(Sound));
        source_clip->setDuration(tape_duration);
        source_clip->setStartPosition(0);
        source_clip->setSourcePackageID(g_Null_UMID);
        source_clip->setSourceTrackID(0);

        if (is_video)
            video_track_number++;
        else
            audio_track_number++;

        track_id++;
    }

    // Preface - ContentStorage - tape SourcePackage - timecode Timeline Track
    Track *tc_track = new Track(mHeaderMetadata);
    tape_package->appendTracks(tc_track);
    tc_track->setTrackName("TC1");
    tc_track->setTrackID(track_id);
    tc_track->setTrackNumber(1);
    tc_track->setEditRate(mClipFrameRate);
    tc_track->setOrigin(0);

    // Preface - ContentStorage - tape SourcePackage - timecode Timeline Track - Sequence
    Sequence *sequence = new Sequence(mHeaderMetadata);
    tc_track->setSequence(sequence);
    sequence->setDataDefinition(MXF_DDEF_L(Timecode));
    sequence->setDuration(tape_duration);

    // Preface - ContentStorage - tape SourcePackage - Timecode Track - TimecodeComponent
    TimecodeComponent *tc_component = new TimecodeComponent(mHeaderMetadata);
    sequence->appendStructuralComponents(tc_component);
    tc_component->setDataDefinition(MXF_DDEF_L(Timecode));
    tc_component->setDuration(tape_duration);
    tc_component->setRoundedTimecodeBase(get_rounded_tc_base(mClipFrameRate));
    tc_component->setDropFrame(false);
    tc_component->setStartTimecode(0);

    // Preface - ContentStorage - tape SourcePackage - TapeDescriptor
    GenericDescriptor *tape_descriptor = dynamic_cast<GenericDescriptor*>(
        mHeaderMetadata->createAndWrap(&MXF_SET_K(TapeDescriptor)));
    tape_package->setDescriptor(tape_descriptor);
    tape_descriptor->setInt32Item(&MXF_ITEM_K(TapeDescriptor, ColorFrame), 0);


    RegisterPhysicalSource(tape_package);

    return tape_package;
}

SourcePackage* AvidClip::CreateDefaultImportSource(string uri, string name,
                                                   uint32_t num_video_tracks, uint32_t num_audio_tracks,
                                                   bool timecode_track)
{
    mxfUMID import_package_uid;
    mxf_generate_aafsdk_umid(&import_package_uid);
    int64_t import_duration = 120 * 60 * 60 * get_rounded_tc_base(mClipFrameRate);

    // Preface - ContentStorage - import SourcePackage
    SourcePackage *import_package = new SourcePackage(mHeaderMetadata);
    mContentStorage->appendPackages(import_package);
    import_package->setPackageUID(import_package_uid);
    import_package->setPackageCreationDate(mCreationDate);
    import_package->setPackageModifiedDate(mCreationDate);
    if (!name.empty())
        import_package->setName(name);
    if (!mProjectName.empty())
        import_package->appendAvidAttribute("_PJ", mProjectName);

    uint32_t track_id = 1;
    uint32_t video_track_number = 1, audio_track_number = 1;
    uint32_t i;
    for (i = 0; i < num_video_tracks + num_audio_tracks; i++) {
        bool is_video = (i < num_video_tracks);

        // Preface - ContentStorage - import SourcePackage - Timeline Track
        Track *track = new Track(mHeaderMetadata);
        import_package->appendTracks(track);
        track->setTrackID(track_id);
        track->setTrackName(get_track_name(is_video, (is_video ? video_track_number : audio_track_number)));
        track->setTrackNumber(is_video ? video_track_number : audio_track_number);
        track->setEditRate(mClipFrameRate);
        track->setOrigin(0);

        // Preface - ContentStorage - import SourcePackage - Timeline Track - Sequence
        Sequence *sequence = new Sequence(mHeaderMetadata);
        track->setSequence(sequence);
        sequence->setDataDefinition(is_video ? MXF_DDEF_L(Picture) : MXF_DDEF_L(Sound));
        sequence->setDuration(import_duration);

        // Preface - ContentStorage - import SourcePackage - Timeline Track - Sequence - SourceClip
        SourceClip *source_clip = new SourceClip(mHeaderMetadata);
        sequence->appendStructuralComponents(source_clip);
        source_clip->setDataDefinition(is_video ? MXF_DDEF_L(Picture) : MXF_DDEF_L(Sound));
        source_clip->setDuration(import_duration);
        source_clip->setStartPosition(0);
        source_clip->setSourcePackageID(g_Null_UMID);
        source_clip->setSourceTrackID(0);

        if (is_video)
            video_track_number++;
        else
            audio_track_number++;

        track_id++;
    }

    if (timecode_track) {
        // Preface - ContentStorage - import SourcePackage - timecode Timeline Track
        Track *tc_track = new Track(mHeaderMetadata);
        import_package->appendTracks(tc_track);
        tc_track->setTrackName("TC1");
        tc_track->setTrackID(track_id);
        tc_track->setTrackNumber(1);
        tc_track->setEditRate(mClipFrameRate);
        tc_track->setOrigin(0);

        // Preface - ContentStorage - import SourcePackage - timecode Timeline Track - Sequence
        Sequence *sequence = new Sequence(mHeaderMetadata);
        tc_track->setSequence(sequence);
        sequence->setDataDefinition(MXF_DDEF_L(Timecode));
        sequence->setDuration(import_duration);

        // Preface - ContentStorage - import SourcePackage - Timecode Track - TimecodeComponent
        TimecodeComponent *tc_component = new TimecodeComponent(mHeaderMetadata);
        sequence->appendStructuralComponents(tc_component);
        tc_component->setDataDefinition(MXF_DDEF_L(Timecode));
        tc_component->setDuration(import_duration);
        tc_component->setRoundedTimecodeBase(get_rounded_tc_base(mClipFrameRate));
        tc_component->setDropFrame(false);
        tc_component->setStartTimecode(0);
    }

    // Preface - ContentStorage - import SourcePackage - ImportDescriptor
    GenericDescriptor *import_descriptor = dynamic_cast<GenericDescriptor*>(
        mHeaderMetadata->createAndWrap(&MXF_SET_K(ImportDescriptor)));
    import_package->setDescriptor(import_descriptor);
    if (!uri.empty()) {
        NetworkLocator *network_locator = new NetworkLocator(mHeaderMetadata);
        import_descriptor->appendLocators(network_locator);
        network_locator->setURLString(uri);
    }


    RegisterPhysicalSource(import_package);

    return import_package;
}

vector<pair<mxfUMID, uint32_t> > AvidClip::GetSourceReferences(mxfpp::SourcePackage *source_package,
                                                               MXFDataDefEnum data_def)
{
    vector<pair<mxfUMID, uint32_t> > references;
    vector<GenericTrack*> tracks = source_package->getTracks();
    size_t i;
    for (i = 0; i < tracks.size(); i++) {
        Track *track = dynamic_cast<Track*>(tracks[i]);
        if (!track || !track->haveTrackID())
            continue;

        StructuralComponent *track_sequence = track->getSequence();
        mxfUL data_def_ul = track_sequence->getDataDefinition();
        if (data_def == mxf_get_ddef_enum(&data_def_ul))
            references.push_back(make_pair(source_package->getPackageUID(), track->getTrackID()));
    }

    return references;
}

void AvidClip::RegisterPhysicalSource(SourcePackage *source_package)
{
    mHavePhysSourceTimecodeTrack = false;
    vector<GenericTrack*> tracks = source_package->getTracks();
    size_t i;
    for (i = 0; i < tracks.size(); i++) {
        Track *track = dynamic_cast<Track*>(tracks[i]);
        if (!track)
            continue;

        StructuralComponent *track_sequence = track->getSequence();
        mxfUL data_def = track_sequence->getDataDefinition();
        if (mxf_is_timecode(&data_def)) {
            mHavePhysSourceTimecodeTrack = true;
            break;
        }
    }

    mPhysicalSourcePackage = source_package;
}

AvidTrack* AvidClip::CreateTrack(EssenceType essence_type)
{
    BMX_CHECK(!mFilenamePrefix.empty());

    bool is_picture = (essence_type != WAVE_PCM);
    uint32_t track_number = 1;
    size_t i;
    for (i = 0; i < mTracks.size(); i++) {
        if (mTracks[i]->IsPicture() == is_picture)
            track_number++;
    }

    char buffer[16];
    bmx_snprintf(buffer, sizeof(buffer), "_%s%u.mxf", (is_picture ? "v" : "a"), track_number);

    string filename = mFilenamePrefix;
    filename.append(buffer);

    return CreateTrack(essence_type, filename);
}

AvidTrack* AvidClip::CreateTrack(EssenceType essence_type, string filename)
{
    mTracks.push_back(AvidTrack::OpenNew(this, mFileFactory->OpenNew(filename), (uint32_t)mTracks.size(),
                                         essence_type));
    return mTracks.back();
}

void AvidClip::PrepareWrite()
{
    // sort tracks, video followed by audio
    stable_sort(mTracks.begin(), mTracks.end(), compare_track);

    uint32_t last_picture_track_number = 0;
    uint32_t last_sound_track_number = 0;
    size_t i;
    for (i = 0; i < mTracks.size(); i++) {
        if (mTracks[i]->IsPicture()) {
            if (!mTracks[i]->IsOutputTrackNumberSet())
                mTracks[i]->SetOutputTrackNumber(last_picture_track_number + 1);
            last_picture_track_number = mTracks[i]->GetOutputTrackNumber();
        } else {
            if (!mTracks[i]->IsOutputTrackNumberSet())
                mTracks[i]->SetOutputTrackNumber(last_sound_track_number + 1);
            last_sound_track_number = mTracks[i]->GetOutputTrackNumber();
        }
    }

    CreateMaterialPackage();

    for (i = 0; i < mTracks.size(); i++)
        mTracks[i]->PrepareWrite();

    if (mHavePhysSourceTimecodeTrack)
        SetPhysicalSourceStartTimecode();
}

void AvidClip::WriteSamples(uint32_t track_index, const unsigned char *data, uint32_t size, uint32_t num_samples)
{
    BMX_CHECK(track_index < mTracks.size());

    mTracks[track_index]->WriteSamples(data, size, num_samples);
}

void AvidClip::CompleteWrite()
{
    UpdateHeaderMetadata();

    size_t i;
    for (i = 0; i < mTracks.size(); i++)
        mTracks[i]->CompleteWrite();
}

int64_t AvidClip::GetDuration() const
{
    int64_t min_duration = -1;
    size_t i;
    for (i = 0; i < mTracks.size(); i++) {
        if (min_duration < 0 || mTracks[i]->GetOutputDuration(true) < min_duration)
            min_duration = mTracks[i]->GetOutputDuration(true);
    }

    return (min_duration < 0 ? 0 : min_duration);
}

int64_t AvidClip::GetFilePosition(uint32_t track_index) const
{
    BMX_CHECK(track_index < mTracks.size());
    return mTracks[track_index]->GetFilePosition();
}

void AvidClip::CreateMinimalHeaderMetadata()
{
    mDataModel = new DataModel();
    mHeaderMetadata = new AvidHeaderMetadata(mDataModel);

    // Preface
    Preface *preface = new Preface(mHeaderMetadata);

    // Preface - ContentStorage
    mContentStorage = new ContentStorage(mHeaderMetadata);
    preface->setContentStorage(mContentStorage);
}

void AvidClip::CreateMaterialPackage()
{
    // Preface - ContentStorage - MaterialPackage
    mMaterialPackage = new MaterialPackage(mHeaderMetadata);
    mContentStorage->appendPackages(mMaterialPackage);
    mMaterialPackage->setPackageUID(mMaterialPackageUID);
    mMaterialPackage->setPackageCreationDate(mMaterialPackageCreationDate);
    mMaterialPackage->setPackageModifiedDate(mMaterialPackageCreationDate);
    if (!mClipName.empty())
        mMaterialPackage->setName(mClipName);
    mMaterialPackage->setBooleanItem(&MXF_ITEM_K(GenericPackage, ConvertFrameRate), false);
    mMaterialPackage->setInt32Item(&MXF_ITEM_K(GenericPackage, AppCode), 7);
    if (!mProjectName.empty())
        mMaterialPackage->appendAvidAttribute("_PJ", mProjectName);
    // user comments and locators are written when completing the file

    bool have_described_track_id = false;
    uint32_t track_id = 1;
    size_t i;
    for (i = 0; i < mTracks.size(); i++) {

        // get picture track id or first audio track id for locators
        if (mTracks[i]->IsPicture() && !have_described_track_id) {
            mLocatorDescribedTrackId = track_id;
            have_described_track_id = true;
        } else if (mLocatorDescribedTrackId == 0) {
            mLocatorDescribedTrackId = track_id;
        }

        // Preface - ContentStorage - MaterialPackage - Timeline Track
        Track *track = new Track(mHeaderMetadata);
        mMaterialPackage->appendTracks(track);
        track->setTrackID(track_id);
        track->setTrackName(get_track_name(mTracks[i]->IsPicture(), mTracks[i]->GetOutputTrackNumber()));
        track->setTrackNumber(mTracks[i]->GetOutputTrackNumber());
        track->setEditRate(mTracks[i]->GetSampleRate());
        track->setOrigin(0);

        mTracks[i]->SetMaterialTrackId(track_id);

        // Preface - ContentStorage - MaterialPackage - Timeline Track - Sequence
        Sequence *sequence = new Sequence(mHeaderMetadata);
        track->setSequence(sequence);
        sequence->setDataDefinition(mTracks[i]->IsPicture() ? MXF_DDEF_L(Picture) : MXF_DDEF_L(Sound));
        sequence->setDuration(-1); // updated when writing completed

        // Preface - ContentStorage - MaterialPackage - Timeline Track - Sequence - SourceClip
        SourceClip *source_clip = new SourceClip(mHeaderMetadata);
        sequence->appendStructuralComponents(source_clip);
        source_clip->setDataDefinition(mTracks[i]->IsPicture() ? MXF_DDEF_L(Picture) : MXF_DDEF_L(Sound));
        source_clip->setDuration(-1); // updated when writing completed
        source_clip->setStartPosition(0);
        pair<mxfUMID, uint32_t> source_ref = mTracks[i]->GetSourceReference();
        source_clip->setSourcePackageID(source_ref.first);
        source_clip->setSourceTrackID(source_ref.second);

        track_id++;
    }


    // add a timecode track to the material package if needed
    if (mStartTimecodeSet &&
        (!mPhysicalSourcePackage || !mHavePhysSourceTimecodeTrack))
    {
        // Preface - ContentStorage - MaterialPackage - timecode Timeline Track
        Track *tc_track = new Track(mHeaderMetadata);
        mMaterialPackage->appendTracks(tc_track);
        tc_track->setTrackName("TC1");
        tc_track->setTrackID(track_id);
        tc_track->setTrackNumber(1);
        tc_track->setEditRate(mClipFrameRate);
        tc_track->setOrigin(0);

        // Preface - ContentStorage - MaterialPackage - timecode Timeline Track - Sequence
        Sequence *sequence = new Sequence(mHeaderMetadata);
        tc_track->setSequence(sequence);
        sequence->setDataDefinition(MXF_DDEF_L(Timecode));
        sequence->setDuration(-1); // updated when writing completed

        // Preface - ContentStorage - MaterialPackage - Timecode Track - TimecodeComponent
        mMaterialTimecodeComponent = new TimecodeComponent(mHeaderMetadata);
        sequence->appendStructuralComponents(mMaterialTimecodeComponent);
        mMaterialTimecodeComponent->setDataDefinition(MXF_DDEF_L(Timecode));
        mMaterialTimecodeComponent->setDuration(-1); // updated when writing completed
        mMaterialTimecodeComponent->setRoundedTimecodeBase(mStartTimecode.GetRoundedTCBase());
        mMaterialTimecodeComponent->setDropFrame(mStartTimecode.IsDropFrame());
        mMaterialTimecodeComponent->setStartTimecode(mStartTimecode.GetOffset());
    }
}

void AvidClip::SetPhysicalSourceStartTimecode()
{
    // set start position in file source package source clips that reference a physical source package
    size_t i;
    for (i = 0; i < mTracks.size(); i++) {
        SourcePackage *ref_source_package = mTracks[i]->GetRefSourcePackage();
        if (!ref_source_package ||
            !ref_source_package->haveDescriptor() ||
            !mTracks[i]->GetDataModel()->isSubclassOf(ref_source_package->getDescriptor(), &MXF_SET_K(PhysicalDescriptor)))
        {
            continue;
        }

        // get physical package start timecode
        TimecodeComponent *phys_tc_component = GetTimecodeComponent(ref_source_package);
        if (!phys_tc_component)
            continue;
        Timecode phys_start_timecode(phys_tc_component->getRoundedTimecodeBase(),
                                     phys_tc_component->getDropFrame(),
                                     phys_tc_component->getStartTimecode());

        // convert to a offset at clip frame rate
        uint16_t rounded_clip_tc_base = get_rounded_tc_base(mClipFrameRate);
        int64_t phys_tc_start_offset = convert_position(phys_start_timecode.GetOffset(),
                                                        rounded_clip_tc_base,
                                                        phys_start_timecode.GetRoundedTCBase(),
                                                        ROUND_AUTO);
        int64_t clip_tc_start_offset = convert_position(mStartTimecode.GetOffset(),
                                                        rounded_clip_tc_base,
                                                        mStartTimecode.GetRoundedTCBase(),
                                                        ROUND_AUTO);
        int64_t start_position = clip_tc_start_offset - phys_tc_start_offset;
        if (start_position < 0) {
            // physical source's start timecode was > start timecode
            log_warn("Not setting start timecode in file source package because start position was negative\n");
            continue;
        }

        // set the start position
        vector<GenericTrack*> tracks = mTracks[i]->GetFileSourcePackage()->getTracks();
        size_t j;
        for (j = 0; j < tracks.size(); j++) {
            Track *track = dynamic_cast<Track*>(tracks[j]);
            if (!track)
                continue;

            StructuralComponent *track_sequence = track->getSequence();
            mxfUL data_def = track_sequence->getDataDefinition();
            if (!mxf_is_picture(&data_def) && !mxf_is_sound(&data_def))
                continue;

            Sequence *sequence = dynamic_cast<Sequence*>(track_sequence);
            BMX_ASSERT(sequence);
            vector<StructuralComponent*> components = sequence->getStructuralComponents();
            BMX_ASSERT(components.size() == 1);
            SourceClip *source_clip = dynamic_cast<SourceClip*>(components[0]);
            BMX_ASSERT(source_clip);

            source_clip->setStartPosition(convert_position(mClipFrameRate, start_position, track->getEditRate(), ROUND_AUTO));
            break;
        }

        // set physical source timecode component's drop frame flag
        phys_tc_component->setDropFrame(mStartTimecode.IsDropFrame());
    }
}

void AvidClip::UpdateHeaderMetadata()
{
    // add user comments and locators
    size_t i;
    for (i = 0; i < mTracks.size(); i++) {

        MaterialPackage *track_material_package = mTracks[i]->GetMaterialPackage();
        AvidHeaderMetadata *track_header_metadata = mTracks[i]->GetHeaderMetadata();

        // add user comments
        map<string, string>::const_iterator iter;
        for (iter = mUserComments.begin(); iter != mUserComments.end(); iter++)
            track_material_package->appendAvidUserComment(iter->first, iter->second);

        // add locators
        if (!mLocators.empty()) {
            BMX_ASSERT(mLocators.size() <= MAX_LOCATORS);
            stable_sort(mLocators.begin(), mLocators.end(), compare_locator);

            // Preface - ContentStorage - MaterialPackage - (DM) Event Track
            // EventMobSlot in Avid AAF file has no name
            // not setting EventOrigin because this results in an error in Avid MediaComposer 3.0
            EventTrack *event_track = new EventTrack(track_header_metadata);
            track_material_package->appendTracks(event_track);
            event_track->setTrackID(DM_TRACK_ID);
            event_track->setTrackNumber(DM_TRACK_NUMBER);
            event_track->setEventEditRate(mClipFrameRate);

            // Preface - ContentStorage - MaterialPackage - (DM) Event Track - (DM) Sequence
            Sequence *sequence = new Sequence(track_header_metadata);
            event_track->setSequence(sequence);
            sequence->setDataDefinition(MXF_DDEF_L(DescriptiveMetadata));

            size_t j;
            for (j = 0; j < mLocators.size(); j++) {
                // Preface - ContentStorage - MaterialPackage - (DM) Event Track - (DM) Sequence - DMSegment
                // duration not set as in Avid sample files
                DMSegment *segment = new DMSegment(track_header_metadata);
                sequence->appendStructuralComponents(segment);
                segment->setDataDefinition(MXF_DDEF_L(DescriptiveMetadata));
                segment->setEventStartPosition(mLocators[j].position);
                segment->setAvidRGBColor(&MXF_ITEM_K(DMSegment, CommentMarkerColor),
                                         AVID_RGB_COLORS[mLocators[j].color - COLOR_WHITE].red,
                                         AVID_RGB_COLORS[mLocators[j].color - COLOR_WHITE].green,
                                         AVID_RGB_COLORS[mLocators[j].color - COLOR_WHITE].blue);
                if (!mLocators[j].comment.empty())
                    segment->setEventComment(mLocators[j].comment);
                if (mLocatorDescribedTrackId > 0)
                    segment->appendTrackIDs(mLocatorDescribedTrackId);
            }
        }
    }

    // update track durations through the reference chain
    for (i = 0; i < mTracks.size(); i++) {
        int64_t track_duration = mTracks[i]->GetOutputDuration(false); // material package edit rate == file package edit rate
        // each track has a copy of the material package and so we loop through the tracks (ie. material packages)
        // and update the track durations corresponding to mTracks[i]
        size_t j;
        for (j = 0; j < mTracks.size(); j++) {
            GenericTrack *gen_track = mTracks[j]->GetMaterialPackage()->findTrack(mTracks[i]->GetMaterialTrackId());
            BMX_ASSERT(gen_track);
            Track *track = dynamic_cast<Track*>(gen_track);
            BMX_ASSERT(track);

            BMX_ASSERT(track->getEditRate() == mTracks[i]->GetSampleRate());
            UpdateTrackDurations(mTracks[i], track, mTracks[i]->GetSampleRate(), track_duration);
        }
    }

    // update timecode track duration in material package and in source package referenced by file source packages
    for (i = 0; i < mTracks.size(); i++) {
        UpdateTimecodeTrackDuration(mTracks[i], mTracks[i]->GetMaterialPackage(), mClipFrameRate);

        if (mTracks[i]->GetRefSourcePackage())
            UpdateTimecodeTrackDuration(mTracks[i], mTracks[i]->GetRefSourcePackage(), mTracks[i]->GetSampleRate());
    }

    // update start timecode
    if (mMaterialTimecodeComponent) {
        mMaterialTimecodeComponent->setRoundedTimecodeBase(mStartTimecode.GetRoundedTCBase());
        mMaterialTimecodeComponent->setDropFrame(mStartTimecode.IsDropFrame());
        mMaterialTimecodeComponent->setStartTimecode(mStartTimecode.GetOffset());
    }
    if (mHavePhysSourceTimecodeTrack)
        SetPhysicalSourceStartTimecode();
}

void AvidClip::UpdateTrackDurations(AvidTrack *avid_track, Track *track, mxfRational edit_rate, int64_t duration)
{
    int64_t track_duration = convert_duration(edit_rate, duration, track->getEditRate(), ROUND_AUTO);

    Sequence *sequence = dynamic_cast<Sequence*>(track->getSequence());
    BMX_ASSERT(sequence);
    if (sequence->getDuration() >= 0) {
        if (sequence->getDuration() < track_duration)
            log_warn("Existing track duration is less than the essence duration\n");
        return;
    }
    sequence->setDuration(track_duration);

    Preface *preface = avid_track->GetHeaderMetadata()->getPreface();
    vector<StructuralComponent*> components = sequence->getStructuralComponents();
    BMX_CHECK(components.size() == 1);
    components[0]->setDuration(track_duration);

    // update duration further down the reference chain
    SourceClip *source_clip = dynamic_cast<SourceClip*>(components[0]);
    if (source_clip) {
        mxfUMID source_package_id = source_clip->getSourcePackageID();
        if (source_package_id != g_Null_UMID) {
            GenericPackage *ref_package = preface->findPackage(source_package_id);
            if (ref_package) {
                GenericTrack *ref_gen_track = ref_package->findTrack(source_clip->getSourceTrackID());
                if (ref_gen_track) {
                    Track *ref_track = dynamic_cast<Track*>(ref_gen_track);
                    BMX_CHECK(ref_track);
                    UpdateTrackDurations(avid_track, ref_track, track->getEditRate(),
                                         source_clip->getStartPosition() + track_duration);
                }
            }
        }
    }
}

void AvidClip::UpdateTimecodeTrackDuration(AvidTrack *avid_track, GenericPackage *package, mxfRational package_edit_rate)
{
    int64_t max_duration = 0;
    vector<GenericTrack*> tracks = package->getTracks();

    // calculate max duration of picture and sound tracks
    size_t j;
    for (j = 0; j < tracks.size(); j++) {
        Track *track = dynamic_cast<Track*>(tracks[j]);
        if (!track)
            continue;

        StructuralComponent *track_sequence = track->getSequence();
        mxfUL data_def = track_sequence->getDataDefinition();
        if (!mxf_is_picture(&data_def) && !mxf_is_sound(&data_def))
            continue;

        int64_t duration = convert_duration(track->getEditRate(), track_sequence->getDuration(), package_edit_rate, ROUND_AUTO);
        if (duration > max_duration)
            max_duration = duration;
    }

    // set timecode track duration to max duration if currently set to -1
    for (j = 0; j < tracks.size(); j++) {
        Track *track = dynamic_cast<Track*>(tracks[j]);
        if (!track)
            continue;

        StructuralComponent *track_sequence = track->getSequence();
        mxfUL data_def = track_sequence->getDataDefinition();
        if (!mxf_is_timecode(&data_def))
            continue;

        if (track_sequence->getDuration() < 0)
            UpdateTrackDurations(avid_track, track, package_edit_rate, max_duration);
    }
}

TimecodeComponent* AvidClip::GetTimecodeComponent(GenericPackage *package)
{
    // find the timecode component in this package
    TimecodeComponent *tc_component = 0;
    vector<GenericTrack*> tracks = package->getTracks();
    size_t i;
    for (i = 0; i < tracks.size(); i++) {
        Track *track = dynamic_cast<Track*>(tracks[i]);
        if (!track)
            continue;

        StructuralComponent *track_sequence = track->getSequence();
        mxfUL data_def = track_sequence->getDataDefinition();
        if (!mxf_is_timecode(&data_def))
            continue;

        Sequence *sequence = dynamic_cast<Sequence*>(track_sequence);
        tc_component = dynamic_cast<TimecodeComponent*>(track_sequence);
        if (sequence) {
            vector<StructuralComponent*> components = sequence->getStructuralComponents();
            size_t j;
            for (j = 0; j < components.size(); j++) {
                tc_component = dynamic_cast<TimecodeComponent*>(components[j]);
                if (tc_component)
                    break;
            }
        }
        if (tc_component)
            break;
    }

    return tc_component;
}

