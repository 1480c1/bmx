/*
 * Copyright (C) 2012, British Broadcasting Corporation
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

#ifndef APP_INFO_OUTPUT_H_
#define APP_INFO_OUTPUT_H_

#include <vector>

#include <bmx/mxf_reader/MXFFileReader.h>
#include <bmx/mxf_reader/MXFAPPInfo.h>


#define DIGIBETA_DROPOUT_MASK   0x01
#define PSE_FAILURE_MASK        0x02
#define TIMECODE_BREAK_MASK     0x04
#define VTR_ERROR_MASK          0x08



namespace bmx
{


class APPInfoOutput
{
public:
    APPInfoOutput();
    ~APPInfoOutput();

    void RegisterExtensions(MXFFileReader *file_reader);
    void ExtractInfo(int event_mask);

    void AddEventTimecodes(int64_t position, Timecode vitc, Timecode ltc);
    void CompleteEventTimecodes();

    void PrintInfo();
    void PrintEvents();

private:
    typedef struct
    {
        ArchiveTimecode vitc;
        ArchiveTimecode ltc;
    } APPTimecodeInfo;

private:
    MXFFileReader *mFileReader;
    MXFAPPInfo mInfo;

    std::vector<APPTimecodeInfo> mPSEFailureTimecodes;
    size_t mPSEFailureTCIndex;
    std::vector<APPTimecodeInfo> mVTRErrorTimecodes;
    size_t mVTRErrorTCIndex;
    std::vector<APPTimecodeInfo> mDigiBetaDropoutTimecodes;
    size_t mDigiBetaDropoutTCIndex;
    std::vector<APPTimecodeInfo> mTimecodeBreakTimecodes;
    size_t mTimecodeBreakTCIndex;
};



};



#endif

