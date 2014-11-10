/*
 * CiderPress
 * Copyright (C) 2007 by faddenSoft, LLC.  All Rights Reserved.
 * See the file LICENSE for distribution terms.
 */
/*
 * Implementation of the Macintosh HFS filesystem.
 *
 * Most of the stuff lives in libhfs.  To avoid problems that could arise
 * from people ejecting floppies or trying to use a disk image while
 * CiderPress is still open, we call hfs_flush() to force updates to be
 * written.  (Even with the "no caching" flag set, the master dir block and
 * volume bitmap aren't written until flush is called.)
 *
 * The libhfs code is licensed under the full GPL, making it awkward to
 * use in a commercial product.  Support for libhfs can be removed with
 * the EXCISE_GPL_CODE ifdefs.  A stub will remain that can recognize HFS
 * volumes, which is useful when dealing with Apple II hard drive and CFFA
 * images.
 */
#include "StdAfx.h"
#include "DiskImgPriv.h"


/*
 * ===========================================================================
 *      DiskFSHFS
 * ===========================================================================
 */

const int kBlkSize = 512;
const int kMasterDirBlock = 2;      // also a copy in next-to-last block
const unsigned short kSignature = 0x4244;   // or 0xd2d7 for MFS
const int kMaxDirectoryDepth = 128;     // not sure what HFS limit is

//namespace DiskImgLib {

/* extent descriptor */
typedef struct ExtDescriptor {
    unsigned short  xdrStABN;       // first allocation block
    unsigned short  xdrNumABlks;    // #of allocation blocks
} ExtDescriptor;
/* extent data record */
typedef struct ExtDataRec {
    ExtDescriptor   extDescriptor[3];
} ExtDataRec;

/*
 * Contents of the HFS MDB.  Information comes from "Inside Macintosh: Files",
 * chapter 2 ("Data Organization on Volumes"), pages 2-60 to 2-62.
 */
typedef struct DiskFSHFS::MasterDirBlock {
    unsigned short  drSigWord;      // volume signature
    unsigned long   drCrDate;       // date/time of volume creation
    unsigned long   drLsMod;        // date/time of last modification
    unsigned short  drAtrb;         // volume attributes
    unsigned short  drNmPls;        // #of files in root directory
    unsigned short  drVBMSt;        // first block of volume bitmap
    unsigned short  drAllocPtr;     // start of next allocation search
    unsigned short  drNmAlBlks;     // number of allocation blocks in volume
    unsigned long   drAlBlkSiz;     // size (bytes) of allocation blocks
    unsigned long   drClpSiz;       // default clump size
    unsigned short  drAlBlSt;       // first allocation block in volume
    unsigned long   drNxtCNID;      // next unused catalog node ID
    unsigned short  drFreeBks;      // number of unused allocation blocks
    unsigned char   drVN[28];       // volume name (pascal string)
    unsigned long   drVolBkUp;      // date/time of last backup
    unsigned short  drVSeqNum;      // volume backup sequence number
    unsigned long   drWrCnt;        // volume write count
    unsigned long   drXTClpSiz;     // clump size for extents overflow file
    unsigned long   drCTClpSiz;     // clump size for catalog file
    unsigned short  drNmRtDirs;     // #of directories in root directory
    unsigned long   drFilCnt;       // #of files in volume
    unsigned long   drDirCnt;       // #of directories in volume
    unsigned long   drFndrInfo[8];  // information used by the Finder
    unsigned short  drVCSize;       // size (blocks) of volume cache
    unsigned short  drVBMCSize;     // size (blocks) of volume bitmap cache
    unsigned short  drCtlCSize;     // size (blocks) of common volume cache
    unsigned long   drXTFlSize;     // size (bytes) of extents overflow file
    ExtDataRec      drXTExtRec;     // extent record for extents overflow file
    unsigned long   drCTFlSize;     // size (bytes) of catalog file
    ExtDataRec      drCTExtRec;     // extent record for catalog file
} MasterDirBlock;

//}; // namespace DiskImgLib

/*
 * Extract fields from a Master Directory Block.
 */
/*static*/ void
DiskFSHFS::UnpackMDB(const unsigned char* buf, MasterDirBlock* pMDB)
{
    pMDB->drSigWord = GetShortBE(&buf[0x00]);
    pMDB->drCrDate = GetLongBE(&buf[0x02]);
    pMDB->drLsMod = GetLongBE(&buf[0x06]);
    pMDB->drAtrb = GetShortBE(&buf[0x0a]);
    pMDB->drNmPls = GetShortBE(&buf[0x0c]);
    pMDB->drVBMSt = GetShortBE(&buf[0x0e]);
    pMDB->drAllocPtr = GetShortBE(&buf[0x10]);
    pMDB->drNmAlBlks = GetShortBE(&buf[0x12]);
    pMDB->drAlBlkSiz = GetLongBE(&buf[0x14]);
    pMDB->drClpSiz = GetLongBE(&buf[0x18]);
    pMDB->drAlBlSt = GetShortBE(&buf[0x1c]);
    pMDB->drNxtCNID = GetLongBE(&buf[0x1e]);
    pMDB->drFreeBks = GetShortBE(&buf[0x22]);
    memcpy(pMDB->drVN, &buf[0x24], sizeof(pMDB->drVN));
    pMDB->drVolBkUp = GetLongBE(&buf[0x40]);
    pMDB->drVSeqNum = GetShortBE(&buf[0x44]);
    pMDB->drWrCnt = GetLongBE(&buf[0x46]);
    pMDB->drXTClpSiz = GetLongBE(&buf[0x4a]);
    pMDB->drCTClpSiz = GetLongBE(&buf[0x4e]);
    pMDB->drNmRtDirs = GetShortBE(&buf[0x52]);
    pMDB->drFilCnt = GetLongBE(&buf[0x54]);
    pMDB->drDirCnt = GetLongBE(&buf[0x58]);
    for (int i = 0; i < (int) NELEM(pMDB->drFndrInfo); i++)
        pMDB->drFndrInfo[i] = GetLongBE(&buf[0x5c + i * 4]);
    pMDB->drVCSize = GetShortBE(&buf[0x7c]);
    pMDB->drVBMCSize = GetShortBE(&buf[0x7e]);
    pMDB->drCtlCSize = GetShortBE(&buf[0x80]);
    pMDB->drXTFlSize = GetLongBE(&buf[0x82]);
    //UnpackExtDataRec(&pMDB->drXTExtRec, &buf[0x86]);  // 12 bytes
    pMDB->drCTFlSize = GetLongBE(&buf[0x92]);
    //UnpackExtDataRec(&pMDB->drXTExtRec, &buf[0x96]);
    // next field at 0xa2
}

/*
 * See if this looks like an HFS volume.
 *
 * We test a few fields in the master directory block for validity.
 */
/*static*/ DIError
DiskFSHFS::TestImage(DiskImg* pImg, DiskImg::SectorOrder imageOrder)
{
    DIError dierr = kDIErrNone;
    MasterDirBlock mdb;
    unsigned char blkBuf[kBlkSize];

    dierr = pImg->ReadBlockSwapped(kMasterDirBlock, blkBuf, imageOrder,
                DiskImg::kSectorOrderProDOS);
    if (dierr != kDIErrNone)
        goto bail;

    UnpackMDB(blkBuf, &mdb);

    if (mdb.drSigWord != kSignature) {
        dierr = kDIErrFilesystemNotFound;
        goto bail;
    }
    if ((mdb.drAlBlkSiz & 0x1ff) != 0) {
        // allocation block size must be a multiple of 512
        WMSG1(" HFS: found allocation block size = %lu, rejecting\n",
            mdb.drAlBlkSiz);
        dierr = kDIErrFilesystemNotFound;
        goto bail;
    }
    if (mdb.drVN[0] == 0 || mdb.drVN[0] > kMaxVolumeName) {
        WMSG1(" HFS: volume name has len = %d, rejecting\n", mdb.drVN[0]);
        dierr = kDIErrFilesystemNotFound;
        goto bail;
    }

    long minBlocks;
    minBlocks = mdb.drNmAlBlks * (mdb.drAlBlkSiz / kBlkSize) + mdb.drAlBlSt + 2;
    if (minBlocks > pImg->GetNumBlocks()) {
        // We're probably trying to open a 1GB volume as if it were only
        // 32MB.  Maybe this is a full HFS partition and we're trying to
        // see if it's a CFFA image.  Whatever the case, we can't do this.
        WMSG2("HFS: volume exceeds disk image size (%ld vs %ld)\n",
            minBlocks, pImg->GetNumBlocks());
        dierr = kDIErrFilesystemNotFound;
        goto bail;
    }

    // looks good!

bail:
    return dierr;
}

/*
 * Test to see if the image is an HFS disk.
 */
/*static*/ DIError
DiskFSHFS::TestFS(DiskImg* pImg, DiskImg::SectorOrder* pOrder,
    DiskImg::FSFormat* pFormat, FSLeniency leniency)
{
    //return kDIErrFilesystemNotFound;      // DEBUG DEBUG DEBUG

    /* must be block format, should be at least 720K */
    if (!pImg->GetHasBlocks() || pImg->GetNumBlocks() < kExpectedMinBlocks)
        return kDIErrFilesystemNotFound;

    DiskImg::SectorOrder ordering[DiskImg::kSectorOrderMax];
    
    DiskImg::GetSectorOrderArray(ordering, *pOrder);

    for (int i = 0; i < DiskImg::kSectorOrderMax; i++) {
        if (ordering[i] == DiskImg::kSectorOrderUnknown)
            continue;
        if (TestImage(pImg, ordering[i]) == kDIErrNone) {
            *pOrder = ordering[i];
            *pFormat = DiskImg::kFormatMacHFS;
            return kDIErrNone;
        }
    }

    WMSG0(" HFS didn't find valid FS\n");
    return kDIErrFilesystemNotFound;
}


/*
 * Load some stuff from the volume header.
 */
DIError
DiskFSHFS::LoadVolHeader(void)
{
    DIError dierr = kDIErrNone;
    MasterDirBlock mdb;
    unsigned char blkBuf[kBlkSize];

    if (fLocalTimeOffset == -1) {
        struct tm* ptm;
        struct tm tmWhen;
        time_t when;
        int isDst;

        when = time(nil);
        isDst = localtime(&when)->tm_isdst;

        ptm = gmtime(&when);
        if (ptm != nil) {
            tmWhen = *ptm;  // make a copy -- static buffers in time functions
            tmWhen.tm_isdst = isDst;

            fLocalTimeOffset = (long) (when - mktime(&tmWhen));
        } else
            fLocalTimeOffset = 0;

        WMSG1(" HFS computed local time offset = %.3f hours\n",
            fLocalTimeOffset / 3600.0);
    }

    dierr = fpImg->ReadBlock(kMasterDirBlock, blkBuf);
    if (dierr != kDIErrNone)
        goto bail;

    UnpackMDB(blkBuf, &mdb);

    /*
     * The minimum size of the volume is "number of allocation blocks" plus
     * "first allocation block" (to avoid the OS overhead) plus 2 (because
     * there's a backup copy of the MDB in the next-to-last block, and
     * nothing at all in the very last block).
     *
     * This isn't the total size, because on larger volumes there can be
     * some padding between the last usable block and the backup MDB.  The
     * only way to find the MDB is to take the DiskImg's block size and
     * subtract 2.
     */
    assert((mdb.drAlBlkSiz % kBlkSize) == 0);
    fNumAllocationBlocks = mdb.drNmAlBlks;
    fAllocationBlockSize = mdb.drAlBlkSiz;
    fTotalBlocks = fpImg->GetNumBlocks();

    unsigned long minBlocks;
    minBlocks = mdb.drNmAlBlks * (mdb.drAlBlkSiz / kBlkSize) + mdb.drAlBlSt + 2;
    assert(fTotalBlocks >= minBlocks);      // verified during fs tests

    int volNameLen;
    volNameLen = mdb.drVN[0];
    if (volNameLen > kMaxVolumeName) {
        assert(false);      // should've been trapped earlier
        volNameLen = kMaxVolumeName;
    }
    memcpy(fVolumeName, &mdb.drVN[1], volNameLen);
    fVolumeName[volNameLen] = '\0';
    SetVolumeID();

    fNumFiles = mdb.drFilCnt;
    fNumDirectories = mdb.drDirCnt;
    fCreatedDateTime = mdb.drCrDate;
    fModifiedDateTime = mdb.drLsMod;

    /*
     * Create a "magic" directory entry for the volume directory.  This
     * must come first in the file list.
     */
    A2FileHFS* pFile;
    pFile = new A2FileHFS(this);
    if (pFile == nil) {
        dierr = kDIErrMalloc;
        goto bail;
    }

    pFile->fIsDir = true;
    pFile->fIsVolumeDir = true;
    pFile->fType = 0;
    pFile->fCreator = 0;
    strcpy(pFile->fFileName, fVolumeName);      // vol names are shorter than
    pFile->SetPathName(":", fVolumeName);       //  filenames, so it fits
    pFile->fDataLength = 0;
    pFile->fRsrcLength = -1;
    pFile->fCreateWhen =
        (time_t) (fCreatedDateTime - kDateTimeOffset) - fLocalTimeOffset;
    pFile->fModWhen =
        (time_t) (fModifiedDateTime - kDateTimeOffset) - fLocalTimeOffset;
    pFile->fAccess = DiskFS::kFileAccessUnlocked;

    //WMSG2("GOT *** '%s' '%s'\n", pFile->fFileName, pFile->fPathName);

    AddFileToList(pFile);

bail:
    return dierr;
}

/*
 * Set the volume ID based on fVolumeName.
 */
void
DiskFSHFS::SetVolumeID(void)
{
    strcpy(fVolumeID, "HFS ");
    strcat(fVolumeID, fVolumeName);
}

/*
 * Blank out the volume usage map.  The HFS volume bitmap is not yet supported.
 */
void
DiskFSHFS::SetVolumeUsageMap(void)
{
    VolumeUsage::ChunkState cstate;
    long block;

    fVolumeUsage.Create(fpImg->GetNumBlocks());

    cstate.isUsed = true;
    cstate.isMarkedUsed = true;
    cstate.purpose = VolumeUsage::kChunkPurposeUnknown;

    for (block = fTotalBlocks-1; block >= 0; block--)
        fVolumeUsage.SetChunkState(block, &cstate);
}


/*
 * Print some interesting fields to the debug log.
 */
void
DiskFSHFS::DumpVolHeader(void)
{
    WMSG0("HFS volume header read:\n");
    WMSG1("  volume name = '%s'\n", fVolumeName);
    WMSG4("  total blocks = %ld (allocSize=%ld [x%lu], numAllocs=%lu)\n",
        fTotalBlocks, fAllocationBlockSize, fAllocationBlockSize / kBlkSize,
        fNumAllocationBlocks);
    WMSG2("  num directories=%ld, num files=%ld\n",
        fNumDirectories, fNumFiles);
    time_t when;
    when = (time_t) (fCreatedDateTime - kDateTimeOffset - fLocalTimeOffset);
    WMSG2("  cre date=0x%08lx %.24s\n", fCreatedDateTime, ctime(&when));
    when = (time_t) (fModifiedDateTime - kDateTimeOffset - fLocalTimeOffset);
    WMSG2("  mod date=0x%08lx %.24s\n", fModifiedDateTime, ctime(&when));
}


#ifndef EXCISE_GPL_CODE

/*
 * Get things rolling.
 *
 * Since we're assured that this is a valid disk, errors encountered from here
 * on out must be handled somehow, possibly by claiming that the disk is
 * completely full and has no files on it.
 */
DIError
DiskFSHFS::Initialize(InitMode initMode)
{
    DIError dierr = kDIErrNone;
    char msg[kMaxVolumeName + 32];

    dierr = LoadVolHeader();
    if (dierr != kDIErrNone)
        goto bail;
    DumpVolHeader();

    if (initMode == kInitHeaderOnly) {
        WMSG0(" HFS - headerOnly set, skipping file load\n");
        goto bail;
    }

    sprintf(msg, "Scanning %s", fVolumeName);
    if (!fpImg->UpdateScanProgress(msg)) {
        WMSG0(" HFS cancelled by user\n");
        dierr = kDIErrCancelled;
        goto bail;
    }

    /*
     * Open the volume with libhfs.  We used to set HFS_OPT_NOCACHE to avoid
     * consistency problems and reduce the risk of disk corruption should
     * CiderPress fail, but it turns out libhfs doesn't write the volume
     * bitmap or master dir block unless explicitly flushed anyway.  Since
     * the caching helps us a lot when just reading -- 4 seconds vs. 9 for
     * a CD-ROM over gigabit Ethernet -- we leave it on, and explicitly
     * flush every time we make a change.
     */
    fHfsVol = hfs_callback_open(LibHFSCB, this, /*HFS_OPT_NOCACHE |*/
                (fpImg->GetReadOnly() ? HFS_MODE_RDONLY : HFS_MODE_RDWR));
    if (fHfsVol == nil) {
        WMSG1("ERROR: hfs_opencallback failed: %s\n", hfs_error);
        return kDIErrGeneric;
    }

    /* volume dir is guaranteed to come first; if not, we need a lookup func */
    A2FileHFS* pVolumeDir;
    pVolumeDir = (A2FileHFS*) GetNextFile(nil);

    dierr = RecursiveDirAdd(pVolumeDir, ":", 0);
    if (dierr != kDIErrNone)
        goto bail;

    SetVolumeUsageMap();

    /*
     * Make sure there's nothing lingering.  libhfs will fiddle around with
     * the MDB if it looks like the volume wasn't unmounted cleanly last time.
     */
    hfs_flush(fHfsVol);

bail:
    return dierr;
}

/*
 * Callback function from libhfs.  Can read/write/seek.
 *
 * This is a little clumsy, but it allows us to maintain a separation from
 * the libhfs code (which is GPLed).
 *
 * Returns -1 on failure.
 */
unsigned long
DiskFSHFS::LibHFSCB(void* vThis, int op, unsigned long arg1, void* arg2)
{
    DiskFSHFS* pThis = (DiskFSHFS*) vThis;
    unsigned long result = (unsigned long) -1;

    assert(pThis != nil);

    switch (op) {
    case HFS_CB_VOLSIZE:
        //WMSG1("  HFSCB vol size = %ld blocks\n", pThis->fTotalBlocks);
        result = pThis->fTotalBlocks;
        break;
    case HFS_CB_READ:       // arg1=block, arg2=buffer
        //WMSG1("  HFSCB read block %lu\n", arg1);
        if (arg1 < pThis->fTotalBlocks && arg2 != nil) {
            DIError err = pThis->fpImg->ReadBlock(arg1, arg2);
            if (err == kDIErrNone)
                result = 0;
            else {
                WMSG1("  HFSCB read %lu failed\n", arg1);
            }
        }
        break;
    case HFS_CB_WRITE:
        WMSG1("  HFSCB write block %lu\n", arg1);
        if (arg1 < pThis->fTotalBlocks && arg2 != nil) {
            DIError err = pThis->fpImg->WriteBlock(arg1, arg2);
            if (err == kDIErrNone)
                result = 0;
            else {
                WMSG1("  HFSCB write %lu failed\n", arg1);
            }
        }
        break;
    case HFS_CB_SEEK:       // arg1=block, arg2=unused
        /* just verify that the seek is legal */
        //WMSG1("  HFSCB seek block %lu\n", arg1);
        if (arg1 < pThis->fTotalBlocks)
            result = arg1;
        break;
    default:
        assert(false);
    }

    //WMSG1("--- HFSCB returning %lu\n", result);
    return result;
}


/*
 * Determine the amount of free space on the disk.
 */
DIError
DiskFSHFS::GetFreeSpaceCount(long* pTotalUnits, long* pFreeUnits,
    int* pUnitSize) const
{
    assert(fHfsVol != nil);

    hfsvolent volEnt;
    if (hfs_vstat(fHfsVol, &volEnt) != 0)
        return kDIErrGeneric;

    *pTotalUnits = volEnt.totbytes / 512;
    *pFreeUnits = volEnt.freebytes / 512;
    *pUnitSize = 512;

    return kDIErrNone;
}

/*
 * Recursively traverse the filesystem.
 */
DIError
DiskFSHFS::RecursiveDirAdd(A2File* pParent, const char* basePath, int depth)
{
    DIError dierr = kDIErrNone;
    hfsdir* dir;
    hfsdirent dirEntry;
    char* pathBuf = nil;
    int nameOffset;

    /* if we get too deep, assume it's a loop */
    if (depth > kMaxDirectoryDepth) {
        dierr = kDIErrDirectoryLoop;
        goto bail;
    }

    //WMSG1(" HFS RecursiveDirAdd '%s'\n", basePath);
    dir = hfs_opendir(fHfsVol, basePath);
    if (dir == nil) {
        printf("  HFS unable to open dir '%s'\n", basePath);
        WMSG1("  HFS unable to open dir '%s'\n", basePath);
        dierr = kDIErrGeneric;
        goto bail;
    }

    if (strcmp(basePath, ":") == 0)
        basePath = "";

    nameOffset = strlen(basePath) +1;
    pathBuf = new char[nameOffset + A2FileHFS::kMaxFileName +1];
    if (pathBuf == nil) {
        dierr = kDIErrMalloc;
        goto bail;
    }
    strcpy(pathBuf, basePath);
    pathBuf[nameOffset-1] = A2FileHFS::kFssep;
    pathBuf[nameOffset] = '\0';     // not needed

    while (hfs_readdir(dir, &dirEntry) != -1) {
        A2FileHFS* pFile;

        pFile = new A2FileHFS(this);

        pFile->InitEntry(&dirEntry);

        pFile->SetPathName(basePath, pFile->fFileName);
        pFile->SetParent(pParent);
        AddFileToList(pFile);

        if (!fpImg->UpdateScanProgress(nil)) {
            WMSG0(" HFS cancelled by user\n");
            dierr = kDIErrCancelled;
            goto bail;
        }

        if (dirEntry.flags & HFS_ISDIR) {
            strcpy(pathBuf + nameOffset, dirEntry.name);
            dierr = RecursiveDirAdd(pFile, pathBuf, depth+1);
            if (dierr != kDIErrNone)
                goto bail;
        }
    }

bail:
    delete[] pathBuf;
    return dierr;
}

/*
 * Initialize an A2FileHFS structure from the stuff in an hfsdirent.
 */
void A2FileHFS::InitEntry(const hfsdirent* dirEntry)
{
    //printf("--- File '%s' flags=0x%08x fdflags=0x%08x type='%s'\n",
    //    dirEntry.name, dirEntry.flags, dirEntry.fdflags,
    //    dirEntry.u.file.type);

    fIsVolumeDir = false;
    memcpy(fFileName, dirEntry->name, A2FileHFS::kMaxFileName+1);
    fFileName[A2FileHFS::kMaxFileName] = '\0';     // make sure

    if (dirEntry->flags & HFS_ISLOCKED)
        fAccess = DiskFS::kFileAccessLocked;
    else
        fAccess = DiskFS::kFileAccessUnlocked;
    if (dirEntry->fdflags & HFS_FNDR_ISINVISIBLE)
        fAccess |= A2FileProDOS::kAccessInvisible;

    if (dirEntry->flags & HFS_ISDIR) {
        fIsDir = true;
        fType = fCreator = 0;
        fDataLength = 0;
        fRsrcLength = -1;
    } else {
        unsigned char* pType;

        fIsDir = false;

        pType = (unsigned char*) dirEntry->u.file.type;
        fType =
            pType[0] << 24 | pType[1] << 16 | pType[2] << 8 | pType[3];
        pType = (unsigned char*) dirEntry->u.file.creator;
        fCreator =
            pType[0] << 24 | pType[1] << 16 | pType[2] << 8 | pType[3];
        fDataLength = dirEntry->u.file.dsize;
        fRsrcLength = dirEntry->u.file.rsize;

        /*
         * Resource fork must be at least 512 bytes for Finder, so if
         * it has zero length then the file must not have one.
         */
        if (fRsrcLength == 0)
            fRsrcLength = -1;
    }

    /*
     * Create/modified dates (we ignore the "last backup" date).  The
     * hfslib functions convert to time_t for us.
     */
    fCreateWhen = dirEntry->crdate;
    fModWhen = dirEntry->mddate;
}

/*
 * Return "true" if "name" is valid for use as an HFS volume name.
 */
/*static*/ bool
DiskFSHFS::IsValidVolumeName(const char* name)
{
    if (name == nil)
        return false;

    int len = strlen(name);
    if (len < 1 || len > kMaxVolumeName)
        return false;

    while (*name != '\0') {
        if (*name == A2FileHFS::kFssep)
            return false;
        name++;
    }

    return true;
}

/*
 * Return "true" if "name" is valid for use as an HFS file name.
 */
/*static*/ bool
DiskFSHFS::IsValidFileName(const char* name)
{
    if (name == nil)
        return false;

    int len = strlen(name);
    if (len < 1 || len > A2FileHFS::kMaxFileName)
        return false;

    while (*name != '\0') {
        if (*name == A2FileHFS::kFssep)
            return false;
        name++;
    }

    return true;
}

/*
 * Format the current volume with HFS.
 */
DIError
DiskFSHFS::Format(DiskImg* pDiskImg, const char* volName)
{
    assert(strlen(volName) > 0 && strlen(volName) <= kMaxVolumeName);

    if (!IsValidVolumeName(volName))
        return kDIErrInvalidArg;

    /* set fpImg so calls that rely on it will work; we un-set it later */
    assert(fpImg == nil);
    SetDiskImg(pDiskImg);

    /* need this for callback function */
    fTotalBlocks = fpImg->GetNumBlocks();

    // need HFS_OPT_2048 for CD-ROM?
    if (hfs_callback_format(LibHFSCB, this, 0, volName) != 0) {
        WMSG1("hfs_callback_format failed (%s)\n", hfs_error);
        return kDIErrGeneric;
    }

    // no need to flush; HFS volume is closed

    SetDiskImg(nil);        // shouldn't really be set by us
    return kDIErrNone;
}


/*
 * Normalize an HFS path.  Invokes DoNormalizePath and handles the buffer
 * management (if the normalized path doesn't fit in "*pNormalizedBufLen"
 * bytes, we set "*pNormalizedBufLen to the required length).
 *
 * This is invoked from the generalized "add" function in CiderPress, which
 * doesn't want to understand the ins and outs of pathnames.
 */
DIError
DiskFSHFS::NormalizePath(const char* path, char fssep,
    char* normalizedBuf, int* pNormalizedBufLen)
{
    DIError dierr = kDIErrNone;
    char* normalizedPath = nil;
    int len;

    assert(pNormalizedBufLen != nil);
    assert(normalizedBuf != nil || *pNormalizedBufLen == 0);

    dierr = DoNormalizePath(path, fssep, &normalizedPath);
    if (dierr != kDIErrNone)
        goto bail;

    assert(normalizedPath != nil);
    len = strlen(normalizedPath);
    if (normalizedBuf == nil || *pNormalizedBufLen <= len) {
        /* too short */
        dierr = kDIErrDataOverrun;
    } else {
        /* fits */
        strcpy(normalizedBuf, normalizedPath);
    }

    *pNormalizedBufLen = len+1;     // alloc room for the '\0'

bail:
    delete[] normalizedPath;
    return dierr;
}

/*
 * Normalize an HFS path.  This requires separating each path component
 * out, making it HFS-compliant, and then putting it back in.
 * The fssep could be anything, so we need to change it to kFssep.
 *
 * The caller must delete[] "*pNormalizedPath".
 */
DIError
DiskFSHFS::DoNormalizePath(const char* path, char fssep,
    char** pNormalizedPath)
{
    DIError dierr = kDIErrNone;
    char* workBuf = nil;
    char* partBuf = nil;
    char* outputBuf = nil;
    char* start;
    char* end;
    char* outPtr;

    assert(path != nil);
    workBuf = new char[strlen(path)+1];
    partBuf = new char[strlen(path)+1 +1];  // need +1 for prepending letter
    outputBuf = new char[strlen(path) * 2];
    if (workBuf == nil || partBuf == nil || outputBuf == nil) {
        dierr = kDIErrMalloc;
        goto bail;
    }

    strcpy(workBuf, path);
    outputBuf[0] = '\0';

    outPtr = outputBuf;
    start = workBuf;
    while (*start != '\0') {
        //char* origStart = start;  // need for debug msg
        int partIdx;

        if (fssep == '\0') {
            end = nil;
        } else {
            end = strchr(start, fssep);
            if (end != nil)
                *end = '\0';
        }
        partIdx = 0;

        /*
         * Copy, converting colons to underscores.  We should strip out any
         * illegal characters here, but there's not much in HFS that's
         * considered illegal.
         */
        while (*start != '\0') {
            if (*start == A2FileHFS::kFssep)
                partBuf[partIdx++] = '_';
            else
                partBuf[partIdx++] = *start;
            start++;
        }

        /*
         * Truncate at 31 chars, preserving anything that looks like a
         * filename extension.  "partIdx" represents the length of the
         * string at this point.  "partBuf" holds the string, which we
         * want to null-terminate before proceeding.
         *
         * Try to keep the filename extension, if any.
         */
        partBuf[partIdx] = '\0';
        if (partIdx > A2FileHFS::kMaxFileName) {
            const char* pDot = strrchr(partBuf, '.');
            //int DEBUGDOTLEN = pDot - partBuf;
            if (pDot != nil && partIdx - (pDot-partBuf) <= kMaxExtensionLen) {
                int dotLen = partIdx - (pDot-partBuf);
                memmove(partBuf + (A2FileProDOS::kMaxFileName - dotLen),
                    pDot, dotLen);      // don't use memcpy, move might overlap
            }
            partIdx = A2FileProDOS::kMaxFileName;
        }
        partBuf[partIdx] = '\0';

        //WMSG2(" HFS   Converted component '%s' to '%s'\n",
        //  origStart, partBuf);

        if (outPtr != outputBuf)
            *outPtr++ = A2FileHFS::kFssep;
        strcpy(outPtr, partBuf);
        outPtr += partIdx;

        /*
         * Continue with next segment.
         */
        if (end == nil)
            break;
        start = end+1;
    }

    *outPtr = '\0';

    WMSG3(" HFS  Converted path '%s' to '%s' (fssep='%c')\n",
        path, outputBuf, fssep);
    assert(*outputBuf != '\0');

    *pNormalizedPath = outputBuf;
    outputBuf = nil;

bail:
    delete[] workBuf;
    delete[] partBuf;
    delete[] outputBuf;
    return dierr;
}

/*
 * Compare two Macintosh filename strings.
 *
 * This requires some effort because the Macintosh Roman character set
 * doesn't sort the same way that ASCII does.  HFS is case-insensitive but
 * case-preserving, so we need to deal with that too.  The hfs_charorder
 * table takes care of it.
 *
 * Returns <0, ==0, or >0 depending on whether sstr1 is lexically less than,
 * equal to, or greater than sstr2.
 */
/*static*/ int
DiskFSHFS::CompareMacFileNames(const char* sstr1, const char* sstr2)
{
    const unsigned char* str1 = (const unsigned char*) sstr1;
    const unsigned char* str2 = (const unsigned char*) sstr2;
    int diff;

    while (*str1 && *str2) {
        diff = hfs_charorder[*str1] - hfs_charorder[*str2];

        if (diff != 0)
            return diff;

        str1++;
        str2++;
    }

    return *str1 - *str2;
}

/*
 * Keep tweaking the filename until it no longer matches an existing file.
 * The first time this is called we don't know if the name is unique or not,
 * so we need to start by checking that.
 *
 * We have our choice between the DiskFS GetFileByName(), which traverses
 * a linear list, and hfs_stat(), which uses more efficient data structures
 * but may require disk reads.  We use the DiskFS interface, on the assumption
 * that someday we'll switch the linear list to a tree structure.
 */
DIError
DiskFSHFS::MakeFileNameUnique(const char* pathName, char** pUniqueName)
{
    A2File* pFile;
    const int kMaxExtra = 3;
    const int kMaxDigits = 999;
    char* uniqueName;
    char* fileName;     // points inside uniqueName

    assert(pathName != nil);
    assert(pathName[0] == A2FileHFS::kFssep);

    /* see if it exists */
    pFile = GetFileByName(pathName+1);
    if (pFile == nil) {
        *pUniqueName = nil;
        return kDIErrNone;
    }

    /* make a copy we can chew on */
    uniqueName = new char[strlen(pathName) + kMaxExtra +1];
    strcpy(uniqueName, pathName);

    fileName = strrchr(uniqueName, A2FileHFS::kFssep);
    assert(fileName != nil);
    fileName++;

    int nameLen = strlen(fileName);
    int dotOffset=0, dotLen=0;
    char dotBuf[kMaxExtensionLen+1];

    /* ensure the result will be null-terminated */
    memset(fileName + nameLen, 0, kMaxExtra+1);

    /*
     * If this has what looks like a filename extension, grab it.  We want
     * to preserve ".gif", ".c", etc., since the filetypes don't necessarily
     * do everything we need.
     */
    const char* cp = strrchr(fileName, '.');
    if (cp != nil) {
        int tmpOffset = cp - fileName;
        if (tmpOffset > 0 && nameLen - tmpOffset <= kMaxExtensionLen) {
            WMSG1("  HFS   (keeping extension '%s')\n", cp);
            assert(strlen(cp) <= kMaxExtensionLen);
            strcpy(dotBuf, cp);
            dotOffset = tmpOffset;
            dotLen = nameLen - dotOffset;
        }
    }

    int digits = 0;
    int digitLen;
    int copyOffset;
    char digitBuf[kMaxExtra+1];
    do {
        if (digits == kMaxDigits)
            return kDIErrFileExists;
        digits++;

        /* not the most efficient way to do this, but it'll do */
        sprintf(digitBuf, "%d", digits);
        digitLen = strlen(digitBuf);
        if (nameLen + digitLen > A2FileHFS::kMaxFileName)
            copyOffset = A2FileHFS::kMaxFileName - dotLen - digitLen;
        else
            copyOffset = nameLen - dotLen;
        memcpy(fileName + copyOffset, digitBuf, digitLen);
        if (dotLen != 0)
            memcpy(fileName + copyOffset + digitLen, dotBuf, dotLen);
    } while (GetFileByName(uniqueName+1) != nil);

    WMSG1(" HFS  converted to unique name: %s\n", uniqueName);

    *pUniqueName = uniqueName;
    return kDIErrNone;
}


/*
 * Create a new file or directory.  Automatically creates the base path
 * if necessary.
 *
 * NOTE: much of this was cloned out of the ProDOS code.  We probably want
 * a stronger set of utility functions in the parent class now that we have
 * more than one hierarchical file system.
 */
DIError
DiskFSHFS::CreateFile(const CreateParms* pParms, A2File** ppNewFile)
{
    DIError dierr = kDIErrNone;
    char typeStr[5], creatorStr[5];
    char* normalizedPath = nil;
    char* basePath = nil;
    char* fileName = nil;
    char* fullPath = nil;
    A2FileHFS* pSubdir = nil;
    A2FileHFS* pNewFile = nil;
    hfsfile* pHfsFile = nil;
    const bool createUnique = (GetParameter(kParm_CreateUnique) != 0);

    assert(fHfsVol != nil);

    if (fpImg->GetReadOnly())
        return kDIErrAccessDenied;

    assert(pParms != nil);
    assert(pParms->pathName != nil);
    assert(pParms->storageType == A2FileProDOS::kStorageSeedling ||
           pParms->storageType == A2FileProDOS::kStorageExtended ||
           pParms->storageType == A2FileProDOS::kStorageDirectory);
    // kStorageVolumeDirHeader not allowed -- that's created by Format
    WMSG1(" HFS ---v--- CreateFile '%s'\n", pParms->pathName);

    /*
     * Normalize the pathname so that all components are HFS-safe
     * and separated by ':'.
     *
     * This must not "sanitize" the path.  We need to be working with the
     * original characters, not the sanitized-for-display versions.
     */
    assert(pParms->pathName != nil);
    dierr = DoNormalizePath(pParms->pathName, pParms->fssep,
                &normalizedPath);
    if (dierr != kDIErrNone)
        goto bail;
    assert(normalizedPath != nil);

    /*
     * The normalized path lacks a leading ':', and might need to
     * have some digits added to make the name unique.
     */
    fullPath = new char[strlen(normalizedPath)+2];
    fullPath[0] = A2FileHFS::kFssep;
    strcpy(fullPath+1, normalizedPath);
    delete[] normalizedPath;
    normalizedPath = nil;

    /*
     * Make the name unique within the current directory.  This requires
     * appending digits until the name doesn't match any others.
     */
    if (createUnique &&
        pParms->storageType != A2FileProDOS::kStorageDirectory)
    {
        char* uniquePath;

        dierr = MakeFileNameUnique(fullPath, &uniquePath);
        if (dierr != kDIErrNone)
            goto bail;
        if (uniquePath != nil) {
            delete[] fullPath;
            fullPath = uniquePath;
        }
    } else {
        /* can't make unique; check to see if it already exists */
        hfsdirent dirEnt;
        if (hfs_stat(fHfsVol, fullPath, &dirEnt) == 0) {
            if (pParms->storageType == A2FileProDOS::kStorageDirectory)
                dierr = kDIErrDirectoryExists;
            else
                dierr = kDIErrFileExists;
            goto bail;
        }
    }

    /*
     * Split the base path and filename apart.
     */
    char* cp;
    cp = strrchr(fullPath, A2FileHFS::kFssep);
    assert(cp != nil);
    if (cp == fullPath) {
        assert(basePath == nil);
        fileName = new char[strlen(fullPath) +1];
        strcpy(fileName, fullPath);
    } else {
        int dirNameLen = cp - fullPath;

        fileName = new char[strlen(cp+1) +1];
        strcpy(fileName, cp+1);
        basePath = new char[dirNameLen+1];
        strncpy(basePath, fullPath, dirNameLen);
        basePath[dirNameLen] = '\0';
    }

    WMSG2("SPLIT: '%s' '%s'\n", basePath, fileName);

    assert(fileName != nil);

    /*
     * Open the base path.  If it doesn't exist, create it recursively.
     */
    if (basePath != nil) {
        WMSG2(" HFS  Creating '%s' in '%s'\n", fileName, basePath);
        /*
         * Open the named subdir, creating it if it doesn't exist.  We need
         * to check basePath+1 because we're comparing against what's in our
         * linear file list, and that doesn't include the leading ':'.
         */
        pSubdir = (A2FileHFS*)GetFileByName(basePath+1, CompareMacFileNames);
        if (pSubdir == nil) {
            WMSG1("  HFS  Creating subdir '%s'\n", basePath);
            A2File* pNewSub;
            CreateParms newDirParms;
            newDirParms.pathName = basePath;
            newDirParms.fssep = A2FileHFS::kFssep;
            newDirParms.storageType = A2FileProDOS::kStorageDirectory;
            newDirParms.fileType = 0;
            newDirParms.auxType = 0;
            newDirParms.access = 0;
            newDirParms.createWhen = newDirParms.modWhen = time(nil);
            dierr = this->CreateFile(&newDirParms, &pNewSub);
            if (dierr != kDIErrNone)
                goto bail;
            assert(pNewSub != nil);

            pSubdir = (A2FileHFS*) pNewSub;
        }

        /*
         * And now the annoying part.  We need to reconstruct basePath out
         * of the filenames actually present, rather than relying on the
         * argument passed in.  That's because HFS is case-insensitive but
         * case-preserving.  It's not crucial for our inner workings, but the
         * linear file list in the DiskFS should have accurate strings.
         * (It'll work just fine, but the display might show the wrong values
         * for parent directories until they reload the disk.)
         *
         * On the bright side, we know exactly how long the string needs
         * to be, so we can just stomp on it in place.  Assuming, of course,
         * that the filename created matches up with what the filename
         * normalizer came up with, which we can guarantee since (a) everybody
         * uses the same normalizer and (b) the "uniqueify" stuff doesn't
         * kick in for subdirs because we wouldn't be creating a new subdir
         * if it didn't already exist.
         *
         * This is essentially the same as RegeneratePathName(), but that's
         * meant for a situation where the filename already exists.
         */
        A2FileHFS* pBaseDir = pSubdir;
        int basePathLen = strlen(basePath);
        while (!pBaseDir->IsVolumeDirectory()) {
            const char* fixedName = pBaseDir->GetFileName();
            int fixedLen = strlen(fixedName);
            if (fixedLen > basePathLen) {
                assert(false);
                break;
            }
            assert(basePathLen == fixedLen ||
                   *(basePath + (basePathLen-fixedLen-1)) == kDIFssep);
            memcpy(basePath + (basePathLen-fixedLen), fixedName, fixedLen);
            basePathLen -= fixedLen+1;

            pBaseDir = (A2FileHFS*) pBaseDir->GetParent();
            assert(pBaseDir != nil);
        }
        // check the math; we should be left with the leading ':'
        if (pSubdir->IsVolumeDirectory())
            assert(basePathLen == 1);
        else
            assert(basePathLen == 0);
    } else {
        /* open the volume directory */
        WMSG1(" HFS  Creating '%s' in volume dir\n", fileName);
        /* volume dir must be first in the list */
        pSubdir = (A2FileHFS*) GetNextFile(nil);
        assert(pSubdir != nil);
        assert(pSubdir->IsVolumeDirectory());
    }
    if (pSubdir == nil) {
        WMSG1(" HFS Unable to open subdir '%s'\n", basePath);
        dierr = kDIErrFileNotFound;
        goto bail;
    }

    /*
     * Figure out file type.
     */
    A2FileHFS::ConvertTypeToHFS(pParms->fileType, pParms->auxType,
                    typeStr, creatorStr);

    /*
     * Create the file or directory.  Populate "dirEnt" with the details.
     */
    hfsdirent dirEnt;
    if (pParms->storageType == A2FileProDOS::kStorageDirectory) {
        /* create the directory */
        if (hfs_mkdir(fHfsVol, fullPath) != 0) {
            WMSG2(" HFS mkdir '%s' failed: %s\n", fullPath, hfs_error);
            dierr = kDIErrGeneric;
            goto bail;
        }
        if (hfs_stat(fHfsVol, fullPath, &dirEnt) != 0) {
            WMSG1(" HFS stat on new dir failed: %s\n", hfs_error);
            dierr = kDIErrGeneric;
            goto bail;
        }
        /* create date *might* be useful, but probably not worth adjusting */
    } else {
        /* create, and open, the file */
        pHfsFile = hfs_create(fHfsVol, fullPath, typeStr, creatorStr);
        if (pHfsFile == nil) {
            WMSG1(" HFS create failed: %s\n", hfs_error);
            dierr = kDIErrGeneric;
            goto bail;
        }
        if (hfs_fstat(pHfsFile, &dirEnt) != 0) {
            WMSG1(" HFS fstat on new file failed: %s\n", hfs_error);
            dierr = kDIErrGeneric;
            goto bail;
        }

        /* set the attributes according to pParms, and update the file */
        dirEnt.crdate = pParms->createWhen;
        dirEnt.mddate = pParms->modWhen;
        if (pParms->access & A2FileProDOS::kAccessInvisible)
            dirEnt.fdflags |= HFS_FNDR_ISINVISIBLE;
        else
            dirEnt.fdflags &= ~HFS_FNDR_ISINVISIBLE;
        if ((pParms->access & ~A2FileProDOS::kAccessInvisible) == kFileAccessLocked)
            dirEnt.flags |= HFS_ISLOCKED;
        else
            dirEnt.flags &= ~HFS_ISLOCKED;

        (void) hfs_fsetattr(pHfsFile, &dirEnt);
        (void) hfs_close(pHfsFile);
        pHfsFile = nil;
    }

    /*
     * Success!
     *
     * Create a new entry and set the structure fields.
     */
    pNewFile = new A2FileHFS(this);
    pNewFile->InitEntry(&dirEnt);
    pNewFile->SetPathName(basePath == nil ? "" : basePath, pNewFile->fFileName);
    pNewFile->SetParent(pSubdir);

    /*
     * Because we're hierarchical, and we guarantee that the contents of
     * subdirectories are grouped together, we must insert the file into an
     * appropriate place in the list rather than just throwing it onto the
     * end.
     *
     * The proper location for the new file in the linear list is in sorted
     * order with the files in the current directory.  We have to be careful
     * here because libhfs is going to use Macintosh Roman sort ordering,
     * which may be different from ASCII ordering.  Worst case: we end up
     * putting it in the wrong place and it jumps around when the disk image
     * is reopened.
     *
     * All files in a subdir appear in the list after that subdir, but there
     * might be intervening entries from deeper directories.  So we have to
     * chase through some or all of the file list to find the right place.
     * Not great, but we don't have enough files or do adds often enough to
     * make this worth optimizing.
     */
    A2File* pLastSubdirFile;
    A2File* pPrevFile;
    A2File* pNextFile;

    pPrevFile = pLastSubdirFile = pSubdir;
    pNextFile = GetNextFile(pPrevFile);
    while (pNextFile != nil) {
        if (pNextFile->GetParent() == pNewFile->GetParent()) {
            /* in same subdir, compare names */
            if (CompareMacFileNames(pNextFile->GetPathName(),
                pNewFile->GetPathName()) > 0)
            {
                /* passed it; insert new after previous file */
                pLastSubdirFile = pPrevFile;
                WMSG2("  HFS Found '%s' > cur(%s)\n", pNextFile->GetPathName(),
                    pNewFile->GetPathName());
                break;
            }

            /* still too early; save in case it's last one in dir */
            pLastSubdirFile = pNextFile;
        }
        pPrevFile = pNextFile;
        pNextFile = GetNextFile(pNextFile);
    }

    /* insert us after last file we saw that was part of the same subdir */
    WMSG2("  HFS inserting '%s' after '%s'\n", pNewFile->GetPathName(),
        pLastSubdirFile->GetPathName());
    InsertFileInList(pNewFile, pLastSubdirFile);
    //WMSG0("LIST NOW:\n");
    //DumpFileList();

    *ppNewFile = pNewFile;
    pNewFile = nil;

bail:
    delete pNewFile;
    delete[] normalizedPath;
    delete[] basePath;
    delete[] fileName;
    delete[] fullPath;
    hfs_flush(fHfsVol);
    WMSG1(" HFS ---^--- CreateFile '%s' DONE\n", pParms->pathName);
    return dierr;
}

/*
 * Delete the named file.
 *
 * We need to use a different call for file vs. directory.
 */
DIError
DiskFSHFS::DeleteFile(A2File* pGenericFile)
{
    DIError dierr = kDIErrNone;
    char* pathName = nil;

    if (fpImg->GetReadOnly())
        return kDIErrAccessDenied;
    if (!fDiskIsGood)
        return kDIErrBadDiskImage;
    if (pGenericFile->IsFileOpen())
        return kDIErrFileOpen;

    A2FileHFS* pFile = (A2FileHFS*) pGenericFile;
    pathName = pFile->GetLibHFSPathName();
    WMSG1("    Deleting '%s'\n", pathName);

    if (pFile->IsDirectory()) {
        if (hfs_rmdir(fHfsVol, pathName) != 0) {
            WMSG2(" HFS rmdir failed '%s': '%s'\n", pathName, hfs_error);
            dierr = kDIErrGeneric;
            goto bail;
        }
    } else {
        if (hfs_delete(fHfsVol, pathName) != 0) {
            WMSG2(" HFS delete failed '%s': '%s'\n", pathName, hfs_error);
            dierr = kDIErrGeneric;
            goto bail;
        }
    }

    /*
     * Remove the A2File* from the list.
     */
    DeleteFileFromList(pFile);

bail:
    hfs_flush(fHfsVol);
    delete[] pathName;
    return kDIErrNone;
}

/*
 * Rename a file.
 *
 * Pass in a pointer to the file and a string with the new filename (just
 * the filename, not a pathname -- this function doesn't move files
 * between directories).  The new name must already be normalized.
 *
 * Renaming the magic volume directory "file" is not allowed.
 *
 * We don't try to keep AppleWorks aux type flags consistent (they're used
 * to determine which characters are lower case on ProDOS disks).  They'll
 * get fixed up when we copy them to a ProDOS disk, which is the only way
 * 8-bit AppleWorks can get at them.
 */
DIError
DiskFSHFS::RenameFile(A2File* pGenericFile, const char* newName)
{
    DIError dierr = kDIErrNone;
    A2FileHFS* pFile = (A2FileHFS*) pGenericFile;
    char* colonOldName = nil;
    char* colonNewName = nil;

    if (pFile == nil || newName == nil)
        return kDIErrInvalidArg;
    if (!IsValidFileName(newName))
        return kDIErrInvalidArg;
    if (pFile->IsVolumeDirectory())
        return kDIErrInvalidArg;
    if (fpImg->GetReadOnly())
        return kDIErrAccessDenied;
    if (!fDiskIsGood)
        return kDIErrBadDiskImage;

    char* lastColon;

    colonOldName = pFile->GetLibHFSPathName();  // adds ':' to start of string
    lastColon = strrchr(colonOldName, A2FileHFS::kFssep);
    assert(lastColon != nil);
    if (lastColon == colonOldName) {
        /* in root dir */
        colonNewName = new char[1 + strlen(newName) +1];
        colonNewName[0] = A2FileHFS::kFssep;
        strcpy(colonNewName+1, newName);
    } else {
        /* prepend subdir */
        int len = lastColon - colonOldName +1;  // e.g. ":path1:path2:"
        colonNewName = new char[len + strlen(newName) +1];
        strncpy(colonNewName, colonOldName, len);
        strcpy(colonNewName+len, newName);
    }

    WMSG2(" HFS renaming '%s' to '%s'\n", colonOldName, colonNewName);

    if (hfs_rename(fHfsVol, colonOldName, colonNewName) != 0) {
        WMSG3(" HFS rename('%s','%s') failed: %s\n",
            colonOldName, colonNewName, hfs_error);
        dierr = kDIErrGeneric;
        goto bail;
    }

    /*
     * Success!  Update the file name.
     */
    strcpy(pFile->fFileName, newName);

    /*
     * Now the fun part.  If we simply renamed a file, we can just update the
     * one entry.  If we renamed a directory, life gets interesting because
     * we store the full pathname in every A2FileHFS entry.  (It's an
     * efficiency win most of the time, but it's really annoying here.)
     *
     * HFS makes this especially unpleasant because it keeps the files
     * arranged in sorted order.  If we change a file's name, we may have to
     * move it to a new position in the linear file list.  If we don't, the
     * list no longer reflects the order in which the files actually appear
     * on the disk, and they'll shift around when we reload.
     *
     * There are two approaches: re-sort the list (awkward, since it's stored
     * in a linked list -- we'd probably want to sort tags in a parallel
     * structure), or find the affected block of files, find the new start
     * position, and shift the entire range in one shot.
     *
     * This doesn't seem like something that anybody but me will ever care
     * about, so I'm going to skip it for now.
     */
    A2File* pCur;
    if (pFile->IsDirectory()) {
        /* do all files that come after us */
        pCur = pFile;
        while (pCur != nil) {
            RegeneratePathName((A2FileHFS*) pCur);
            pCur = GetNextFile(pCur);
        }
    } else {
        RegeneratePathName(pFile);
    }

bail:
    delete[] colonOldName;
    delete[] colonNewName;
    hfs_flush(fHfsVol);
    return kDIErrNone;
}

/*
 * Regenerate fPathName for the specified file.
 *
 * Has no effect on the magic volume dir entry.
 *
 * This could be implemented more efficiently, but it's only used when
 * renaming files, so there's not much point.
 *
 * [This was lifted straight out of the ProDOS sources.  It should probably
 * be moved into generic DiskFS.]
 */
DIError
DiskFSHFS::RegeneratePathName(A2FileHFS* pFile)
{
    A2FileHFS* pParent;
    char* buf = nil;
    int len;

    /* nothing to do here */
    if (pFile->IsVolumeDirectory())
        return kDIErrNone;

    /* compute the length of the path name */
    len = strlen(pFile->GetFileName());
    pParent = (A2FileHFS*) pFile->GetParent();
    while (!pParent->IsVolumeDirectory()) {
        len++;      // leave space for the ':'
        len += strlen(pParent->GetFileName());

        pParent = (A2FileHFS*) pParent->GetParent();
    }

    buf = new char[len+1];
    if (buf == nil)
        return kDIErrMalloc;

    /* generate the new path name */
    int partLen;
    partLen = strlen(pFile->GetFileName());
    strcpy(buf + len - partLen, pFile->GetFileName());
    len -= partLen;

    pParent = (A2FileHFS*) pFile->GetParent();
    while (!pParent->IsVolumeDirectory()) {
        assert(len > 0);
        buf[--len] = A2FileHFS::kFssep;

        partLen = strlen(pParent->GetFileName());
        strncpy(buf + len - partLen, pParent->GetFileName(), partLen);
        len -= partLen;
        assert(len >= 0);

        pParent = (A2FileHFS*) pParent->GetParent();
    }

    WMSG2("Replacing '%s' with '%s'\n", pFile->GetPathName(), buf);
    pFile->SetPathName("", buf);
    delete[] buf;

    return kDIErrNone;
}

/*
 * Change the HFS volume name.
 *
 * This uses the same libhfs interface that we use for renaming files.  The
 * Mac convention is to *not* start the volume name with a colon.  In fact,
 * the libhfs convention is to *end* the volume names with a colon.
 */
DIError
DiskFSHFS::RenameVolume(const char* newName)
{
    DIError dierr = kDIErrNone;
    A2FileHFS* pFile;
    char* oldNameColon = nil;
    char* newNameColon = nil;

    if (!IsValidVolumeName(newName))
        return kDIErrInvalidArg;
    if (fpImg->GetReadOnly())
        return kDIErrAccessDenied;

    /* get file list entry for volume name */
    pFile = (A2FileHFS*) GetNextFile(nil);
    assert(strcmp(pFile->GetFileName(), fVolumeName) == 0);

    oldNameColon = new char[strlen(fVolumeName)+2];
    strcpy(oldNameColon, fVolumeName);
    strcat(oldNameColon, ":");
    newNameColon = new char[strlen(newName)+2];
    strcpy(newNameColon, newName);
    strcat(newNameColon, ":");

    if (hfs_rename(fHfsVol, oldNameColon, newNameColon) != 0) {
        WMSG3(" HFS rename '%s' -> '%s' failed: %s\n",
            oldNameColon, newNameColon, hfs_error);
        dierr = kDIErrGeneric;
        goto bail;
    }

    /* update stuff */
    strcpy(fVolumeName, newName);
    SetVolumeID();
    strcpy(pFile->fFileName, newName);
    pFile->SetPathName("", newName);

bail:
    delete[] oldNameColon;
    delete[] newNameColon;
    hfs_flush(fHfsVol);
    return dierr;
}

/*
 * Set file attributes.
 */
DIError
DiskFSHFS::SetFileInfo(A2File* pGenericFile, long fileType, long auxType,
    long accessFlags)
{
    DIError dierr = kDIErrNone;
    A2FileHFS* pFile = (A2FileHFS*) pGenericFile;
    hfsdirent dirEnt;
    char* colonPath;

    if (fpImg->GetReadOnly())
        return kDIErrAccessDenied;
    if (pFile == nil)
        return kDIErrInvalidArg;
    if (pFile->IsDirectory() || pFile->IsVolumeDirectory())
        return kDIErrNone;      // impossible; just ignore it

    colonPath = pFile->GetLibHFSPathName();

    if (hfs_stat(fHfsVol, colonPath, &dirEnt) != 0) {
        WMSG2(" HFS unable to stat '%s': %s\n", colonPath, hfs_error);
        dierr = kDIErrGeneric;
        goto bail;
    }

    A2FileHFS::ConvertTypeToHFS(fileType, auxType,
                    dirEnt.u.file.type, dirEnt.u.file.creator);

    if (accessFlags & A2FileProDOS::kAccessInvisible)
        dirEnt.fdflags |= HFS_FNDR_ISINVISIBLE;
    else
        dirEnt.fdflags &= ~HFS_FNDR_ISINVISIBLE;

    if ((accessFlags & ~A2FileProDOS::kAccessInvisible) == kFileAccessLocked)
        dirEnt.flags |= HFS_ISLOCKED;
    else
        dirEnt.flags &= ~HFS_ISLOCKED;

    WMSG3(" HFS setting '%s' to fdflags=0x%04x flags=0x%04x\n",
        colonPath, dirEnt.fdflags, dirEnt.flags);
    WMSG2("  type=0x%08lx creator=0x%08lx\n", fileType, auxType);

    if (hfs_setattr(fHfsVol, colonPath, &dirEnt) != 0) {
        WMSG2(" HFS setattr '%s' failed: %s\n", colonPath, hfs_error);
        dierr = kDIErrGeneric;
        goto bail;
    }

    /* update our local copy */
    pFile->fType = fileType;
    pFile->fCreator = auxType;
    pFile->fAccess = accessFlags;   // should actually base them on HFS vals

bail:
    delete[] colonPath;
    hfs_flush(fHfsVol);
    return dierr;
}

#endif // !EXCISE_GPL_CODE


/*
 * ===========================================================================
 *      A2FileHFS
 * ===========================================================================
 */

/*
 * Dump the contents of the A2File structure.
 */
void
A2FileHFS::Dump(void) const
{
    WMSG1("A2FileHFS '%s'\n", fFileName);
}

/* convert hex to decimal */
inline int FromHex(char hexVal)
{
    if (hexVal >= '0' && hexVal <= '9')
        return hexVal - '0';
    else if (hexVal >= 'a' && hexVal <= 'f')
        return hexVal -'a' + 10;
    else if (hexVal >= 'A' && hexVal <= 'F')
        return hexVal - 'A' + 10;
    else
        return -1;
}

/*
 * If this has a ProDOS filetype, convert it.
 *
 * This stuff is defined in Technical Note PT515, "Apple File Exchange Q&As".
 * In theory we should convert type=BINA and type=TEXT regardless of the
 * creator, but since those just go to generic text/binary types I don't
 * think we need to handle it here (and I'm more comfortable leaving them
 * with their Macintosh creators).
 *
 * In some respects, converting to ProDOS types is a bad idea, because we
 * don't have a 1:1 mapping.  If we copy a pdos/p\0\0\0 file we will store it
 * as pdos/BINA instead.  In practice, for the Apple II world they are
 * equivalent, and CiderPress really doesn't need the "raw" file type.  If
 * it becomes annoying, we can add a DiskFSParameter to control it.
 */
long A2FileHFS::GetFileType(void) const
{
    if (fCreator != kPdosType)
        return fType;

    if ((fType & 0xffff) == 0x2020) {
        // 'XY  ', where XY are hex digits for ProDOS file type
        int digit1, digit2;

        digit1 = FromHex((char) (fType >> 24));
        digit2 = FromHex((char) (fType >> 16));
        if (digit1 < 0 || digit2 < 0) {
            WMSG1("  Unexpected: pdos + %08lx\n", fType);
            return 0x00;
        }
        return digit1 << 4 | digit2;
    }

    unsigned char flag = (unsigned char)(fType >> 24);
    if (flag == 0x70) {     // 'p'
        /* type and aux embedded within */
        return (fType >> 16) & 0xff;
    } else {
        /* type stored as a string */
        if (fType == 0x42494e41)        // 'BINA'
            return 0x00;                // NON
        else if (fType == 0x54455854)   // 'TEXT'
            return 0x04;
        else if (fType == 0x50535953)   // 'PSYS'
            return 0xff;
        else if (fType == 0x50533136)   // 'PS16'
            return 0xb3;
        else
            return 0x00;
    }
};

/*
 * If this has a ProDOS aux type, convert it.
 */
long A2FileHFS::GetAuxType(void) const
{
    if (fCreator != kPdosType)
        return fCreator;

    unsigned char flag = (unsigned char)(fType >> 24);
    if (flag == 0x70) {     // 'p'
        /* type and aux embedded within */
        return fType & 0xffff;
    } else {
        return 0x0000;
    }
}

/*
 * Set the full pathname to a combination of the base path and the
 * current file's name.
 *
 * If we're in the volume directory, pass in "" for the base path (not nil).
 */
void
A2FileHFS::SetPathName(const char* basePath, const char* fileName)
{
    assert(basePath != nil && fileName != nil);
    if (fPathName != nil)
        delete[] fPathName;

    // strip leading ':' (but treat ":" specially for volume dir entry)
    if (basePath[0] == ':' && basePath[1] != '\0')
        basePath++;

    int baseLen = strlen(basePath);
    fPathName = new char[baseLen + 1 + strlen(fileName)+1];
    strcpy(fPathName, basePath);
    if (baseLen != 0 &&
        !(baseLen == 1 && basePath[0] == ':'))
    {
        *(fPathName + baseLen) = kFssep;
        baseLen++;
    }
    strcpy(fPathName + baseLen, fileName);
}


#ifndef EXCISE_GPL_CODE

/*
 * Return a copy of the pathname that libhfs will like.
 *
 * The caller must delete[] the return value.
 */
char*
A2FileHFS::GetLibHFSPathName(void) const
{
    char* nameBuf;

    nameBuf = new char[strlen(fPathName)+2];
    nameBuf[0] = kFssep;
    strcpy(nameBuf+1, fPathName);

    return nameBuf;
}

/*
 * Convert numeric file/aux type to HFS strings.  "pType" and "pCreator" must
 * be able to hold 5 bytes each (4-byte type + nul).
 *
 * Follows the PT515 recommendations, mostly.  The "PSYS" and "PS16"
 * conversions discard the file's aux type and therefore are unsuitable,
 * and the conversion of SRC throws away its identity.
 */
/*static*/ void
A2FileHFS::ConvertTypeToHFS(long fileType, long auxType,
        char* pType, char* pCreator)
{
    if (fileType == 0x00 && auxType == 0x0000) {
        strcpy(pCreator, "pdos");
        strcpy(pType, "BINA");
    } else if (fileType == 0x04 && auxType == 0x0000) {
        strcpy(pCreator, "pdos");
        strcpy(pType, "TEXT");
    } else if (fileType >= 0 && fileType <= 0xff &&
        auxType >= 0 && auxType <= 0xffff)
    {
        pType[0] = 'p';
        pType[1] = (unsigned char) fileType;
        pType[2] = (unsigned char) (auxType >> 8);
        pType[3] = (unsigned char) auxType;
        pType[4] = '\0';
        pCreator[0] = 'p';
        pCreator[1] = 'd';
        pCreator[2] = 'o';
        pCreator[3] = 's';
        pCreator[4] = '\0';
    } else {
        pType[0] = (unsigned char)(fileType >> 24);
        pType[1] = (unsigned char)(fileType >> 16);
        pType[2] = (unsigned char)(fileType >> 8);
        pType[3] = (unsigned char) fileType;
        pType[4] = '\0';
        pCreator[0] = (unsigned char)(auxType >> 24);
        pCreator[1] = (unsigned char)(auxType >> 16);
        pCreator[2] = (unsigned char)(auxType >> 8);
        pCreator[3] = (unsigned char) auxType;
        pCreator[4] = '\0';
    }
}


/*
 * Open a file through libhfs.
 *
 * libhfs wants filenames to begin with ':' unless they start with the
 * name of the volume.  This is the opposite of the convention followed
 * by the rest of CiderPress (and most of the civilized world), so instead
 * of storing the pathname that way we just tack it on here.
 */
DIError
A2FileHFS::Open(A2FileDescr** ppOpenFile, bool readOnly,
    bool rsrcFork /*=false*/)
{
    DIError dierr = kDIErrNone;
    A2FDHFS* pOpenFile = nil;
    hfsfile* pHfsFile;
    char* nameBuf = nil;

    if (fpOpenFile != nil)
        return kDIErrAlreadyOpen;
    //if (rsrcFork && fRsrcLength < 0)
    //  return kDIErrForkNotFound;

    nameBuf = GetLibHFSPathName();

    DiskFSHFS* pDiskFS = (DiskFSHFS*) GetDiskFS();
    pHfsFile = hfs_open(pDiskFS->GetHfsVol(), nameBuf);
    if (pHfsFile == NULL) {
        WMSG2(" HFS hfs_open(%s) failed: %s\n", nameBuf, hfs_error);
        dierr = kDIErrGeneric;  // better value might be in errno
        goto bail;
    }
    hfs_setfork(pHfsFile, rsrcFork ? 1 : 0);

    pOpenFile = new A2FDHFS(this, pHfsFile);

    fpOpenFile = pOpenFile;
    *ppOpenFile = pOpenFile;

bail:
    delete[] nameBuf;
    return dierr;
}


/*
 * ===========================================================================
 *      A2FDHFS
 * ===========================================================================
 */

/*
 * Read a chunk of data from the fake file.
 */
DIError
A2FDHFS::Read(void* buf, size_t len, size_t* pActual)
{
    long result;

    WMSG3(" HFS reading %d bytes from '%s' (offset=%ld)\n",
        len, fpFile->GetPathName(), hfs_seek(fHfsFile, 0, HFS_SEEK_CUR));

    //A2FileHFS* pFile = (A2FileHFS*) fpFile;

    result = hfs_read(fHfsFile, buf, len);
    if (result < 0)
        return kDIErrReadFailed;

    if (pActual != nil) {
        *pActual = (size_t) result;
    } else if (result != (long) len) {
        // short read, can't report it, return error
        return kDIErrDataUnderrun;
    }

    /*
     * To do this right we need to break the hfs_read() into smaller
     * pieces.  However, it only really affects us for files that are
     * getting reformatted, because that's the only time we grab the
     * entire thing in one big piece.
     */
    long offset = hfs_seek(fHfsFile, 0, HFS_SEEK_CUR);
    if (!UpdateProgress(offset)) {
        return kDIErrCancelled;
    }

    return kDIErrNone;
}

/*
 * Write data at the current offset.
 *
 * (In the current implementation, the entire file is always written in
 * one piece.  This function does work correctly with multiple smaller
 * pieces though, because it lets libhfs do all the work.)
 */
DIError
A2FDHFS::Write(const void* buf, size_t len, size_t* pActual)
{
    long result;

    WMSG3(" HFS writing %d bytes to '%s' (offset=%ld)\n",
        len, fpFile->GetPathName(), hfs_seek(fHfsFile, 0, HFS_SEEK_CUR));

    fModified = true;       // assume something gets changed

    //A2FileHFS* pFile = (A2FileHFS*) fpFile;

    result = hfs_write(fHfsFile, buf, len);
    if (result < 0)
        return kDIErrWriteFailed;

    if (pActual != nil) {
        *pActual = (size_t) result;
    } else if (result != (long) len) {
        // short write, can't report it, return error
        return kDIErrDataUnderrun;
    }

    /* to make this work right, we need to break hfs_write into pieces */
    long offset = hfs_seek(fHfsFile, 0, HFS_SEEK_CUR);
    if (!UpdateProgress(offset)) {
        return kDIErrCancelled;
    }

    /*
     * We don't hfs_flush here, because we don't expect the application to
     * hold the file open, and we flush in Close().
     */

    return kDIErrNone;
}

/*
 * Seek to a new offset.
 */
DIError
A2FDHFS::Seek(di_off_t offset, DIWhence whence)
{
    int hfsWhence;
    unsigned long result;

    switch (whence) {
    case kSeekSet:      hfsWhence = HFS_SEEK_SET;   break;
    case kSeekEnd:      hfsWhence = HFS_SEEK_END;   break;
    case kSeekCur:      hfsWhence = HFS_SEEK_CUR;   break;
    default:
        assert(false);
        return kDIErrInvalidArg;
    }

    result = hfs_seek(fHfsFile, (long) offset, hfsWhence);
    if (result == (unsigned long) -1) {
        DebugBreak();
        return kDIErrGeneric;
    }
    return kDIErrNone;
}

/*
 * Return current offset.
 */
di_off_t
A2FDHFS::Tell(void)
{
    di_off_t offset;

    /* get current position without moving pointer */
    offset = hfs_seek(fHfsFile, 0, HFS_SEEK_CUR);
    return offset;
}

/*
 * Release file state, and tell our parent to destroy us.
 */
DIError
A2FDHFS::Close(void)
{
    hfsdirent dirEnt;

    /*
     * If the file was written to, update our info.
     */
    if (fModified) {
        if (hfs_fstat(fHfsFile, &dirEnt) == 0) {
            A2FileHFS* pFile = (A2FileHFS*) fpFile;
            pFile->fDataLength = dirEnt.u.file.dsize;
            pFile->fRsrcLength = dirEnt.u.file.rsize;
            if (pFile->fRsrcLength == 0)
                pFile->fRsrcLength = -1;
            WMSG2(" HFS close set dataLen=%ld rsrcLen=%ld\n",
                (long) pFile->fDataLength, (long) pFile->fRsrcLength);
        } else {
            WMSG1(" HFS Close fstat failed: %s\n", hfs_error);
            // close it anyway
        }
    }

    hfs_close(fHfsFile);
    fHfsFile = nil;

    /* flush changes */
    if (fModified) {
        DiskFSHFS* pDiskFS = (DiskFSHFS*) fpFile->GetDiskFS();

        if (hfs_flush(pDiskFS->GetHfsVol()) != 0) {
            WMSG0("HEY: Close flush failed!\n");
            DebugBreak();
        }
    }

    fpFile->CloseDescr(this);
    return kDIErrNone;
}

/*
 * Return the #of sectors/blocks in the file.  Not supported, but since HFS
 * doesn't support "sparse" files we can fake it.
 */
long
A2FDHFS::GetSectorCount(void) const
{
    A2FileHFS* pFile = (A2FileHFS*) fpFile;
    return (long) ((pFile->fDataLength+255) / 256 +
                   (pFile->fRsrcLength+255) / 256);
}
long
A2FDHFS::GetBlockCount(void) const
{
    A2FileHFS* pFile = (A2FileHFS*) fpFile;
    return (long) ((pFile->fDataLength+511) / 512 +
                   (pFile->fRsrcLength+511) / 512);
}

/*
 * Return the Nth track/sector in this file.  Not supported.
 */
DIError
A2FDHFS::GetStorage(long sectorIdx, long* pTrack, long* pSector) const
{
    return kDIErrNotSupported;
}
/*
 * Return the Nth 512-byte block in this file.  Not supported.
 */
DIError
A2FDHFS::GetStorage(long blockIdx, long* pBlock) const
{
    return kDIErrNotSupported;
}




#else // EXCISE_GPL_CODE  -----------------------------------------------------

/*
 * Get things rolling.
 *
 * Since we're assured that this is a valid disk, errors encountered from here
 * on out must be handled somehow, possibly by claiming that the disk is
 * completely full and has no files on it.
 */
DIError
DiskFSHFS::Initialize(InitMode initMode)
{
    DIError dierr = kDIErrNone;

    dierr = LoadVolHeader();
    if (dierr != kDIErrNone)
        goto bail;
    DumpVolHeader();

    CreateFakeFile();

    SetVolumeUsageMap();

bail:
    return dierr;
}


/*
 * Fill a buffer with some interesting stuff, and add it to the file list.
 */
void
DiskFSHFS::CreateFakeFile(void)
{
    A2FileHFS* pFile;
    char buf[768];      // currently running about 475
    static const char* kFormatMsg =
"The Macintosh HFS filesystem is not supported.  CiderPress knows how to\r"
"recognize HFS volumes so that it can identify partitions on CFFA-formatted\r"
"CompactFlash cards and Apple II CD-ROMs, but the current version does not\r"
"know how to view or extract files.\r"
"\r"
"Some information about this HFS volume:\r"
"\r"
"  Volume name       : '%s'\r"
"  Storage capacity  : %ld blocks (%.2fMB)\r"
"  Number of files   : %ld\r"
"  Number of folders : %ld\r"
"  Last modified     : %s\r"
"\r"
;
    char dateBuf[32];
    long capacity;
    const char* timeStr;

    capacity = (fAllocationBlockSize / kBlkSize) * fNumAllocationBlocks;

    /* get the mod time, format it, and remove the trailing '\n' */
    time_t when =
        (time_t) (fModifiedDateTime - kDateTimeOffset - fLocalTimeOffset);
    timeStr = ctime(&when);
    if (timeStr == nil) {
        WMSG2("Invalid date %ld (orig=%ld)\n", when, fModifiedDateTime);
        strcpy(dateBuf, "<no date>");
    } else
        strncpy(dateBuf, timeStr, sizeof(dateBuf));
    int len = strlen(dateBuf);
    if (len > 0)
        dateBuf[len-1] = '\0';

    memset(buf, 0, sizeof(buf));
    sprintf(buf, kFormatMsg,
        fVolumeName,
        capacity,
        (double) capacity / 2048.0,
        fNumFiles,
        fNumDirectories,
        dateBuf);

    pFile = new A2FileHFS(this);
    pFile->fIsDir = false;
    pFile->fIsVolumeDir = false;
    pFile->fType = 0;
    pFile->fCreator = 0;
    pFile->SetFakeFile(buf, strlen(buf));
    strcpy(pFile->fFileName, "(not supported)");
    pFile->SetPathName("", pFile->fFileName);
    pFile->fDataLength = 0;
    pFile->fRsrcLength = -1;
    pFile->fCreateWhen = 0;
    pFile->fModWhen = 0;

    pFile->SetFakeFile(buf, strlen(buf));

    AddFileToList(pFile);
}

/*
 * We could do this, but there's not much point.
 */
DIError GetFreeSpaceCount(long* pTotalUnits, long* pFreeUnits,
    int* pUnitSize) const
{
    return kDIErrNotSupported;
}

/*
 * Not a whole lot to do.
 */
DIError
A2FileHFS::Open(A2FileDescr** ppOpenFile, bool readOnly,
    bool rsrcFork /*=false*/)
{
    A2FDHFS* pOpenFile = nil;

    if (fpOpenFile != nil)
        return kDIErrAlreadyOpen;
    if (rsrcFork && fRsrcLength < 0)
        return kDIErrForkNotFound;
    assert(readOnly == true);

    pOpenFile = new A2FDHFS(this, nil);

    fpOpenFile = pOpenFile;
    *ppOpenFile = pOpenFile;

    return kDIErrNone;
}


/*
 * ===========================================================================
 *      A2FDHFS
 * ===========================================================================
 */

/*
 * Read a chunk of data from the fake file.
 */
DIError
A2FDHFS::Read(void* buf, size_t len, size_t* pActual)
{
    WMSG3(" HFS reading %d bytes from '%s' (offset=%ld)\n",
        len, fpFile->GetPathName(), (long) fOffset);

    A2FileHFS* pFile = (A2FileHFS*) fpFile;

    /* don't allow them to read past the end of the file */
    if (fOffset + (long)len > pFile->fDataLength) {
        if (pActual == nil)
            return kDIErrDataUnderrun;
        len = (size_t) (pFile->fDataLength - fOffset);
    }
    if (pActual != nil)
        *pActual = len;

    memcpy(buf, pFile->GetFakeFileBuf(), len);

    fOffset += len;

    return kDIErrNone;
}

/*
 * Write data at the current offset.
 */
DIError
A2FDHFS::Write(const void* buf, size_t len, size_t* pActual)
{
    return kDIErrNotSupported;
}

/*
 * Seek to a new offset.
 */
DIError
A2FDHFS::Seek(di_off_t offset, DIWhence whence)
{
    di_off_t fileLen = ((A2FileHFS*) fpFile)->fDataLength;

    switch (whence) {
    case kSeekSet:
        if (offset < 0 || offset > fileLen)
            return kDIErrInvalidArg;
        fOffset = offset;
        break;
    case kSeekEnd:
        if (offset > 0 || offset < -fileLen)
            return kDIErrInvalidArg;
        fOffset = fileLen + offset;
        break;
    case kSeekCur:
        if (offset < -fOffset ||
            offset >= (fileLen - fOffset))
        {
            return kDIErrInvalidArg;
        }
        fOffset += offset;
        break;
    default:
        assert(false);
        return kDIErrInvalidArg;
    }

    assert(fOffset >= 0 && fOffset <= fileLen);
    return kDIErrNone;
}

/*
 * Return current offset.
 */
di_off_t
A2FDHFS::Tell(void)
{
    return fOffset;
}

/*
 * Release file state, and tell our parent to destroy us.
 */
DIError
A2FDHFS::Close(void)
{
    fpFile->CloseDescr(this);
    return kDIErrNone;
}

/*
 * Return the #of sectors/blocks in the file.
 */
long
A2FDHFS::GetSectorCount(void) const
{
    A2FileHFS* pFile = (A2FileHFS*) fpFile;
    return (long) ((pFile->fDataLength+255) / 256);
}
long
A2FDHFS::GetBlockCount(void) const
{
    A2FileHFS* pFile = (A2FileHFS*) fpFile;
    return (long) ((pFile->fDataLength+511) / 512);
}

/*
 * Return the Nth track/sector in this file.
 */
DIError
A2FDHFS::GetStorage(long sectorIdx, long* pTrack, long* pSector) const
{
    return kDIErrNotSupported;
}
/*
 * Return the Nth 512-byte block in this file.
 */
DIError
A2FDHFS::GetStorage(long blockIdx, long* pBlock) const
{
    return kDIErrNotSupported;
}

#endif  // EXCISE_GPL_CODE ---------------------------------------------------
