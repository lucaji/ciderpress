/*
 * CiderPress
 * Copyright (C) 2007 by faddenSoft, LLC.  All Rights Reserved.
 * See the file LICENSE for distribution terms.
 */
/*
 * Implementation of ImageFormatDialog class.
 */
#include "stdafx.h"
#include "ImageFormatDialog.h"
#include "HelpTopics.h"
#include "../diskimg/DiskImg.h"


BEGIN_MESSAGE_MAP(ImageFormatDialog, CDialog)
    ON_BN_CLICKED(IDC_DECONF_HELP, OnHelp)
    ON_WM_HELPINFO()
END_MESSAGE_MAP()


/*
 * Conversion tables.
 *
 * If you add something else, remember to turn off "sort" in the drop list.
 *
 * The tables contain only the formats we currently support, and are in the
 * order in which we want to present them to the user.
 *
 * THOUGHT: drop "name" from the tables, and use a DiskImg::ToString lookup
 * to get the text string.  That way we'd be consistent.
 */
typedef struct ImageFormatDialog::ConvTable {
    int          enumval;       // a DiskImg::enum type
    const WCHAR* name;
} ConvTable;

const int kLastEntry = -1;

/* DiskImg::OuterFormat */
static const ConvTable gOuterFormats[] = {
    { DiskImg::kOuterFormatUnknown,         L"Unknown format" },
    { DiskImg::kOuterFormatNone,            L"(none)" },
//  { DiskImg::kOuterFormatCompress,        L"UNIX compress" },
    { DiskImg::kOuterFormatGzip,            L"gzip" },
//  { DiskImg::kOuterFormatBzip2,           L"bzip2" },
    { DiskImg::kOuterFormatZip,             L"Zip archive" },
    { kLastEntry,                           nil }
};
/* DiskImg::FileFormat */
static const ConvTable gFileFormats[] = {
    { DiskImg::kFileFormatUnknown,          L"Unknown format" },
    { DiskImg::kFileFormatUnadorned,        L"Unadorned raw data" },
    { DiskImg::kFileFormat2MG,              L"2MG" },
    { DiskImg::kFileFormatNuFX,             L"NuFX (ShrinkIt)" },
    { DiskImg::kFileFormatDiskCopy42,       L"DiskCopy 4.2" },
//  { DiskImg::kFileFormatDiskCopy60,       L"DiskCopy 6.0" },
//  { DiskImg::kFileFormatDavex,            L"Davex volume image" },
    { DiskImg::kFileFormatSim2eHDV,         L"Sim //e HDV" },
    { DiskImg::kFileFormatDDD,              L"DDD" },
    { DiskImg::kFileFormatTrackStar,        L"TrackStar image" },
    { DiskImg::kFileFormatFDI,              L"FDI image" },
//  { DiskImg::kFileFormatDDDDeluxe,        L"DDDDeluxe" },
    { kLastEntry,                           nil }
};
/* DiskImg::PhysicalFormat */
static const ConvTable gPhysicalFormats[] = {
    { DiskImg::kPhysicalFormatUnknown,      L"Unknown format" },
    { DiskImg::kPhysicalFormatSectors,      L"Sectors" },
    { DiskImg::kPhysicalFormatNib525_6656,  L"Raw nibbles (6656-byte)" },
    { DiskImg::kPhysicalFormatNib525_6384,  L"Raw nibbles (6384-byte)" },
    { DiskImg::kPhysicalFormatNib525_Var,   L"Raw nibbles (variable len)" },
    { kLastEntry,                           nil }
};
/* DiskImg::SectorOrder */
static const ConvTable  gSectorOrders[] = {
    { DiskImg::kSectorOrderUnknown,         L"Unknown ordering" },
    { DiskImg::kSectorOrderProDOS,          L"ProDOS block ordering" },
    { DiskImg::kSectorOrderDOS,             L"DOS sector ordering" },
    { DiskImg::kSectorOrderCPM,             L"CP/M block ordering" },
    { DiskImg::kSectorOrderPhysical,        L"Physical sector ordering" },
    { kLastEntry,                           nil }
};
/* DiskImg::FSFormat */
static const ConvTable gFSFormats[] = {
    { DiskImg::kFormatUnknown,              L"Unknown filesystem" },
    { DiskImg::kFormatGenericDOSOrd,        L"Generic DOS sectors" },
    { DiskImg::kFormatGenericProDOSOrd,     L"Generic ProDOS blocks" },
    { DiskImg::kFormatGenericPhysicalOrd,   L"Generic raw sectors" },
    { DiskImg::kFormatGenericCPMOrd,        L"Generic CP/M blocks" },
    { DiskImg::kFormatProDOS,               L"ProDOS" },
    { DiskImg::kFormatDOS33,                L"DOS 3.3" },
    { DiskImg::kFormatDOS32,                L"DOS 3.2" },
    { DiskImg::kFormatPascal,               L"Pascal" },
    { DiskImg::kFormatMacHFS,               L"HFS" },
//  { DiskImg::kFormatMacMFS,               L"MFS" },
//  { DiskImg::kFormatLisa,                 L"Lisa" },
    { DiskImg::kFormatCPM,                  L"CP/M" },
    { DiskImg::kFormatMSDOS,                L"MS-DOS FAT" },
//  { DiskImg::kFormatISO9660,              L"ISO-9660" },
    { DiskImg::kFormatUNIDOS,               L"UNIDOS (400K DOS x2)" },
    { DiskImg::kFormatOzDOS,                L"OzDOS (400K DOS x2)" },
    { DiskImg::kFormatCFFA4,                L"CFFA (4 or 6 partitions)" },
    { DiskImg::kFormatCFFA8,                L"CFFA (8 partitions)" },
    { DiskImg::kFormatMacPart,              L"Macintosh partitioned disk" },
    { DiskImg::kFormatMicroDrive,           L"MicroDrive partitioned disk" },
    { DiskImg::kFormatFocusDrive,           L"FocusDrive partitioned disk" },
    { DiskImg::kFormatRDOS33,               L"RDOS 3.3 (16-sector)" },
    { DiskImg::kFormatRDOS32,               L"RDOS 3.2 (13-sector)" },
    { DiskImg::kFormatRDOS3,                L"RDOS 3 (cracked 13-sector)" },
    { kLastEntry,                           nil }
};


/*
 * Initialize our members by querying the associated DiskImg.
 */
void
ImageFormatDialog::InitializeValues(const DiskImg* pImg)
{
    fOuterFormat = pImg->GetOuterFormat();
    fFileFormat = pImg->GetFileFormat();
    fPhysicalFormat = pImg->GetPhysicalFormat();
    fSectorOrder = pImg->GetSectorOrder();
    fFSFormat = pImg->GetFSFormat();

    if (pImg->ShowAsBlocks())
        fDisplayFormat = kShowAsBlocks;
    else
        fDisplayFormat = kShowAsSectors;
    if (!pImg->GetHasBlocks() && !pImg->GetHasSectors())
        fDisplayFormat = kShowAsNibbles;

    fHasSectors = pImg->GetHasSectors();
    fHasBlocks = pImg->GetHasBlocks();
    fHasNibbles = pImg->GetHasNibbles();

    // "Unknown" formats default to sectors, but sometimes it's block-only
    if (fDisplayFormat == kShowAsSectors && !fHasSectors)
        fDisplayFormat = kShowAsBlocks;

    fInitialized = true;
}


/*
 * Configure the combo boxes.
 */
BOOL
ImageFormatDialog::OnInitDialog(void)
{
    ASSERT(fInitialized);

    LoadComboBoxes();

    return CDialog::OnInitDialog();     // do DDX/DDV
}

/*
 * Load the combo boxes with every possible entry, and set the current
 * value appropriately.
 *
 * While we're at it, initialize the "source" edit text box and the
 * "show as blocks" checkbox.
 */
void
ImageFormatDialog::LoadComboBoxes(void)
{
    CWnd* pWnd;
    CButton* pButton;

    pWnd = GetDlgItem(IDC_DECONF_SOURCE);
    ASSERT(pWnd != nil);
    pWnd->SetWindowText(fFileSource);

    if (fQueryDisplayFormat) {
        pButton = (CButton*) GetDlgItem(IDC_DECONF_VIEWASSECTORS);
        ASSERT(pButton != nil);
        pButton->SetCheck(fDisplayFormat == kShowAsSectors);
        if (!fHasSectors)
            pButton->EnableWindow(FALSE);

        pButton = (CButton*) GetDlgItem(IDC_DECONF_VIEWASBLOCKS);
        ASSERT(pButton != nil);
        pButton->SetCheck(fDisplayFormat == kShowAsBlocks);
        if (!fHasBlocks)
            pButton->EnableWindow(FALSE);

        pButton = (CButton*) GetDlgItem(IDC_DECONF_VIEWASNIBBLES);
        ASSERT(pButton != nil);
        pButton->SetCheck(fDisplayFormat == kShowAsNibbles);
        if (!fHasNibbles)
            pButton->EnableWindow(FALSE);
    } else {
        /* if we don't need to ask, don't show the buttons */
        pWnd = GetDlgItem(IDC_DECONF_VIEWAS);
        pWnd->DestroyWindow();
        pWnd = GetDlgItem(IDC_DECONF_VIEWASBLOCKS);
        pWnd->DestroyWindow();
        pWnd = GetDlgItem(IDC_DECONF_VIEWASSECTORS);
        pWnd->DestroyWindow();
        pWnd = GetDlgItem(IDC_DECONF_VIEWASNIBBLES);
        pWnd->DestroyWindow();
    }

    LoadComboBox(IDC_DECONF_OUTERFORMAT, gOuterFormats, fOuterFormat);
    LoadComboBox(IDC_DECONF_FILEFORMAT, gFileFormats, fFileFormat);
    LoadComboBox(IDC_DECONF_PHYSICAL, gPhysicalFormats, fPhysicalFormat);
    LoadComboBox(IDC_DECONF_SECTORORDER, gSectorOrders, fSectorOrder);
    LoadComboBox(IDC_DECONF_FSFORMAT, gFSFormats, fFSFormat);
}

/*
 * Load the strings from ConvTable into the combo box, setting the
 * entry matching "default" as the current entry.
 */
void
ImageFormatDialog::LoadComboBox(int boxID, const ConvTable* pTable, int dflt)
{
    CComboBox* pCombo;
//  const ConvTable* pBaseTable = pTable;
    int current = -1;
    int idx, idxShift;

    pCombo = (CComboBox*) GetDlgItem(boxID);
    ASSERT(pCombo != nil);

    idx = idxShift = 0;
    while (pTable[idx].enumval != kLastEntry) {
        /* special-case the generic FS formats */
        if (pTable == gFSFormats && !fAllowGenericFormats &&
            DiskImg::IsGenericFormat((DiskImg::FSFormat)pTable[idx].enumval))
        {
            WMSG1("LoadComboBox skipping '%ls'\n", pTable[idx].name);
            idxShift++;
        } else {
            // Note to self: AddString returns the combo box item ID;
            // should probably use that instead of doing math.
            pCombo->AddString(pTable[idx].name);
            pCombo->SetItemData(idx - idxShift, pTable[idx].enumval);
        }

        if (pTable[idx].enumval == dflt)
            current = idx - idxShift;

        idx++;
    }

    if (current != -1) {
        WMSG3(" Set default for %d/%d to %d\n", boxID, dflt, current);
        pCombo->SetCurSel(current);
    } else {
        WMSG2(" No matching default for %d (%d)\n", boxID, dflt);
    }

}

/*
 * Find the enum value for the specified index.
 */
int
ImageFormatDialog::ConvComboSel(int boxID, const ConvTable* pTable)
{
    CComboBox* pCombo;
    int idx, enumval;

    pCombo = (CComboBox*) GetDlgItem(boxID);
    ASSERT(pCombo != nil);
    idx = pCombo->GetCurSel();

    if (idx < 0) {
        /* nothing selected?! */
        ASSERT(false);
        return 0;
    }

//  enumval = pTable[idx].enumval;
    enumval = pCombo->GetItemData(idx);
    ASSERT(enumval >= 0 && enumval < 100);

    if (pTable != gFSFormats) {
        ASSERT(enumval == pTable[idx].enumval);
    }

    WMSG3(" Returning ev=%d for %d entry '%ls'\n",
        enumval, boxID, pTable[idx].name);

    return enumval;
}

/*
 * Handle the "OK" button by extracting values from the dialog and
 * verifying that reasonable settings are in place.
 */
void
ImageFormatDialog::OnOK(void)
{
    CButton* pButton;

    if (fQueryDisplayFormat) {
        pButton = (CButton*) GetDlgItem(IDC_DECONF_VIEWASSECTORS);
        ASSERT(pButton != nil);
        if (pButton->GetCheck())
            fDisplayFormat = kShowAsSectors;

        pButton = (CButton*) GetDlgItem(IDC_DECONF_VIEWASBLOCKS);
        ASSERT(pButton != nil);
        if (pButton->GetCheck())
            fDisplayFormat = kShowAsBlocks;

        pButton = (CButton*) GetDlgItem(IDC_DECONF_VIEWASNIBBLES);
        ASSERT(pButton != nil);
        if (pButton->GetCheck())
            fDisplayFormat = kShowAsNibbles;
    }

    /* outer format, file format, and physical format are immutable */

    fSectorOrder = (DiskImg::SectorOrder)
                    ConvComboSel(IDC_DECONF_SECTORORDER, gSectorOrders);
    fFSFormat = (DiskImg::FSFormat)
                    ConvComboSel(IDC_DECONF_FSFORMAT, gFSFormats);

    if (fSectorOrder == DiskImg::kSectorOrderUnknown) {
        MessageBox(L"You must choose a sector ordering.", L"Error",
            MB_OK | MB_ICONEXCLAMATION);
        return;
    }

    if (fFSFormat == DiskImg::kFormatUnknown &&
        !fAllowUnknown)
    {
        MessageBox(L"You must choose a filesystem format. If not known,"
                   L" use one of the 'generic' entries.",
            L"Error", MB_OK | MB_ICONEXCLAMATION);
        return;
    }

    CDialog::OnOK();
}

/*
 * F1 key hit, or '?' button in title bar used to select help for an
 * item in the dialog.
 */
BOOL
ImageFormatDialog::OnHelpInfo(HELPINFO* lpHelpInfo)
{
    WinHelp(lpHelpInfo->iCtrlId, HELP_CONTEXTPOPUP);
    return TRUE;    // indicate success??
}

/*
 * User pressed the "Help" button.
 */
void
ImageFormatDialog::OnHelp(void)
{
    WinHelp(HELP_TOPIC_DISK_IMAGES, HELP_CONTEXT);
}
